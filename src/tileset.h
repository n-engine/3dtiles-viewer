// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
// 3D Tiles 1.0 / 1.1 tileset.json parser.
//
// v0.2: keeps the full tile tree so the renderer can do screen-space-error
// driven refinement. The previous flat `leaves` list is still emitted as a
// derived view for callers that only need to iterate content tiles.

#pragma once

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace v3dt
{

class ITileLoader;

// 3D Tiles bounding volumes (box / sphere / region) are normalized to a single
// world-space sphere here. Boxes are over-approximated by the enclosing sphere
// of the OBB; regions get a generous sphere derived from the geometric error.
struct BoundingSphere
{
    glm::vec3 center = glm::vec3(0.0f);
    float     radius = 0.0f;
};

// Per the 3D Tiles spec, "refine" controls whether children replace their
// parent (LOD pyramid) or add to it (additive detail).
enum class TileRefine
{
    Replace,
    Add
};

struct TileNode
{
    // Cumulative world transform up to and including this node.
    glm::mat4      transform = glm::mat4(1.0f);
    BoundingSphere bv{};
    float          geometric_error = 0.0f;
    TileRefine     refine          = TileRefine::Replace;
    // Empty when this node has no content.
    std::string                            content_uri;
    std::vector<std::unique_ptr<TileNode>> children;
};

struct Tileset
{
    std::unique_ptr<TileNode> root;
    glm::mat4                 root_transform       = glm::mat4(1.0f);
    float                     root_geometric_error = 0.0f;
    std::size_t               total_leaves         = 0;  // content-bearing nodes

    // Apply a floating-origin rebase + frame alignment to every node in the
    // tree. `anchor` is subtracted from the translation column of each
    // transform; `align` is then left-multiplied.
    void apply_world_anchor(const glm::vec3& anchor, const glm::mat4& align);
};

// Parse the tileset.json fetched via `loader`. Returns false + sets *err on
// failure. Populates `out.root` (full tree).
bool parse_tileset(ITileLoader& loader, Tileset& out, std::string* err);

}  // namespace v3dt
