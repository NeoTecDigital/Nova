#version 450

layout(location = 0) in vec2 frag_pos;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in vec4 frag_color;
layout(location = 3) in vec4 frag_rect_bounds; // x, y, width, height in physical pixels
layout(location = 4) in vec4 frag_radii;       // top-left, top-right, bottom-right, bottom-left
layout(location = 5) in vec4 frag_params;      // border_thickness, softness, render_mode, extra

layout(set = 0, binding = 0) uniform sampler2D ui_texture;

layout(location = 0) out vec4 out_color;

// Signed distance to a rounded rectangle with variable corner radii
float sdRoundedBox(vec2 p, vec2 b, vec4 r) {
    // Select corner radius based on quadrant:
    // r.x = top-left, r.y = top-right, r.z = bottom-right, r.w = bottom-left
    float radius = (p.x > 0.0) ? 
        ((p.y > 0.0) ? r.z : r.y) : 
        ((p.y > 0.0) ? r.w : r.x);
    
    vec2 q = abs(p) - b + vec2(radius);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

void main() {
    float mode = frag_params.z;
    float border_thickness = frag_params.x;
    float softness = max(frag_params.y, 1.0); // Minimum 1 pixel AA softness

    // Mode 0: Procedural Rounded Box (fill + optional border)
    if (mode == 0.0) {
        vec2 rect_center = frag_rect_bounds.xy + frag_rect_bounds.zw * 0.5;
        vec2 half_size = frag_rect_bounds.zw * 0.5;
        vec2 p = frag_pos - rect_center;

        float d = sdRoundedBox(p, half_size, frag_radii);

        // Compute edge antialiased alpha
        float fill_alpha = 1.0 - smoothstep(-softness * 0.5, softness * 0.5, d);

        if (fill_alpha <= 0.0) {
            discard;
        }

        if (border_thickness > 0.0) {
            // Draw inner border
            float border_d = d + border_thickness;
            float border_alpha = smoothstep(-softness * 0.5, softness * 0.5, border_d);
            // Blend fill with border or render border
            out_color = vec4(frag_color.rgb, frag_color.a * fill_alpha * border_alpha);
        } else {
            out_color = vec4(frag_color.rgb, frag_color.a * fill_alpha);
        }
        return;
    }

    // Mode 1: Textured Quad (Wayland client surface / image) with optional corner rounding
    if (mode == 1.0) {
        vec4 tex_color = texture(ui_texture, frag_uv) * frag_color;

        // Apply corner rounding if specified
        if (frag_radii.x > 0.0 || frag_radii.y > 0.0 || frag_radii.z > 0.0 || frag_radii.w > 0.0) {
            vec2 rect_center = frag_rect_bounds.xy + frag_rect_bounds.zw * 0.5;
            vec2 half_size = frag_rect_bounds.zw * 0.5;
            vec2 p = frag_pos - rect_center;
            float d = sdRoundedBox(p, half_size, frag_radii);
            float corner_alpha = 1.0 - smoothstep(-softness * 0.5, softness * 0.5, d);
            tex_color.a *= corner_alpha;
        }

        if (tex_color.a <= 0.0) {
            discard;
        }
        out_color = tex_color;
        return;
    }

    // Mode 2: Font Glyph (Alpha mask sampled from font atlas)
    if (mode == 2.0) {
        float alpha = texture(ui_texture, frag_uv).r;
        float final_alpha = frag_color.a * alpha;
        if (final_alpha <= 0.001) {
            discard;
        }
        out_color = vec4(frag_color.rgb, final_alpha);
        return;
    }

    // Mode 3: Simple Color / Primitive
    out_color = frag_color;
}
