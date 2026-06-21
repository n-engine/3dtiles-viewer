// The #version directive is injected at load time from the detected GL caps
// (see gpu_caps); this source stays version-agnostic across GL 3.3..4.6 / ES 3.

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

uniform mat4 u_mvp;
uniform mat4 u_model;
uniform mat3 u_normal_mat;

out vec3 v_world_pos;
out vec3 v_world_normal;
out vec2 v_uv;

void main() {
    vec4 wp = u_model * vec4(a_pos, 1.0);
    v_world_pos = wp.xyz;
    v_world_normal = normalize(u_normal_mat * a_normal);
    v_uv = a_uv;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
