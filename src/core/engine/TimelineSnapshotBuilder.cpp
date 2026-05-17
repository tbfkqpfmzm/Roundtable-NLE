#include "engine/TimelineSnapshotBuilder.h"

#include "timeline/AdjustmentClip.h"
#include "timeline/AudioClip.h"
#include "timeline/CaptionClip.h"
#include "timeline/Clip.h"
#include "timeline/GraphicClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/Timeline.h"
#include "timeline/Position2D.h"
#include "timeline/TitleClip.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace rt {
namespace {

[[nodiscard]] uint64_t sourceIdForPath(const std::string& path)
{
    return path.empty() ? 0u : static_cast<uint64_t>(std::hash<std::string>{}(path));
}

[[nodiscard]] uint64_t transitionStableId(const Transition& transition)
{
    uint64_t value = 1469598103934665603ull;
    auto mix = [&value](uint64_t part) {
        value ^= part;
        value *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(transition.type));
    mix(static_cast<uint64_t>(transition.editPointTick));
    mix(transition.leftClipId);
    mix(transition.rightClipId);
    return value;
}

[[nodiscard]] RenderNodeKind nodeKindFor(const Clip& clip)
{
    if (const auto* video = dynamic_cast<const VideoClip*>(&clip); video && video->isVideoCharacter()) {
        return RenderNodeKind::Character;
    }

    switch (clip.clipType()) {
    case ClipType::Video:
    case ClipType::Image:      return RenderNodeKind::Media;
    case ClipType::Audio:      return RenderNodeKind::Audio;
    case ClipType::Spine:      return RenderNodeKind::Character;
    case ClipType::Graphic:    return RenderNodeKind::Graphic;
    case ClipType::Title:      return RenderNodeKind::Title;
    case ClipType::Adjustment: return RenderNodeKind::Adjustment;
    case ClipType::Sequence:   return RenderNodeKind::NestedSequence;
    }
    return RenderNodeKind::Generated;
}

void populateSource(RenderNode& node, const Clip& clip)
{
    if (const auto* video = dynamic_cast<const VideoClip*>(&clip)) {
        node.sourcePath = video->mediaPath();
        node.sourceId = video->mediaId() != 0 ? video->mediaId()
                                             : sourceIdForPath(video->mediaPath());
        node.sourceWidth = video->sourceWidth();
        node.sourceHeight = video->sourceHeight();
        node.audioNeeded = video->hasAudio();
        node.clipVolume = video->volume();
        node.cropLeft = video->cropLeft();
        node.cropRight = video->cropRight();
        node.cropTop = video->cropTop();
        node.cropBottom = video->cropBottom();
        node.stillSource = video->sourceDuration() <= 0;
        node.characterSource = video->isVideoCharacter();
        return;
    }
    if (const auto* audio = dynamic_cast<const AudioClip*>(&clip)) {
        node.sourcePath = audio->mediaPath();
        node.sourceId = audio->mediaId() != 0 ? audio->mediaId()
                                             : sourceIdForPath(audio->mediaPath());
        node.audioNeeded = true;
        node.clipVolume = audio->volume().evaluate(node.localTick);
        node.clipPan = audio->pan().evaluate(node.localTick);
        return;
    }
    if (const auto* image = dynamic_cast<const ImageClip*>(&clip)) {
        node.sourcePath = image->mediaPath();
        node.sourceId = image->mediaId() != 0 ? image->mediaId()
                                             : sourceIdForPath(image->mediaPath());
        node.sourceWidth = image->sourceWidth();
        node.sourceHeight = image->sourceHeight();
        node.cropLeft = image->cropLeft();
        node.cropRight = image->cropRight();
        node.cropTop = image->cropTop();
        node.cropBottom = image->cropBottom();
        node.stillSource = true;
        return;
    }
    if (const auto* title = dynamic_cast<const TitleClip*>(&clip)) {
        node.sourcePath = title->text();
        node.sourceId = sourceIdForPath(node.sourcePath);
        return;
    }
    if (const auto* graphic = dynamic_cast<const GraphicClip*>(&clip)) {
        node.sourcePath = graphic->label();
        node.sourceId = graphic->id();
        node.graphicLayerCount = graphic->layerCount();
        return;
    }
    if (const auto* sequence = dynamic_cast<const SequenceClip*>(&clip)) {
        node.sourcePath = sequence->sequenceName();
        node.sourceId = static_cast<uint64_t>(sequence->sequenceIndex() + 1);
        return;
    }
    if (const auto* spine = dynamic_cast<const SpineClip*>(&clip)) {
        node.sourcePath = spine->characterName() + "|" + spine->outfit() + "|" +
            spine->animationName();
        node.sourceId = sourceIdForPath(node.sourcePath);
        node.cropLeft = spine->cropLeft();
        node.cropRight = spine->cropRight();
        node.cropTop = spine->cropTop();
        node.cropBottom = spine->cropBottom();
        node.looping = spine->isLooping();
        node.characterSource = true;
    }
}

[[nodiscard]] RenderNode makeClipNode(
    const Clip& clip,
    const Track& track,
    int trackIndex,
    int stackOrder,
    int64_t tick)
{
    const int64_t localTick = std::max<int64_t>(0, tick - clip.timelineIn());

    RenderNode node;
    node.stableId = clip.id();
    node.kind = nodeKindFor(clip);
    node.timelineIn = clip.timelineIn();
    node.duration = clip.duration();
    node.localTick = localTick;
    node.sourceIn = clip.sourceIn();
    node.sourceOut = clip.sourceOut();
    node.playbackSpeed = clip.speed();
    node.effectiveSpeed = clip.effectiveSpeed(localTick);
    node.sourceTick = clip.sourceIn() + static_cast<int64_t>(static_cast<double>(localTick) * node.effectiveSpeed);
    node.trackIndex = trackIndex;
    node.stackOrder = stackOrder;
    node.blendMode = clip.blendMode();
    node.opacity = clip.opacity().evaluate(localTick);
    {
        auto p2 = evaluatePosition2D(clip.positionX(), clip.positionY(), localTick);
        node.positionX = p2.first;
        node.positionY = p2.second;
    }
    node.scaleX = clip.scaleX().evaluate(localTick);
    node.scaleY = clip.scaleY().evaluate(localTick);
    node.rotation = clip.rotation().evaluate(localTick);
    node.trackVolume = track.volume();
    node.trackPan = track.pan();
    node.enabled = clip.isEnabled();
    node.trackMuted = track.isMuted();
    node.maintainPitch = clip.maintainPitch();
    node.effectCount = clip.effects().effectCount();
    node.maskCount = clip.maskCount();
    node.hasActiveEffects = clip.effects().hasActiveEffects();
    node.hasMasks = node.maskCount > 0;
    node.groupId = clip.groupId();
    node.label = clip.label();
    node.shotName = clip.shotName();
    node.layerId = clip.layerId();
    populateSource(node, clip);
    return node;
}

[[nodiscard]] RenderNode makeTransitionNode(
    const Transition& transition,
    const Track& track,
    int trackIndex,
    int stackOrder,
    int64_t tick)
{
    int64_t start = 0;
    int64_t end = 0;
    transition.getRange(start, end);

    RenderNode node;
    node.stableId = transitionStableId(transition);
    node.sourceId = transition.leftClipId;
    node.peerSourceId = transition.rightClipId;
    node.kind = RenderNodeKind::Transition;
    node.timelineIn = start;
    node.duration = std::max<int64_t>(0, end - start);
    node.sourceTick = std::max<int64_t>(0, tick - start);
    node.trackIndex = trackIndex;
    node.stackOrder = stackOrder;
    node.transitionType = static_cast<int32_t>(transition.type);
    node.transitionProgress = transition.progress(tick);
    node.opacity = node.transitionProgress;
    node.enabled = true;
    node.trackMuted = track.isMuted();
    return node;
}

[[nodiscard]] bool hasClipNode(
    const std::vector<RenderNode>& nodes,
    int trackIndex,
    uint64_t clipId)
{
    return std::any_of(nodes.begin(), nodes.end(), [trackIndex, clipId](const RenderNode& node) {
        return node.kind != RenderNodeKind::Transition &&
               node.trackIndex == trackIndex &&
               node.stableId == clipId;
    });
}

void appendTransitionPeerClipNode(
    TimelineSnapshot& snapshot,
    const Track& track,
    int trackIndex,
    int& stackOrder,
    uint64_t clipId,
    int64_t tick)
{
    if (clipId == 0 || hasClipNode(snapshot.nodes, trackIndex, clipId)) {
        return;
    }

    const size_t clipIndex = track.findClipIndexById(clipId);
    if (clipIndex >= track.clipCount()) {
        return;
    }

    if (const auto* clip = track.clip(clipIndex)) {
        snapshot.nodes.push_back(makeClipNode(*clip, track, trackIndex, stackOrder++, tick));
    }
}

[[nodiscard]] RenderResourceRequest makeResourceRequest(
    const RenderNode& node,
    RenderResourceKind kind,
    const RenderGraphBuildOptions& options)
{
    RenderResourceRequest request;
    request.kind = kind;
    request.nodeId = node.stableId;
    request.sourceId = node.sourceId;
    request.peerSourceId = node.peerSourceId;
    request.sourceTick = node.sourceTick;
    request.duration = node.duration;
    request.requestType = options.requestType;
    request.quality = options.quality;
    request.exactness = options.exactness;
    request.sourceWidth = node.sourceWidth;
    request.sourceHeight = node.sourceHeight;
    request.effectCount = node.effectCount;
    request.maskCount = node.maskCount;
    request.graphicLayerCount = node.graphicLayerCount;
    request.audioNeeded = node.audioNeeded;
    request.stillSource = node.stillSource;
    request.characterSource = node.characterSource;
    request.sourcePath = node.sourcePath;
    return request;
}

void appendPrimaryResource(
    const RenderNode& node,
    const RenderGraphBuildOptions& options,
    std::vector<RenderResourceRequest>& resources)
{
    switch (node.kind) {
    case RenderNodeKind::Media:
        resources.push_back(makeResourceRequest(node, RenderResourceKind::MediaFrame, options));
        break;
    case RenderNodeKind::Audio:
        resources.push_back(makeResourceRequest(node, RenderResourceKind::AudioSamples, options));
        break;
    case RenderNodeKind::Character:
        resources.push_back(makeResourceRequest(node, RenderResourceKind::CharacterPose, options));
        break;
    case RenderNodeKind::Graphic:
        resources.push_back(makeResourceRequest(node, RenderResourceKind::GraphicRaster, options));
        break;
    case RenderNodeKind::Title:
    case RenderNodeKind::Caption:
        resources.push_back(makeResourceRequest(node, RenderResourceKind::TitleRaster, options));
        break;
    case RenderNodeKind::NestedSequence:
        resources.push_back(makeResourceRequest(node, RenderResourceKind::NestedSequence, options));
        break;
    case RenderNodeKind::Transition:
        resources.push_back(makeResourceRequest(node, RenderResourceKind::Transition, options));
        break;
    case RenderNodeKind::Adjustment:
    case RenderNodeKind::Mask:
    case RenderNodeKind::Generated:
        break;
    }
}

} // namespace

TimelineSnapshot TimelineSnapshotBuilder::buildAt(
    const Timeline& timeline,
    int64_t tick,
    const TimelineSnapshotBuildOptions& options) const
{
    TimelineSnapshot snapshot;
    snapshot.editVersion = options.editVersion;
    snapshot.evaluationTick = tick;
    snapshot.sequenceDuration = timeline.duration();
    snapshot.frameRate = options.frameRate;

    int stackOrder = 0;
    for (size_t trackOffset = timeline.trackCount(); trackOffset > 0; --trackOffset) {
        const size_t trackIndex = trackOffset - 1;
        const auto* track = timeline.track(trackIndex);
        if (!track || track->isDivider()) {
            continue;
        }
        if (track->isMuted() && !options.includeMutedTracks) {
            continue;
        }

        for (size_t clipIndex = 0; clipIndex < track->clipCount(); ++clipIndex) {
            const auto* clip = track->clip(clipIndex);
            if (!clip) {
                continue;
            }
            if (!clip->isEnabled() && !options.includeDisabledClips) {
                continue;
            }
            if (clip->timelineIn() <= tick && tick < clip->timelineOut()) {
                snapshot.nodes.push_back(makeClipNode(
                    *clip, *track, static_cast<int>(trackIndex), stackOrder++, tick));
            }
        }

        for (const auto& transition : track->transitions()) {
            const float progress = transition.progress(tick);
            if (progress >= 0.0f) {
                if (options.includeTransitionPeerClips) {
                    appendTransitionPeerClipNode(snapshot, *track, static_cast<int>(trackIndex),
                                                 stackOrder, transition.leftClipId, tick);
                    appendTransitionPeerClipNode(snapshot, *track, static_cast<int>(trackIndex),
                                                 stackOrder, transition.rightClipId, tick);
                }
                snapshot.nodes.push_back(makeTransitionNode(
                    transition, *track, static_cast<int>(trackIndex), stackOrder++, tick));
            }
        }
    }

    return snapshot;
}

TimelineSnapshot buildTimelineSnapshot(
    const Timeline& timeline,
    int64_t tick,
    const TimelineSnapshotBuildOptions& options)
{
    return TimelineSnapshotBuilder{}.buildAt(timeline, tick, options);
}

RenderGraph buildRenderGraph(
    const TimelineSnapshot& snapshot,
    const RenderGraphBuildOptions& options)
{
    RenderGraph graph;
    graph.editVersion = snapshot.editVersion;
    graph.evaluationTick = snapshot.evaluationTick;
    graph.frameRate = snapshot.frameRate;
    graph.nodes.reserve(snapshot.nodes.size());

    for (const auto& node : snapshot.nodes) {
        RenderGraphNode graphNode;
        graphNode.nodeId = node.stableId;
        graphNode.kind = node.kind;
        graphNode.trackIndex = node.trackIndex;
        graphNode.stackOrder = node.stackOrder;
        graphNode.firstResource = graph.resources.size();

        appendPrimaryResource(node, options, graph.resources);
        if (node.hasMasks || node.maskCount > 0) {
            graph.resources.push_back(makeResourceRequest(node, RenderResourceKind::Mask, options));
        }
        if (node.hasActiveEffects || node.effectCount > 0) {
            graph.resources.push_back(makeResourceRequest(node, RenderResourceKind::Effect, options));
        }

        graphNode.resourceCount = graph.resources.size() - graphNode.firstResource;
        graph.nodes.push_back(graphNode);
    }

    return graph;
}

} // namespace rt