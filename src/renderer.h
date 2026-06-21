// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#pragma once

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <string>

namespace v3dt
{

struct GltfScene;
struct Tileset;
class Camera;

struct DrawStats
{
    std::uint64_t frames               = 0;
    std::uint32_t draws_last_frame     = 0;
    std::uint32_t triangles_last_frame = 0;
    std::uint32_t tiles_visited        = 0;
    std::uint32_t tiles_refined        = 0;
    std::uint32_t tiles_missing        = 0;
    std::uint32_t tiles_culled         = 0;
    float         last_frame_ms        = 0.0f;
};

class Renderer
{
   public:
    bool init(const std::string& shader_dir);
    void shutdown();

    void begin_frame(int fb_width, int fb_height);

    // Caller supplies a fetch callback that resolves a content URI to an
    // uploaded GltfScene. The callback returns nullptr when the scene is not
    // resident yet (async loader still working / failed); the renderer simply
    // skips that tile for this frame. `priority` is a fetch-ordering hint
    // (smaller = fetch sooner); the renderer passes the camera-to-tile distance.
    using SceneFetchFn = std::function<GltfScene*(const std::string& uri, float priority)>;
    void draw_tileset(const Camera& cam, const Tileset& tileset, const SceneFetchFn& fetch,
                      const glm::mat4& object_transform = glm::mat4(1.0f));
    void end_frame();

    DrawStats stats() const
    {
        return stats_;
    }

    // Public knobs.
    bool  wireframe    = false;
    bool  frustum_cull = true;   // skip tiles whose bounding sphere is off-screen.
    float sse_pixels   = 16.0f;  // refine when projected geometric error exceeds this.

   private:
    void draw_scene(const GltfScene& scene, const glm::mat4& model, const glm::mat4& vp);

    std::uint32_t prog_               = 0;
    std::int32_t  loc_mvp_            = -1;
    std::int32_t  loc_model_          = -1;
    std::int32_t  loc_normal_mat_     = -1;
    std::int32_t  loc_base_color_     = -1;
    std::int32_t  loc_base_color_tex_ = -1;
    std::int32_t  loc_has_tex_        = -1;
    std::int32_t  loc_camera_pos_     = -1;
    std::int32_t  loc_sun_dir_        = -1;

    int viewport_w_ = 1;
    int viewport_h_ = 1;

    DrawStats stats_{};
};

}  // namespace v3dt
