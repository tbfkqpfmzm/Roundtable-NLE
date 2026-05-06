/*
 * Transition — visual transition between two adjacent clips.
 */

#pragma once

#include <cstdint>

namespace rt {

enum class TransitionType : uint8_t
{
    CrossDissolve,
    FadeToBlack,
    FadeFromBlack,
    FadeToWhite,
    FadeFromWhite,
    WipeLeft,
    WipeRight,
    WipeUp,
    WipeDown,
    PushLeft,
    PushRight,
    PushUp,
    PushDown,

    // ── Dissolve family ──
    DipToBlack,
    DipToWhite,
    FilmDissolve,
    AdditiveDissolve,

    // ── Wipe family ──
    BarnDoor,
    ClockWipe,
    RadialWipe,
    IrisRound,
    IrisDiamond,
    IrisCross,
    DiagonalWipe,
    CheckerWipe,
    VenetianBlinds,
    Inset,

    // ── Slide family ──
    SlideLeft,
    SlideRight,
    SlideUp,
    SlideDown,
    Split,
    CenterSplit,
    Swap,

    // ── Zoom / motion ──
    Zoom,
    CrossZoom,
    WhipPan,

    // ── Stylized ──
    RandomBlocks,
    MorphCut,
    GradientWipe,
};

struct Transition
{
    TransitionType type{TransitionType::CrossDissolve};
    int64_t        duration{0};       // In TimeTicks
    int64_t        offset{0};         // Position relative to edit point

    /// Clip IDs that this transition is between.
    /// leftClipId=0 means fade in from nothing; rightClipId=0 means fade out to nothing.
    uint64_t       leftClipId{0};
    uint64_t       rightClipId{0};

    /// The timeline position of the edit point (where leftClip ends / rightClip begins).
    int64_t        editPointTick{0};

    // Additional parameters for specific transition types
    float param1{0.0f};  // e.g., softness for wipes
    float param2{0.0f};  // reserved

    /// Compute region this transition covers on the timeline.
    /// For a fade-in (leftClipId==0): [editPointTick, editPointTick + duration]
    /// For a fade-out (rightClipId==0): [editPointTick - duration, editPointTick]
    /// For a cross-dissolve: [editPointTick - duration/2, editPointTick + duration/2]
    void getRange(int64_t& outStart, int64_t& outEnd) const
    {
        if (leftClipId == 0) {
            // Fade-in at start of rightClip
            outStart = editPointTick;
            outEnd   = editPointTick + duration;
        } else if (rightClipId == 0) {
            // Fade-out at end of leftClip
            outStart = editPointTick - duration;
            outEnd   = editPointTick;
        } else {
            // Cross-dissolve centered on edit point
            outStart = editPointTick - duration / 2;
            outEnd   = editPointTick + duration / 2;
        }
    }

    /// Compute transition progress (0.0 → 1.0) for a given tick.
    /// Returns < 0 if tick is outside the transition range.
    float progress(int64_t tick) const
    {
        int64_t start, end;
        getRange(start, end);
        if (tick < start || tick >= end || end <= start) return -1.0f;
        return static_cast<float>(tick - start) / static_cast<float>(end - start);
    }
};

} // namespace rt
