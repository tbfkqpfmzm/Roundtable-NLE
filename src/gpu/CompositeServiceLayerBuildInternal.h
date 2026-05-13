/*
 * CompositeServiceLayerBuildInternal.h - Internal helpers for layer building.
 *
 * Declares PerClipContext (shared per-clip state) and helper methods
 * extracted from buildLayersForFrame() to keep TUs focused.
 */

#pragma once

#include "media/FrameCache.h"       // CachedFrame
#include "timeline/Transition.h"    // TransitionType

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt {

class Clip;
class VideoClip;
class SpineClip;
struct LayerInfo;

/// Shared per-clip evaluation context extracted from the buildLayersForFrame
/// loop body.  Passed to per-type helper methods to avoid passing ~20
/// individual parameters.
struct PerClipContext
{
    int64_t  tick;
    int64_t  localTick;
    uint64_t clipId;

    // Common transform properties (evaluated from keyframes)
    float opac{1.0f};
    float px{0.0f}, py{0.0f};
    float sx{1.0f}, sy{1.0f};
    float rot{0.0f};

    // Wipe/transition info
    TransitionType wipeType{TransitionType::CrossDissolve};
    float          wipeProgress{-1.0f};
    float          wipeSoftness{-1.0f};
    uint64_t       wipePeerClipId{0};
    bool           isWipeOutgoing{false};

    // Output dimensions
    uint32_t outW{0};
    uint32_t outH{0};

    // Perf logging
    bool                                               perfLog{false};
    std::chrono::high_resolution_clock::time_point     perfT0{};

    // Output layer collection
    std::vector<LayerInfo>& layers;
};

} // namespace rt
