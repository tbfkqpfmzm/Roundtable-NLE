/*
 * Position2D — joint 2D evaluator for Position keyframes (Premiere's
 * Motion Position, with spatial interpolation).
 *
 * When both kf.spatialInterp values along a segment are Linear (the default),
 * this falls back to per-axis evaluation — straight-line motion, identical to
 * historical behavior. When any side is Bezier / Auto Bezier / Continuous
 * Bezier, the position is sampled along a 2D cubic-Bezier path built from the
 * endpoint spatial handles, while temporal interpolation still drives the
 * parameter `t` along the curve (so ease/hold/etc. continue to control speed).
 *
 * Callers that previously did
 *   float x = clip->positionX().evaluate(t);
 *   float y = clip->positionY().evaluate(t);
 * should switch to
 *   auto [x, y] = evaluatePosition2D(clip->positionX(), clip->positionY(), t);
 * so the motion path is honored.
 */

#pragma once

#include <cstdint>
#include <utility>

#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"

namespace rt {

/// Returns true if the position keyframe pair at index `i` produces a
/// straight-line segment to the next keyframe (no spatial curve needed).
inline bool isStraightSpatialSegment(const Keyframe<float>& kx0,
                                     const Keyframe<float>& ky0,
                                     const Keyframe<float>& kx1,
                                     const Keyframe<float>& ky1) noexcept
{
    (void)kx1; (void)ky1;
    return kx0.spatialInterp == InterpMode::Linear
        && ky0.spatialInterp == InterpMode::Linear;
}

/// Joint 2D position evaluator. `trackX` and `trackY` must have identical
/// keyframe times for the spatial path to be well defined; when they don't,
/// the function falls back to per-axis evaluation (legacy behavior).
inline std::pair<float, float> evaluatePosition2D(
    const KeyframeTrack<float>& trackX,
    const KeyframeTrack<float>& trackY,
    int64_t time) noexcept
{
    // Fast path: any track without keyframes uses per-axis evaluation.
    const size_t nx = trackX.keyframeCount();
    const size_t ny = trackY.keyframeCount();
    if (nx == 0 || ny == 0 || nx != ny)
        return { trackX.evaluate(time), trackY.evaluate(time) };

    // Locate the active segment by time on the X track. Position keyframes
    // are always added X/Y in lockstep (see KeyframeTrack::writeValue + the
    // higher-level write paths), so X/Y indices align.
    if (time <= trackX.keyframe(0).time)
        return { trackX.keyframe(0).value, trackY.keyframe(0).value };
    if (time >= trackX.keyframe(nx - 1).time)
        return { trackX.keyframe(nx - 1).value, trackY.keyframe(nx - 1).value };

    size_t i1 = 0;
    for (; i1 < nx; ++i1)
        if (trackX.keyframe(i1).time > time) break;
    if (i1 == 0 || i1 >= nx)
        return { trackX.evaluate(time), trackY.evaluate(time) };
    const size_t i0 = i1 - 1;

    const auto& kx0 = trackX.keyframe(i0);
    const auto& kx1 = trackX.keyframe(i1);
    const auto& ky0 = trackY.keyframe(i0);
    const auto& ky1 = trackY.keyframe(i1);

    // X/Y must share keyframe times for a coherent path.
    if (kx0.time != ky0.time || kx1.time != ky1.time)
        return { trackX.evaluate(time), trackY.evaluate(time) };

    // Linear spatial → straight line; per-axis evaluation handles temporal ease.
    if (isStraightSpatialSegment(kx0, ky0, kx1, ky1))
        return { trackX.evaluate(time), trackY.evaluate(time) };

    // Temporal parameter t ∈ [0,1] along the segment, honoring temporal
    // interpolation (so ease/hold/auto modify SPEED along the path).
    const double segDt = static_cast<double>(kx1.time - kx0.time);
    if (segDt <= 0.0) return { kx0.value, ky0.value };
    const float linearT = static_cast<float>((time - kx0.time) / segDt);

    // Reuse the X track's evaluator to get temporal progress in [0,1].
    // Trick: re-map kx0/kx1 onto a temporary track of value=0..1 to get
    // progress. Simpler: derive progress directly from trackX.evaluate
    // relative to (kx0.value, kx1.value) — but a held/eased X axis might
    // diverge from Y. To keep speed-along-path consistent, compute progress
    // from the X track's temporal interpolation math directly:
    float progress = linearT;
    if (kx0.interp != InterpMode::Linear || kx1.interp != InterpMode::Linear) {
        const double v0 = kx0.value;
        const double v1 = kx1.value;
        const double dv = v1 - v0;
        if (dv != 0.0) {
            // X evaluates to v0 + progress*(v1-v0); invert.
            const double vx = trackX.evaluate(time);
            progress = static_cast<float>((vx - v0) / dv);
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
        } else {
            // X is constant — sample temporal progress via the Y axis.
            const double w0 = ky0.value;
            const double w1 = ky1.value;
            const double dw = w1 - w0;
            if (dw != 0.0) {
                const double vy = trackY.evaluate(time);
                progress = static_cast<float>((vy - w0) / dw);
                if (progress < 0.0f) progress = 0.0f;
                if (progress > 1.0f) progress = 1.0f;
            }
        }
    }

    // Build the 2D cubic-Bezier control polygon. spatialOut* is the offset
    // from kf0's position; spatialIn* is the offset from kf1's position.
    const float p0x = kx0.value;
    const float p0y = ky0.value;
    const float p3x = kx1.value;
    const float p3y = ky1.value;
    const float p1x = p0x + kx0.spatialOutX;
    const float p1y = p0y + ky0.spatialOutY;
    const float p2x = p3x + kx1.spatialInX;
    const float p2y = p3y + ky1.spatialInY;

    const float u  = 1.0f - progress;
    const float uu = u * u;
    const float tt = progress * progress;
    const float w0 = uu * u;
    const float w1 = 3.0f * uu * progress;
    const float w2 = 3.0f * u  * tt;
    const float w3 = tt * progress;

    const float x = w0 * p0x + w1 * p1x + w2 * p2x + w3 * p3x;
    const float y = w0 * p0y + w1 * p1y + w2 * p2y + w3 * p3y;
    return { x, y };
}

} // namespace rt
