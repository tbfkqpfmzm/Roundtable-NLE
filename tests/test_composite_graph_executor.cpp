#include "CompositeGraphExecutor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>

namespace rt {
namespace {

RenderNode makeLayer(uint64_t id, RenderNodeKind kind, int stackOrder)
{
    RenderNode node;
    node.stableId = id;
    node.kind = kind;
    node.trackIndex = stackOrder;
    node.stackOrder = stackOrder;
    node.opacity = 0.75f;
    node.positionX = 12.0f;
    node.positionY = -8.0f;
    node.scaleX = 1.25f;
    node.scaleY = 0.8f;
    node.rotation = 15.0f;
    node.cropLeft = 0.1f;
    node.cropRight = 0.2f;
    node.blendMode = 2;
    return node;
}

RenderGraph graphWithResources(std::initializer_list<RenderResourceKind> kinds)
{
    RenderGraph graph;
    uint64_t nodeId = 10;
    for (const auto kind : kinds) {
        graph.resources.push_back(RenderResourceRequest{kind, nodeId++});
    }
    return graph;
}

bool hasPass(const CompositeExecutionPlan& plan, CompositePassKind pass)
{
    return std::find(plan.passes.begin(), plan.passes.end(), pass) != plan.passes.end();
}

TEST(CompositeGraphExecutorTest, PlaybackRequestUsesBestEffortAndGpuDirectOutput)
{
    const auto request = CompositeGraphExecutor::makeRequest(48000, 1920, 1080, false, true);

    EXPECT_EQ(request.renderRequest.type, RenderRequestType::Playback);
    EXPECT_EQ(request.renderRequest.exactness, RenderExactness::BestEffortAllowed);
    EXPECT_EQ(request.renderRequest.quality, RenderQuality::Auto);
    EXPECT_FALSE(request.requiresCpuReadback);

    const auto plan = CompositeGraphExecutor{}.plan(TimelineSnapshot{}, RenderGraph{}, request);
    EXPECT_TRUE(plan.gpuDirectOutput);
    EXPECT_FALSE(plan.requiresCpuReadback);
    EXPECT_TRUE(hasPass(plan, CompositePassKind::LayerGather));
    EXPECT_TRUE(hasPass(plan, CompositePassKind::FinalComposite));
    EXPECT_FALSE(hasPass(plan, CompositePassKind::CpuReadback));
}

TEST(CompositeGraphExecutorTest, ScrubRequestRequiresExactCpuReadableFrame)
{
    const auto request = CompositeGraphExecutor::makeRequest(48000, 1280, 720, true, true);
    const auto plan = CompositeGraphExecutor{}.plan(TimelineSnapshot{}, RenderGraph{}, request);

    EXPECT_EQ(request.renderRequest.type, RenderRequestType::Scrub);
    EXPECT_EQ(request.renderRequest.exactness, RenderExactness::ExactRequired);
    EXPECT_TRUE(plan.requiresCpuReadback);
    EXPECT_FALSE(plan.gpuDirectOutput);
    EXPECT_TRUE(hasPass(plan, CompositePassKind::CpuReadback));
}

TEST(CompositeGraphExecutorTest, OfflineRequestsKeepExactPolicyWithCpuReadback)
{
    const auto exportRequest = CompositeGraphExecutor::makeRequest(
        96000, 3840, 2160, RenderRequestType::Export, true);
    const auto thumbnailRequest = CompositeGraphExecutor::makeRequest(
        12000, 480, 270, RenderRequestType::Thumbnail, true);

    EXPECT_EQ(exportRequest.renderRequest.type, RenderRequestType::Export);
    EXPECT_EQ(exportRequest.renderRequest.quality, RenderQuality::Full);
    EXPECT_EQ(exportRequest.renderRequest.exactness, RenderExactness::ExactRequired);
    EXPECT_TRUE(exportRequest.requiresCpuReadback);

    EXPECT_EQ(thumbnailRequest.renderRequest.type, RenderRequestType::Thumbnail);
    EXPECT_EQ(thumbnailRequest.renderRequest.quality, RenderQuality::Quarter);
    EXPECT_EQ(thumbnailRequest.renderRequest.exactness, RenderExactness::ExactRequired);
    EXPECT_TRUE(thumbnailRequest.requiresCpuReadback);

    const auto exportPlan = CompositeGraphExecutor{}.plan(
        TimelineSnapshot{}, RenderGraph{}, exportRequest);
    EXPECT_FALSE(exportPlan.gpuDirectOutput);
    EXPECT_TRUE(exportPlan.requiresCpuReadback);
    EXPECT_TRUE(hasPass(exportPlan, CompositePassKind::CpuReadback));
}

TEST(CompositeGraphExecutorTest, PreservesLayerOrderAndTransformMetadata)
{
    TimelineSnapshot snapshot;
    snapshot.nodes.push_back(makeLayer(101, RenderNodeKind::Media, 0));
    snapshot.nodes.push_back(makeLayer(202, RenderNodeKind::Character, 1));
    snapshot.nodes[1].characterSource = true;
    snapshot.nodes[1].hasMasks = true;
    snapshot.nodes[1].hasActiveEffects = true;
    snapshot.nodes[1].transitionProgress = 0.5f;
    snapshot.nodes.push_back(RenderNode{303, 0, 0, 0, RenderNodeKind::Audio});

    const auto request = CompositeGraphExecutor::makeRequest(0, 1920, 1080, false, false);
    const auto plan = CompositeGraphExecutor{}.plan(snapshot, RenderGraph{}, request);

    ASSERT_EQ(plan.layers.size(), 2u);
    EXPECT_EQ(plan.layerNodeIds[0], 101u);
    EXPECT_EQ(plan.layerNodeIds[1], 202u);

    const auto& layer = plan.layers[1];
    EXPECT_TRUE(layer.characterLayer);
    EXPECT_TRUE(layer.hasMasks);
    EXPECT_TRUE(layer.hasEffects);
    EXPECT_TRUE(layer.transitionParticipant);
    EXPECT_FLOAT_EQ(layer.opacity, 0.75f);
    EXPECT_FLOAT_EQ(layer.positionX, 12.0f);
    EXPECT_FLOAT_EQ(layer.positionY, -8.0f);
    EXPECT_FLOAT_EQ(layer.scaleX, 1.25f);
    EXPECT_FLOAT_EQ(layer.scaleY, 0.8f);
    EXPECT_FLOAT_EQ(layer.rotation, 15.0f);
    EXPECT_FLOAT_EQ(layer.cropLeft, 0.1f);
    EXPECT_FLOAT_EQ(layer.cropRight, 0.2f);
    EXPECT_EQ(layer.blendMode, 2);
}

TEST(CompositeGraphExecutorTest, RoutesResourceKindsToExplicitPasses)
{
    const auto request = CompositeGraphExecutor::makeRequest(0, 1920, 1080, false, false);
    const auto graph = graphWithResources({
        RenderResourceKind::MediaFrame,
        RenderResourceKind::CharacterPose,
        RenderResourceKind::GraphicRaster,
        RenderResourceKind::TitleRaster,
        RenderResourceKind::Mask,
        RenderResourceKind::Effect,
        RenderResourceKind::Transition,
        RenderResourceKind::AudioSamples});

    const auto plan = CompositeGraphExecutor{}.plan(TimelineSnapshot{}, graph, request);

    EXPECT_EQ(plan.mediaResourceCount, 1u);
    EXPECT_EQ(plan.characterResourceCount, 1u);
    EXPECT_EQ(plan.graphicResourceCount, 2u);
    EXPECT_EQ(plan.maskResourceCount, 1u);
    EXPECT_EQ(plan.effectResourceCount, 1u);
    EXPECT_EQ(plan.transitionResourceCount, 1u);
    EXPECT_TRUE(hasPass(plan, CompositePassKind::MediaResolve));
    EXPECT_TRUE(hasPass(plan, CompositePassKind::CharacterResolve));
    EXPECT_TRUE(hasPass(plan, CompositePassKind::GraphicResolve));
    EXPECT_TRUE(hasPass(plan, CompositePassKind::MaskResolve));
    EXPECT_TRUE(hasPass(plan, CompositePassKind::EffectResolve));
    EXPECT_TRUE(hasPass(plan, CompositePassKind::TransitionResolve));
}

} // namespace
} // namespace rt