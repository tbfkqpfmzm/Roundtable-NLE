#include "engine/TimelineSnapshotBuilder.h"

#include "Constants.h"
#include "effects/Blur.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/AudioClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include <gtest/gtest.h>

#include <memory>

namespace rt {
namespace {

TEST(TimelineSnapshotBuilderTest, BuildsImmutableSnapshotForActiveVideoClips)
{
    Timeline timeline;
    auto* lowerTrack = timeline.addVideoTrack("V1");
    auto* upperTrack = timeline.addVideoTrack("V2");

    auto lowerClip = std::make_unique<VideoClip>("assets/lower.mp4");
    lowerClip->setTimelineIn(0);
    lowerClip->setSourceIn(100);
    lowerClip->setDuration(kTicksPerSecond * 2);
    lowerClip->setMediaId(11);
    const auto lowerId = lowerClip->id();
    lowerTrack->addClip(std::move(lowerClip));

    auto upperClip = std::make_unique<VideoClip>("assets/upper.mp4");
    upperClip->setTimelineIn(kTicksPerSecond / 2);
    upperClip->setDuration(kTicksPerSecond);
    upperClip->setMediaId(22);
    const auto upperId = upperClip->id();
    upperTrack->addClip(std::move(upperClip));

    const auto snapshot = buildTimelineSnapshot(
        timeline,
        kTicksPerSecond,
        TimelineSnapshotBuildOptions{42, 24.0});

    ASSERT_EQ(snapshot.nodes.size(), 2u);
    EXPECT_EQ(snapshot.editVersion, 42u);
    EXPECT_EQ(snapshot.evaluationTick, kTicksPerSecond);
    EXPECT_EQ(snapshot.sequenceDuration, kTicksPerSecond * 2);
    EXPECT_DOUBLE_EQ(snapshot.frameRate, 24.0);

    EXPECT_EQ(snapshot.nodes[0].stableId, upperId);
    EXPECT_EQ(snapshot.nodes[0].sourceId, 22u);
    EXPECT_EQ(snapshot.nodes[0].sourcePath, "assets/upper.mp4");
    EXPECT_EQ(snapshot.nodes[0].trackIndex, 1);
    EXPECT_EQ(snapshot.nodes[0].stackOrder, 0);
    EXPECT_EQ(snapshot.nodes[0].localTick, kTicksPerSecond / 2);
    EXPECT_EQ(snapshot.nodes[0].sourceIn, 0);
    EXPECT_EQ(snapshot.nodes[0].sourceOut, kTicksPerSecond);
    EXPECT_EQ(snapshot.nodes[0].sourceTick, kTicksPerSecond / 2);

    EXPECT_EQ(snapshot.nodes[1].stableId, lowerId);
    EXPECT_EQ(snapshot.nodes[1].sourceId, 11u);
    EXPECT_EQ(snapshot.nodes[1].trackIndex, 0);
    EXPECT_EQ(snapshot.nodes[1].stackOrder, 1);
    EXPECT_EQ(snapshot.nodes[1].sourceTick, kTicksPerSecond + 100);
}

TEST(TimelineSnapshotBuilderTest, SkipsMutedTracksDisabledClipsAndBoundaryEnds)
{
    Timeline timeline;
    auto* mutedTrack = timeline.addVideoTrack("Muted");
    auto* activeTrack = timeline.addVideoTrack("Active");
    mutedTrack->setMuted(true);

    auto mutedClip = std::make_unique<VideoClip>("muted.mp4");
    mutedClip->setDuration(kTicksPerSecond);
    mutedTrack->addClip(std::move(mutedClip));

    auto disabledClip = std::make_unique<VideoClip>("disabled.mp4");
    disabledClip->setDuration(kTicksPerSecond);
    disabledClip->setEnabled(false);
    activeTrack->addClip(std::move(disabledClip));

    auto boundaryClip = std::make_unique<VideoClip>("boundary.mp4");
    boundaryClip->setTimelineIn(0);
    boundaryClip->setDuration(kTicksPerSecond);
    activeTrack->addClip(std::move(boundaryClip));

    EXPECT_TRUE(buildTimelineSnapshot(timeline, kTicksPerSecond).nodes.empty());

    auto inclusiveSnapshot = buildTimelineSnapshot(
        timeline,
        kTicksPerSecond / 2,
        TimelineSnapshotBuildOptions{0, 0.0, true, true});

    ASSERT_EQ(inclusiveSnapshot.nodes.size(), 3u);
    EXPECT_TRUE(inclusiveSnapshot.nodes[2].trackMuted);
    EXPECT_FALSE(inclusiveSnapshot.nodes[0].enabled);
}

TEST(TimelineSnapshotBuilderTest, CapturesTransitionsAsRenderNodes)
{
    Timeline timeline;
    auto* track = timeline.addVideoTrack("V1");

    auto left = std::make_unique<VideoClip>("left.mp4");
    left->setTimelineIn(0);
    left->setDuration(kTicksPerSecond);
    const uint64_t leftId = left->id();
    track->addClip(std::move(left));

    auto right = std::make_unique<VideoClip>("right.mp4");
    right->setTimelineIn(kTicksPerSecond);
    right->setDuration(kTicksPerSecond);
    const uint64_t rightId = right->id();
    track->addClip(std::move(right));

    Transition transition;
    transition.type = TransitionType::CrossDissolve;
    transition.duration = kTicksPerSecond / 2;
    transition.editPointTick = kTicksPerSecond;
    transition.leftClipId = leftId;
    transition.rightClipId = rightId;
    track->addTransition(transition);

    const auto snapshot = buildTimelineSnapshot(timeline, kTicksPerSecond);

    ASSERT_EQ(snapshot.nodes.size(), 2u);
    const auto& transitionNode = snapshot.nodes[1];
    EXPECT_EQ(transitionNode.kind, RenderNodeKind::Transition);
    EXPECT_EQ(transitionNode.sourceId, leftId);
    EXPECT_EQ(transitionNode.peerSourceId, rightId);
    EXPECT_EQ(transitionNode.transitionType, static_cast<int32_t>(TransitionType::CrossDissolve));
    EXPECT_FLOAT_EQ(transitionNode.transitionProgress, 0.5f);
}

TEST(TimelineSnapshotBuilderTest, CanIncludeTransitionPeerClipsForCompositorLayerGathering)
{
    Timeline timeline;
    auto* track = timeline.addVideoTrack("V1");

    auto left = std::make_unique<VideoClip>("left.mp4");
    left->setTimelineIn(0);
    left->setDuration(kTicksPerSecond);
    const uint64_t leftId = left->id();
    track->addClip(std::move(left));

    auto right = std::make_unique<VideoClip>("right.mp4");
    right->setTimelineIn(kTicksPerSecond);
    right->setDuration(kTicksPerSecond);
    const uint64_t rightId = right->id();
    track->addClip(std::move(right));

    Transition transition;
    transition.type = TransitionType::CrossDissolve;
    transition.duration = kTicksPerSecond / 2;
    transition.editPointTick = kTicksPerSecond;
    transition.leftClipId = leftId;
    transition.rightClipId = rightId;
    track->addTransition(transition);

    TimelineSnapshotBuildOptions options;
    options.includeTransitionPeerClips = true;
    const auto snapshot = buildTimelineSnapshot(timeline, kTicksPerSecond, options);

    ASSERT_EQ(snapshot.nodes.size(), 3u);
    EXPECT_EQ(snapshot.nodes[0].stableId, rightId);
    EXPECT_EQ(snapshot.nodes[0].kind, RenderNodeKind::Media);
    EXPECT_EQ(snapshot.nodes[1].stableId, leftId);
    EXPECT_EQ(snapshot.nodes[1].kind, RenderNodeKind::Media);
    EXPECT_EQ(snapshot.nodes[2].kind, RenderNodeKind::Transition);
    EXPECT_EQ(snapshot.nodes[2].sourceId, leftId);
    EXPECT_EQ(snapshot.nodes[2].peerSourceId, rightId);
}

TEST(TimelineSnapshotBuilderTest, ClassifiesNonMediaRenderNodesAndFlagsNeeds)
{
    Timeline timeline;
    auto* videoTrack = timeline.addVideoTrack("V1");
    auto* audioTrack = timeline.addAudioTrack("A1");

    auto adjustment = std::make_unique<AdjustmentClip>();
    adjustment->setDuration(kTicksPerSecond);
    adjustment->addMask({});
    const uint64_t adjustmentId = adjustment->id();
    videoTrack->addClip(std::move(adjustment));

    auto graphic = std::make_unique<GraphicClip>();
    graphic->setTimelineIn(kTicksPerSecond);
    graphic->setDuration(kTicksPerSecond);
    videoTrack->addClip(std::move(graphic));

    auto sequence = std::make_unique<SequenceClip>();
    sequence->setTimelineIn(kTicksPerSecond * 2);
    sequence->setDuration(kTicksPerSecond);
    sequence->setSequenceIndex(3);
    sequence->setSequenceName("Nested");
    videoTrack->addClip(std::move(sequence));

    auto audio = std::make_unique<AudioClip>("dialog.wav");
    audio->setDuration(kTicksPerSecond);
    audio->setMediaId(123);
    const uint64_t audioId = audio->id();
    audioTrack->addClip(std::move(audio));

    const auto firstSnapshot = buildTimelineSnapshot(timeline, 0);
    ASSERT_EQ(firstSnapshot.nodes.size(), 2u);
    EXPECT_EQ(firstSnapshot.nodes[0].kind, RenderNodeKind::Audio);
    EXPECT_EQ(firstSnapshot.nodes[0].stableId, audioId);
    EXPECT_TRUE(firstSnapshot.nodes[0].audioNeeded);
    EXPECT_EQ(firstSnapshot.nodes[0].sourceId, 123u);
    EXPECT_EQ(firstSnapshot.nodes[1].kind, RenderNodeKind::Adjustment);
    EXPECT_EQ(firstSnapshot.nodes[1].stableId, adjustmentId);
    EXPECT_TRUE(firstSnapshot.nodes[1].hasMasks);

    const auto graphicSnapshot = buildTimelineSnapshot(timeline, kTicksPerSecond);
    ASSERT_EQ(graphicSnapshot.nodes.size(), 1u);
    EXPECT_EQ(graphicSnapshot.nodes[0].kind, RenderNodeKind::Graphic);

    const auto nestedSnapshot = buildTimelineSnapshot(timeline, kTicksPerSecond * 2);
    ASSERT_EQ(nestedSnapshot.nodes.size(), 1u);
    EXPECT_EQ(nestedSnapshot.nodes[0].kind, RenderNodeKind::NestedSequence);
    EXPECT_EQ(nestedSnapshot.nodes[0].sourceId, 4u);
    EXPECT_EQ(nestedSnapshot.nodes[0].sourcePath, "Nested");
}

TEST(TimelineSnapshotBuilderTest, CapturesRenderGraphInputsForMediaGraphicsAndCharacters)
{
    Timeline timeline;
    auto* videoTrack = timeline.addVideoTrack("V1");
    videoTrack->setVolume(0.75f);
    videoTrack->setPan(-0.25f);

    auto characterVideo = std::make_unique<VideoClip>("characters/hero_talk.mov");
    characterVideo->setTimelineIn(0);
    characterVideo->setSourceIn(100);
    characterVideo->setDuration(kTicksPerSecond);
    characterVideo->setMediaId(88);
    characterVideo->setSourceResolution(1920, 1080);
    characterVideo->setSourceDuration(kTicksPerSecond * 10);
    characterVideo->setHasAudio(true);
    characterVideo->setVolume(0.5f);
    characterVideo->setCrop(1.0f, 2.0f, 3.0f, 4.0f);
    characterVideo->setCharacterName("Hero");
    characterVideo->setOutfit("jacket");
    characterVideo->setAnimationName("talk");
    characterVideo->setSpeed(2.0);
    characterVideo->setMaintainPitch(false);
    characterVideo->setLabel("Hero Talk");
    characterVideo->setShotName("Opening");
    characterVideo->setLayerId("char_0");
    characterVideo->setGroupId(77);
    characterVideo->opacity().writeValue(0, 0.6f);
    characterVideo->positionX().writeValue(0, 42.0f);
    characterVideo->positionY().writeValue(0, -24.0f);
    characterVideo->scaleX().writeValue(0, 1.25f);
    characterVideo->scaleY().writeValue(0, 0.9f);
    characterVideo->rotation().writeValue(0, 12.0f);
    characterVideo->effects().addEffect(std::make_unique<Blur>());
    videoTrack->addClip(std::move(characterVideo));

    const auto mediaSnapshot = buildTimelineSnapshot(timeline, kTicksPerSecond / 4);
    ASSERT_EQ(mediaSnapshot.nodes.size(), 1u);
    const auto& mediaNode = mediaSnapshot.nodes[0];
    EXPECT_EQ(mediaNode.kind, RenderNodeKind::Character);
    EXPECT_EQ(mediaNode.sourceId, 88u);
    EXPECT_EQ(mediaNode.sourcePath, "characters/hero_talk.mov");
    EXPECT_EQ(mediaNode.sourceWidth, 1920u);
    EXPECT_EQ(mediaNode.sourceHeight, 1080u);
    EXPECT_EQ(mediaNode.localTick, kTicksPerSecond / 4);
    EXPECT_EQ(mediaNode.sourceTick, 100 + kTicksPerSecond / 2);
    EXPECT_DOUBLE_EQ(mediaNode.playbackSpeed, 2.0);
    EXPECT_DOUBLE_EQ(mediaNode.effectiveSpeed, 2.0);
    EXPECT_FALSE(mediaNode.maintainPitch);
    EXPECT_FLOAT_EQ(mediaNode.opacity, 0.6f);
    EXPECT_FLOAT_EQ(mediaNode.positionX, 42.0f);
    EXPECT_FLOAT_EQ(mediaNode.positionY, -24.0f);
    EXPECT_FLOAT_EQ(mediaNode.scaleX, 1.25f);
    EXPECT_FLOAT_EQ(mediaNode.scaleY, 0.9f);
    EXPECT_FLOAT_EQ(mediaNode.rotation, 12.0f);
    EXPECT_FLOAT_EQ(mediaNode.cropLeft, 1.0f);
    EXPECT_FLOAT_EQ(mediaNode.cropRight, 2.0f);
    EXPECT_FLOAT_EQ(mediaNode.cropTop, 3.0f);
    EXPECT_FLOAT_EQ(mediaNode.cropBottom, 4.0f);
    EXPECT_FLOAT_EQ(mediaNode.clipVolume, 0.5f);
    EXPECT_FLOAT_EQ(mediaNode.trackVolume, 0.75f);
    EXPECT_FLOAT_EQ(mediaNode.trackPan, -0.25f);
    EXPECT_EQ(mediaNode.effectCount, 1u);
    EXPECT_TRUE(mediaNode.hasActiveEffects);
    EXPECT_TRUE(mediaNode.audioNeeded);
    EXPECT_TRUE(mediaNode.characterSource);
    EXPECT_EQ(mediaNode.groupId, 77u);
    EXPECT_EQ(mediaNode.label, "Hero Talk");
    EXPECT_EQ(mediaNode.shotName, "Opening");
    EXPECT_EQ(mediaNode.layerId, "char_0");

    auto* graphicsTrack = timeline.addVideoTrack("Graphics");
    auto graphic = std::make_unique<GraphicClip>();
    graphic->setTimelineIn(kTicksPerSecond);
    graphic->setDuration(kTicksPerSecond);
    graphic->addTextLayer("Lower third");
    graphic->addShapeLayer();
    graphicsTrack->addClip(std::move(graphic));

    const auto graphicSnapshot = buildTimelineSnapshot(timeline, kTicksPerSecond);
    ASSERT_EQ(graphicSnapshot.nodes.size(), 1u);
    EXPECT_EQ(graphicSnapshot.nodes[0].kind, RenderNodeKind::Graphic);
    EXPECT_EQ(graphicSnapshot.nodes[0].graphicLayerCount, 2u);

    auto* stillTrack = timeline.addVideoTrack("Still");
    auto image = std::make_unique<ImageClip>("plate.png");
    image->setTimelineIn(kTicksPerSecond * 2);
    image->setDuration(kTicksPerSecond);
    image->setSourceResolution(3840, 2160);
    image->setCrop(5.0f, 6.0f, 7.0f, 8.0f);
    stillTrack->addClip(std::move(image));

    const auto imageSnapshot = buildTimelineSnapshot(timeline, kTicksPerSecond * 2);
    ASSERT_EQ(imageSnapshot.nodes.size(), 1u);
    EXPECT_EQ(imageSnapshot.nodes[0].kind, RenderNodeKind::Media);
    EXPECT_TRUE(imageSnapshot.nodes[0].stillSource);
    EXPECT_EQ(imageSnapshot.nodes[0].sourceWidth, 3840u);
    EXPECT_EQ(imageSnapshot.nodes[0].sourceHeight, 2160u);
    EXPECT_FLOAT_EQ(imageSnapshot.nodes[0].cropBottom, 8.0f);

    auto* spineTrack = timeline.addVideoTrack("Character");
    auto spine = std::make_unique<SpineClip>("Riley", "default");
    spine->setTimelineIn(kTicksPerSecond * 3);
    spine->setDuration(kTicksPerSecond);
    spine->setAnimationName("idle");
    spine->setLooping(true);
    spine->setCrop(0.1f, 0.2f, 0.3f, 0.4f);
    spineTrack->addClip(std::move(spine));

    const auto spineSnapshot = buildTimelineSnapshot(timeline, kTicksPerSecond * 3);
    ASSERT_EQ(spineSnapshot.nodes.size(), 1u);
    EXPECT_EQ(spineSnapshot.nodes[0].kind, RenderNodeKind::Character);
    EXPECT_EQ(spineSnapshot.nodes[0].sourcePath, "Riley|default|idle");
    EXPECT_TRUE(spineSnapshot.nodes[0].looping);
    EXPECT_TRUE(spineSnapshot.nodes[0].characterSource);
    EXPECT_FLOAT_EQ(spineSnapshot.nodes[0].cropTop, 0.3f);
}

TEST(TimelineSnapshotBuilderTest, BuildsDeterministicRenderGraphResourceRequests)
{
    Timeline timeline;
    auto* videoTrack = timeline.addVideoTrack("V1");
    auto* audioTrack = timeline.addAudioTrack("A1");

    auto video = std::make_unique<VideoClip>("clip.mp4");
    video->setDuration(kTicksPerSecond);
    video->setMediaId(44);
    video->setSourceResolution(1280, 720);
    video->effects().addEffect(std::make_unique<Blur>());
    video->addMask({});
    const uint64_t videoId = video->id();
    videoTrack->addClip(std::move(video));

    auto audio = std::make_unique<AudioClip>("dialog.wav");
    audio->setDuration(kTicksPerSecond);
    audio->setMediaId(55);
    const uint64_t audioId = audio->id();
    audioTrack->addClip(std::move(audio));

    const auto snapshot = buildTimelineSnapshot(timeline, kTicksPerSecond / 2);
    const auto graph = buildRenderGraph(snapshot, {
        RenderRequestType::Scrub,
        RenderQuality::Half,
        RenderExactness::ExactRequired
    });

    ASSERT_EQ(graph.nodes.size(), 2u);
    ASSERT_EQ(graph.resources.size(), 4u);
    EXPECT_EQ(graph.evaluationTick, kTicksPerSecond / 2);

    EXPECT_EQ(graph.nodes[0].nodeId, audioId);
    EXPECT_EQ(graph.nodes[0].kind, RenderNodeKind::Audio);
    EXPECT_EQ(graph.nodes[0].firstResource, 0u);
    EXPECT_EQ(graph.nodes[0].resourceCount, 1u);
    EXPECT_EQ(graph.resources[0].kind, RenderResourceKind::AudioSamples);
    EXPECT_EQ(graph.resources[0].sourceId, 55u);
    EXPECT_EQ(graph.resources[0].requestType, RenderRequestType::Scrub);
    EXPECT_EQ(graph.resources[0].quality, RenderQuality::Half);
    EXPECT_EQ(graph.resources[0].exactness, RenderExactness::ExactRequired);

    EXPECT_EQ(graph.nodes[1].nodeId, videoId);
    EXPECT_EQ(graph.nodes[1].kind, RenderNodeKind::Media);
    EXPECT_EQ(graph.nodes[1].firstResource, 1u);
    EXPECT_EQ(graph.nodes[1].resourceCount, 3u);
    EXPECT_EQ(graph.resources[1].kind, RenderResourceKind::MediaFrame);
    EXPECT_EQ(graph.resources[1].sourceId, 44u);
    EXPECT_EQ(graph.resources[1].sourceWidth, 1280u);
    EXPECT_EQ(graph.resources[1].sourceHeight, 720u);
    EXPECT_EQ(graph.resources[1].sourcePath, "clip.mp4");
    EXPECT_EQ(graph.resources[2].kind, RenderResourceKind::Mask);
    EXPECT_EQ(graph.resources[2].maskCount, 1u);
    EXPECT_EQ(graph.resources[3].kind, RenderResourceKind::Effect);
    EXPECT_EQ(graph.resources[3].effectCount, 1u);
}

} // namespace
} // namespace rt