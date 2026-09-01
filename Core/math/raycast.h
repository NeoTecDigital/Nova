#pragma once

#include "./quaternion_transform.h"
#include <limits>

namespace NovaMath {

struct Ray3D {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};

    constexpr Ray3D() = default;
    Ray3D(const glm::vec3& orig, const glm::vec3& dir)
        : origin(orig), direction(glm::normalize(dir)) {}

    glm::vec3 getPoint(float t) const {
        return origin + direction * t;
    }
};

struct RayHit {
    bool hit = false;
    float distance = std::numeric_limits<float>::infinity();
    glm::vec3 world_point{0.0f};
    glm::vec3 local_point{0.0f};
    glm::vec2 uv{0.0f}; // Normalized surface coordinate in [0, 1] x [0, 1] (0,0 is top-left)
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
};

/**
 * Unprojects screen pixel coordinates (0..screen_width, 0..screen_height)
 * into a 3D world space Ray using the inverse View-Projection matrix.
 */
inline Ray3D unprojectScreenRay(const glm::vec2& screen_pixel, const glm::vec2& screen_size, const glm::mat4& inv_view_proj) {
    // Convert screen coordinates to NDC [-1, 1]
    float ndc_x = (screen_pixel.x / screen_size.x) * 2.0f - 1.0f;
    float ndc_y = (screen_pixel.y / screen_size.y) * 2.0f - 1.0f;

    // Vulkan NDC near is z=0, far is z=1
    glm::vec4 near_clip(ndc_x, ndc_y, 0.0f, 1.0f);
    glm::vec4 far_clip(ndc_x, ndc_y, 1.0f, 1.0f);

    glm::vec4 near_world = inv_view_proj * near_clip;
    glm::vec4 far_world = inv_view_proj * far_clip;

    if (std::abs(near_world.w) > 1e-6f) near_world /= near_world.w;
    if (std::abs(far_world.w) > 1e-6f) far_world /= far_world.w;

    glm::vec3 origin = glm::vec3(near_world);
    glm::vec3 direction = glm::normalize(glm::vec3(far_world - near_world));

    return Ray3D(origin, direction);
}

/**
 * Intersects a 3D ray with an oriented rectangular surface plane in Quaternionic space.
 * Quad lies on the local XY plane centered at (0, 0) with dimensions (quad_size.x, quad_size.y).
 * Top-left UV is (0, 0) at (+y, -x) or (-x, +y), bottom-right is (1, 1).
 */
inline bool intersectOrientedQuad(const Ray3D& ray, const QuatTransform& transform, const glm::vec2& quad_size, RayHit& out_hit) {
    // Transform ray origin and direction into the local quaternionic space of the quad
    glm::vec3 local_orig = transform.inverseTransformPoint(ray.origin);
    glm::vec3 local_dir = glm::normalize(glm::conjugate(transform.orientation) * ray.direction);

    // Quad plane is Z = 0 in local space
    if (std::abs(local_dir.z) < 1e-6f) {
        return false; // Ray is parallel to quad plane
    }

    float t = -local_orig.z / local_dir.z;
    if (t < 0.0f) {
        return false; // Intersection is behind ray origin
    }

    glm::vec3 local_hit = local_orig + local_dir * t;

    float half_w = quad_size.x * 0.5f;
    float half_h = quad_size.y * 0.5f;

    // Check bounds on local XY plane
    if (local_hit.x < -half_w || local_hit.x > half_w ||
        local_hit.y < -half_h || local_hit.y > half_h) {
        return false; // Outside quad dimensions
    }

    out_hit.hit = true;
    out_hit.distance = t;
    out_hit.local_point = local_hit;
    out_hit.world_point = transform.transformPoint(local_hit);
    out_hit.normal = transform.orientation * glm::vec3(0.0f, 0.0f, (local_dir.z < 0.0f ? 1.0f : -1.0f));

    // Calculate UV coordinates with (0,0) at top-left and (1,1) at bottom-right (Wayland convention)
    float u = (local_hit.x + half_w) / quad_size.x;
    float v = (half_h - local_hit.y) / quad_size.y; // Invert Y so top is v=0, bottom is v=1

    out_hit.uv = glm::clamp(glm::vec2(u, v), glm::vec2(0.0f), glm::vec2(1.0f));
    return true;
}

} // namespace NovaMath
