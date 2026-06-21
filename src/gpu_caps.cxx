// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "gpu_caps.h"

#include <epoxy/gl.h>

#include <algorithm>
#include <cctype>

namespace v3dt
{

namespace
{

GpuCaps g_caps;

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string gl_str(GLenum name)
{
    const GLubyte* s = glGetString(name);
    return s ? reinterpret_cast<const char*>(s) : "";
}

}  // namespace

const GpuCaps& detect_gpu_caps()
{
    GpuCaps c;
    c.vendor   = gl_str(GL_VENDOR);
    c.renderer = gl_str(GL_RENDERER);
    c.version  = gl_str(GL_VERSION);
    c.glsl     = gl_str(GL_SHADING_LANGUAGE_VERSION);

    // GL_MAJOR/MINOR_VERSION exist on every context we accept (>= GL 3.0 / ES 3.0).
    glGetIntegerv(GL_MAJOR_VERSION, &c.gl_major);
    glGetIntegerv(GL_MINOR_VERSION, &c.gl_minor);

    // ES contexts report "OpenGL ES 3.x ..." in GL_VERSION.
    c.es = to_lower(c.version).find("opengl es") != std::string::npos;

    // Software rasterizers, detected by renderer string (capability axis, not
    // vendor): Mesa llvmpipe/softpipe/swrast, generic "software".
    const std::string r = to_lower(c.renderer);
    for (const char* sw : {"llvmpipe", "softpipe", "swrast", "software"})
        if (r.find(sw) != std::string::npos)
            c.is_software = true;

    c.has_anisotropy = epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic") ||
                       epoxy_has_gl_extension("GL_ARB_texture_filter_anisotropic");
    if (c.has_anisotropy)
    {
        GLfloat m = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &m);
        c.max_anisotropy = m;
    }

    // One shader source tree; the loader prepends the right directive. GLSL ES
    // 3.00 needs a default float precision; desktop core 330 does not.
    if (c.es)
    {
        c.glsl_directive     = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
        c.imgui_glsl_version = "#version 300 es";
    }
    else
    {
        c.glsl_directive     = "#version 330 core\n";
        c.imgui_glsl_version = "#version 330 core";
    }

    g_caps = c;
    return g_caps;
}

const GpuCaps& gpu_caps()
{
    return g_caps;
}

}  // namespace v3dt
