// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "camera.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace v3dt
{

void Camera::set_perspective(float fov_deg, float aspect, float near_p, float far_p)
{
    fov_deg_ = fov_deg;
    aspect_  = aspect;
    near_p_  = near_p;
    far_p_   = far_p;
    proj_    = glm::perspective(glm::radians(fov_deg_), aspect_, near_p_, far_p_);
}

void Camera::on_resize(int width, int height)
{
    if (height <= 0)
        return;
    aspect_ = static_cast<float>(width) / static_cast<float>(height);
    proj_   = glm::perspective(glm::radians(fov_deg_), aspect_, near_p_, far_p_);
}

void Camera::look_at(const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up)
{
    pos_          = eye;
    world_up_     = glm::normalize(up);
    glm::vec3 dir = glm::normalize(target - eye);
    pitch_        = std::asin(glm::clamp(dir.y, -1.0f, 1.0f));
    yaw_          = std::atan2(dir.z, dir.x);
    recompute_forward();
}

void Camera::save_home()
{
    home_pos_   = pos_;
    home_yaw_   = yaw_;
    home_pitch_ = pitch_;
}

void Camera::reset_home()
{
    pos_   = home_pos_;
    yaw_   = home_yaw_;
    pitch_ = home_pitch_;
    recompute_forward();
}

glm::mat4 Camera::view() const
{
    return glm::lookAt(pos_, pos_ + forward_, world_up_);
}

void Camera::update(const InputState& in, float dt)
{
    // Mouse look: hold right button.
    if (in.mouse_right)
    {
        yaw_ += static_cast<float>(in.mouse_dx) * mouse_sensitivity;
        pitch_ -= static_cast<float>(in.mouse_dy) * mouse_sensitivity;
        const float lim = glm::radians(89.0f);
        pitch_          = std::clamp(pitch_, -lim, lim);
        recompute_forward();
    }

    glm::vec3 right = glm::normalize(glm::cross(forward_, world_up_));
    glm::vec3 up    = world_up_;
    float     v     = speed * (in.keys[GLFW_KEY_LEFT_SHIFT] ? speed_boost : 1.0f) * dt;

    if (in.keys[GLFW_KEY_W])
        pos_ += forward_ * v;
    if (in.keys[GLFW_KEY_S])
        pos_ -= forward_ * v;
    if (in.keys[GLFW_KEY_A])
        pos_ -= right * v;
    if (in.keys[GLFW_KEY_D])
        pos_ += right * v;
    // AZERTY-friendly: physical "A" (top-left) = GLFW_KEY_Q -> up
    //                   physical "E" (next to it)  = GLFW_KEY_E -> down
    if (in.keys[GLFW_KEY_Q] || in.keys[GLFW_KEY_SPACE])
        pos_ += up * v;
    if (in.keys[GLFW_KEY_E] || in.keys[GLFW_KEY_LEFT_CONTROL])
        pos_ -= up * v;

    // Scroll changes fly speed (factor 1.2 per notch).
    if (in.scroll_dy != 0.0)
    {
        speed *= std::pow(1.2f, static_cast<float>(in.scroll_dy));
        speed = std::clamp(speed, 0.05f, 5000.0f);
    }
}

void Camera::apply_rates(const glm::vec3& trans_rate, const glm::vec2& orbit_rate, float dt)
{
    if (glm::dot(trans_rate, trans_rate) > 0.0f)
    {
        glm::vec3 right = glm::normalize(glm::cross(forward_, world_up_));
        glm::vec3 step  = forward_ * trans_rate.x + right * trans_rate.y + world_up_ * trans_rate.z;
        pos_ += step * (speed * dt);
    }
    if (orbit_rate.x != 0.0f || orbit_rate.y != 0.0f)
    {
        yaw_ += orbit_rate.x * dt;
        pitch_ += orbit_rate.y * dt;
        const float lim = glm::radians(89.0f);
        pitch_          = std::clamp(pitch_, -lim, lim);
        recompute_forward();
    }
}

void Camera::multiply_speed(float factor)
{
    speed = std::clamp(speed * factor, 0.05f, 5000.0f);
}

void Camera::recompute_forward()
{
    forward_.x = std::cos(pitch_) * std::cos(yaw_);
    forward_.y = std::sin(pitch_);
    forward_.z = std::cos(pitch_) * std::sin(yaw_);
    forward_   = glm::normalize(forward_);
}

}  // namespace v3dt
