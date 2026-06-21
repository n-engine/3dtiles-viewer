// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "touch_ui.h"

#include "gpu_caps.h"

#include <epoxy/gl.h>
// epoxy must come first. GLFW_INCLUDE_NONE prevents GLFW pulling its own GL
// header, and the macro line doubles as a clang-format barrier so SortIncludes
// does not rearrange the order across this point.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace v3dt::touch_ui
{

namespace
{

// Colour-coded buttons mirroring the design mockup. Each ID corresponds to a
// button number in the design (1..8); 9/10 are the bottom-left utility tiles.
struct Btn
{
    const char* label;
    ImU32       col;
    ImU32       col_hover;
    ImU32       col_active;
};

ImU32 darken(ImU32 c, float f)
{
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.x *= f;
    v.y *= f;
    v.z *= f;
    return ImGui::ColorConvertFloat4ToU32(v);
}

constexpr ImU32 BTN1 = IM_COL32(255, 140, 60, 255);   // orange
constexpr ImU32 BTN2 = IM_COL32(245, 226, 80, 255);   // yellow
constexpr ImU32 BTN3 = IM_COL32(70, 200, 80, 255);    // green
constexpr ImU32 BTN4 = IM_COL32(115, 220, 240, 255);  // cyan
constexpr ImU32 BTN5 = IM_COL32(45, 50, 180, 255);    // blue
constexpr ImU32 BTN6 = IM_COL32(220, 50, 200, 255);   // magenta
constexpr ImU32 BTN7 = IM_COL32(255, 130, 130, 255);  // pink
constexpr ImU32 BTN8 = IM_COL32(160, 240, 130, 255);  // light green

// Returns true while the button is held (continuous). Single click = true for
// one frame. We use IsItemActive() to detect hold for the continuous rates.
bool color_button(const char* id, ImU32 col, ImVec2 size, bool* held = nullptr)
{
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, darken(col, 1.15f));
    // Flash to near-white on press -- capacitive screens cause uncertainty
    // about whether a tap actually registered. Bright active state plus the
    // outline ring below makes the feedback unmistakable.
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, darken(col, 1.8f));
    bool clicked = ImGui::Button(id, size);
    bool is_held = ImGui::IsItemActive();
    if (is_held)
    {
        ImVec2 lo = ImGui::GetItemRectMin();
        ImVec2 hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(lo, hi, IM_COL32(255, 255, 255, 230),
                                            ImGui::GetStyle().FrameRounding, 0, 3.0f);
    }
    ImGui::PopStyleColor(3);
    if (held)
        *held = is_held;
    return clicked;
}

// 4-way D-pad: U / D / L / R. Each direction returns a held flag in `out`.
// The cross is laid out in a 3x3 grid with corners empty.
struct DPadHeld
{
    bool up, down, left, right;
};
DPadHeld dpad(const char* id_base, float cell)
{
    DPadHeld h{};
    ImVec2   origin = ImGui::GetCursorScreenPos();
    ImVec2   sz(cell, cell);
    auto     pos = [&](int row, int col)
    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + col * (cell + 4), origin.y + row * (cell + 4)));
    };
    char  buf[32];
    ImU32 col_pad = IM_COL32(70, 80, 110, 255);

    pos(0, 1);
    std::snprintf(buf, sizeof(buf), "%sU", id_base);
    color_button(buf, col_pad, sz, &h.up);
    pos(1, 0);
    std::snprintf(buf, sizeof(buf), "%sL", id_base);
    color_button(buf, col_pad, sz, &h.left);
    pos(1, 2);
    std::snprintf(buf, sizeof(buf), "%sR", id_base);
    color_button(buf, col_pad, sz, &h.right);
    pos(2, 1);
    std::snprintf(buf, sizeof(buf), "%sD", id_base);
    color_button(buf, col_pad, sz, &h.down);

    // Declare the dpad's full extent to ImGui via a Dummy() at origin -- using
    // SetCursorScreenPos alone trips the SetCursorPos-extends-bounds assertion
    // in recent ImGui (10198).
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(3 * cell + 8, 3 * cell + 8));
    return h;
}

}  // namespace

static GLFWwindow* g_window = nullptr;

bool init(GLFWwindow* window)
{
    g_window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGuiStyle& s               = ImGui::GetStyle();
    s.WindowRounding            = 8.0f;
    s.FrameRounding             = 6.0f;
    s.WindowPadding             = ImVec2(14, 14);
    s.ItemSpacing               = ImVec2(8, 8);
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.78f);
    s.Colors[ImGuiCol_Text]     = ImVec4(0.95f, 0.95f, 0.97f, 1.0f);
    // On small capacitive panels the buttons can fall below the 9-11 mm
    // finger comfort range and clicks miss at the bezel. Extend each
    // widget's reactive bounding box without changing visuals. Cap at
    // ItemSpacing/2 so adjacent rows/cols never overlap their hit zones --
    // overlap silently routes the earlier button's hit and the later one
    // stops firing.
    s.TouchExtraPadding = ImVec2(4, 4);

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true))
        return false;
    if (!ImGui_ImplOpenGL3_Init(gpu_caps().imgui_glsl_version.c_str()))
        return false;
    return true;
}

void shutdown()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void begin_frame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void draw_panels(UiState& out, float cam_speed)
{
    out = UiState{};

    // Auto-hide: after HIDE_AFTER s with no touch/key, fade out over 0.5 s
    // and skip all panel Begin()/End() so the input region is released -- a
    // tap then goes straight to the scene controller and wakes the UI back
    // up. Override with env var VIEWER_UI_HIDE_AFTER=<seconds>; set to 0 to
    // disable auto-hide entirely; negative falls back to default.
    static double    last_input_t = 0.0;
    static double    hide_after   = -1.0;
    constexpr double FADE_DUR     = 0.5;
    if (hide_after < 0.0)
    {
        hide_after = 30.0;
        if (const char* env = std::getenv("VIEWER_UI_HIDE_AFTER"); env && *env)
        {
            char*  end = nullptr;
            double v   = std::strtod(env, &end);
            if (end != env && v >= 0.0)
                hide_after = v;
        }
    }
    const bool auto_hide_disabled = (hide_after == 0.0);

    const double now = ImGui::GetTime();
    if (last_input_t == 0.0)
        last_input_t = now;

    {
        const ImGuiIO& io      = ImGui::GetIO();
        bool           any_key = false;
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        {
            if (ImGui::IsKeyDown(static_cast<ImGuiKey>(k)))
            {
                any_key = true;
                break;
            }
        }
        const bool active =
            ImGui::IsAnyMouseDown() || io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f || any_key;
        if (active)
            last_input_t = now;
    }

    const double idle  = now - last_input_t;
    float        alpha = 1.0f;
    if (!auto_hide_disabled)
    {
        if (idle >= hide_after + FADE_DUR)
            return;
        if (idle >= hide_after)
        {
            alpha = 1.0f - static_cast<float>((idle - hide_after) / FADE_DUR);
        }
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float          W  = vp->WorkSize.x;
    const float          H  = vp->WorkSize.y;

    const float margin = 16.0f;
    // Title + separator dropped -- panel content is now:
    //   2*WindowPadding.y (28) + 2 button rows (2*btn) + ItemSpacing.y (8)
    //   + Spacing() (8) + 3 D-pad rows (3*cell) + 2 dpad inter-gaps (8) + 8
    // = 2*btn + 3*cell + 60
    // Two stacked panels + 3 margins (top/middle/bottom):
    //   2*(2*btn + 3*cell + 60) + 3*margin <= H
    // With cell = btn (D-pad cells match colored button height -- touch-
    // critical) => 10*btn + 168 <= H => btn <= (H - 168) / 10
    const float btn  = std::min(64.0f, std::max(40.0f, (H - 168.0f - 3.0f * margin) / 10.0f));
    const float cell = btn;
    // Panel width sized from the D-pad's 3 columns + window padding (~14 each
    // side from style.WindowPadding) + a small slack column.
    const float panel_w = 3 * cell + 2 * 4 + 28 + 8;
    // Colored top buttons span the full panel inner width (2 per row).
    // ItemSpacing.x = 8 between the two buttons.
    const float top_btn_w   = (panel_w - 28.0f - 8.0f) / 2.0f;
    const float panel_h_obj = 2 * btn + 3 * cell + 60;
    const float panel_h_cam = panel_h_obj;

    // ---- OBJECT panel (top-right) ----
    ImGui::SetNextWindowPos(ImVec2(W - panel_w - margin + vp->WorkPos.x, margin + vp->WorkPos.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h_obj), ImGuiCond_Always);
    if (ImGui::Begin("##obj_panel", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar))
    {
        bool h1 = false, h2 = false, h3 = false, h4 = false;
        color_button("1", BTN1, ImVec2(top_btn_w, btn), &h1);
        ImGui::SameLine();
        color_button("2", BTN2, ImVec2(top_btn_w, btn), &h2);
        color_button("3", BTN3, ImVec2(top_btn_w, btn), &h3);
        ImGui::SameLine();
        color_button("4", BTN4, ImVec2(top_btn_w, btn), &h4);

        ImGui::Spacing();
        DPadHeld d = dpad("##obj_dp_", cell);

        // Apply (rad/s, m/s).
        const float yaw_rate   = 1.2f;  // ~70 deg/s
        const float pitch_rate = 1.2f;
        const float trans_rate = 1.0f;  // 1 m/s

        if (h1)
            out.object_rotate_rate.y -= yaw_rate;  // yaw left
        if (h2)
            out.object_rotate_rate.y += yaw_rate;  // yaw right
        if (h3)
            out.object_rotate_rate.x += pitch_rate;  // pitch down
        if (h4)
            out.object_rotate_rate.x -= pitch_rate;  // pitch up

        if (d.up)
            out.object_translate_rate.y += trans_rate;
        if (d.down)
            out.object_translate_rate.y -= trans_rate;
        if (d.left)
            out.object_translate_rate.x -= trans_rate;
        if (d.right)
            out.object_translate_rate.x += trans_rate;
    }
    ImGui::End();

    // ---- CAMERA panel (bottom-right) ----
    ImGui::SetNextWindowPos(
        ImVec2(W - panel_w - margin + vp->WorkPos.x, H - panel_h_cam - margin + vp->WorkPos.y),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h_cam), ImGuiCond_Always);
    if (ImGui::Begin("##cam_panel", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar))
    {
        bool h5 = false, h6 = false, h7 = false, h8 = false;
        color_button("5", BTN5, ImVec2(top_btn_w, btn), &h5);
        ImGui::SameLine();
        color_button("6", BTN6, ImVec2(top_btn_w, btn), &h6);
        color_button("7", BTN7, ImVec2(top_btn_w, btn), &h7);
        ImGui::SameLine();
        color_button("8", BTN8, ImVec2(top_btn_w, btn), &h8);

        ImGui::Spacing();
        DPadHeld d = dpad("##cam_dp_", cell);

        if (h5)
            out.camera_translate_rate.x += 1.0f;  // forward
        if (h6)
            out.camera_translate_rate.x -= 1.0f;  // backward
        if (h7)
            out.camera_translate_rate.z -= 1.0f;  // descend (down)
        if (h8)
            out.camera_translate_rate.z += 1.0f;  // ascend (up)

        const float orbit_rate = 1.2f;
        if (d.left)
            out.camera_orbit_rate.x -= orbit_rate;
        if (d.right)
            out.camera_orbit_rate.x += orbit_rate;
        if (d.up)
            out.camera_orbit_rate.y += orbit_rate;
        if (d.down)
            out.camera_orbit_rate.y -= orbit_rate;
    }
    ImGui::End();

    // ---- Bottom-left utility strip ----
    const float util_w = 4 * btn + 3 * 8 + 28;
    const float util_h = btn + 30 + 28;
    ImGui::SetNextWindowPos(ImVec2(margin + vp->WorkPos.x, H - util_h - margin + vp->WorkPos.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(util_w, util_h), ImGuiCond_Always);
    if (ImGui::Begin("##util_strip", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar))
    {
        ImGui::Text("Speed %.1f m/s", static_cast<double>(cam_speed));
        if (color_button("Home", IM_COL32(80, 90, 110, 255), ImVec2(btn, btn)))
        {
            out.action_home = true;
        }
        ImGui::SameLine();
        if (color_button("Fit", IM_COL32(80, 90, 110, 255), ImVec2(btn, btn)))
        {
            out.action_fit_to_view = true;
        }
        ImGui::SameLine();
        if (color_button("-", IM_COL32(120, 90, 80, 255), ImVec2(btn, btn)))
        {
            out.camera_speed_step = -1;
        }
        ImGui::SameLine();
        if (color_button("+", IM_COL32(80, 110, 90, 255), ImVec2(btn, btn)))
        {
            out.camera_speed_step = 1;
        }
    }
    ImGui::End();

    ImGui::PopStyleVar();
}

void end_frame()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace v3dt::touch_ui
