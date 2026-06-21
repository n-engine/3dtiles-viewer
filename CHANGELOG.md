# Changelog

Notable changes to this project. The version is the CMake `project()` version,
surfaced via `--version`.

## 0.2.0 - First public release

- LOD streaming: screen-space-error driven tile refinement; `.glb` leaves
  fetched on demand through a worker thread pool + LRU cache.
- Hierarchical frustum culling (Gribb-Hartmann); off-screen subtrees are never
  traversed or fetched.
- Distance-prioritized fetch queue with live re-prioritization as the camera
  moves.
- Portable GL context negotiation: a version ladder from desktop core 3.3..4.6
  down to a portable floor, or GL ES 3.0 via `--gles`. Runs on real GPUs and on
  software / virtualized stacks (llvmpipe, virgl). One shader source serves
  every accepted context; MSAA is skipped on software rasterizers.
- Untrusted-input hardening of the `.glb` / `tileset.json` parsers (all content
  is treated as hostile): overflow-safe accessor / bufferView / sparse bounds,
  component-alignment checks, index range-checking and clamping, allocation and
  tree-depth caps, texture decompression-bomb guards, and OOM-safe loading.
- Non-finite (NaN / Inf) rejection in transforms, bounding volumes, and vertex
  data, via a bit-pattern test that survives `-ffinite-math-only` (which is why
  `-ffast-math` is deliberately not used).
- libcurl transport lock-down: restricted to `http,https` on both the request
  and redirects, with a redirect cap and a low-speed abort.
- RFC 3986 URI resolution for content URIs (relative `..`, absolute paths, query
  strings, scheme-relative), replacing the naive base + relative join.
- CI (GCC + Clang, HTTP loader on and off), a unit-tested URI resolver, and a
  `clang-format` gate.
- `file://` and `http(s)://` tileset sources; touchscreen control overlay.

## 0.1.0 - Unreleased

Internal predecessor. Never published.
