#pragma once

#include "engine/EngineContracts.h"

#include <cstdint>
#include <vector>

namespace rt {

enum class CompositePassKind : uint8_t
{
    LayerGather,
    MediaResolve,
    CharacterResolve,
    GraphicResolve,
    MaskResolve,
    EffectResolve,
    TransitionResolve,
    FinalComposite,
    CpuReadback
};

struct CompositeExecutionRequest
{
    RenderRequest renderRequest;
    bool scrubMode{false};
    bool gpuDisplayMode{false};
    bool requiresCpuReadback{true};
};

struct CompositePreparedLayer
{
    uint64_t nodeId{0};
    RenderNodeKind kind{RenderNodeKind::Media};
    int trackIndex{0};
    int stackOrder{0};
    float opacity{1.0f};
    float positionX{0.0f};
    float positionY{0.0f};
    float scaleX{1.0f};
    float scaleY{1.0f};
    float rotation{0.0f};
    float cropLeft{0.0f};
    float cropRight{0.0f};
    float cropTop{0.0f};
    float cropBottom{0.0f};
    int32_t blendMode{0};
    bool hasMasks{false};
    bool hasEffects{false};
    bool characterLayer{false};
    bool transitionParticipant{false};
};

struct CompositeExecutionPlan
{
    RenderRequest request;
    std::vector<CompositePassKind> passes;
    std::vector<CompositePreparedLayer> layers;
    std::vector<uint64_t> layerNodeIds;
    size_t mediaResourceCount{0};
    size_t characterResourceCount{0};
    size_t graphicResourceCount{0};
    size_t maskResourceCount{0};
    size_t effectResourceCount{0};
    size_t transitionResourceCount{0};
    bool gpuDirectOutput{false};
    bool requiresCpuReadback{true};
};

class CompositeGraphExecutor
{
public:
    [[nodiscard]] static CompositeExecutionRequest makeRequest(
        int64_t tick,
        uint32_t outputWidth,
        uint32_t outputHeight,
        bool scrubMode,
        bool gpuDisplayMode,
        const char* caller = "CompositeService");

    [[nodiscard]] static CompositeExecutionRequest makeRequest(
        int64_t tick,
        uint32_t outputWidth,
        uint32_t outputHeight,
        RenderRequestType requestType,
        bool gpuDisplayMode,
        const char* caller = "CompositeService");

    [[nodiscard]] CompositeExecutionPlan plan(
        const TimelineSnapshot& snapshot,
        const RenderGraph& graph,
        const CompositeExecutionRequest& request) const;
};

[[nodiscard]] const char* toString(CompositePassKind pass) noexcept;

} // namespace rt