// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
// Abstraction over a tileset data source.
//
// Two intended implementations:
//   - FileTileLoader  : reads tileset.json + .glb from a local filesystem path
//   - HttpTileLoader  : reads them over HTTP(S) via libcurl
//
// Both expose the same blocking API for v0.1. v0.2 will add an async variant
// that returns std::future<Bytes> so streaming + frustum-driven loading don't
// block the render thread.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace v3dt
{

using Bytes = std::vector<std::uint8_t>;

enum class TileLoaderMode
{
    File,
    Http
};

class ITileLoader
{
   public:
    virtual ~ITileLoader() = default;

    // Source descriptor (filesystem path or URL).
    virtual std::string root() const = 0;

    // Read a logical resource path, relative to the tileset root.
    // Returns empty Bytes + sets *err on failure.
    virtual Bytes fetch(const std::string& relative_path, std::string* err = nullptr) = 0;

    // Human-readable backend tag for the overlay.
    virtual const char* backend_name() const = 0;
};

std::unique_ptr<ITileLoader> make_file_loader(const std::string& tileset_path, std::string* err);

#if defined(VIEWER_HAS_HTTP)
std::unique_ptr<ITileLoader> make_http_loader(const std::string& tileset_url, std::string* err);
#endif

}  // namespace v3dt
