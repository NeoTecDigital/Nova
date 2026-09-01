#pragma once

#include "./raycast.h"
#include "./spatial_cluster.h"
#include "./engine_physics.h"
#include <array>
#include <cmath>

namespace NovaMath {

/**
 * InputRayFilter - Precision Filter & Acceleration Pipeline for 3D Input
 * 
 * Provides:
 * - Sub-pixel temporal/spatial dithering (for micro-accurate ray hits on thin or distant rotated geometry)
 * - Laser Focus acceleration (concentrates narrowphase ray checks around active focal entities)
 * - Multi-resolution index traversal
 */
class InputRayFilter {
public:
    InputRayFilter() = default;

    // Generate filtered 3D world ray from screen pixel with optional sub-pixel dithering
    Ray3D filterScreenRay(const glm::vec2& raw_pixel,
                          const glm::vec2& screen_size,
                          const glm::mat4& inv_view_proj,
                          const EnginePhysicsConfig& config,
                          uint32_t frame_index) {
        glm::vec2 filtered_px = raw_pixel;

        if (config.dither_enabled && config.dither_amplitude > 0.0f) {
            // 2D Halton sequence (bases 2 and 3) for sub-pixel jitter
            float jx = halton(frame_index % 16, 2) - 0.5f;
            float jy = halton(frame_index % 16, 3) - 0.5f;
            filtered_px += glm::vec2(jx, jy) * config.dither_amplitude;
        }

        return unprojectScreenRay(filtered_px, screen_size, inv_view_proj);
    }

    // Halton sequence generator for low-discrepancy sampling
    static float halton(uint32_t index, uint32_t base) {
        float f = 1.0f;
        float r = 0.0f;
        uint32_t i = index + 1;
        while (i > 0) {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(i % base);
            i /= base;
        }
        return r;
    }
};

} // namespace NovaMath
