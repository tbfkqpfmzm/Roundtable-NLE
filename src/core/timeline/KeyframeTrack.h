/*
 * KeyframeTrack — sorted container of keyframes with evaluation.
 *
 * Provides O(log n) lookup and interpolation between keyframes.
 * Each property that can be animated gets its own KeyframeTrack<T>.
 */

#pragma once

#include <algorithm>
#include <vector>

#include "timeline/Keyframe.h"

namespace rt {

template <typename T>
class KeyframeTrack
{
public:
    /// Construct with a default value (track starts with no keyframes).
    explicit KeyframeTrack(T defaultValue = T{})
        : m_defaultValue(defaultValue)
    {
    }

    /// Default value returned when track has no keyframes.
    [[nodiscard]] T defaultValue() const noexcept { return m_defaultValue; }
    void setDefaultValue(T v) noexcept { m_defaultValue = v; }

    /// Write a value at a time (Premiere-style).
    /// - Static track (no keyframes): updates only the default value. A property
    ///   is never auto-converted to animated — the user must enable the
    ///   stopwatch first, which seeds the initial keyframe.
    /// - Animated track (stopwatch ON): adds a new keyframe at `time`, or
    ///   updates the existing keyframe if one already sits exactly there. This
    ///   is what makes moving the playhead and changing a value record a new
    ///   keyframe, exactly like Premiere Pro.
    void writeValue(int64_t time, T value)
    {
        if (m_keyframes.empty()) {
            m_defaultValue = value;
            return;
        }
        addKeyframe(time, value);
    }

    /// Restore a full keyframe (including bezier handles). If a keyframe at the
    /// same time already exists, it is entirely replaced. Otherwise inserted in order.
    void restoreKeyframe(const Keyframe<T>& kf)
    {
        auto it = std::lower_bound(m_keyframes.begin(), m_keyframes.end(), kf);
        if (it != m_keyframes.end() && it->time == kf.time)
            *it = kf;
        else
            m_keyframes.insert(it, kf);
    }

    /// Check if a keyframe exists at the given time.
    [[nodiscard]] bool hasKeyframeAt(int64_t time) const noexcept
    {
        return std::any_of(m_keyframes.begin(), m_keyframes.end(),
            [time](const Keyframe<T>& kf) { return kf.time == time; });
    }

    /// Evaluate the track at a given time, interpolating between keyframes.
    ///
    /// The shape of a segment kf[i] → kf[i+1] is built from kf[i]'s outgoing
    /// side and kf[i+1]'s incoming side (Premiere Pro model). If either side
    /// is Hold the segment holds at kf[i]; otherwise the segment is a cubic
    /// bezier in normalized [0,1]² space with control points computed per
    /// side from each endpoint's interpolation mode.
    [[nodiscard]] T evaluate(int64_t time) const noexcept
    {
        if (m_keyframes.empty())
            return m_defaultValue;

        if (m_keyframes.size() == 1)
            return m_keyframes[0].value;

        // Constant extrapolation outside the keyframed range.
        if (time <= m_keyframes.front().time) return m_keyframes.front().value;
        if (time >= m_keyframes.back().time)  return m_keyframes.back().value;

        auto it = std::lower_bound(
            m_keyframes.begin(), m_keyframes.end(), time,
            [](const Keyframe<T>& kf, int64_t t) { return kf.time < t; }
        );
        size_t idx = std::distance(m_keyframes.begin(), it);
        if (idx == 0) return m_keyframes[0].value;

        const size_t i0 = idx - 1;
        const size_t i1 = idx;
        const auto& kf0 = m_keyframes[i0];
        const auto& kf1 = m_keyframes[i1];

        // Hold takes precedence regardless of kf1's incoming mode.
        if (kf0.interp == InterpMode::Hold) return kf0.value;

        const double segDt = static_cast<double>(kf1.time - kf0.time);
        if (segDt <= 0.0) return kf0.value;

        const double segDv = static_cast<double>(kf1.value - kf0.value);
        const float  t     = static_cast<float>((time - kf0.time) / segDt);

        // Fast path: both sides Linear → straight chord.
        if (isLinearSide(kf0.interp, /*outgoing=*/true) &&
            isLinearSide(kf1.interp, /*outgoing=*/false))
            return lerp(kf0.value, kf1.value, t);

        const auto outH = effectiveOutHandle(i0, i1, segDt, segDv);
        const auto inH  = effectiveInHandle (i0, i1, segDt, segDv);

        const float bt = solveBezierT(0.0f, outH.x, inH.x, 1.0f, t);
        const float progress = evalCubicBezier(0.0f, outH.y, inH.y, 1.0f, bt);
        return lerp(kf0.value, kf1.value, progress);
    }

    /// Compute the effective outgoing handle for keyframe i0 on segment i0→i1,
    /// in normalized [0,1]² segment space (x = time fraction, y = value
    /// fraction with 0 = kf0.value and 1 = kf1.value). Public so the UI can
    /// draw the curve and tangent handles without duplicating the math.
    struct Handle2D { float x; float y; };

    [[nodiscard]] Handle2D effectiveOutHandle(size_t i0, size_t i1,
                                              double segDt, double segDv) const noexcept
    {
        (void)i1; (void)segDt;
        const auto& kf = m_keyframes[i0];
        switch (kf.interp)
        {
        case InterpMode::Bezier:
        case InterpMode::ContinuousBezier:
            return { kf.bezierOutX, kf.bezierOutY };

        case InterpMode::AutoBezier:
            return autoTangentOut(i0, segDv);

        case InterpMode::EaseOut:
            // Slow accelerating out of kf — flat tangent on the out side.
            return { 1.0f/3.0f, 0.0f };

        case InterpMode::EaseIn:
            // EaseIn modifies the incoming side; the outgoing side is linear.
        case InterpMode::Linear:
        case InterpMode::Hold:
        default:
            return { 1.0f/3.0f, 1.0f/3.0f };
        }
    }

    [[nodiscard]] Handle2D effectiveInHandle(size_t i0, size_t i1,
                                             double segDt, double segDv) const noexcept
    {
        (void)i0; (void)segDt;
        const auto& kf = m_keyframes[i1];
        switch (kf.interp)
        {
        case InterpMode::Bezier:
        case InterpMode::ContinuousBezier:
            return { kf.bezierInX, kf.bezierInY };

        case InterpMode::AutoBezier:
            return autoTangentIn(i1, segDv);

        case InterpMode::EaseIn:
            // Slow decelerating into kf — flat tangent on the in side.
            return { 2.0f/3.0f, 1.0f };

        case InterpMode::EaseOut:
            // EaseOut modifies the outgoing side; the incoming side is linear.
        case InterpMode::Linear:
        case InterpMode::Hold:
        default:
            return { 2.0f/3.0f, 2.0f/3.0f };
        }
    }

    // ── Keyframe management ─────────────────────────────────────────────
    void addKeyframe(int64_t time, T value, InterpMode interp = InterpMode::Linear)
    {
        Keyframe<T> kf{time, value, interp};
        auto it = std::lower_bound(m_keyframes.begin(), m_keyframes.end(), kf);

        // Replace existing keyframe at same time
        if (it != m_keyframes.end() && it->time == time)
        {
            it->value = value;
            it->interp = interp;
        }
        else
        {
            m_keyframes.insert(it, kf);
        }
    }

    void removeKeyframe(size_t index)
    {
        if (index < m_keyframes.size())
            m_keyframes.erase(m_keyframes.begin() + index);
    }

    void removeKeyframeAtTime(int64_t time)
    {
        auto it = std::find_if(m_keyframes.begin(), m_keyframes.end(),
            [time](const Keyframe<T>& kf) { return kf.time == time; });
        if (it != m_keyframes.end())
            m_keyframes.erase(it);
    }

    [[nodiscard]] size_t keyframeCount() const noexcept { return m_keyframes.size(); }
    [[nodiscard]] const Keyframe<T>& keyframe(size_t i) const { return m_keyframes[i]; }
    [[nodiscard]] Keyframe<T>& keyframe(size_t i) { return m_keyframes[i]; }

    [[nodiscard]] const std::vector<Keyframe<T>>& keyframes() const noexcept { return m_keyframes; }

    /// Returns true if track has no keyframes (not animated)
    [[nodiscard]] bool isStatic() const noexcept { return m_keyframes.empty(); }

private:
    T m_defaultValue{};
    std::vector<Keyframe<T>> m_keyframes;

    static T lerp(T a, T b, float t) noexcept
    {
        return static_cast<T>(a + (b - a) * t);
    }

    /// True if this keyframe contributes a Linear shape on the given side.
    /// EaseIn behaves linearly on the outgoing side; EaseOut behaves linearly
    /// on the incoming side. Hold is handled separately by evaluate().
    static bool isLinearSide(InterpMode m, bool outgoing) noexcept
    {
        switch (m)
        {
        case InterpMode::Linear:                            return true;
        case InterpMode::EaseIn:  return  outgoing;         // ease only on the in-side
        case InterpMode::EaseOut: return !outgoing;         // ease only on the out-side
        default:                                            return false;
        }
    }

    /// Auto Bezier out-tangent at keyframe i: Catmull-Rom-like slope through
    /// (i-1, i+1) projected into segment-normalized space, flattened at local
    /// extrema (Premiere's "flatten at extremes" rule).
    Handle2D autoTangentOut(size_t i, double segDv) const noexcept
    {
        if (i + 1 >= m_keyframes.size() || i == 0)
            return { 1.0f/3.0f, 1.0f/3.0f };
        const auto& kPrev = m_keyframes[i - 1];
        const auto& kCurr = m_keyframes[i];
        const auto& kNext = m_keyframes[i + 1];
        const double dvPrev = static_cast<double>(kCurr.value - kPrev.value);
        const double dvNext = static_cast<double>(kNext.value - kCurr.value);
        // Local extremum (sign change or flat side) → flatten the handle.
        if ((dvPrev > 0.0) != (dvNext > 0.0) || dvPrev == 0.0 || dvNext == 0.0)
            return { 1.0f/3.0f, 0.0f };
        const double globalDt = static_cast<double>(kNext.time - kPrev.time);
        const double globalDv = static_cast<double>(kNext.value - kPrev.value);
        const double slope    = (globalDt != 0.0) ? (globalDv / globalDt) : 0.0;
        const double segDt    = static_cast<double>(m_keyframes[i + 1].time - kCurr.time);
        // y-frac so that the cubic's slope at t=0 matches `slope` in real units.
        const double y = (segDv != 0.0) ? ((1.0/3.0) * slope * segDt / segDv) : 0.0;
        return { 1.0f/3.0f, static_cast<float>(y) };
    }

    /// Auto Bezier in-tangent at keyframe i (mirror of autoTangentOut for the
    /// incoming side; handle x is 2/3 because it's anchored to kf[i] from the
    /// previous keyframe's perspective).
    Handle2D autoTangentIn(size_t i, double segDv) const noexcept
    {
        if (i == 0 || i + 1 >= m_keyframes.size())
            return { 2.0f/3.0f, 2.0f/3.0f };
        const auto& kPrev = m_keyframes[i - 1];
        const auto& kCurr = m_keyframes[i];
        const auto& kNext = m_keyframes[i + 1];
        const double dvPrev = static_cast<double>(kCurr.value - kPrev.value);
        const double dvNext = static_cast<double>(kNext.value - kCurr.value);
        if ((dvPrev > 0.0) != (dvNext > 0.0) || dvPrev == 0.0 || dvNext == 0.0)
            return { 2.0f/3.0f, 1.0f };
        const double globalDt = static_cast<double>(kNext.time - kPrev.time);
        const double globalDv = static_cast<double>(kNext.value - kPrev.value);
        const double slope    = (globalDt != 0.0) ? (globalDv / globalDt) : 0.0;
        const double segDt    = static_cast<double>(kCurr.time - kPrev.time);
        const double y = (segDv != 0.0) ? (1.0 - (1.0/3.0) * slope * segDt / segDv) : 1.0;
        return { 2.0f/3.0f, static_cast<float>(y) };
    }
};

} // namespace rt
