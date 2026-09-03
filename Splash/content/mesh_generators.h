// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Nova/pipeline/spatial_vertex.h"

namespace Splash {

/**
 * SpatialMeshGenerator - CPU geometry for the SDK's flat and curved surfaces.
 *
 * Every method is a pure function from parameters to Nova::MeshData: no device,
 * no allocator, no state. Handing the result to a GPU is Nova::SpatialMeshBuffer's
 * job and nothing here knows it happens.
 */
class SpatialMeshGenerator {
public:
    // Generate an oriented 3D rectangular plane centered at local origin (0, 0, 0)
    // with UVs (0,0) top-left to (1,1) bottom-right
    static Nova::MeshData createPlanarQuad(const glm::vec2& size,
                                           const glm::vec4& color = glm::vec4(1.0f),
                                           float border_thickness = 0.0f,
                                           float corner_radius = 0.0f,
                                           float render_mode = 0.0f);

    // Generate a curved cylindrical arc quad in 3D space.
    // A radius at or below MIN_ARC_RADIUS has no arc to sweep and degenerates
    // to a planar quad rather than dividing by zero.
    static Nova::MeshData createCurvedArc(const glm::vec2& size,
                                          float radius,
                                          uint32_t segments = 32,
                                          const glm::vec4& color = glm::vec4(1.0f),
                                          float border_thickness = 0.0f,
                                          float corner_radius = 0.0f,
                                          float render_mode = 0.0f);

    // Generate a 3D reticle (annular circle ring + crosshair bars)
    static Nova::MeshData createReticle(const glm::vec4& circle_color,
                                        const glm::vec4& crosshair_color,
                                        float radius = 0.06f,
                                        float ring_thickness = 0.006f,
                                        float crosshair_length = 0.08f,
                                        float crosshair_thickness = 0.005f);

    static constexpr float MIN_ARC_RADIUS = 1e-5f;
};

/**
 * PillMeshGenerator - Parametric 3D capsule geometry.
 *
 * The visual body of SpatialPill, one of the three OATS types this SDK owns, so
 * it belongs beside the other generators rather than inside the one app that
 * happens to be the first consumer of the type.
 */
class PillMeshGenerator {
public:
    static Nova::MeshData createPill(
        float radius = 0.08f,
        float cylinder_height = 0.25f,
        uint32_t radial_segments = 24,
        uint32_t cap_rings = 8,
        const glm::vec4& color = glm::vec4(0.2f, 0.5f, 0.85f, 0.95f),
        float render_mode = 0.0f
    );
};

} // namespace Splash
