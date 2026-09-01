#version 450

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in vec4 frag_state_primary; // [a, b, c, w]_0: Real / Spatial
layout(location = 4) in vec4 frag_state_dual;    // [a, b, c, w]_1: Complex Phase / Color
layout(location = 5) in vec4 frag_params;        // [border_thickness, corner_radius, render_mode, phase_factor]
layout(location = 6) in vec4 frag_surface_dim;   // [width, height, aspect, time]

layout(set = 0, binding = 0) uniform sampler2D surface_sampler;

layout(location = 0) out vec4 out_color;

#define PI 3.14159265359

// Signed distance to a rounded rectangle in UV space
float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Convert complex number z = (re, im) to phase-colored RGBA
vec4 complexToColor(vec2 z, float alpha) {
    float magnitude = length(z);
    float phase = atan(z.y, z.x); // [-PI, PI]
    float norm_phase = (phase + PI) / (2.0 * PI); // [0, 1]

    // Non-linear spectral rainbow from complex phase
    vec3 col = 0.5 + 0.5 * cos(2.0 * PI * (norm_phase + vec3(0.0, 0.33, 0.67)));
    float intensity = tanh(magnitude);
    return vec4(col * intensity, alpha);
}

void main() {
    float border_thickness = frag_params.x;
    float corner_radius = frag_params.y;
    float mode = frag_params.z;

    vec2 size = frag_surface_dim.xy;
    if (size.x <= 0.0) size.x = 1.0;
    if (size.y <= 0.0) size.y = 1.0;

    vec2 p = (frag_uv - 0.5) * size;
    vec2 half_size = size * 0.5;

    // Mode 0: Procedural SDF Panel (Glass / Acrylic with dual state chromatic tint)
    if (mode == 0.0) {
        float d = sdRoundedBox(p, half_size, corner_radius);
        float aa = max(fwidth(d), 0.001);
        float fill_alpha = 1.0 - smoothstep(-aa * 0.5, aa * 0.5, d);

        if (fill_alpha <= 0.001) {
            discard;
        }

        vec4 col = frag_state_dual;

        if (border_thickness > 0.0) {
            float border_d = d + border_thickness;
            float border_mask = smoothstep(-aa * 0.5, aa * 0.5, border_d);
            col = mix(col, vec4(col.rgb * 1.6 + vec3(0.12), col.a), border_mask);
        }

        out_color = vec4(col.rgb, col.a * fill_alpha);
        return;
    }

    // Mode 1: Textured Quad (Wayland Client Frame / External Vulkan Texture)
    if (mode == 1.0) {
        vec4 tex_color = texture(surface_sampler, frag_uv) * frag_state_dual;

        if (corner_radius > 0.0) {
            float d = sdRoundedBox(p, half_size, corner_radius);
            float aa = max(fwidth(d), 0.001);
            float corner_alpha = 1.0 - smoothstep(-aa * 0.5, aa * 0.5, d);
            tex_color.a *= corner_alpha;
        }

        if (tex_color.a <= 0.001) {
            discard;
        }

        out_color = tex_color;
        return;
    }

    // Mode 2: Font Glyph Alpha Mask
    if (mode == 2.0) {
        float alpha = texture(surface_sampler, frag_uv).r;
        float final_alpha = frag_state_dual.a * alpha;
        if (final_alpha <= 0.001) {
            discard;
        }
        out_color = vec4(frag_state_dual.rgb, final_alpha);
        return;
    }

    // Mode 3: Non-linear Complex Phase Field Distribution
    if (mode == 3.0) {
        // Evaluate complex (re, im) across the node surface
        vec2 z = frag_state_primary.xy + vec2(p.x, p.y) * 4.0;
        // Non-linear complex mapping: f(z) = sinh(z) or z^2 + c
        vec2 z_sq = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + frag_state_dual.xy;
        out_color = complexToColor(z_sq, frag_state_dual.a);
        return;
    }

    // Fallback: Dual state RGBA
    out_color = frag_state_dual;
}
