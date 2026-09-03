// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./mesh_generators.h"
#include <cmath>

namespace Splash {

Nova::MeshData SpatialMeshGenerator::createPlanarQuad(const glm::vec2& size,
                                                      const glm::vec4& color,
                                                      float border_thickness,
                                                      float corner_radius,
                                                      float render_mode) {
    Nova::MeshData mesh;
    float half_w = size.x * 0.5f;
    float half_h = size.y * 0.5f;

    glm::vec4 params(border_thickness, corner_radius, render_mode, 0.0f);
    glm::vec3 normal(0.0f, 0.0f, 1.0f);

    // Top-left, Top-right, Bottom-right, Bottom-left
    mesh.vertices = {
        { Nova::Math::Hyper4(-half_w,  half_h, 0.0f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(0.0f, 0.0f), params },
        { Nova::Math::Hyper4( half_w,  half_h, 0.0f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(1.0f, 0.0f), params },
        { Nova::Math::Hyper4( half_w, -half_h, 0.0f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(1.0f, 1.0f), params },
        { Nova::Math::Hyper4(-half_w, -half_h, 0.0f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(0.0f, 1.0f), params },
    };

    mesh.indices = {
        0, 1, 2,
        2, 3, 0
    };

    return mesh;
}

namespace {

// Two triangles per segment across a top/bottom vertex pair strip.
void appendQuadStripIndices(Nova::MeshData& mesh, uint32_t segments) {
    for (uint32_t i = 0; i < segments; ++i) {
        const uint32_t tl = i * 2;
        const uint32_t bl = tl + 1;
        const uint32_t tr = tl + 2;
        const uint32_t br = tl + 3;

        mesh.indices.push_back(tl);
        mesh.indices.push_back(tr);
        mesh.indices.push_back(br);

        mesh.indices.push_back(br);
        mesh.indices.push_back(bl);
        mesh.indices.push_back(tl);
    }
}

} // namespace

Nova::MeshData SpatialMeshGenerator::createCurvedArc(const glm::vec2& size,
                                                     float radius,
                                                     uint32_t segments,
                                                     const glm::vec4& color,
                                                     float border_thickness,
                                                     float corner_radius,
                                                     float render_mode) {
    // A zero (or sub-epsilon) radius sweeps no arc; size.x / radius would be
    // inf or NaN and poison every vertex. An infinite-radius arc IS a flat
    // quad, so degenerate to one rather than emitting garbage geometry.
    if (!(std::fabs(radius) > SpatialMeshGenerator::MIN_ARC_RADIUS)) {
        return createPlanarQuad(size, color, border_thickness, corner_radius, render_mode);
    }

    Nova::MeshData mesh;
    if (segments < 2) segments = 2;

    float half_h = size.y * 0.5f;
    float arc_angle = size.x / radius; // Angle spanned by width
    float start_angle = -arc_angle * 0.5f;
    float step_angle = arc_angle / static_cast<float>(segments);

    glm::vec4 params(border_thickness, corner_radius, render_mode, 0.0f);

    for (uint32_t i = 0; i <= segments; ++i) {
        float theta = start_angle + static_cast<float>(i) * step_angle;
        float u = static_cast<float>(i) / static_cast<float>(segments);

        float x = radius * std::sin(theta);
        float z = radius * (1.0f - std::cos(theta)); // Curve inwards/backwards
        glm::vec3 normal(-std::sin(theta), 0.0f, std::cos(theta));

        // Top vertex (v=0)
        mesh.vertices.push_back({ Nova::Math::Hyper4(x,  half_h, z, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(u, 0.0f), params });
        // Bottom vertex (v=1)
        mesh.vertices.push_back({ Nova::Math::Hyper4(x, -half_h, z, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(u, 1.0f), params });
    }

    appendQuadStripIndices(mesh, segments);
    return mesh;
}

Nova::MeshData SpatialMeshGenerator::createReticle(const glm::vec4& circle_color,
                                                   const glm::vec4& crosshair_color,
                                                   float radius,
                                                   float ring_thickness,
                                                   float crosshair_length,
                                                   float crosshair_thickness) {
    Nova::MeshData mesh;
    uint32_t segments = 36;
    float r_in = radius - ring_thickness * 0.5f;
    float r_out = radius + ring_thickness * 0.5f;
    glm::vec3 normal(0.0f, 0.0f, 1.0f);
    glm::vec4 circle_params(0.0f, 0.0f, 0.0f, 0.0f);

    // 1. Annular Circle Ring
    uint32_t ring_start_idx = static_cast<uint32_t>(mesh.vertices.size());
    for (uint32_t i = 0; i <= segments; ++i) {
        float theta = static_cast<float>(i) / static_cast<float>(segments) * glm::two_pi<float>();
        float cos_t = std::cos(theta);
        float sin_t = std::sin(theta);

        glm::vec3 pos_in(r_in * cos_t, r_in * sin_t, 0.0f);
        glm::vec3 pos_out(r_out * cos_t, r_out * sin_t, 0.0f);

        mesh.vertices.push_back({
            Nova::Math::Hyper4(pos_in.x, pos_in.y, pos_in.z, 1.0f),
            Nova::Math::Hyper4(circle_color.r, circle_color.g, circle_color.b, circle_color.a),
            normal,
            glm::vec2(0.0f),
            circle_params
        });

        mesh.vertices.push_back({
            Nova::Math::Hyper4(pos_out.x, pos_out.y, pos_out.z, 1.0f),
            Nova::Math::Hyper4(circle_color.r, circle_color.g, circle_color.b, circle_color.a),
            normal,
            glm::vec2(1.0f),
            circle_params
        });
    }

    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t in0 = ring_start_idx + i * 2;
        uint32_t out0 = in0 + 1;
        uint32_t in1 = in0 + 2;
        uint32_t out1 = in0 + 3;

        mesh.indices.push_back(in0);
        mesh.indices.push_back(out0);
        mesh.indices.push_back(out1);

        mesh.indices.push_back(out1);
        mesh.indices.push_back(in1);
        mesh.indices.push_back(in0);
    }

    // 2. Horizontal Crosshair Bar
    float hx = crosshair_length * 0.5f;
    float hy = crosshair_thickness * 0.5f;
    uint32_t h_start = static_cast<uint32_t>(mesh.vertices.size());
    glm::vec4 ch_params(0.0f, 0.0f, 0.0f, 0.0f);

    mesh.vertices.push_back({ Nova::Math::Hyper4(-hx,  hy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(0.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4( hx,  hy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(1.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4( hx, -hy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(1.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4(-hx, -hy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(0.0f), ch_params });

    mesh.indices.push_back(h_start + 0);
    mesh.indices.push_back(h_start + 1);
    mesh.indices.push_back(h_start + 2);
    mesh.indices.push_back(h_start + 2);
    mesh.indices.push_back(h_start + 3);
    mesh.indices.push_back(h_start + 0);

    // 3. Vertical Crosshair Bar
    float vx = crosshair_thickness * 0.5f;
    float vy = crosshair_length * 0.5f;
    uint32_t v_start = static_cast<uint32_t>(mesh.vertices.size());

    mesh.vertices.push_back({ Nova::Math::Hyper4(-vx,  vy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(0.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4( vx,  vy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(1.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4( vx, -vy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(1.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4(-vx, -vy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(0.0f), ch_params });

    mesh.indices.push_back(v_start + 0);
    mesh.indices.push_back(v_start + 1);
    mesh.indices.push_back(v_start + 2);
    mesh.indices.push_back(v_start + 2);
    mesh.indices.push_back(v_start + 3);
    mesh.indices.push_back(v_start + 0);

    return mesh;
}

// ---------------------------------------------------------------------------
// PillMeshGenerator
// ---------------------------------------------------------------------------
Nova::MeshData PillMeshGenerator::createPill(
    float radius,
    float cylinder_height,
    uint32_t radial_segments,
    uint32_t cap_rings,
    const glm::vec4& color,
    float render_mode
) {
    Nova::MeshData mesh;
    float half_h = cylinder_height * 0.5f;

    // 1. Top Hemisphere Vertices (Z from +half_h to +half_h + radius)
    for (uint32_t ring = 0; ring <= cap_rings; ++ring) {
        float phi = (float(ring) / float(cap_rings)) * (glm::pi<float>() * 0.5f); // 0 to pi/2
        float cos_phi = std::cos(phi);
        float sin_phi = std::sin(phi);

        for (uint32_t seg = 0; seg <= radial_segments; ++seg) {
            float theta = (float(seg) / float(radial_segments)) * glm::two_pi<float>();
            float cos_theta = std::cos(theta);
            float sin_theta = std::sin(theta);

            glm::vec3 normal(sin_phi * cos_theta, sin_phi * sin_theta, cos_phi);
            glm::vec3 pos = normal * radius + glm::vec3(0.0f, 0.0f, half_h);

            Nova::SpatialVertex v;
            v.state_primary = Nova::Math::Hyper4(pos.x, pos.y, pos.z, 1.0f);
            v.state_dual = Nova::Math::Hyper4(color.r, color.g, color.b, color.a);
            v.normal = normal;
            v.uv = glm::vec2(float(seg) / float(radial_segments), float(ring) / float(cap_rings * 2 + 1));
            v.params = glm::vec4(0.0f, 0.0f, render_mode, 1.0f);
            mesh.vertices.push_back(v);
        }
    }

    // 2. Bottom Hemisphere Vertices (Z from -half_h to -half_h - radius)
    for (uint32_t ring = 0; ring <= cap_rings; ++ring) {
        float phi = (glm::pi<float>() * 0.5f) + (float(ring) / float(cap_rings)) * (glm::pi<float>() * 0.5f); // pi/2 to pi
        float cos_phi = std::cos(phi);
        float sin_phi = std::sin(phi);

        for (uint32_t seg = 0; seg <= radial_segments; ++seg) {
            float theta = (float(seg) / float(radial_segments)) * glm::two_pi<float>();
            float cos_theta = std::cos(theta);
            float sin_theta = std::sin(theta);

            glm::vec3 normal(sin_phi * cos_theta, sin_phi * sin_theta, cos_phi);
            glm::vec3 pos = normal * radius - glm::vec3(0.0f, 0.0f, half_h);

            Nova::SpatialVertex v;
            v.state_primary = Nova::Math::Hyper4(pos.x, pos.y, pos.z, 1.0f);
            v.state_dual = Nova::Math::Hyper4(color.r, color.g, color.b, color.a);
            v.normal = normal;
            v.uv = glm::vec2(float(seg) / float(radial_segments), 0.5f + float(ring) / float(cap_rings * 2 + 1));
            v.params = glm::vec4(0.0f, 0.0f, render_mode, 1.0f);
            mesh.vertices.push_back(v);
        }
    }

    // Generate Triangles
    uint32_t ring_vertex_count = radial_segments + 1;
    uint32_t total_rings = (cap_rings + 1) * 2;

    for (uint32_t r = 0; r < total_rings - 1; ++r) {
        for (uint32_t s = 0; s < radial_segments; ++s) {
            uint32_t current = r * ring_vertex_count + s;
            uint32_t next = current + ring_vertex_count;

            mesh.indices.push_back(current);
            mesh.indices.push_back(next);
            mesh.indices.push_back(current + 1);

            mesh.indices.push_back(current + 1);
            mesh.indices.push_back(next);
            mesh.indices.push_back(next + 1);
        }
    }

    return mesh;
}

} // namespace Splash
