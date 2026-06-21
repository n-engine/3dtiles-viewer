// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
// Free-fly camera with WASD + Q/E + mouse-look while right-button held.

#pragma once

#include <glm/glm.hpp>
#include "window.h"

namespace v3dt
{

class Camera
{
   public:
    void set_perspective(float fov_deg, float aspect, float near_p, float far_p);
    void look_at(const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up);
    void on_resize(int width, int height);

    // Snapshot the current pose as the "home" pose. Pressing Home resets to it.
    void save_home();
    void reset_home();

    // Update from input and elapsed seconds.
    void update(const InputState& in, float dt);

    // Apply continuous rates coming from the touch UI (gamepad-equivalent).
    // `trans_rate` is in local camera frame: x=forward, y=right, z=up. Scaled
    // by `speed` then by dt. `orbit_rate` is yaw/pitch in rad/s, scaled by dt.
    void apply_rates(const glm::vec3& trans_rate, const glm::vec2& orbit_rate, float dt);

    void multiply_speed(float factor);

    glm::mat4 view() const;
    glm::mat4 proj() const
    {
        return proj_;
    }
    glm::vec3 position() const
    {
        return pos_;
    }
    glm::vec3 forward() const
    {
        return forward_;
    }

    float speed             = 10.0f;  // m/s
    float speed_boost       = 8.0f;   // shift multiplier
    float mouse_sensitivity = 0.0025f;

   private:
    glm::vec3 pos_{0.0f, 0.0f, 0.0f};
    glm::vec3 forward_{0.0f, 0.0f, -1.0f};
    glm::vec3 world_up_{0.0f, 1.0f, 0.0f};

    // Yaw/pitch in radians; rebuilt from look_at() on init.
    float yaw_   = 0.0f;
    float pitch_ = 0.0f;

    float fov_deg_ = 60.0f;
    float aspect_  = 1.0f;
    float near_p_  = 0.1f;
    float far_p_   = 5000.0f;

    glm::mat4 proj_ = glm::mat4(1.0f);

    // Home pose snapshot for the Home-key reset.
    glm::vec3 home_pos_{0.0f};
    float     home_yaw_   = 0.0f;
    float     home_pitch_ = 0.0f;

    void recompute_forward();
};

}  // namespace v3dt
