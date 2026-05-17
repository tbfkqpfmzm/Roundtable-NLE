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

/// Interpolation mode of a keyframe, matching Premiere Pro's Temporal
/// Interpolation menu. The mode lives on the keyframe itself; the shape of a
/// segment kf[i] → kf[i+1] is determined by combining kf[i]'s outgoing side
/// with kf[i+1]'s incoming side (see KeyframeTrack::evaluate). Numeric values
/// are stable for binary serialization (0/1/2 match the original enum).
enum class InterpMode : uint8_t
{
    Linear           = 0,  // Straight line between values
    Bezier           = 1,  // Manual cubic-bezier handles (in/out independent)
    Hold             = 2,  // Step — hold this keyframe's value until next
    AutoBezier       = 3,  // Smooth, auto-tangent from neighbors; flattens at extrema
    ContinuousBezier = 4,  // Manual handles but locked collinear (smooth velocity)
    EaseIn           = 5,  // Bezier ease on the incoming side (decelerate into kf)
    EaseOut          = 6   // Bezier ease on the outgoing side (accelerate out of kf)
};

/// A single keyframe: time + value + temporal interpolation + spatial-path
/// interpolation (used only on 2D position keyframes — see evaluatePosition2D).
template <typename T>
struct Keyframe
{
    int64_t    time{0};            // TimeTick position
    T          value{};            // Value at this keyframe
    InterpMode interp{InterpMode::Linear};

    // Temporal bezier handles (normalized 0–1 within the temporal segment).
    float bezierOutX{0.33f};       // out-tangent X (this keyframe → next)
    float bezierOutY{0.0f};        // out-tangent Y
    float bezierInX{0.67f};        // in-tangent X (prev keyframe → this)
    float bezierInY{1.0f};         // in-tangent Y

    // ── Spatial (motion path) — meaningful for Position keyframes ───────
    // spatialInterp drives the SHAPE of the 2D path between Position
    // keyframes (Premiere's "Spatial Interpolation" menu). The handles are
    // offsets from the keyframe's (x,y) position in the same units as the
    // position value (REF-1920 px). Default = Linear (straight-line motion),
    // matching Premiere's out-of-the-box behavior.
    InterpMode spatialInterp{InterpMode::Linear};
    float      spatialOutX{0.0f};
    float      spatialOutY{0.0f};
    float      spatialInX{0.0f};
    float      spatialInY{0.0f};

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
