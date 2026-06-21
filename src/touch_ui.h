// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
// Touchscreen-friendly control surface for the viewer.
//
// Two panels (top-right OBJECT, bottom-right CAMERA) each carrying four
// coloured action buttons + a 4-way D-pad. A bottom-left strip holds the
// non-modal utility controls (home, fit-to-view, camera speed +/-).
//
// Each frame the UI populates a UiState the application then applies to the
// scene object transform and the camera.

#pragma once

#include <glm/glm.hpp>

struct GLFWwindow;

namespace v3dt
{

struct UiState
{
    // Per-frame rates (set when buttons are held). Units: rad/s and m/s.
    glm::vec3 object_rotate_rate{0.0f};     // x=pitch, y=yaw, z=roll
    glm::vec3 object_translate_rate{0.0f};  // local screen-space x/y/z

    glm::vec3 camera_translate_rate{0.0f};  // forward(x) / strafe(y) / vertical(z)
    glm::vec2 camera_orbit_rate{0.0f};      // yaw, pitch (D-pad)

    // One-shot actions fired by clicking the bottom-left bar (true for one frame).
    bool action_home        = false;
    bool action_fit_to_view = false;

    // Camera speed step: +1 / -1 / 0 (one-shot per click).
    int camera_speed_step = 0;

    // Style.
    float ui_scale = 1.0f;
};

namespace touch_ui
{

bool init(GLFWwindow* window);
void shutdown();

// Call once per frame, before drawing the 3D scene.
void begin_frame();

// Render the touch panels and pull the resulting actions/rates into `out`.
// `cam_speed` is shown in the bottom-left strip; pass the current camera
// fly speed so the operator has feedback.
void draw_panels(UiState& out, float cam_speed);

// Call after the 3D scene is drawn; flushes the UI draw lists to GL.
void end_frame();

}  // namespace touch_ui
}  // namespace v3dt
