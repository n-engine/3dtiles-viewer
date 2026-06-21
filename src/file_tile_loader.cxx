// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "tile_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace v3dt
{

namespace fs = std::filesystem;

class FileTileLoader final : public ITileLoader
{
   public:
    explicit FileTileLoader(fs::path tileset_path) : tileset_path_(std::move(tileset_path))
    {
        root_dir_ = tileset_path_.parent_path();
    }

    std::string root() const override
    {
        return tileset_path_.string();
    }
    const char* backend_name() const override
    {
        return "file";
    }

    Bytes fetch(const std::string& relative_path, std::string* err) override
    {
        // A content URI may carry a query string or fragment (cache-busting on
        // tilesets authored for HTTP); the filesystem has no use for either, and
        // leaving them on turns a valid tile into a "cannot open". Strip first.
        std::string rel = relative_path;
        const auto  qf  = rel.find_first_of("?#");
        if (qf != std::string::npos)
            rel.erase(qf);

        // A reference that was nothing but a query/fragment (e.g. "?v=1") strips
        // to empty; "root_dir_ / "" " would resolve to the directory itself and
        // ifstream on a directory is platform-dependent. Reject cleanly.
        if (rel.empty())
        {
            if (err)
                *err = "empty content path";
            return {};
        }

        // If caller passes the tileset.json itself, support both "absolute
        // (same as root)" and "filename only".
        fs::path target;
        if (rel == tileset_path_.filename().string() || rel == tileset_path_.string())
        {
            target = tileset_path_;
        }
        else
        {
            // lexically_normal collapses "." / ".." so a relative content URI
            // maps to a clean path.
            target = (root_dir_ / rel).lexically_normal();
        }

        std::ifstream f(target, std::ios::binary);
        if (!f)
        {
            if (err)
                *err = "cannot open " + target.string();
            return {};
        }
        f.seekg(0, std::ios::end);
        std::streamsize n = f.tellg();
        f.seekg(0, std::ios::beg);
        if (n < 0)
        {
            if (err)
                *err = "cannot size " + target.string();
            return {};
        }
        // Bound the allocation: a tile (or a content URI pointed at a huge local
        // file) must not drive an unbounded allocation. Mirrors the HTTP loader's
        // per-fetch cap.
        constexpr std::streamsize kMaxFileBytes = 256ll * 1024 * 1024;  // 256 MB
        if (n > kMaxFileBytes)
        {
            if (err)
                *err = "file exceeds size cap: " + target.string();
            return {};
        }
        Bytes data(static_cast<std::size_t>(n));
        if (n > 0)
            f.read(reinterpret_cast<char*>(data.data()), n);
        if (!f)
        {
            if (err)
                *err = "short read on " + target.string();
            return {};
        }
        return data;
    }

   private:
    fs::path tileset_path_;
    fs::path root_dir_;
};

std::unique_ptr<ITileLoader> make_file_loader(const std::string& tileset_path, std::string* err)
{
    std::error_code ec;
    if (!fs::exists(tileset_path, ec) || ec)
    {
        if (err)
            *err = "no such file: " + tileset_path;
        return {};
    }
    return std::make_unique<FileTileLoader>(fs::path(tileset_path));
}

}  // namespace v3dt
