# Roadmap

`3dtiles-viewer` is a lean, security-conscious 3D Tiles viewer. This document
tracks what is in place and what is planned. Items are grouped by theme and
ordered roughly by priority within each group.

## Shipped

- Screen-space-error driven LOD with REPLACE / ADD refinement.
- Hierarchical frustum culling (Gribb-Hartmann); a fully off-screen subtree is
  never traversed or fetched.
- Distance-prioritized streaming: a worker-pool fetches bytes, the render thread
  parses glTF and uploads GL objects, with an LRU-bounded resident set.
- Portable GL: a context version ladder (desktop core 3.3..4.6, or GL ES 3.0)
  with software-rasterizer awareness (llvmpipe / virgl); one shader source
  serves every target via load-time `#version` injection.
- Untrusted-input hardening of the `.glb` / `tileset.json` parsers: overflow-safe
  accessor / bufferView / sparse bounds, component-alignment checks, index
  clamping, allocation and tree-depth caps, bounded HTTP fetch, OOM-safe loading.
- Non-finite (NaN / Inf) rejection in transforms, bounding volumes, and vertex
  data before it reaches culling, SSE, or the GPU (bit-pattern test, immune to
  `-ffinite-math-only`).
- libcurl transport lock-down: `http,https` only on the request and on redirects,
  a redirect cap, and a low-speed abort, so a hostile redirect cannot escape to
  another protocol or pin a worker thread.
- RFC 3986 URI resolution for content URIs (relative `..`, absolute paths, query
  strings, scheme-relative references) instead of naive base + relative joining.
- CI (GCC + Clang matrix, HTTP loader on and off) with a unit-tested URI resolver
  and a `clang-format` gate.

## Stability

- Per-tile resource budgets (CPU bytes, estimated GPU bytes, primitive /
  material / texture counts, total texture pixels) rather than a single element
  cap; move the LRU from leaf-count to estimated-cost eviction.
- External sub-tilesets: a content URI pointing at another `tileset.json`,
  resolved against its own base rather than the root (the resolver already
  handles the URI math; this is the loader/cache plumbing).
- Move-only RAII wrappers for GL objects (buffer / texture / VAO / program) to
  make partial-failure paths leak-free by construction.

## Tooling

- Continuous fuzzing (libFuzzer / AFL++ with ASan + UBSan) of `load_glb()`,
  `parse_tileset()`, and URI resolution, with a seeded corpus of adversarial
  inputs (overflowing / sparse accessors, broken alignment, embedded images,
  malformed JSON, deep trees).

## Features

- Online path tracer: a progressive path-traced view for high-fidelity stills
  over the streamed geometry.
- Live vehicle position injection: stream a pose and draw a vehicle moving along
  a traceable path in real time.
- Route tracing over a navigation mesh: A* / navmesh pathfinding (Recast/Detour
  class) across the loaded terrain.
- GL ES 2.0 floor via a load-time GLSL dialect rewrite (legacy embedded targets),
  gated on the uint32-index trade-off that ES 2.0 lacks without extensions.

## Extensibility

The viewer should be a base others build on, not a closed app:

- Scene-object plugins: a stable API to inject external 3D content -- points of
  interest, models with their own geometry and textures, billboards / markers --
  anchored to geospatial coordinates and decoupled from the tile stream.
  Example: a vendor overlays a 3D model of their drone detector at its real
  location, optionally driven by live data. (Live vehicle position injection,
  above, is one instance of this same injection layer.)
- UI plugins: pluggable panels and controls so integrators can add their own
  overlays and interactions without forking the viewer.

## Format support

Supported today: `.glb` with embedded buffers, glTF 2.0 baseline geometry,
PNG / JPEG textures (always) and WebP (when built with `ENABLE_WEBP_TEXTURE`).

Not supported: Draco / meshopt compression, KTX2 / Basis textures, external
`.bin` buffers, and the legacy b3dm / pnts / i3dm tile payloads.
