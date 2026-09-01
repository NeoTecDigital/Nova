#version 450

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec4 in_rect_bounds; // x, y, width, height
layout(location = 4) in vec4 in_radii;       // top-left, top-right, bottom-right, bottom-left
layout(location = 5) in vec4 in_params;      // border_thickness, softness, render_mode, shadow_spread

layout(push_constant) uniform PushConstants {
    vec2 screen_size;
    vec2 scale;
} push;

layout(location = 0) out vec2 frag_pos;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out vec4 frag_color;
layout(location = 3) out vec4 frag_rect_bounds;
layout(location = 4) out vec4 frag_radii;
layout(location = 5) out vec4 frag_params;

void main() {
    // Transform screen pixel coords (0..width, 0..height) to Vulkan NDC (-1..1, -1..1)
    vec2 pos = in_pos * push.scale;
    vec2 ndc = (pos / push.screen_size) * 2.0 - 1.0;
    
    gl_Position = vec4(ndc, 0.0, 1.0);
    
    frag_pos = pos;
    frag_uv = in_uv;
    frag_color = in_color;
    frag_rect_bounds = vec4(in_rect_bounds.xy * push.scale, in_rect_bounds.zw * push.scale);
    frag_radii = in_radii * push.scale.x;
    frag_params = in_params;
}
