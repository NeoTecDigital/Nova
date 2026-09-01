#pragma once

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <cmath>

namespace NovaMath {

/**
 * Dual Quaternion representing rigid 3D spatial transformation (SE(3))
 * Dual number: q_hat = real + epsilon * dual (epsilon^2 = 0)
 */
struct DualQuat {
    glm::quat real{1.0f, 0.0f, 0.0f, 0.0f}; // Unit quaternion rotation
    glm::quat dual{0.0f, 0.0f, 0.0f, 0.0f}; // Translation displacement (0.5 * pos_quat * real)

    DualQuat() = default;
    DualQuat(const glm::quat& r, const glm::quat& d) : real(r), dual(d) {}

    static DualQuat fromRotationTranslation(const glm::quat& q, const glm::vec3& t) {
        glm::quat norm_q = glm::normalize(q);
        glm::quat t_quat(0.0f, t.x, t.y, t.z);
        glm::quat d = (t_quat * norm_q) * 0.5f;
        return DualQuat(norm_q, d);
    }

    glm::quat getRotation() const {
        return real;
    }

    glm::vec3 getTranslation() const {
        glm::quat t_quat = (dual * 2.0f) * glm::conjugate(real);
        return glm::vec3(t_quat.x, t_quat.y, t_quat.z);
    }
};

/**
 * QuatTransform - Primary Quaternionic Spatial Coordinate Transform
 * Handles full 3D/4D spatial pose: orientation (q in H), position (t in R^3), scale (s in R^3).
 */
struct QuatTransform {
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f, 1.0f, 1.0f};

    constexpr QuatTransform() = default;

    QuatTransform(const glm::vec3& pos)
        : orientation(1.0f, 0.0f, 0.0f, 0.0f), position(pos), scale(1.0f, 1.0f, 1.0f) {}

    QuatTransform(const glm::vec3& pos, const glm::quat& rot, const glm::vec3& scl = glm::vec3(1.0f))
        : orientation(rot), position(pos), scale(scl) {}

    static QuatTransform fromEuler(const glm::vec3& pos, const glm::vec3& euler_radians, const glm::vec3& scl = glm::vec3(1.0f)) {
        return QuatTransform(pos, glm::quat(euler_radians), scl);
    }

    static QuatTransform fromAxisAngle(const glm::vec3& pos, const glm::vec3& axis, float angle_radians) {
        return QuatTransform(pos, glm::angleAxis(angle_radians, glm::normalize(axis)), glm::vec3(1.0f));
    }

    QuatTransform combine(const QuatTransform& child) const {
        return QuatTransform(
            transformPoint(child.position),
            orientation * child.orientation,
            scale * child.scale
        );
    }

    // Generate 4x4 Model Matrix: M = T * R * S
    glm::mat4 toMatrix() const {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), position);
        glm::mat4 r = glm::mat4_cast(orientation);
        glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
        return t * r * s;
    }

    // Generate Inverse 4x4 Model Matrix: M^-1 = S^-1 * R^-1 * T^-1
    glm::mat4 toInverseMatrix() const {
        glm::mat4 inv_s = glm::scale(glm::mat4(1.0f), 1.0f / scale);
        glm::mat4 inv_r = glm::mat4_cast(glm::conjugate(orientation));
        glm::mat4 inv_t = glm::translate(glm::mat4(1.0f), -position);
        return inv_s * inv_r * inv_t;
    }

    // Transform point from local space to world space
    glm::vec3 transformPoint(const glm::vec3& p) const {
        return position + (orientation * (p * scale));
    }

    // Transform point from world space to local space
    glm::vec3 inverseTransformPoint(const glm::vec3& p) const {
        glm::vec3 unscaled = glm::conjugate(orientation) * (p - position);
        return unscaled / scale;
    }

    // Transform direction vector from local to world
    glm::vec3 transformDirection(const glm::vec3& dir) const {
        return glm::normalize(orientation * dir);
    }

    // Transform direction vector from world to local
    glm::vec3 inverseTransformDirection(const glm::vec3& dir) const {
        return glm::normalize(glm::conjugate(orientation) * dir);
    }

    // Directional vectors
    glm::vec3 forward() const { return orientation * glm::vec3(0.0f, 0.0f, -1.0f); }
    glm::vec3 back()    const { return orientation * glm::vec3(0.0f, 0.0f,  1.0f); }
    glm::vec3 up()      const { return orientation * glm::vec3(0.0f, 1.0f,  0.0f); }
    glm::vec3 down()    const { return orientation * glm::vec3(0.0f, -1.0f, 0.0f); }
    glm::vec3 right()   const { return orientation * glm::vec3(1.0f, 0.0f,  0.0f); }
    glm::vec3 left()    const { return orientation * glm::vec3(-1.0f, 0.0f, 0.0f); }

    // Hierarchical composition (parent * child)
    QuatTransform operator*(const QuatTransform& child) const {
        QuatTransform res;
        res.position = transformPoint(child.position);
        res.orientation = glm::normalize(orientation * child.orientation);
        res.scale = scale * child.scale;
        return res;
    }

    // Spherical Linear Interpolation between transforms
    QuatTransform slerp(const QuatTransform& target, float t) const {
        QuatTransform res;
        res.position = glm::mix(position, target.position, t);
        res.orientation = glm::slerp(orientation, target.orientation, t);
        res.scale = glm::mix(scale, target.scale, t);
        return res;
    }

    // Convert to Dual Quaternion
    DualQuat toDualQuat() const {
        return DualQuat::fromRotationTranslation(orientation, position);
    }
};

} // namespace NovaMath
