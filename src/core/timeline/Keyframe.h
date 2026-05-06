/*
 * Keyframe — animation value at a point in time with interpolation.
 *
 * Supports linear, bezier (cubic), and step/hold interpolation.
 * Used for all animated properties: position, scale, rotation, opacity,
 * volume, pan, effect parameters, etc.
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

namespace rt {

/// Interpolation mode between keyframes
enum class InterpMode : uint8_t
{
    Linear,   // Straight line between values
    Bezier,   // Cubic bezier curve (smooth ease in/out)
    Hold      // Step function — hold previous value until next keyframe
};

/// A single keyframe: time + value + interpolation parameters
template <typename T>
struct Keyframe
{
    int64_t    time{0};            // TimeTick position
    T          value{};            // Value at this keyframe
    InterpMode interp{InterpMode::Linear};

    // Bezier tangent handles (normalized 0–1 within segment)
    float bezierOutX{0.33f};       // Right handle X (this keyframe's out tangent)
    float bezierOutY{0.0f};        // Right handle Y
    float bezierInX{0.67f};        // Left handle X (next keyframe's in tangent)
    float bezierInY{1.0f};         // Left handle Y

    bool operator<(const Keyframe& other) const noexcept
    {
        return time < other.time;
    }
};

/// Evaluate cubic bezier curve at parameter t (0–1)
inline float evalCubicBezier(float p0, float p1, float p2, float p3, float t) noexcept
{
    float u = 1.0f - t;
    return u * u * u * p0 + 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t * p3;
}

/// Solve bezier parameter t for a given X value (Newton's method)
inline float solveBezierT(float x0, float x1, float x2, float x3, float targetX) noexcept
{
    float t = targetX; // Initial guess (linear approximation)
    for (int i = 0; i < 8; ++i)
    {
        float x = evalCubicBezier(x0, x1, x2, x3, t) - targetX;
        float dx = 3.0f * (1 - t) * (1 - t) * (x1 - x0) +
                   6.0f * (1 - t) * t * (x2 - x1) +
                   3.0f * t * t * (x3 - x2);
        if (std::abs(dx) < 1e-7f) break;
        t -= x / dx;
        t = std::clamp(t, 0.0f, 1.0f);
    }
    return t;
}

} // namespace rt
