/*
 * Constants.h — Project-wide constants for the Roundtable engine.
 *
 * Single source of truth for timing, duration, and conversion constants
 * used across core, media, timeline, and UI layers.
 */

#pragma once

#include <cstdint>

namespace rt {

/// Project-wide time representation: 64-bit sample-accurate ticks.
/// At 48kHz audio, this gives sub-frame precision at any video framerate.
using TimeTick = int64_t;

/// Ticks per second — matches 48kHz audio sample rate for sub-frame precision.
constexpr int64_t kTicksPerSecond = 48000;

/// Convert seconds → ticks
constexpr TimeTick secondsToTicks(double seconds) noexcept
{
    return static_cast<TimeTick>(seconds * kTicksPerSecond);
}

/// Convert ticks → seconds
constexpr double ticksToSeconds(TimeTick ticks) noexcept
{
    return static_cast<double>(ticks) / kTicksPerSecond;
}

/// Minimum clip duration after trimming (1 frame at 24fps).
constexpr int64_t kMinClipDuration = 2000; // 48000/24

/// Default offset for duplicate-paste (0.5 seconds).
constexpr int64_t kDuplicateOffset = 24000;

/// Default transition duration: 15 frames at 30fps = 0.5 seconds = 24000 ticks.
constexpr int64_t kDefaultTransitionDuration = 24000;

} // namespace rt
