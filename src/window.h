// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
// Window + OpenGL context wrapper (GLFW).
// Creates the highest context the build wants and steps down a version ladder
// to a portable floor (GL 3.3 core, or GL ES 3.0), so the viewer starts on
// modern GPUs and on software / virtualized stacks alike. Wayland is opt-in at
// build time (WAYLAND_ONLY); otherwise GLFW picks the platform.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace v3dt
{

struct WindowConfig
{
    int         width      = 1280;
    int         height     = 800;
    std::string title      = "3D Tiles Viewer";
    bool        fullscreen = false;
    bool        vsync      = true;
    bool        gles       = false;  // request a GL ES context instead of desktop core
    int         samples    = 4;      // MSAA samples; 0 disables (e.g. software raster)
};

struct InputState
{
    bool   keys[512]    = {};
    bool   mouse_left   = false;
    bool   mouse_right  = false;
    double mouse_x      = 0.0;
    double mouse_y      = 0.0;
    double mouse_dx     = 0.0;
    double mouse_dy     = 0.0;
    double scroll_dy    = 0.0;
    bool   just_resized = false;
};

class Window
{
   public:
    Window() = default;
    ~Window();
    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    bool init(const WindowConfig& cfg);

    // Returns false when the window should close.
    bool poll(InputState& out);
    void swap_buffers();

    int width() const
    {
        return width_;
    }
    int height() const
    {
        return height_;
    }
    GLFWwindow* glfw_handle() const
    {
        return handle_;
    }

   private:
    GLFWwindow* handle_ = nullptr;
    int         width_  = 0;
    int         height_ = 0;

    InputState input_{};

    // GLFW callbacks need static thunks; we register the Window as user pointer.
    static void on_resize(GLFWwindow* w, int width, int height);
    static void on_key(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void on_mouse_button(GLFWwindow* w, int button, int action, int mods);
    static void on_cursor(GLFWwindow* w, double x, double y);
    static void on_scroll(GLFWwindow* w, double xoff, double yoff);
};

}  // namespace v3dt
