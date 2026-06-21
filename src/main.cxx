// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
//
// 3dtiles-viewer -- entry point.
//
// Loads a 3D Tiles 1.0 / 1.1 tileset.json (3DTILES_content_gltf extension or
// native glTF content), walks the tile tree with screen-space-error driven LOD
// selection, frustum-culls off-screen nodes, streams .glb leaves on demand
// through a worker pool + LRU cache, and renders the scene under a free-fly
// camera in an OpenGL window (desktop core 3.3..4.6, or GL ES 3.0 via --gles;
// the context version is negotiated at startup). The tileset source (local
// path or HTTP(S) URL) is selected with --mode.
//
// See README.md for controls, CLI flags, and build instructions. The package
// version is the CMake project() VERSION, surfaced here via --version.

#include <epoxy/gl.h>
// epoxy must come before any GL/gl.h. GLFW_INCLUDE_NONE keeps GLFW from
// pulling its own GL header (also a clang-format barrier so SortIncludes
// does not rearrange the order across this point).
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "gltf_scene.h"
#include "renderer.h"
#include "tile_cache.h"
#include "tile_loader.h"
#include "tileset.h"
#include "touch_ui.h"
#include "window.h"

#ifndef VIEWER_VERSION
#define VIEWER_VERSION "0.0.0-dev"
#endif

namespace fs = std::filesystem;
using namespace v3dt;

namespace
{

struct Cli
{
    std::string    source;  // path or URL
    TileLoaderMode mode       = TileLoaderMode::File;
    int            width      = 1600;
    int            height     = 1000;
    bool           fullscreen = false;
    bool           vsync      = true;
    int            max_leaves = 256;    // resident-tile LRU cap; 0 = unlimited
    float          sse_pixels = 16.0f;  // screen-space-error refinement threshold
    bool           cull       = true;   // frustum-cull off-screen tiles
    bool           gles       = false;  // request a GL ES 3.0 context
    bool           msaa       = true;   // 4x MSAA (turn off for software rasterizers)
};

void print_help(const char* argv0)
{
    std::printf(
        "Usage: %s --mode <file|http> --source <path-or-url> [options]\n"
        "\n"
        "Options:\n"
        "  --mode file|http      Choose tileset data source (default: file)\n"
        "  --source <S>          Path to tileset.json (file) or URL (http)\n"
        "  --width N             Window width (default 1600)\n"
        "  --height N            Window height (default 1000)\n"
        "  --fullscreen          Start fullscreen\n"
        "  --no-vsync            Disable vsync\n"
        "  --max-leaves N        LRU cap on resident tiles (default 256; 0 = unlimited)\n"
        "  --sse-pixels N        Refine when projected geometric error exceeds N px (default 16)\n"
        "  --no-cull             Disable frustum culling (draw off-screen tiles too)\n"
        "  --gles                Request a GL ES 3.0 context instead of desktop core\n"
        "  --no-msaa             Disable MSAA (recommended on software / VM rasterizers)\n"
        "  --version             Print version and exit\n"
        "  -h, --help            Show this help\n",
        argv0);
}

bool parse_cli(int argc, char** argv, Cli& out)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a    = argv[i];
        auto        need = [&](int n) -> bool { return i + n < argc; };
        if (a == "-h" || a == "--help")
        {
            print_help(argv[0]);
            std::exit(0);
        }
        else if (a == "--version")
        {
            std::printf("3dtiles-viewer %s\n", VIEWER_VERSION);
            std::exit(0);
        }
        else if (a == "--mode" && need(1))
        {
            std::string v = argv[++i];
            if (v == "file")
                out.mode = TileLoaderMode::File;
            else if (v == "http")
                out.mode = TileLoaderMode::Http;
            else
            {
                std::fprintf(stderr, "unknown --mode %s\n", v.c_str());
                return false;
            }
        }
        else if (a == "--source" && need(1))
            out.source = argv[++i];
        else if (a == "--width" && need(1))
            out.width = std::atoi(argv[++i]);
        else if (a == "--height" && need(1))
            out.height = std::atoi(argv[++i]);
        else if (a == "--fullscreen")
            out.fullscreen = true;
        else if (a == "--no-vsync")
            out.vsync = false;
        else if (a == "--max-leaves" && need(1))
            out.max_leaves = std::atoi(argv[++i]);
        else if (a == "--sse-pixels" && need(1))
            out.sse_pixels = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--no-cull")
            out.cull = false;
        else if (a == "--gles")
            out.gles = true;
        else if (a == "--no-msaa")
            out.msaa = false;
        else
        {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return false;
        }
    }
    if (out.source.empty())
    {
        std::fprintf(stderr, "--source required\n");
        print_help(argv[0]);
        return false;
    }
    return true;
}

std::string find_shaders_dir(const fs::path& exe_dir)
{
    // Dev / build tree: CMake POST_BUILD copies the shaders next to the binary.
    if (fs::exists(exe_dir / "shaders" / "mesh.vert"))
        return (exe_dir / "shaders").string();
    // Installed release package: self-contained bin/ + data/ layout.
    if (fs::exists(exe_dir / ".." / "data" / "shaders" / "mesh.vert"))
        return (exe_dir / ".." / "data" / "shaders").string();
    return "shaders";
}

}  // namespace

int main(int argc, char** argv)
{
    Cli cli;
    if (!parse_cli(argc, argv, cli))
        return 2;

    std::fprintf(stderr, "[main] 3dtiles-viewer %s\n", VIEWER_VERSION);

    // ----- Loader -----
    std::string                  err;
    std::unique_ptr<ITileLoader> loader;
    if (cli.mode == TileLoaderMode::File)
    {
        loader = make_file_loader(cli.source, &err);
    }
    else
    {
#if defined(VIEWER_HAS_HTTP)
        loader = make_http_loader(cli.source, &err);
#else
        std::fprintf(stderr, "HTTP loader not compiled (ENABLE_HTTP_LOADER=OFF)\n");
        return 2;
#endif
    }
    if (!loader)
    {
        std::fprintf(stderr, "loader init: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "[main] backend=%s root=%s\n", loader->backend_name(),
                 loader->root().c_str());

    Tileset tileset;
    if (!parse_tileset(*loader, tileset, &err))
    {
        std::fprintf(stderr, "parse_tileset: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "[main] tileset loaded: %zu leaves, root geomErr=%.1f\n",
                 tileset.total_leaves, tileset.root_geometric_error);

    // ----- Window + GL -----
    WindowConfig wc{};
    wc.width      = cli.width;
    wc.height     = cli.height;
    wc.fullscreen = cli.fullscreen;
    wc.vsync      = cli.vsync;
    wc.gles       = cli.gles;
    wc.samples    = cli.msaa ? 4 : 0;
    Window win;
    if (!win.init(wc))
        return 1;

    // ----- Renderer -----
    // weakly_canonical + error_code so a weird argv[0] cannot throw at startup.
    std::error_code exe_ec;
    fs::path        exe_path = fs::weakly_canonical(fs::path(argv[0]), exe_ec);
    if (exe_ec)
        exe_path = fs::path(argv[0]);
    fs::path    exe_dir    = exe_path.parent_path();
    std::string shader_dir = find_shaders_dir(exe_dir);
    Renderer    rend;
    if (!rend.init(shader_dir))
        return 1;

    // ----- Touch UI -----
    if (!touch_ui::init(win.glfw_handle()))
    {
        std::fprintf(stderr, "[main] touch_ui::init failed\n");
        return 1;
    }

    // ----- Floating origin + ECEF "up" alignment.
    //
    // Two problems with a raw ECEF-transformed scene:
    //
    //   1. Translations of magnitude 1e6-1e7 m crush f32 precision once
    //      view*model*pos is computed by the GPU. We rebase by subtracting
    //      the scene anchor from every leaf translation.
    //
    //   2. The "geographic up" at the scene location is the radial direction
    //      from Earth centre, which is an oblique vector in our untouched
    //      ECEF frame. Using it as world_up makes WASD / mouse-look move
    //      along skewed axes and feel disorienting. We rotate the entire
    //      scene so that this radial direction aligns with +Y. From then on
    //      the camera uses standard (0,1,0) up and everything reads natural.
    //
    // The anchor is taken from the root tile's bounding sphere center (in raw
    // ECEF), so the alignment is known before any .glb has been fetched. This
    // lets us defer all geometry loads to the render thread's on-demand cache.
    glm::vec3 world_anchor = tileset.root ? tileset.root->bv.center : glm::vec3(0.0f, 0.0f, 0.0f);
    // The center is finite (the tileset parser guarantees it), but a finite-yet-
    // enormous value squares to +Inf inside length(), which then poisons
    // normalize() -> the ECEF up vector -> the whole alignment / camera chain.
    // Drop to a no-anchor, identity-up setup if the magnitude is not usable.
    float anchor_len = glm::length(world_anchor);
    if (!std::isfinite(anchor_len))
    {
        world_anchor = glm::vec3(0.0f);
        anchor_len   = 0.0f;
    }
    glm::vec3 up_ecef =
        (anchor_len > 1.0f) ? glm::normalize(world_anchor) : glm::vec3(0.0f, 1.0f, 0.0f);

    glm::mat4 align(1.0f);
    {
        const glm::vec3 ty(0.0f, 1.0f, 0.0f);
        float           c = glm::clamp(glm::dot(up_ecef, ty), -1.0f, 1.0f);
        if (c < 0.99999f)
        {
            glm::vec3 axis;
            float     angle;
            if (c < -0.99999f)
            {
                axis  = glm::vec3(1.0f, 0.0f, 0.0f);
                angle = glm::pi<float>();
            }
            else
            {
                axis  = glm::normalize(glm::cross(up_ecef, ty));
                angle = std::acos(c);
            }
            align = glm::rotate(glm::mat4(1.0f), angle, axis);
        }
    }

    tileset.apply_world_anchor(world_anchor, align);

    // ----- Camera placement from the (now rebased + aligned) root bounding
    // sphere. center == origin by construction; radius is the dataset's
    // physical extent.
    Camera          cam;
    const glm::vec3 center = tileset.root ? tileset.root->bv.center : glm::vec3(0.0f);
    const float     radius = tileset.root ? std::max(1.0f, tileset.root->bv.radius) : 100.0f;
    const float     far_p  = std::max(50000.0f, radius * 50.0f);
    cam.set_perspective(60.0f, float(win.width()) / float(win.height()), 0.1f, far_p);
    glm::vec3 eye = center + glm::vec3(radius, radius * 0.6f, radius);
    cam.look_at(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    cam.save_home();
    cam.speed = std::max(1.0f, radius * 0.5f);

    std::fprintf(stderr,
                 "[main] world_anchor ECEF=(%.1f,%.1f,%.1f) root radius=%.1f cam speed=%.1f\n",
                 world_anchor.x, world_anchor.y, world_anchor.z, radius, cam.speed);

    // ----- Async on-demand tile cache.
    //
    // Worker threads fetch bytes in parallel; the render thread parses the
    // resulting glTF + uploads GL objects in pump_uploads() once per frame.
    // --max-leaves caps the resident set: above the cap, get() returns
    // nullptr and the renderer skips that tile this frame.
    const std::size_t cap = cli.max_leaves > 0 ? static_cast<std::size_t>(cli.max_leaves) : 0;
    TileCache         cache(*loader, 4, cap);
    auto              fetch_scene = [&cache](const std::string& uri, float priority) -> GltfScene*
    { return cache.get(uri, priority); };

    rend.sse_pixels   = cli.sse_pixels;
    rend.frustum_cull = cli.cull;

    // ----- Object manipulation state (applied as a pre-transform in the
    // renderer; identity = original mesh placement).
    glm::quat obj_rot(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 obj_trans(0.0f);
    auto      build_object_transform = [&](const glm::vec3& center)
    {
        return glm::translate(glm::mat4(1.0f), obj_trans + center) * glm::mat4_cast(obj_rot) *
               glm::translate(glm::mat4(1.0f), -center);
    };

    // ----- Main loop -----
    auto t_prev        = std::chrono::steady_clock::now();
    int  report_frames = 0;
    auto t_report      = t_prev;

    while (true)
    {
        InputState in;
        if (!win.poll(in))
            break;
        if (in.just_resized)
            cam.on_resize(win.width(), win.height());

        {
            static bool f1_prev = false;
            if (in.keys[GLFW_KEY_F1] && !f1_prev)
                rend.wireframe = !rend.wireframe;
            f1_prev = in.keys[GLFW_KEY_F1];
        }
        {
            static bool home_prev = false;
            if (in.keys[GLFW_KEY_HOME] && !home_prev)
            {
                cam.reset_home();
                obj_rot   = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                obj_trans = glm::vec3(0.0f);
                std::fprintf(stderr, "[input] reset to home pose\n");
            }
            home_prev = in.keys[GLFW_KEY_HOME];
        }

        auto  t_now = std::chrono::steady_clock::now();
        float dt    = std::chrono::duration<float>(t_now - t_prev).count();
        t_prev      = t_now;

        cam.update(in, dt);

        // ----- Touch UI -----
        touch_ui::begin_frame();
        UiState ui;
        touch_ui::draw_panels(ui, cam.speed);

        // Camera rates from the touch panels.
        cam.apply_rates(ui.camera_translate_rate, ui.camera_orbit_rate, dt);

        // Object rates: integrate into our quat+vec3, then rebuild the matrix.
        if (ui.object_rotate_rate.y != 0.0f)
        {
            obj_rot = glm::angleAxis(ui.object_rotate_rate.y * dt, glm::vec3(0, 1, 0)) * obj_rot;
        }
        if (ui.object_rotate_rate.x != 0.0f)
        {
            obj_rot = glm::angleAxis(ui.object_rotate_rate.x * dt, glm::vec3(1, 0, 0)) * obj_rot;
        }
        obj_trans += ui.object_translate_rate * dt;

        // One-shot actions.
        if (ui.action_home)
        {
            cam.reset_home();
            obj_rot   = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            obj_trans = glm::vec3(0.0f);
        }
        if (ui.action_fit_to_view)
        {
            // Fit currently reuses the home pose, which is already framed to the
            // root bounding sphere. A future revision could recompute the eye
            // from the visible set's AABB.
            cam.reset_home();
        }
        if (ui.camera_speed_step > 0)
            cam.multiply_speed(1.5f);
        if (ui.camera_speed_step < 0)
            cam.multiply_speed(1.0f / 1.5f);

        glm::mat4 object_transform = build_object_transform(center);

        // ----- Render -----
        cache.pump_uploads();
        rend.begin_frame(win.width(), win.height());
        rend.draw_tileset(cam, tileset, fetch_scene, object_transform);
        rend.end_frame();
        touch_ui::end_frame();
        win.swap_buffers();

        ++report_frames;
        if (std::chrono::duration<float>(t_now - t_report).count() >= 2.0f)
        {
            auto s = rend.stats();
            std::fprintf(stderr,
                         "[stats] fps=%.1f frame=%.2fms draws=%u tris=%u visited=%u refined=%u "
                         "culled=%u missing=%u resident=%zu pending=%zu backend=%s\n",
                         report_frames / 2.0f, s.last_frame_ms, s.draws_last_frame,
                         s.triangles_last_frame, s.tiles_visited, s.tiles_refined, s.tiles_culled,
                         s.tiles_missing, cache.resident_count(), cache.pending_count(),
                         loader->backend_name());
            report_frames = 0;
            t_report      = t_now;
        }
    }

    touch_ui::shutdown();
    // TileCache destructor joins workers and destroys resident GltfScenes.
    rend.shutdown();
    return 0;
}
