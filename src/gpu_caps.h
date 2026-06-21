// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
// GPU / GL capability probe.
//
// One place that asks the driver what it actually is, instead of the renderer
// assuming a fixed GL version. Populated once, right after the context is
// current, then read-only for the rest of the process. Everything that needs
// a version-dependent decision (GLSL #version to emit, whether anisotropic
// filtering exists, whether we are on a software rasterizer) consults this
// rather than hardcoding 4.6.
//
// Portability axis is GL version + extension presence, never vendor name:
// branching on "is this NVIDIA / AMD" is a fragile anti-pattern, the driver
// already abstracts that. We branch on what the driver reports it can do.

#pragma once

#include <string>

namespace v3dt
{

struct GpuCaps
{
    int  gl_major = 0;
    int  gl_minor = 0;
    bool es       = false;  // OpenGL ES context (reserved; desktop core for now)

    std::string vendor;    // GL_VENDOR
    std::string renderer;  // GL_RENDERER
    std::string version;   // GL_VERSION
    std::string glsl;      // GL_SHADING_LANGUAGE_VERSION

    // True for CPU rasterizers (llvmpipe / softpipe / swrast / "software").
    // Callers use it to trim cost: no MSAA, conservative defaults.
    bool is_software = false;

    bool  has_anisotropy = false;
    float max_anisotropy = 1.0f;

    // The directive the shader loader prepends to GLSL sources, so one source
    // tree compiles across every context we accept (e.g. "#version 330 core\n").
    std::string glsl_directive;
    // The glsl_version string Dear ImGui's GL3 backend wants (no trailing nl).
    std::string imgui_glsl_version;
};

// Probe the current GL context. Must be called with a context made current and
// the GL loader (libepoxy) ready. Fills and installs the process-wide caps.
const GpuCaps& detect_gpu_caps();

// Read the installed caps. Valid only after detect_gpu_caps() has run; before
// that it returns a zero-initialized struct (gl_major == 0).
const GpuCaps& gpu_caps();

}  // namespace v3dt
