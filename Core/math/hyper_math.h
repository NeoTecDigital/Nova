#pragma once

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cmath>
#include <algorithm>
#include <complex>
#include <array>
#include <string>

namespace Nova::Math {

/**
 * Hyper4 - Unified 4D Primordial [a, b, c, w] Structure
 * 
 * Unifies:
 * - Spatial/Projective: (x, y, z, w) = (a, b, c, w)
 * - Chromatic/Alpha:    (r, g, b, a) = (a, b, c, w)
 * - Complex Bicyclic:   (re0, im0, re1, im1) where z0 = a + i*b, z1 = c + i*w
 * - Quaternionic Rotor: (qx, qy, qz, qw) = (a, b, c, w)
 */
struct Hyper4 {
    float a = 0.0f; // x / r / re0 / qx
    float b = 0.0f; // y / g / im0 / qy
    float c = 0.0f; // z / b_col / re1 / qz
    float w = 0.0f; // w / alpha / im1 / qw

    constexpr Hyper4() = default;
    constexpr Hyper4(float _a, float _b, float _c, float _w = 0.0f) : a(_a), b(_b), c(_c), w(_w) {}

    // GLM Interoperability Constructors
    Hyper4(const glm::vec4& v) : a(v.x), b(v.y), c(v.z), w(v.w) {}
    Hyper4(const glm::vec3& v, float _w = 1.0f) : a(v.x), b(v.y), c(v.z), w(_w) {}
    Hyper4(const glm::vec2& v, float _c = 0.0f, float _w = 0.0f) : a(v.x), b(v.y), c(_c), w(_w) {}
    Hyper4(const glm::quat& q) : a(q.x), b(q.y), c(q.z), w(q.w) {}

    // Type conversion operators
    glm::vec4 toVec4() const { return glm::vec4(a, b, c, w); }
    glm::vec3 toVec3() const { return glm::vec3(a, b, c); }
    glm::vec2 toVec2() const { return glm::vec2(a, b); }
    glm::quat toQuat() const { return glm::quat(w, a, b, c); } // GLM quat constructor: (w, x, y, z)

    operator glm::vec4() const { return toVec4(); }
    operator glm::vec3() const { return toVec3(); }

    // Coordinate Accessors
    float& x() { return a; }
    float x() const { return a; }
    float& y() { return b; }
    float y() const { return b; }
    float& z() { return c; }
    float z() const { return c; }

    // Chromatic Accessors
    float& r() { return a; }
    float r() const { return a; }
    float& g() { return b; }
    float g() const { return b; }
    float& b_col() { return c; }
    float b_col() const { return c; }
    float& alpha() { return w; }
    float alpha() const { return w; }

    // Complex / Phase Accessors
    float& re0() { return a; }
    float re0() const { return a; }
    float& im0() { return b; }
    float im0() const { return b; }
    float& re1() { return c; }
    float re1() const { return c; }
    float& im1() { return w; }
    float im1() const { return w; }

    // Factories
    static constexpr Hyper4 fromComplex(float re, float im, float extra_re = 0.0f, float extra_im = 0.0f) {
        return Hyper4(re, im, extra_re, extra_im);
    }

    static Hyper4 fromPhase(float radius, float theta) {
        return Hyper4(radius * std::cos(theta), radius * std::sin(theta), 0.0f, 1.0f);
    }

    static constexpr Hyper4 fromSpatial(float x, float y, float z, float w = 1.0f) {
        return Hyper4(x, y, z, w);
    }

    static constexpr Hyper4 fromColor(float r, float g, float b, float alpha = 1.0f) {
        return Hyper4(r, g, b, alpha);
    }

    // -------------------------------------------------------------
    // Complex & Phase Arithmetic (Non-linear & Spectral)
    // -------------------------------------------------------------
    
    float getPhase0() const {
        return std::atan2(b, a);
    }

    float getMagnitude0() const {
        return std::sqrt(a * a + b * b);
    }

    float getPhase1() const {
        return std::atan2(w, c);
    }

    float getMagnitude1() const {
        return std::sqrt(c * c + w * w);
    }

    Hyper4 rotatePhase0(float phi) const {
        float cos_p = std::cos(phi);
        float sin_p = std::sin(phi);
        return Hyper4(
            a * cos_p - b * sin_p,
            a * sin_p + b * cos_p,
            c, w
        );
    }

    Hyper4 complexMul0(const Hyper4& rhs) const {
        return Hyper4(
            a * rhs.a - b * rhs.b,
            a * rhs.b + b * rhs.a,
            c * rhs.c - w * rhs.w,
            c * rhs.w + w * rhs.c
        );
    }

    Hyper4 nonLinearWarp(float lambda = 1.0f) const {
        float r0 = getMagnitude0();
        float p0 = getPhase0();
        float warped_r0 = std::tanh(r0 * lambda);

        float r1 = getMagnitude1();
        float p1 = getPhase1();
        float warped_r1 = std::tanh(r1 * lambda);

        return Hyper4(
            warped_r0 * std::cos(p0),
            warped_r0 * std::sin(p0),
            warped_r1 * std::cos(p1),
            warped_r1 * std::sin(p1)
        );
    }

    // -------------------------------------------------------------
    // Quaternionic & Matrix Transformations
    // -------------------------------------------------------------
    
    // Hamilton Product: q1 * q2
    Hyper4 quatMul(const Hyper4& q2) const {
        return Hyper4(
            w * q2.a + a * q2.w + b * q2.c - c * q2.b,
            w * q2.b - a * q2.c + b * q2.w + c * q2.a,
            w * q2.c + a * q2.b - b * q2.a + c * q2.w,
            w * q2.w - a * q2.a - b * q2.b - c * q2.c
        );
    }

    Hyper4 quatConjugate() const {
        return Hyper4(-a, -b, -c, w);
    }

    Hyper4 transformMatrix(const glm::mat4& m) const {
        glm::vec4 res = m * toVec4();
        return Hyper4(res);
    }

    float norm() const {
        return std::sqrt(a * a + b * b + c * c + w * w);
    }

    Hyper4 normalized() const {
        float n = norm();
        if (n < 1e-7f) return Hyper4(0.0f, 0.0f, 0.0f, 1.0f);
        return Hyper4(a / n, b / n, c / n, w / n);
    }

    // Vector operations
    Hyper4 operator+(const Hyper4& o) const { return {a + o.a, b + o.b, c + o.c, w + o.w}; }
    Hyper4 operator-(const Hyper4& o) const { return {a - o.a, b - o.b, c - o.c, w - o.w}; }
    Hyper4 operator*(float s) const { return {a * s, b * s, c * s, w * s}; }
    Hyper4 operator/(float s) const { return {a / s, b / s, c / s, w / s}; }

    Hyper4& operator+=(const Hyper4& o) { a += o.a; b += o.b; c += o.c; w += o.w; return *this; }
    Hyper4& operator-=(const Hyper4& o) { a -= o.a; b -= o.b; c -= o.c; w -= o.w; return *this; }
    Hyper4& operator*=(float s) { a *= s; b *= s; c *= s; w *= s; return *this; }

    float dot(const Hyper4& o) const { return a * o.a + b * o.b + c * o.c + w * o.w; }

    Hyper4 cross(const Hyper4& o) const {
        return Hyper4(
            b * o.c - c * o.b,
            c * o.a - a * o.c,
            a * o.b - b * o.a,
            0.0f
        );
    }
};

/**
 * PhaseState8 - Coupled Dual [a,b,c,w] Primordial Node State
 */
struct PhaseState8 {
    Hyper4 primary; // [a, b, c, w]_0: Spatial pose / Real manifold component
    Hyper4 dual;    // [a, b, c, w]_1: Chromatic / Phase / Imaginary manifold component

    constexpr PhaseState8() = default;
    constexpr PhaseState8(const Hyper4& p, const Hyper4& d) : primary(p), dual(d) {}

    static PhaseState8 fromSpatialColor(float x, float y, float z, float r, float g, float b, float a = 1.0f) {
        return PhaseState8(
            Hyper4::fromSpatial(x, y, z, 1.0f),
            Hyper4::fromColor(r, g, b, a)
        );
    }

    static PhaseState8 fromComplexPhase(float re, float im, float phase_freq, float decay = 1.0f) {
        return PhaseState8(
            Hyper4::fromComplex(re, im),
            Hyper4::fromComplex(std::cos(phase_freq), std::sin(phase_freq), decay, 1.0f)
        );
    }

    PhaseState8 coupleNonLinear(float dt, float coupling_strength = 1.0f) const {
        float phase = dual.getPhase0();
        Hyper4 modulated_primary = primary.rotatePhase0(phase * coupling_strength * dt);
        modulated_primary = modulated_primary.nonLinearWarp(1.0f);

        Hyper4 evolved_dual = dual.rotatePhase0(dt * 2.0f);
        return PhaseState8(modulated_primary, evolved_dual);
    }

    PhaseState8 slerp(const PhaseState8& target, float t) const {
        float dot_p = std::clamp(primary.dot(target.primary), -1.0f, 1.0f);
        float theta_p = std::acos(dot_p) * t;
        Hyper4 rel_p = (target.primary - primary * dot_p).normalized();
        Hyper4 interp_p = (primary * std::cos(theta_p) + rel_p * std::sin(theta_p)).normalized();

        float dot_d = std::clamp(dual.dot(target.dual), -1.0f, 1.0f);
        float theta_d = std::acos(dot_d) * t;
        Hyper4 rel_d = (target.dual - dual * dot_d).normalized();
        Hyper4 interp_d = (dual * std::cos(theta_d) + rel_d * std::sin(theta_d)).normalized();

        return PhaseState8(interp_p, interp_d);
    }
};

} // namespace Nova::Math
