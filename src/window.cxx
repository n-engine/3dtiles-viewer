// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "window.h"

#include "gpu_caps.h"

#include <epoxy/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>

namespace v3dt
{

Window::~Window()
{
    if (handle_)
    {
        glfwDestroyWindow(handle_);
        handle_ = nullptr;
    }
    glfwTerminate();
}

bool Window::init(const WindowConfig& cfg)
{
#if defined(VIEWER_WAYLAND_ONLY)
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
#endif
    if (!glfwInit())
    {
        std::fprintf(stderr, "[window] glfwInit failed\n");
        return false;
    }
#if defined(VIEWER_WAYLAND_ONLY)
    const int platform = glfwGetPlatform();
    if (platform != GLFW_PLATFORM_WAYLAND)
    {
        std::fprintf(stderr, "[window] requested Wayland but got platform=%d\n", platform);
        return false;
    }
#endif

    // Version ladder: try the highest context down to a portable floor that
    // still has everything we use (VAOs, GLSL in/out, uint32 indices) -- GL 3.3
    // core, or GL ES 3.0. A software / virtualized stack (llvmpipe caps at 4.5,
    // virgl at 3.3) lands on the first rung it supports instead of getting a
    // NULL window from an over-specified 4.6 request.
    struct GlVersion
    {
        int major;
        int minor;
    };
    static const GlVersion kDesktop[] = {{4, 6}, {4, 5}, {4, 3}, {4, 1}, {3, 3}};
    static const GlVersion kEs[]      = {{3, 2}, {3, 1}, {3, 0}};
    const GlVersion*       ladder     = cfg.gles ? kEs : kDesktop;
    const int              rungs      = cfg.gles ? 3 : 5;
    GLFWmonitor*           monitor    = cfg.fullscreen ? glfwGetPrimaryMonitor() : nullptr;

    auto create = [&](int samples) -> GLFWwindow*
    {
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
        glfwWindowHint(GLFW_SAMPLES, samples > 0 ? samples : 0);
        GLFWwindow* w = nullptr;
        for (int i = 0; i < rungs && !w; ++i)
        {
            glfwWindowHint(GLFW_CLIENT_API, cfg.gles ? GLFW_OPENGL_ES_API : GLFW_OPENGL_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, ladder[i].major);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, ladder[i].minor);
            glfwWindowHint(GLFW_OPENGL_PROFILE,
                           cfg.gles ? GLFW_OPENGL_ANY_PROFILE : GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, cfg.gles ? GLFW_FALSE : GLFW_TRUE);
            w = glfwCreateWindow(cfg.width, cfg.height, cfg.title.c_str(), monitor, nullptr);
        }
        return w;
    };

    handle_ = create(cfg.samples);
    if (!handle_)
    {
        std::fprintf(stderr, "[window] no %s context available (need >= %d.%d)\n",
                     cfg.gles ? "GL ES" : "GL core", ladder[rungs - 1].major,
                     ladder[rungs - 1].minor);
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(handle_);
    detect_gpu_caps();

    // On a software rasterizer the multisample framebuffer is pure cost. If MSAA
    // was requested, recreate the window once without it -- so the speedup is
    // real, not just a disabled GL_MULTISAMPLE flag over an allocated MSAA buffer
    // (we cannot know the renderer string until a context exists).
    if (gpu_caps().is_software && cfg.samples > 0)
    {
        std::fprintf(stderr, "[gpu] software rasterizer: recreating without MSAA\n");
        glfwDestroyWindow(handle_);
        handle_ = create(0);
        if (!handle_)
        {
            std::fprintf(stderr, "[window] recreate without MSAA failed\n");
            glfwTerminate();
            return false;
        }
        glfwMakeContextCurrent(handle_);
        detect_gpu_caps();
    }

    glfwSwapInterval(cfg.vsync ? 1 : 0);

    glfwSetWindowUserPointer(handle_, this);
    glfwSetFramebufferSizeCallback(handle_, &Window::on_resize);
    glfwSetKeyCallback(handle_, &Window::on_key);
    glfwSetMouseButtonCallback(handle_, &Window::on_mouse_button);
    glfwSetCursorPosCallback(handle_, &Window::on_cursor);
    glfwSetScrollCallback(handle_, &Window::on_scroll);

    // libepoxy auto-loads GL function pointers on first call; no init needed.

    glfwGetFramebufferSize(handle_, &width_, &height_);

    const GpuCaps& caps = gpu_caps();
    std::fprintf(stderr, "[gpu] %s | %s\n", caps.vendor.c_str(), caps.renderer.c_str());
    std::fprintf(stderr, "[gpu] %dx%d | GL %s | GLSL %s%s%s\n", width_, height_,
                 caps.version.c_str(), caps.glsl.c_str(),
                 caps.is_software ? " | SOFTWARE-RASTER" : "",
                 caps.has_anisotropy ? "" : " | no-aniso");
    return true;
}

bool Window::poll(InputState& out)
{
    input_.mouse_dx     = 0.0;
    input_.mouse_dy     = 0.0;
    input_.scroll_dy    = 0.0;
    input_.just_resized = false;
    double prev_x = input_.mouse_x, prev_y = input_.mouse_y;

    glfwPollEvents();

    input_.mouse_dx = input_.mouse_x - prev_x;
    input_.mouse_dy = input_.mouse_y - prev_y;

    out = input_;
    return !glfwWindowShouldClose(handle_);
}

void Window::swap_buffers()
{
    glfwSwapBuffers(handle_);
}

void Window::on_resize(GLFWwindow* w, int width, int height)
{
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (!self)
        return;
    self->width_              = width;
    self->height_             = height;
    self->input_.just_resized = true;
    glViewport(0, 0, width, height);
}

void Window::on_key(GLFWwindow* w, int key, int /*scancode*/, int action, int /*mods*/)
{
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (!self || key < 0 || key >= 512)
        return;
    if (action == GLFW_PRESS)
        self->input_.keys[key] = true;
    else if (action == GLFW_RELEASE)
        self->input_.keys[key] = false;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
    {
        glfwSetWindowShouldClose(w, GLFW_TRUE);
    }
}

void Window::on_mouse_button(GLFWwindow* w, int button, int action, int /*mods*/)
{
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (!self)
        return;
    bool down = (action == GLFW_PRESS);
    if (button == GLFW_MOUSE_BUTTON_LEFT)
        self->input_.mouse_left = down;
    else if (button == GLFW_MOUSE_BUTTON_RIGHT)
        self->input_.mouse_right = down;
}

void Window::on_cursor(GLFWwindow* w, double x, double y)
{
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (!self)
        return;
    self->input_.mouse_x = x;
    self->input_.mouse_y = y;
}

void Window::on_scroll(GLFWwindow* w, double /*xoff*/, double yoff)
{
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (!self)
        return;
    self->input_.scroll_dy += yoff;
}

}  // namespace v3dt
