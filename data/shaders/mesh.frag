// The #version directive is injected at load time from the detected GL caps
// (see gpu_caps); this source stays version-agnostic across GL 3.3..4.6 / ES 3.

in vec3 v_world_pos;
in vec3 v_world_normal;
in vec2 v_uv;

uniform vec4 u_base_color;
uniform sampler2D u_base_color_tex;
uniform bool u_has_tex;
uniform vec3 u_camera_pos;
uniform vec3 u_sun_dir;

out vec4 o_color;

// 3D Tiles baseColor textures are sRGB-encoded per glTF 2.0. We sample them as
// GL_RGBA8 (no hardware decode) and the default framebuffer is linear (no
// GL_FRAMEBUFFER_SRGB), so we do the full pipeline in-shader: linearize the
// sampled colour, light in linear space, encode back to sRGB at output.
vec3 srgb_to_linear(vec3 c) {
    bvec3 lo = lessThanEqual(c, vec3(0.04045));
    vec3 a = c / 12.92;
    vec3 b = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(b, a, vec3(lo));
}

vec3 linear_to_srgb(vec3 c) {
    bvec3 lo = lessThanEqual(c, vec3(0.0031308));
    vec3 a = 12.92 * c;
    vec3 b = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(b, a, vec3(lo));
}

void main() {
    vec3 n = normalize(v_world_normal);
    if (!gl_FrontFacing) n = -n;

    // baseColorFactor is linear per glTF 2.0 spec -- don't linearize it.
    // baseColorTexture stores sRGB-encoded data and is uploaded as GL_RGBA8
    // (no hardware decode), so linearize it here.
    vec3 base = u_base_color.rgb;
    if (u_has_tex) {
        vec4 t = texture(u_base_color_tex, v_uv);
        base *= srgb_to_linear(t.rgb);
    }

    vec3 L = normalize(u_sun_dir);
    vec3 V = normalize(u_camera_pos - v_world_pos);
    vec3 H = normalize(L + V);
    float ndl = max(dot(n, L), 0.0);
    float ndh = max(dot(n, H), 0.0);
    float spec = pow(ndh, 24.0) * 0.06;

    // Hemisphere ambient (linear). Slightly cooler sky, warmer ground -- values
    // here are linear-space, so they look numerically lower than the previous
    // sRGB-encoded constants but represent the same luminance.
    vec3 sky_col    = vec3(0.45, 0.55, 0.72);
    vec3 ground_col = vec3(0.28, 0.22, 0.16);
    float hemi_t = 0.5 + 0.5 * n.y;
    vec3 ambient = mix(ground_col, sky_col, hemi_t) * base * 0.65;

    vec3 sun_col = vec3(1.10, 1.04, 0.92);
    vec3 color   = ambient + base * ndl * 1.30 * sun_col + vec3(spec);

    // Mild saturation lift in linear (luma-pivot, factor 1.08).
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, 1.08);

    color = clamp(color, 0.0, 1.0);
    o_color = vec4(linear_to_srgb(color), 1.0);
}
