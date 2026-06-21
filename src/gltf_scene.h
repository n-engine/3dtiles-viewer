// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
// Load a .glb (cgltf) and upload its primitives to GPU buffers.
// One Mesh per primitive; primitives sharing a material share its GL texture.

#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace v3dt
{

struct GpuMesh
{
    std::uint32_t vao            = 0;
    std::uint32_t vbo_pos        = 0;
    std::uint32_t vbo_normal     = 0;
    std::uint32_t vbo_uv         = 0;
    std::uint32_t ebo            = 0;
    std::uint32_t index_count    = 0;
    std::uint32_t index_type     = 0;  // GL_UNSIGNED_INT / SHORT / BYTE
    std::int32_t  material_index = -1;

    glm::vec3 aabb_min{0};
    glm::vec3 aabb_max{0};
};

struct GpuMaterial
{
    std::uint32_t base_color_tex = 0;  // 0 == no texture, use base_color_factor
    glm::vec4     base_color_factor{1.0f};
    bool          double_sided = false;
};

struct GltfScene
{
    std::vector<GpuMesh>     meshes;
    std::vector<GpuMaterial> materials;
    glm::vec3                aabb_min{0};
    glm::vec3                aabb_max{0};

    void destroy();
};

// Load a glTF binary blob in memory and upload everything to the GPU.
// Returns false + sets *err on failure.
bool load_glb(const std::uint8_t* data, std::size_t size, GltfScene& out, std::string* err);

}  // namespace v3dt
