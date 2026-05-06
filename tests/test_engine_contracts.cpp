#include "engine/EngineContracts.h"
#include "engine/EngineServices.h"
#include "engine/MonitorPresenter.h"
#include "command/CommandStack.h"
#include "media/AudioEngine.h"
#include "media/AVSyncClock.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include "timeline/Timeline.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace rt {
namespace {

TEST(EngineContractsTest, RenderRequestDefaultsToPlaybackAutoBestEffort)
{
    RenderRequest request;

    EXPECT_EQ(request.requestId, 0u);
    EXPECT_EQ(request.type, RenderRequestType::Playback);
    EXPECT_EQ(request.quality, RenderQuality::Auto);
    EXPECT_EQ(request.exactness, RenderExactness::BestEffortAllowed);
    EXPECT_EQ(request.timelineTick, 0);
    EXPECT_EQ(request.outputWidth, 0u);
    EXPECT_EQ(request.outputHeight, 0u);
}

TEST(EngineContractsTest, ContractEnumsHaveStableDiagnosticStrings)
{
    EXPECT_STREQ(toString(RenderRequestType::Scrub), "Scrub");
    EXPECT_STREQ(toString(RenderRequestType::SourceMonitor), "SourceMonitor");
    EXPECT_STREQ(toString(RenderQuality::Quarter), "Quarter");
    EXPECT_STREQ(toString(RenderExactness::ExactRequired), "ExactRequired");
    EXPECT_STREQ(toString(RenderResultStatus::HeldPrevious), "HeldPrevious");
    EXPECT_STREQ(toString(RenderResourceKind::CharacterPose), "CharacterPose");
    EXPECT_STREQ(toString(RenderResourceKind::Effect), "Effect");
}

TEST(EngineContractsTest, RequestTypesChooseDefaultQualityAndExactness)
{
    EXPECT_EQ(defaultQualityFor(RenderRequestType::Playback), RenderQuality::Auto);
    EXPECT_EQ(defaultExactnessFor(RenderRequestType::Playback), RenderExactness::BestEffortAllowed);

    EXPECT_EQ(defaultQualityFor(RenderRequestType::Scrub), RenderQuality::Auto);
    EXPECT_EQ(defaultExactnessFor(RenderRequestType::Scrub), RenderExactness::ExactRequired);

    EXPECT_EQ(defaultQualityFor(RenderRequestType::Thumbnail), RenderQuality::Quarter);
    EXPECT_EQ(defaultExactnessFor(RenderRequestType::Thumbnail), RenderExactness::ExactRequired);

    EXPECT_EQ(defaultQualityFor(RenderRequestType::SourceMonitor), RenderQuality::Half);
    EXPECT_EQ(defaultExactnessFor(RenderRequestType::SourceMonitor), RenderExactness::ExactRequired);

    EXPECT_EQ(defaultQualityFor(RenderRequestType::Export), RenderQuality::Full);
    EXPECT_EQ(defaultExactnessFor(RenderRequestType::Export), RenderExactness::ExactRequired);
}

TEST(EngineContractsTest, TimelineSnapshotCarriesImmutableRenderNodes)
{
    TimelineSnapshot snapshot;
    snapshot.editVersion = 42;
    snapshot.sequenceDuration = 48000;
    snapshot.frameRate = 60.0;
    RenderNode node;
    node.stableId = 7;
    node.kind = RenderNodeKind::Media;
    node.timelineIn = 100;
    node.duration = 24000;
    node.sourceTick = 12;
    node.trackIndex = 3;
    node.opacity = 0.5f;
    node.enabled = true;
    snapshot.nodes.push_back(node);

    ASSERT_EQ(snapshot.nodes.size(), 1u);
    EXPECT_EQ(snapshot.editVersion, 42u);
    EXPECT_EQ(snapshot.nodes.front().stableId, 7u);
    EXPECT_EQ(snapshot.nodes.front().kind, RenderNodeKind::Media);
    EXPECT_EQ(snapshot.nodes.front().trackIndex, 3);
    EXPECT_FLOAT_EQ(snapshot.nodes.front().opacity, 0.5f);
}

TEST(MonitorPresenterTest, PlansCpuViewportWhenPixelsAreAvailable)
{
    RenderRequest request;
    request.type = RenderRequestType::Still;
    request.outputWidth = 640;
    request.outputHeight = 360;

    MonitorFrameDescriptor frame;
    frame.width = 640;
    frame.height = 360;
    frame.storage = MonitorFrameStorage::CpuPixels;
    frame.pixelsAvailable = true;

    const auto plan = MonitorPresenter{}.planPresentation(request, frame);

    EXPECT_EQ(plan.route, MonitorPresentationRoute::CpuViewport);
    EXPECT_TRUE(plan.consumesCpuPixels);
    EXPECT_FALSE(plan.consumesGpuHandle);
    EXPECT_EQ(plan.diagnostics.status, RenderResultStatus::Ready);
    EXPECT_STREQ(toString(plan.route), "CpuViewport");
}

TEST(MonitorPresenterTest, PrefersGpuDirectWhenGpuDisplayHasHandle)
{
    RenderRequest request;
    request.type = RenderRequestType::Playback;
    request.outputWidth = 1920;
    request.outputHeight = 1080;

    MonitorFrameDescriptor frame;
    frame.width = 1920;
    frame.height = 1080;
    frame.storage = MonitorFrameStorage::GpuHandle;
    frame.gpuReady = true;
    frame.gpuImageView = 10;
    frame.gpuSampler = 20;

    MonitorPresentationOptions options;
    options.preferGpuDisplay = true;

    const auto plan = MonitorPresenter{}.planPresentation(request, frame, options);

    EXPECT_EQ(plan.route, MonitorPresentationRoute::GpuDirect);
    EXPECT_TRUE(plan.consumesGpuHandle);
    EXPECT_FALSE(plan.consumesCpuPixels);
    EXPECT_EQ(plan.diagnostics.status, RenderResultStatus::Ready);
}

TEST(MonitorPresenterTest, UploadsCpuPixelsWhenGpuDisplayLacksHandle)
{
    RenderRequest request;
    request.type = RenderRequestType::Playback;

    MonitorFrameDescriptor frame;
    frame.width = 1280;
    frame.height = 720;
    frame.storage = MonitorFrameStorage::CpuPixels;
    frame.pixelsAvailable = true;

    MonitorPresentationOptions options;
    options.preferGpuDisplay = true;
    options.allowCpuUploadToGpu = true;

    const auto plan = MonitorPresenter{}.planPresentation(request, frame, options);

    EXPECT_EQ(plan.route, MonitorPresentationRoute::CpuUploadToGpu);
    EXPECT_TRUE(plan.consumesCpuPixels);
    EXPECT_FALSE(plan.consumesGpuHandle);
}

TEST(MonitorPresenterTest, ReportsClearAndMissingFrames)
{
    RenderRequest request;
    MonitorPresenter presenter;

    MonitorFrameDescriptor clearFrame;
    EXPECT_EQ(presenter.planPresentation(request, clearFrame).route,
              MonitorPresentationRoute::Clear);

    MonitorFrameDescriptor missingFrame;
    missingFrame.width = 320;
    missingFrame.height = 180;
    const auto missingPlan = presenter.planPresentation(request, missingFrame);

    EXPECT_EQ(missingPlan.route, MonitorPresentationRoute::MissingFrame);
    EXPECT_EQ(missingPlan.diagnostics.status, RenderResultStatus::Pending);
    EXPECT_FALSE(missingPlan.diagnostics.warning.empty());
}

TEST(EngineServicesTest, ReportsBoundLegacyServiceGroups)
{
    auto* timeline = reinterpret_cast<Timeline*>(static_cast<std::uintptr_t>(0x01));
    auto* commandStack = reinterpret_cast<CommandStack*>(static_cast<std::uintptr_t>(0x02));
    auto* playbackController = reinterpret_cast<PlaybackController*>(static_cast<std::uintptr_t>(0x03));
    auto* mediaPool = reinterpret_cast<MediaPool*>(static_cast<std::uintptr_t>(0x04));
    auto* audioEngine = reinterpret_cast<AudioEngine*>(static_cast<std::uintptr_t>(0x05));
    auto* syncClock = reinterpret_cast<AVSyncClock*>(static_cast<std::uintptr_t>(0x06));

    EngineServices services;
    services.bindLegacyServices({
        timeline,
        commandStack,
        nullptr,
        audioEngine,
        syncClock,
        playbackController,
        mediaPool,
        nullptr
    });

    EXPECT_TRUE(services.hasTimelineEditingServices());
    EXPECT_TRUE(services.hasPlaybackServices());
    EXPECT_TRUE(services.hasAudioClockServices());
    EXPECT_EQ(services.timeline(), timeline);
    EXPECT_EQ(services.mediaPool(), mediaPool);

    services.clear();
    EXPECT_FALSE(services.hasTimelineEditingServices());
    EXPECT_FALSE(services.hasPlaybackServices());
    EXPECT_FALSE(services.hasAudioClockServices());
}

} // namespace
} // namespace rt