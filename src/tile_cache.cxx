// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "tile_cache.h"

#include <algorithm>
#include <cstdio>
#include <new>
#include <utility>
#include <vector>

namespace v3dt
{

namespace
{
// Upper bound on the pending-fetch queue. A hostile or simply enormous tileset
// can mark a huge number of tiles "visible" in one frame; without a cap the
// queue (and its index) would grow unbounded. Excess tiles are not lost -- they
// are simply re-requested on a later frame once the queue drains.
constexpr std::size_t kMaxPending = 8192;

// Upper bound on fetched-but-not-yet-uploaded payloads. pump_uploads drains at
// most a handful per frame, so without backpressure the workers could buffer an
// unbounded number of (up to 256 MB) payloads. Workers block once this many are
// queued; the render thread wakes them as it drains.
constexpr std::size_t kMaxResults = 16;
}  // namespace

TileCache::TileCache(ITileLoader& loader, int n_workers, std::size_t resident_cap)
    : loader_(loader), resident_cap_(resident_cap)
{
    if (n_workers < 1)
        n_workers = 1;
    workers_.reserve(static_cast<std::size_t>(n_workers));
    for (int i = 0; i < n_workers; ++i)
        workers_.emplace_back(&TileCache::worker_loop, this);
}

TileCache::~TileCache()
{
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        stop_ = true;
        // Drop the pending backlog so workers exit promptly instead of draining
        // every queued fetch on shutdown.
        pq_.clear();
        pq_index_.clear();
    }
    queue_cv_.notify_all();
    result_cv_.notify_all();  // wake any worker blocked on result backpressure
    for (auto& t : workers_)
        if (t.joinable())
            t.join();
    for (auto& kv : cache_)
        if (kv.second.scene)
            kv.second.scene->destroy();
}

void TileCache::worker_loop()
{
    while (true)
    {
        std::string uri;
        bool        have_uri = false;
        // Any allocation on this thread (map ops, fetch buffer, result push) can
        // throw; an exception escaping a worker thread is std::terminate. Contain
        // it: log, release the in-flight slot, and carry on with the next fetch.
        try
        {
            {
                std::unique_lock<std::mutex> lk(queue_mtx_);
                queue_cv_.wait(lk, [&] { return stop_.load() || !pq_.empty(); });
                if (stop_.load())
                    return;             // exit immediately on shutdown; do not drain the backlog
                auto it = pq_.begin();  // smallest key = most urgent
                uri     = std::move(it->second);
                pq_.erase(it);
                pq_index_.erase(uri);
                in_flight_.insert(uri);
                have_uri = true;
            }
            std::string fetch_err;
            Bytes       bytes = loader_.fetch(uri, &fetch_err);
            Result      r;
            r.uri = uri;  // keep `uri` valid for the catch's in-flight cleanup
            if (bytes.empty())
            {
                std::fprintf(stderr, "[cache] fetch %s failed: %s\n", uri.c_str(),
                             fetch_err.c_str());
                r.failed = true;
            }
            else
            {
                r.bytes = std::move(bytes);
            }
            {
                std::unique_lock<std::mutex> lk(result_mtx_);
                // Backpressure: block until the render thread has drained room (or
                // we are shutting down). Bounds peak buffered-payload memory.
                result_cv_.wait(lk, [&] { return stop_.load() || results_.size() < kMaxResults; });
                if (stop_.load())
                    return;  // drop this payload; we are tearing down
                results_.push_back(std::move(r));
            }
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "[cache] worker error: %s\n", e.what());
            if (have_uri)
            {
                std::lock_guard<std::mutex> lk(queue_mtx_);
                in_flight_.erase(uri);  // let it be retried on a later frame
            }
        }
    }
}

GltfScene* TileCache::get(const std::string& uri, float priority)
{
    auto it = cache_.find(uri);
    if (it != cache_.end())
    {
        it->second.last_used_frame = current_frame_;
        return it->second.failed ? nullptr : it->second.scene.get();
    }

    // Enqueue (or re-prioritize) if not already owned by a worker. Eviction in
    // pump_uploads keeps the resident set bounded.
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        if (in_flight_.count(uri))
            return nullptr;  // a worker already picked this fetch up
        auto idx = pq_index_.find(uri);
        if (idx != pq_index_.end())
        {
            // Already pending: promote it if this request is more urgent. Insert
            // the new entry BEFORE erasing the old so a throwing allocation leaves
            // the existing entry intact.
            if (priority < idx->second->first)
            {
                try
                {
                    auto h = pq_.emplace(priority, uri);
                    pq_.erase(idx->second);
                    idx->second = h;
                }
                catch (const std::bad_alloc&)
                {
                    // Keep the existing (lower-urgency) entry; it will still fetch.
                }
            }
        }
        else
        {
            // Bound the queue, and never let an allocation failure here unwind
            // into the render loop: on either, skip and retry on a later frame.
            if (pq_.size() >= kMaxPending)
                return nullptr;
            try
            {
                auto h = pq_.emplace(priority, uri);
                try
                {
                    pq_index_.emplace(uri, h);
                }
                catch (...)
                {
                    pq_.erase(h);
                    return nullptr;
                }
                queue_cv_.notify_one();
            }
            catch (const std::bad_alloc&)
            {
                return nullptr;
            }
        }
    }
    return nullptr;
}

void TileCache::pump_uploads(int max_uploads_per_frame)
{
    ++current_frame_;
    int n_uploaded = 0;
    while (n_uploaded < max_uploads_per_frame)
    {
        Result r;
        {
            std::lock_guard<std::mutex> lk(result_mtx_);
            if (results_.empty())
                break;
            r = std::move(results_.front());
            results_.pop_front();
        }
        // Release the in-flight slot regardless of outcome.
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            in_flight_.erase(r.uri);
        }

        // Parsing / GL upload / cache insertion can throw (bad_alloc); contain it
        // so a single hostile tile cannot terminate the render loop.
        try
        {
            Entry entry;
            if (r.failed)
            {
                entry.failed = true;
            }
            else
            {
                auto        scene = std::make_unique<GltfScene>();
                std::string load_err;
                if (!load_glb(r.bytes.data(), r.bytes.size(), *scene, &load_err))
                {
                    std::fprintf(stderr, "[cache] glTF parse %s failed: %s\n", r.uri.c_str(),
                                 load_err.c_str());
                    entry.failed = true;
                }
                else
                {
                    entry.scene = std::move(scene);
                }
            }
            entry.last_used_frame = current_frame_;
            cache_.emplace(std::move(r.uri), std::move(entry));
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "[cache] upload error: %s\n", e.what());
        }
        ++n_uploaded;
    }
    // A worker may be blocked on backpressure; we just freed result slots.
    result_cv_.notify_all();
    evict_if_over_cap();
}

void TileCache::evict_if_over_cap()
{
    if (resident_cap_ == 0 || cache_.size() <= resident_cap_)
        return;

    // Gather (last_used, uri) pairs, oldest first. Skip entries touched this
    // frame: they are still considered visible. The temporary vector can throw
    // under memory pressure; if so, skip eviction this frame rather than letting
    // bad_alloc escape into the render loop.
    std::vector<std::pair<std::uint64_t, std::string>> candidates;
    try
    {
        candidates.reserve(cache_.size());
        for (const auto& kv : cache_)
        {
            if (kv.second.last_used_frame == current_frame_)
                continue;
            candidates.emplace_back(kv.second.last_used_frame, kv.first);
        }
    }
    catch (const std::exception&)
    {
        return;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Drop down to 90 % of cap to avoid evicting one tile per frame under
    // steady-state pressure.
    const std::size_t target   = (resident_cap_ * 9) / 10;
    std::size_t       to_evict = cache_.size() > target ? cache_.size() - target : 0;
    if (to_evict > candidates.size())
        to_evict = candidates.size();

    for (std::size_t i = 0; i < to_evict; ++i)
    {
        const std::string& uri = candidates[i].second;
        auto               it  = cache_.find(uri);
        if (it == cache_.end())
            continue;
        if (it->second.scene)
            it->second.scene->destroy();
        cache_.erase(it);
    }
}

std::size_t TileCache::pending_count()
{
    std::lock_guard<std::mutex> lk(queue_mtx_);
    return pq_.size() + in_flight_.size();
}

}  // namespace v3dt
