#include "CompositeGraphExecutor.h"

#include <algorithm>

namespace rt {
namespace {

[[nodiscard]] bool isVisualLayer(RenderNodeKind kind) noexcept
{
    switch (kind) {
    case RenderNodeKind::Media:
    case RenderNodeKind::Character:
    case RenderNodeKind::Graphic:
    case RenderNodeKind::Title:
    case RenderNodeKind::Caption:
    case RenderNodeKind::NestedSequence:
    case RenderNodeKind::Generated:
        return true;
    case RenderNodeKind::Audio:
    case RenderNodeKind::Adjustment:
    case RenderNodeKind::Transition:
    case RenderNodeKind::Mask:
        return false;
    }
    return false;
}

void addPassOnce(std::vector<CompositePassKind>& passes, CompositePassKind pass)
{
    if (std::find(passes.begin(), passes.end(), pass) == passes.end()) {
        passes.push_back(pass);
    }
}

} // namespace

CompositeExecutionRequest CompositeGraphExecutor::makeRequest(
    int64_t tick,
    uint32_t outputWidth,
    uint32_t outputHeight,
    bool scrubMode,
    bool gpuDisplayMode,
    const char* caller)
{
    const auto type = scrubMode ? RenderRequestType::Scrub : RenderRequestType::Playback;

    return makeRequest(tick, outputWidth, outputHeight, type, gpuDisplayMode, caller);
}

CompositeExecutionRequest CompositeGraphExecutor::makeRequest(
    int64_t tick,
    uint32_t outputWidth,
    uint32_t outputHeight,
    RenderRequestType requestType,
    bool gpuDisplayMode,
    const char* caller)
{
    const bool scrubMode = requestType == RenderRequestType::Scrub;

    CompositeExecutionRequest request;
    request.renderRequest.type = requestType;
    request.renderRequest.quality = defaultQualityFor(requestType);
    request.renderRequest.exactness = defaultExactnessFor(requestType);
    request.renderRequest.timelineTick = tick;
    request.renderRequest.outputWidth = outputWidth;
    request.renderRequest.outputHeight = outputHeight;
    request.renderRequest.caller = caller ? caller : "CompositeService";
    request.scrubMode = scrubMode;
    request.gpuDisplayMode = gpuDisplayMode;
    request.requiresCpuReadback = !gpuDisplayMode || requestType != RenderRequestType::Playback;
    return request;
}

CompositeExecutionPlan CompositeGraphExecutor::plan(
    const TimelineSnapshot& snapshot,
    const RenderGraph& graph,
    const CompositeExecutionRequest& request) const
{
    CompositeExecutionPlan plan;
    plan.request = request.renderRequest;
    plan.gpuDirectOutput = request.gpuDisplayMode && !request.scrubMode;
    plan.requiresCpuReadback = request.requiresCpuReadback;

    addPassOnce(plan.passes, CompositePassKind::LayerGather);

    plan.layers.reserve(snapshot.nodes.size());
    plan.layerNodeIds.reserve(snapshot.nodes.size());
    for (const auto& node : snapshot.nodes) {
        if (!isVisualLayer(node.kind)) {
            continue;
        }

        CompositePreparedLayer layer;
        layer.nodeId = node.stableId;
        layer.kind = node.kind;
        layer.trackIndex = node.trackIndex;
        layer.stackOrder = node.stackOrder;
        layer.opacity = node.opacity;
        layer.positionX = node.positionX;
        layer.positionY = node.positionY;
        layer.scaleX = node.scaleX;
        layer.scaleY = node.scaleY;
        layer.rotation = node.rotation;
        layer.cropLeft = node.cropLeft;
        layer.cropRight = node.cropRight;
        layer.cropTop = node.cropTop;
        layer.cropBottom = node.cropBottom;
        layer.blendMode = node.blendMode;
        layer.hasMasks = node.hasMasks || node.maskCount > 0;
        layer.hasEffects = node.hasActiveEffects || node.effectCount > 0;
        layer.characterLayer = node.characterSource || node.kind == RenderNodeKind::Character;
        layer.transitionParticipant = node.transitionProgress >= 0.0f || node.peerSourceId != 0;

        plan.layerNodeIds.push_back(node.stableId);
        plan.layers.push_back(layer);
    }

    for (const auto& resource : graph.resources) {
        switch (resource.kind) {
        case RenderResourceKind::MediaFrame:
        case RenderResourceKind::NestedSequence:
            ++plan.mediaResourceCount;
            addPassOnce(plan.passes, CompositePassKind::MediaResolve);
            break;
        case RenderResourceKind::CharacterPose:
            ++plan.characterResourceCount;
            addPassOnce(plan.passes, CompositePassKind::CharacterResolve);
            break;
        case RenderResourceKind::GraphicRaster:
        case RenderResourceKind::TitleRaster:
            ++plan.graphicResourceCount;
            addPassOnce(plan.passes, CompositePassKind::GraphicResolve);
            break;
        case RenderResourceKind::Mask:
            ++plan.maskResourceCount;
            addPassOnce(plan.passes, CompositePassKind::MaskResolve);
            break;
        case RenderResourceKind::Effect:
            ++plan.effectResourceCount;
            addPassOnce(plan.passes, CompositePassKind::EffectResolve);
            break;
        case RenderResourceKind::Transition:
            ++plan.transitionResourceCount;
            addPassOnce(plan.passes, CompositePassKind::TransitionResolve);
            break;
        case RenderResourceKind::AudioSamples:
            break;
        }
    }

    addPassOnce(plan.passes, CompositePassKind::FinalComposite);
    if (plan.requiresCpuReadback) {
        addPassOnce(plan.passes, CompositePassKind::CpuReadback);
    }

    return plan;
}

const char* toString(CompositePassKind pass) noexcept
{
    switch (pass) {
    case CompositePassKind::LayerGather:       return "LayerGather";
    case CompositePassKind::MediaResolve:      return "MediaResolve";
    case CompositePassKind::CharacterResolve:  return "CharacterResolve";
    case CompositePassKind::GraphicResolve:    return "GraphicResolve";
    case CompositePassKind::MaskResolve:       return "MaskResolve";
    case CompositePassKind::EffectResolve:     return "EffectResolve";
    case CompositePassKind::TransitionResolve: return "TransitionResolve";
    case CompositePassKind::FinalComposite:    return "FinalComposite";
    case CompositePassKind::CpuReadback:       return "CpuReadback";
    }
    return "Unknown";
}

} // namespace rt