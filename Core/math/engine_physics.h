#pragma once

#include "./hyper_math.h"
#include <string>
#include <algorithm>

namespace Nova::Math {

enum class AccelerationMode {
    LaserFocus = 0,    // Pinpoint sub-pixel raycast laser on active focal node
    ClusteredDither = 1, // Broadphase cluster hierarchy with temporal dithering
    LazyUniform = 2     // Lazy evaluation with uniform spatial grid
};

/**
 * EnginePhysicsConfig - Runtime Physics & Mathematical Substrate Parameters
 *
 * Interactive control granularity, recovered from the deleted EngineHUD (see
 * commit 70673fb). These are the increments an editor should step these fields
 * by; they are not defaults and nothing here reads them. Kept at the fields
 * they edit because that is where the next settings surface will look:
 *
 *   phase_coupling_strength  step 0.2   clamp to [0.0, 5.0]
 *   phase_velocity           step 0.5   clamp to [-10.0, 10.0]
 *   accel_mode               cycle forward through the 3 AccelerationMode values
 *   dither_enabled           toggle
 *   (all)                    resetDefaults()
 *
 * The phase_velocity clamp is the documented range below, NOT what EngineHUD
 * did: it stepped omega by 0.5 with no bound at all, so its own control could
 * drive the field outside the range this struct declares. The range is right
 * and the control was wrong.
 */
struct EnginePhysicsConfig {
    // Non-linear complex phase dynamics
    float phase_coupling_strength = 1.0f;  // lambda in [0.0, 5.0]
    float phase_velocity = 2.0f;           // omega in [-10.0, 10.0] rad/s
    float phase_decay = 0.98f;             // Harmonic dissipation rate

    // Spatial Index & Raycast Acceleration
    AccelerationMode accel_mode = AccelerationMode::LaserFocus;
    float laser_precision = 1.0f;          // Resolution multiplier for raycast precision
    int cluster_depth = 4;                 // Depth subdivisions in hierarchical spatial index (precision * depth)
    bool dither_enabled = true;            // Sub-pixel temporal/spatial jitter
    float dither_amplitude = 0.5f;         // Dither offset magnitude in pixels

    // Viewport & Rigid Dynamics
    float camera_damping = 12.0f;          // Inertia damping for camera motion
    float spring_stiffness = 40.0f;        // 3D window snap / attractor stiffness
    float drag_coefficient = 0.85f;        // Aerodynamic damping on spatial nodes

    // Telemetry stats
    float current_fps = 60.0f;
    uint32_t active_nodes = 0;
    float last_ray_depth = 0.0f;
    uint32_t cluster_tests_per_frame = 0;

    void resetDefaults() {
        phase_coupling_strength = 1.0f;
        phase_velocity = 2.0f;
        phase_decay = 0.98f;
        accel_mode = AccelerationMode::LaserFocus;
        laser_precision = 1.0f;
        cluster_depth = 4;
        dither_enabled = true;
        dither_amplitude = 0.5f;
        camera_damping = 12.0f;
        spring_stiffness = 40.0f;
        drag_coefficient = 0.85f;
    }

    const char* getModeName() const {
        switch (accel_mode) {
            case AccelerationMode::LaserFocus: return "LaserFocus";
            case AccelerationMode::ClusteredDither: return "ClusteredDither";
            case AccelerationMode::LazyUniform: return "LazyUniform";
            default: return "Unknown";
        }
    }
};

} // namespace Nova::Math
