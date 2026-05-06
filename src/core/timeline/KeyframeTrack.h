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
#include "timeline/KeyframeMode.h"

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

    /// Write a value: if animated (has keyframes), add/update at time; otherwise update default.
    /// When the global Auto-Keyframe mode is OFF (default), value writes on animated
    /// tracks only update an existing keyframe at the exact time — they will NOT
    /// create new keyframes (Premiere-style). The static default is also updated
    /// so undo paths and unanimated reads stay consistent.
    void writeValue(int64_t time, T value)
    {
        if (m_keyframes.empty()) {
            m_defaultValue = value;
            return;
        }
        if (KeyframeMode::isAutoEnabled()) {
            addKeyframe(time, value);
            return;
        }
        // Auto-keyframe OFF: only update an existing keyframe at this exact time.
        for (auto& kf : m_keyframes) {
            if (kf.time == time) { kf.value = value; return; }
        }
        // No keyframe at this time — leave the animated curve untouched.
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

    /// Evaluate the track at a given time (interpolates between keyframes)
    [[nodiscard]] T evaluate(int64_t time) const noexcept
    {
        if (m_keyframes.empty())
            return m_defaultValue;

        if (m_keyframes.size() == 1)
            return m_keyframes[0].value;

        // Before first keyframe
        if (time <= m_keyframes.front().time)
            return m_keyframes.front().value;

        // After last keyframe
        if (time >= m_keyframes.back().time)
            return m_keyframes.back().value;

        // Binary search for the segment containing `time`
        auto it = std::lower_bound(
            m_keyframes.begin(), m_keyframes.end(), time,
            [](const Keyframe<T>& kf, int64_t t) { return kf.time < t; }
        );

        size_t idx = std::distance(m_keyframes.begin(), it);
        if (idx == 0) return m_keyframes[0].value;

        const auto& kf0 = m_keyframes[idx - 1];
        const auto& kf1 = m_keyframes[idx];

        // Normalized position within segment [0, 1]
        double segmentDuration = static_cast<double>(kf1.time - kf0.time);
        if (segmentDuration <= 0.0) return kf0.value;

        float t = static_cast<float>((time - kf0.time) / segmentDuration);

        switch (kf0.interp)
        {
        case InterpMode::Hold:
            return kf0.value;

        case InterpMode::Linear:
            return lerp(kf0.value, kf1.value, t);

        case InterpMode::Bezier:
        {
            // Evaluate bezier curve
            float bt = solveBezierT(0.0f, kf0.bezierOutX, kf1.bezierInX, 1.0f, t);
            float progress = evalCubicBezier(0.0f, kf0.bezierOutY, kf1.bezierInY, 1.0f, bt);
            return lerp(kf0.value, kf1.value, progress);
        }
        }

        return kf0.value; // fallback
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
};

} // namespace rt
