#version 450

layout(location = 0) in vec4 in_state_primary; // [a, b, c, w]_0: Spatial (x, y, z, w) / (re0, im0)
layout(location = 1) in vec4 in_state_dual;    // [a, b, c, w]_1: Chromatic (r, g, b, a) / Complex Phase
layout(location = 2) in vec3 in_normal;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_params;        // [border_thickness, corner_radius, render_mode, phase_factor]

layout(push_constant) uniform PushConstants {
    mat4 view_proj;
    mat4 model;
    vec4 camera_pos;
    vec4 surface_dim; // [width, height, aspect, time]
} push;

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_normal;
layout(location = 2) out vec2 frag_uv;
layout(location = 3) out vec4 frag_state_primary;
layout(location = 4) out vec4 frag_state_dual;
layout(location = 5) out vec4 frag_params;
layout(location = 6) out vec4 frag_surface_dim;

void main() {
    // Spatial coordinate extracted from [a, b, c, w]_0
    vec4 pos = vec4(in_state_primary.xyz, in_state_primary.w);
    vec4 world_pos = push.model * pos;
    gl_Position = push.view_proj * world_pos;

    frag_world_pos = world_pos.xyz;
    frag_normal = mat3(push.model) * in_normal;
    frag_uv = in_uv;
    frag_state_primary = in_state_primary;
    frag_state_dual = in_state_dual;
    frag_params = in_params;
    frag_surface_dim = push.surface_dim;
}
