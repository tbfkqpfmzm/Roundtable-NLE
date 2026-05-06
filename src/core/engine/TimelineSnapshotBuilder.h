#pragma once

#include "engine/EngineContracts.h"

#include <cstdint>

namespace rt {

class Timeline;

struct TimelineSnapshotBuildOptions
{
    uint64_t editVersion{0};
    double frameRate{0.0};
    bool includeMutedTracks{false};
    bool includeDisabledClips{false};
    bool includeTransitionPeerClips{false};
};

struct RenderGraphBuildOptions
{
    RenderRequestType requestType{RenderRequestType::Playback};
    RenderQuality quality{RenderQuality::Auto};
    RenderExactness exactness{RenderExactness::BestEffortAllowed};
};

class TimelineSnapshotBuilder
{
public:
    [[nodiscard]] TimelineSnapshot buildAt(
        const Timeline& timeline,
        int64_t tick,
        const TimelineSnapshotBuildOptions& options = {}) const;
};

[[nodiscard]] TimelineSnapshot buildTimelineSnapshot(
    const Timeline& timeline,
    int64_t tick,
    const TimelineSnapshotBuildOptions& options = {});

[[nodiscard]] RenderGraph buildRenderGraph(
    const TimelineSnapshot& snapshot,
    const RenderGraphBuildOptions& options = {});

} // namespace rt