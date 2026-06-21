// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
// Two-stage tile cache for v0.2 streaming.
//
//   stage 1 (worker pool) : ITileLoader::fetch -> Bytes
//   stage 2 (render thread): load_glb -> GltfScene with GL objects
//
// GL calls must stay on the thread that owns the context, so glTF parse +
// buffer/texture upload run on the render thread only. The slow part -- HTTP
// or disk fetch -- is what we parallelize.

#pragma once

#include "gltf_scene.h"
#include "tile_loader.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace v3dt
{

class TileCache
{
   public:
    TileCache(ITileLoader& loader, int n_workers, std::size_t resident_cap);
    ~TileCache();

    TileCache(const TileCache&)            = delete;
    TileCache& operator=(const TileCache&) = delete;

    // Called from the render thread. Returns the resident scene if loaded,
    // or nullptr if not yet ready (in which case an async fetch is enqueued
    // for next frames). Returns nullptr permanently for tiles whose fetch
    // or parse failed.
    //
    // `priority` orders the fetch queue: smaller is fetched first. The renderer
    // passes the camera-to-tile distance, so on-screen, close tiles stream in
    // before far ones. A still-queued tile re-requested with a smaller value is
    // promoted, so priorities track the camera as it moves.
    GltfScene* get(const std::string& uri, float priority);

    // Drain completed worker payloads on the render thread: parse + GL upload
    // them into the resident cache. Call once per frame before drawing.
    // `max_uploads_per_frame` caps the work done per call to avoid hitching;
    // pending payloads stay queued for next frame. Also bumps the frame
    // counter used by LRU and runs eviction if the cache is over `cap`.
    void pump_uploads(int max_uploads_per_frame = 8);

    std::size_t resident_count() const
    {
        return cache_.size();
    }
    std::size_t pending_count();

   private:
    struct Result
    {
        std::string uri;
        Bytes       bytes;
        bool        failed = false;
    };
    struct Entry
    {
        std::unique_ptr<GltfScene> scene;
        bool                       failed          = false;
        std::uint64_t              last_used_frame = 0;
    };

    void evict_if_over_cap();

    void worker_loop();

    ITileLoader& loader_;
    std::size_t  resident_cap_;

    std::vector<std::thread> workers_;
    std::atomic<bool>        stop_{false};

    // pq_, pq_index_ and in_flight_ are protected by queue_mtx_.
    std::mutex              queue_mtx_;
    std::condition_variable queue_cv_;
    // Pending fetches ordered by priority (smallest key = most urgent). A tile
    // still waiting here can be re-prioritized as the camera moves; pq_index_
    // maps its uri to its slot so get() can find and re-key it in O(log n).
    std::multimap<float, std::string>                                            pq_;
    std::unordered_map<std::string, std::multimap<float, std::string>::iterator> pq_index_;
    // Tiles a worker has already picked up and is fetching, or that finished
    // but are not yet pumped. Not re-prioritizable; kept only for dedup.
    std::unordered_set<std::string> in_flight_;

    // results_ is protected by result_mtx_. result_cv_ provides backpressure: a
    // worker blocks before pushing once results_ reaches its cap, so a hostile or
    // huge visible set cannot pile up fetched-but-unpumped payloads without bound.
    std::mutex              result_mtx_;
    std::condition_variable result_cv_;
    std::deque<Result>      results_;

    // cache_ + current_frame_ are render-thread-only.
    std::unordered_map<std::string, Entry> cache_;
    std::uint64_t                          current_frame_ = 0;
};

}  // namespace v3dt
