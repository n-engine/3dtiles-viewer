// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "renderer.h"
#include "camera.h"
#include "gltf_scene.h"
#include "gpu_caps.h"
#include "tileset.h"

#include <epoxy/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

namespace v3dt
{

namespace
{

// Bit-pattern finiteness check (exponent all-ones => Inf/NaN); integer test, so
// it is not folded away by float optimizations.
bool comp_finite(float v)
{
    std::uint32_t u;
    std::memcpy(&u, &v, sizeof(u));
    return (u & 0x7F800000u) != 0x7F800000u;
}

bool mat_finite(const glm::mat4& m)
{
    const float* p = glm::value_ptr(m);
    for (int i = 0; i < 16; ++i)
        if (!comp_finite(p[i]))
            return false;
    return true;
}

bool mat_finite(const glm::mat3& m)
{
    const float* p = glm::value_ptr(m);
    for (int i = 0; i < 9; ++i)
        if (!comp_finite(p[i]))
            return false;
    return true;
}

}  // namespace

namespace
{

std::string slurp(const std::string& path)
{
    std::ifstream f(path);
    if (!f)
        return {};
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

// Prepend the GLSL #version the caps layer chose, so one shader source serves
// every context we accept (desktop core 3.3..4.6 and GL ES 3.0). A #version
// already present in the source is dropped first -- it must be the first line.
std::string with_version(std::string src)
{
    if (src.rfind("#version", 0) == 0)
    {
        const std::size_t nl = src.find('\n');
        src                  = (nl == std::string::npos) ? std::string() : src.substr(nl + 1);
    }
    return gpu_caps().glsl_directive + src;
}

std::uint32_t compile_shader(GLenum type, const std::string& src, const char* tag)
{
    std::uint32_t s = glCreateShader(type);
    const char*   p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[renderer] %s compile failed:\n%s\n", tag, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

std::uint32_t link_program(std::uint32_t vs, std::uint32_t fs)
{
    std::uint32_t p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[renderer] link failed:\n%s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// Six clip-space frustum planes extracted from a view-projection matrix
// (Gribb-Hartmann). Each plane is stored as (a, b, c, d) with the xyz part
// normalized, so a*x + b*y + c*z + d is the signed distance of a world point
// to the plane; positive is inside the frustum.
struct Frustum
{
    glm::vec4 planes[6];
};

Frustum make_frustum(const glm::mat4& m)
{
    // glm is column-major: row i of m is (m[0][i], m[1][i], m[2][i], m[3][i]).
    const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);

    Frustum f;
    f.planes[0] = r3 + r0;  // left
    f.planes[1] = r3 - r0;  // right
    f.planes[2] = r3 + r1;  // bottom
    f.planes[3] = r3 - r1;  // top
    f.planes[4] = r3 + r2;  // near
    f.planes[5] = r3 - r2;  // far
    for (auto& p : f.planes)
    {
        const float len = glm::length(glm::vec3(p));
        if (len > 0.0f)
            p /= len;
    }
    return f;
}

// True when the world-space sphere lies entirely outside at least one plane,
// i.e. it cannot contribute any pixel and the whole subtree can be skipped.
bool sphere_outside_frustum(const Frustum& f, const glm::vec3& c, float radius)
{
    for (const auto& p : f.planes)
    {
        const float signed_dist = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
        if (signed_dist < -radius)
            return true;
    }
    return false;
}

}  // namespace

bool Renderer::init(const std::string& shader_dir)
{
    std::string vs_src = slurp(shader_dir + "/mesh.vert");
    std::string fs_src = slurp(shader_dir + "/mesh.frag");
    if (vs_src.empty() || fs_src.empty())
    {
        std::fprintf(stderr, "[renderer] missing shaders in %s\n", shader_dir.c_str());
        return false;
    }
    vs_src           = with_version(vs_src);
    fs_src           = with_version(fs_src);
    std::uint32_t vs = compile_shader(GL_VERTEX_SHADER, vs_src, "vertex");
    std::uint32_t fs = compile_shader(GL_FRAGMENT_SHADER, fs_src, "fragment");
    if (!vs || !fs)
    {
        // One may have compiled while the other failed; do not leak it.
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return false;
    }
    prog_ = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!prog_)
        return false;

    loc_mvp_            = glGetUniformLocation(prog_, "u_mvp");
    loc_model_          = glGetUniformLocation(prog_, "u_model");
    loc_normal_mat_     = glGetUniformLocation(prog_, "u_normal_mat");
    loc_base_color_     = glGetUniformLocation(prog_, "u_base_color");
    loc_base_color_tex_ = glGetUniformLocation(prog_, "u_base_color_tex");
    loc_has_tex_        = glGetUniformLocation(prog_, "u_has_tex");
    loc_camera_pos_     = glGetUniformLocation(prog_, "u_camera_pos");
    loc_sun_dir_        = glGetUniformLocation(prog_, "u_sun_dir");

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (!gpu_caps().is_software)
        glEnable(GL_MULTISAMPLE);
    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
    return true;
}

void Renderer::shutdown()
{
    if (prog_)
    {
        glDeleteProgram(prog_);
        prog_ = 0;
    }
}

void Renderer::begin_frame(int fb_w, int fb_h)
{
    viewport_w_ = std::max(1, fb_w);
    viewport_h_ = std::max(1, fb_h);
    glViewport(0, 0, fb_w, fb_h);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // glPolygonMode is desktop-GL only; it does not exist on GL ES (it would
    // raise GL_INVALID_ENUM every frame), so wireframe is a desktop-only aid.
    if (!gpu_caps().es)
        glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
    stats_.draws_last_frame     = 0;
    stats_.triangles_last_frame = 0;
    stats_.tiles_visited        = 0;
    stats_.tiles_refined        = 0;
    stats_.tiles_missing        = 0;
    stats_.tiles_culled         = 0;
}

void Renderer::draw_scene(const GltfScene& scene, const glm::mat4& model, const glm::mat4& vp)
{
    glm::mat4 mvp  = vp * model;
    glm::mat3 nmat = glm::transpose(glm::inverse(glm::mat3(model)));
    // The tile transform is untrusted: it can be finite-but-overflowing (mvp ->
    // Inf) or finite-but-singular (zero scale -> NaN normal matrix from the
    // inverse). Never push Inf/NaN to the GPU -- skip the tile. Logged once (a
    // hostile tile would recur every frame).
    if (!mat_finite(mvp) || !mat_finite(model) || !mat_finite(nmat))
    {
        static bool warned = false;
        if (!warned)
        {
            std::fprintf(stderr, "[renderer] skipping tile with non-finite transform\n");
            warned = true;
        }
        return;
    }
    glUniformMatrix4fv(loc_mvp_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(loc_model_, 1, GL_FALSE, glm::value_ptr(model));
    glUniformMatrix3fv(loc_normal_mat_, 1, GL_FALSE, glm::value_ptr(nmat));

    for (const auto& m : scene.meshes)
    {
        const GpuMaterial* mat =
            (m.material_index >= 0 &&
             static_cast<std::size_t>(m.material_index) < scene.materials.size())
                ? &scene.materials[m.material_index]
                : nullptr;
        glm::vec4 color = mat ? mat->base_color_factor : glm::vec4(0.8f);
        glUniform4fv(loc_base_color_, 1, glm::value_ptr(color));

        bool has_tex = (mat && mat->base_color_tex != 0);
        glUniform1i(loc_has_tex_, has_tex ? 1 : 0);
        if (has_tex)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, mat->base_color_tex);
            glUniform1i(loc_base_color_tex_, 0);
        }

        if (mat && mat->double_sided)
            glDisable(GL_CULL_FACE);
        else
        {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        }

        glBindVertexArray(m.vao);
        glDrawElements(GL_TRIANGLES, m.index_count, m.index_type, nullptr);
        stats_.draws_last_frame++;
        stats_.triangles_last_frame += m.index_count / 3;
    }
}

void Renderer::draw_tileset(const Camera& cam, const Tileset& tileset, const SceneFetchFn& fetch,
                            const glm::mat4& object_transform)
{
    auto t0 = std::chrono::steady_clock::now();
    if (!tileset.root)
        return;
    glUseProgram(prog_);
    const glm::mat4 vp = cam.proj() * cam.view();

    glUniform3fv(loc_camera_pos_, 1, glm::value_ptr(cam.position()));
    const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f));
    glUniform3fv(loc_sun_dir_, 1, glm::value_ptr(sun));

    // proj[1][1] = 1 / tan(fov_y/2) for a standard perspective projection,
    // exactly the factor we want in the SSE numerator.
    const float     inv_tan_half_fov = cam.proj()[1][1];
    const glm::vec3 cam_pos          = cam.position();

    // object_transform pivots the whole scene in world space (user-driven
    // rotate/translate from the touch UI). The bv was computed pre-object;
    // we transform its center the same way the geometry will move.
    auto world_center = [&](const glm::vec3& c) -> glm::vec3
    { return glm::vec3(object_transform * glm::vec4(c, 1.0f)); };

    // Hierarchical frustum culling: a node whose bounding sphere is fully
    // off-screen cannot contribute pixels, and neither can its children (their
    // bounding volumes are contained in the parent's), so the whole subtree is
    // skipped -- no traversal, no fetch. object_transform is rigid (rotate +
    // translate, no scale), so the sphere radius is unchanged in world space.
    const Frustum frustum = make_frustum(vp);

    std::function<void(const TileNode&)> visit = [&](const TileNode& n)
    {
        stats_.tiles_visited++;

        const glm::vec3 bv_world_c = world_center(n.bv.center);
        if (frustum_cull && sphere_outside_frustum(frustum, bv_world_c, n.bv.radius))
        {
            stats_.tiles_culled++;
            return;
        }

        const float center_dist  = glm::distance(bv_world_c, cam_pos);
        const float surface_dist = std::max(center_dist - n.bv.radius, 0.01f);
        const float sse_px =
            (n.geometric_error * static_cast<float>(viewport_h_) * inv_tan_half_fov) /
            (2.0f * surface_dist);
        const bool should_refine = !n.children.empty() && sse_px > sse_pixels;

        if (should_refine && n.refine == TileRefine::Replace)
        {
            stats_.tiles_refined++;
            for (const auto& c : n.children)
                visit(*c);
            return;
        }

        if (!n.content_uri.empty())
        {
            // Closer tiles get a smaller priority value, so they stream first.
            GltfScene* scene = fetch(n.content_uri, surface_dist);
            if (scene)
            {
                draw_scene(*scene, object_transform * n.transform, vp);
            }
            else
            {
                stats_.tiles_missing++;
            }
        }

        if (should_refine && n.refine == TileRefine::Add)
        {
            stats_.tiles_refined++;
            for (const auto& c : n.children)
                visit(*c);
        }
    };
    visit(*tileset.root);
    glBindVertexArray(0);

    auto t1              = std::chrono::steady_clock::now();
    stats_.last_frame_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
}

void Renderer::end_frame()
{
    stats_.frames++;
}

}  // namespace v3dt
