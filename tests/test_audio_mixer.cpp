/*
 * test_audio_mixer.cpp — Unit tests for Step 25: Audio Mixer Panel
 *
 * Tests:
 *   - ChannelStrip static conversion utilities (fader↔volume, dial↔pan)
 *   - Track volume/pan model fields
 *   - Mixer commands (SetTrackVolumeCommand, SetTrackPanCommand, mute/solo)
 *   - Command undo/redo integration
 *   - Command merge (continuous fader drags collapse into one undo step)
 *   - AudioMixer panel strip creation and rebuild
 *   - Strip sync with track model
 *   - Master strip existence
 */

#include <gtest/gtest.h>

#include "panels/audio/AudioMixer.h"

#include "command/Command.h"
#include "command/CommandStack.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <QApplication>

#include <cmath>
#include <memory>

// ═════════════════════════════════════════════════════════════════════════════
// QApplication fixture
// ═════════════════════════════════════════════════════════════════════════════

namespace {

int    g_argc = 1;
char   g_arg0[] = "test_audio_mixer";
char*  g_argv[] = {g_arg0, nullptr};

class AudioMixerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!QApplication::instance()) {
            m_app = std::make_unique<QApplication>(g_argc, g_argv);
        }
    }

    std::unique_ptr<QApplication> m_app;
};

} // anonymous namespace

using namespace rt;

// ═════════════════════════════════════════════════════════════════════════════
// ChannelStrip conversion utilities
// ═════════════════════════════════════════════════════════════════════════════

TEST(ChannelStripConversion, FaderToVolumeZeroIsSilent)
{
    EXPECT_FLOAT_EQ(ChannelStrip::faderToVolume(0), 0.0f);
}

TEST(ChannelStripConversion, FaderToVolumeMaxIsAboveUnity)
{
    float vol = ChannelStrip::faderToVolume(1000);
    // At fader max (1000), volume should correspond to +6 dB ≈ 1.995
    EXPECT_GT(vol, 1.5f);
    EXPECT_LT(vol, 2.5f);
}

TEST(ChannelStripConversion, FaderToVolumeMonotonic)
{
    float prev = 0.0f;
    for (int i = 1; i <= 1000; i += 10) {
        float vol = ChannelStrip::faderToVolume(i);
        EXPECT_GE(vol, prev) << "Fader value " << i << " should produce >= previous";
        prev = vol;
    }
}

TEST(ChannelStripConversion, VolumeToFaderRoundTrip)
{
    // Round-trip: volume → fader → volume should be close
    for (float vol : {0.001f, 0.01f, 0.1f, 0.5f, 1.0f, 1.5f, 2.0f}) {
        int fader = ChannelStrip::volumeToFader(vol);
        float recovered = ChannelStrip::faderToVolume(fader);
        EXPECT_NEAR(recovered, vol, 0.05f)
            << "Round-trip failed for volume=" << vol << " fader=" << fader;
    }
}

TEST(ChannelStripConversion, VolumeToFaderSilent)
{
    EXPECT_EQ(ChannelStrip::volumeToFader(0.0f), 0);
    EXPECT_EQ(ChannelStrip::volumeToFader(-1.0f), 0);
}

TEST(ChannelStripConversion, VolumeToDbString)
{
    EXPECT_EQ(ChannelStrip::volumeToDbString(0.0f), "-inf dB");
    QString dbStr = ChannelStrip::volumeToDbString(1.0f);
    EXPECT_TRUE(dbStr.contains("0.0")) << dbStr.toStdString();
}

TEST(ChannelStripConversion, DialToPanCenter)
{
    EXPECT_FLOAT_EQ(ChannelStrip::dialToPan(0), 0.0f);
}

TEST(ChannelStripConversion, DialToPanExtremes)
{
    EXPECT_FLOAT_EQ(ChannelStrip::dialToPan(-100), -1.0f);
    EXPECT_FLOAT_EQ(ChannelStrip::dialToPan(100), 1.0f);
}

TEST(ChannelStripConversion, PanToDialRoundTrip)
{
    for (float pan : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        int dial = ChannelStrip::panToDial(pan);
        float recovered = ChannelStrip::dialToPan(dial);
        EXPECT_NEAR(recovered, pan, 0.02f);
    }
}

TEST(ChannelStripConversion, PanToStringCenter)
{
    EXPECT_EQ(ChannelStrip::panToString(0.0f), "C");
}

TEST(ChannelStripConversion, PanToStringLeft)
{
    QString s = ChannelStrip::panToString(-0.5f);
    EXPECT_TRUE(s.startsWith("L")) << s.toStdString();
}

TEST(ChannelStripConversion, PanToStringRight)
{
    QString s = ChannelStrip::panToString(0.75f);
    EXPECT_TRUE(s.startsWith("R")) << s.toStdString();
}

// ═════════════════════════════════════════════════════════════════════════════
// Track volume/pan model
// ═════════════════════════════════════════════════════════════════════════════

TEST(TrackAudioModel, DefaultVolume)
{
    Track t(TrackType::Audio, "A1");
    EXPECT_FLOAT_EQ(t.volume(), 1.0f);
}

TEST(TrackAudioModel, DefaultPan)
{
    Track t(TrackType::Audio, "A1");
    EXPECT_FLOAT_EQ(t.pan(), 0.0f);
}

TEST(TrackAudioModel, SetVolume)
{
    Track t(TrackType::Audio, "A1");
    t.setVolume(0.5f);
    EXPECT_FLOAT_EQ(t.volume(), 0.5f);
}

TEST(TrackAudioModel, SetPan)
{
    Track t(TrackType::Audio, "A1");
    t.setPan(-0.7f);
    EXPECT_FLOAT_EQ(t.pan(), -0.7f);
}

TEST(TrackAudioModel, VolumeBoost)
{
    Track t(TrackType::Audio, "A1");
    t.setVolume(2.0f);
    EXPECT_FLOAT_EQ(t.volume(), 2.0f);
}

TEST(TrackAudioModel, VideoTrackAlsoHasVolume)
{
    Track t(TrackType::Video, "V1");
    EXPECT_FLOAT_EQ(t.volume(), 1.0f);
    t.setVolume(0.75f);
    EXPECT_FLOAT_EQ(t.volume(), 0.75f);
}

// ═════════════════════════════════════════════════════════════════════════════
// AudioMixer panel — strip creation
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AudioMixerTest, ConstructWithoutTimeline)
{
    AudioMixer mixer;
    EXPECT_EQ(mixer.stripCount(), 0u);
}

TEST_F(AudioMixerTest, MasterStripExists)
{
    AudioMixer mixer;
    mixer.rebuildStrips();
    EXPECT_TRUE(mixer.masterStrip().isMaster);
    EXPECT_NE(mixer.masterStrip().vuMeter, nullptr);
    EXPECT_NE(mixer.masterStrip().fader, nullptr);
}

TEST_F(AudioMixerTest, StripPerAudioTrack)
{
    Timeline tl;
    tl.addVideoTrack("V1");
    tl.addAudioTrack("A1");
    tl.addAudioTrack("A2");
    tl.addVideoTrack("V2");
    tl.addAudioTrack("A3");

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    // Should have 3 audio strips (one per audio track)
    EXPECT_EQ(mixer.stripCount(), 3u);
}

TEST_F(AudioMixerTest, StripTrackIndexCorrect)
{
    Timeline tl;
    tl.addVideoTrack("V1");   // index 0
    tl.addAudioTrack("A1");   // index 1
    tl.addAudioTrack("A2");   // index 2

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 2u);
    EXPECT_EQ(mixer.strip(0).trackIndex, 1u);  // first audio is timeline track 1
    EXPECT_EQ(mixer.strip(1).trackIndex, 2u);  // second audio is timeline track 2
}

TEST_F(AudioMixerTest, StripHasCorrectWidgets)
{
    Timeline tl;
    tl.addAudioTrack("A1");

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    const auto& s = mixer.strip(0);
    EXPECT_NE(s.nameLabel, nullptr);
    EXPECT_NE(s.vuMeter, nullptr);
    EXPECT_NE(s.fader, nullptr);
    EXPECT_NE(s.faderLabel, nullptr);
    EXPECT_NE(s.panDial, nullptr);
    EXPECT_NE(s.panLabel, nullptr);
    EXPECT_NE(s.muteButton, nullptr);
    EXPECT_NE(s.soloButton, nullptr);
}

TEST_F(AudioMixerTest, StripNameFromTrack)
{
    Timeline tl;
    tl.addAudioTrack("BGM");

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    EXPECT_EQ(mixer.strip(0).nameLabel->text(), "BGM");
}

TEST_F(AudioMixerTest, NoAudioTracksNoStrips)
{
    Timeline tl;
    tl.addVideoTrack("V1");
    tl.addVideoTrack("V2");

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    EXPECT_EQ(mixer.stripCount(), 0u);
}

TEST_F(AudioMixerTest, RebuildClearsOldStrips)
{
    Timeline tl;
    tl.addAudioTrack("A1");
    tl.addAudioTrack("A2");

    AudioMixer mixer;
    mixer.setTimeline(&tl);
    EXPECT_EQ(mixer.stripCount(), 2u);

    // Remove an audio track and rebuild
    tl.removeTrack(1);
    mixer.rebuildStrips();
    EXPECT_EQ(mixer.stripCount(), 1u);
}

TEST_F(AudioMixerTest, StripForTrackLookup)
{
    Timeline tl;
    tl.addVideoTrack("V1");   // index 0
    tl.addAudioTrack("A1");   // index 1

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    EXPECT_NE(mixer.stripForTrack(1), nullptr);
    EXPECT_EQ(mixer.stripForTrack(0), nullptr);   // video track
    EXPECT_EQ(mixer.stripForTrack(99), nullptr);   // nonexistent
}

// ═════════════════════════════════════════════════════════════════════════════
// Fader / pan widget defaults
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AudioMixerTest, FaderDefaultMatchesUnityVolume)
{
    Timeline tl;
    tl.addAudioTrack("A1");

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    int faderVal = mixer.strip(0).fader->value();
    float vol = ChannelStrip::faderToVolume(faderVal);
    // Unity = 1.0, tolerance for discretization
    EXPECT_NEAR(vol, 1.0f, 0.1f);
}

TEST_F(AudioMixerTest, PanDialDefaultIsCenter)
{
    Timeline tl;
    tl.addAudioTrack("A1");

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    EXPECT_EQ(mixer.strip(0).panDial->value(), 0);
}

TEST_F(AudioMixerTest, TrackVolumeAffectsFader)
{
    Timeline tl;
    Track* t = tl.addAudioTrack("A1");
    t->setVolume(0.5f);

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    int faderVal = mixer.strip(0).fader->value();
    float vol = ChannelStrip::faderToVolume(faderVal);
    EXPECT_NEAR(vol, 0.5f, 0.1f);
}

TEST_F(AudioMixerTest, TrackPanAffectsDial)
{
    Timeline tl;
    Track* t = tl.addAudioTrack("A1");
    t->setPan(0.5f);

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    EXPECT_EQ(mixer.strip(0).panDial->value(), 50);
}

// ═════════════════════════════════════════════════════════════════════════════
// Command system integration
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AudioMixerTest, VolumeCommandUndoRedo)
{
    Track t(TrackType::Audio, "A1");
    t.setVolume(1.0f);
    CommandStack stack;

    // Set volume to 0.5
    stack.execute(std::make_unique<SetTrackVolumeCommand>(&t, 1.0f, 0.5f));
    EXPECT_FLOAT_EQ(t.volume(), 0.5f);

    // Undo
    stack.undo();
    EXPECT_FLOAT_EQ(t.volume(), 1.0f);

    // Redo
    stack.redo();
    EXPECT_FLOAT_EQ(t.volume(), 0.5f);
}

TEST_F(AudioMixerTest, PanCommandUndoRedo)
{
    Track t(TrackType::Audio, "A1");
    t.setPan(0.0f);
    CommandStack stack;

    stack.execute(std::make_unique<SetTrackPanCommand>(&t, 0.0f, -0.75f));
    EXPECT_FLOAT_EQ(t.pan(), -0.75f);

    stack.undo();
    EXPECT_FLOAT_EQ(t.pan(), 0.0f);

    stack.redo();
    EXPECT_FLOAT_EQ(t.pan(), -0.75f);
}

TEST_F(AudioMixerTest, MuteCommandUndoRedo)
{
    Track t(TrackType::Audio, "A1");
    EXPECT_FALSE(t.isMuted());
    CommandStack stack;

    stack.execute(std::make_unique<SetTrackMuteCommand>(&t, true));
    EXPECT_TRUE(t.isMuted());

    stack.undo();
    EXPECT_FALSE(t.isMuted());

    stack.redo();
    EXPECT_TRUE(t.isMuted());
}

TEST_F(AudioMixerTest, SoloCommandUndoRedo)
{
    Track t(TrackType::Audio, "A1");
    EXPECT_FALSE(t.isSoloed());
    CommandStack stack;

    stack.execute(std::make_unique<SetTrackSoloCommand>(&t, true));
    EXPECT_TRUE(t.isSoloed());

    stack.undo();
    EXPECT_FALSE(t.isSoloed());
}

TEST_F(AudioMixerTest, VolumeCommandMerge)
{
    Track t(TrackType::Audio, "A1");
    t.setVolume(1.0f);
    CommandStack stack;

    // Simulate dragging fader: multiple small volume changes
    stack.execute(std::make_unique<SetTrackVolumeCommand>(&t, 1.0f, 0.9f));
    stack.execute(std::make_unique<SetTrackVolumeCommand>(&t, 0.9f, 0.7f));
    stack.execute(std::make_unique<SetTrackVolumeCommand>(&t, 0.7f, 0.5f));

    EXPECT_FLOAT_EQ(t.volume(), 0.5f);

    // All merged into one command — single undo goes back to 1.0
    EXPECT_EQ(stack.undoCount(), 1u);
    stack.undo();
    EXPECT_FLOAT_EQ(t.volume(), 1.0f);
}

TEST_F(AudioMixerTest, PanCommandMerge)
{
    Track t(TrackType::Audio, "A1");
    t.setPan(0.0f);
    CommandStack stack;

    stack.execute(std::make_unique<SetTrackPanCommand>(&t, 0.0f, 0.1f));
    stack.execute(std::make_unique<SetTrackPanCommand>(&t, 0.1f, 0.3f));
    stack.execute(std::make_unique<SetTrackPanCommand>(&t, 0.3f, 0.5f));

    EXPECT_FLOAT_EQ(t.pan(), 0.5f);
    EXPECT_EQ(stack.undoCount(), 1u);
    stack.undo();
    EXPECT_FLOAT_EQ(t.pan(), 0.0f);
}

TEST_F(AudioMixerTest, DifferentTracksDontMerge)
{
    Track t1(TrackType::Audio, "A1");
    Track t2(TrackType::Audio, "A2");
    CommandStack stack;

    stack.execute(std::make_unique<SetTrackVolumeCommand>(&t1, 1.0f, 0.5f));
    stack.execute(std::make_unique<SetTrackVolumeCommand>(&t2, 1.0f, 0.3f));

    // Different tracks — should NOT merge
    EXPECT_EQ(stack.undoCount(), 2u);
}

TEST_F(AudioMixerTest, VolumeAndPanDontMerge)
{
    Track t(TrackType::Audio, "A1");
    CommandStack stack;

    stack.execute(std::make_unique<SetTrackVolumeCommand>(&t, 1.0f, 0.5f));
    stack.execute(std::make_unique<SetTrackPanCommand>(&t, 0.0f, 0.5f));

    // Different command types — should NOT merge
    EXPECT_EQ(stack.undoCount(), 2u);
}

// ═════════════════════════════════════════════════════════════════════════════
// Command with stack integration in mixer
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AudioMixerTest, CommandStackIntegration)
{
    Timeline tl;
    tl.addAudioTrack("A1");

    CommandStack stack;
    AudioMixer mixer;
    mixer.setTimeline(&tl);
    mixer.setCommandStack(&stack);

    EXPECT_EQ(stack.undoCount(), 0u);
}

TEST_F(AudioMixerTest, MuteButtonCheckable)
{
    Timeline tl;
    tl.addAudioTrack("A1");

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    EXPECT_TRUE(mixer.strip(0).muteButton->isCheckable());
    EXPECT_FALSE(mixer.strip(0).muteButton->isChecked());
}

TEST_F(AudioMixerTest, SoloButtonCheckable)
{
    Timeline tl;
    tl.addAudioTrack("A1");

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    EXPECT_TRUE(mixer.strip(0).soloButton->isCheckable());
    EXPECT_FALSE(mixer.strip(0).soloButton->isChecked());
}

TEST_F(AudioMixerTest, MuteInitialStateFromTrack)
{
    Timeline tl;
    Track* t = tl.addAudioTrack("A1");
    t->setMuted(true);

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    EXPECT_TRUE(mixer.strip(0).muteButton->isChecked());
}

TEST_F(AudioMixerTest, SoloInitialStateFromTrack)
{
    Timeline tl;
    Track* t = tl.addAudioTrack("A1");
    t->setSoloed(true);

    AudioMixer mixer;
    mixer.setTimeline(&tl);

    ASSERT_EQ(mixer.stripCount(), 1u);
    EXPECT_TRUE(mixer.strip(0).soloButton->isChecked());
}

// ═════════════════════════════════════════════════════════════════════════════
// Command description
// ═════════════════════════════════════════════════════════════════════════════

TEST(MixerCommandDesc, VolumeDescription)
{
    Track t(TrackType::Audio, "A1");
    SetTrackVolumeCommand cmd(&t, 1.0f, 0.5f);
    EXPECT_EQ(cmd.description(), "Set Track Volume");
}

TEST(MixerCommandDesc, PanDescription)
{
    Track t(TrackType::Audio, "A1");
    SetTrackPanCommand cmd(&t, 0.0f, 0.5f);
    EXPECT_EQ(cmd.description(), "Set Track Pan");
}

TEST(MixerCommandDesc, MuteDescription)
{
    Track t(TrackType::Audio, "A1");
    SetTrackMuteCommand cmd(&t, true);
    EXPECT_EQ(cmd.description(), "Mute Track");
}

TEST(MixerCommandDesc, UnmuteDescription)
{
    Track t(TrackType::Audio, "A1");
    SetTrackMuteCommand cmd(&t, false);
    EXPECT_EQ(cmd.description(), "Unmute Track");
}

TEST(MixerCommandDesc, SoloDescription)
{
    Track t(TrackType::Audio, "A1");
    SetTrackSoloCommand cmd(&t, true);
    EXPECT_EQ(cmd.description(), "Solo Track");
}

// ═════════════════════════════════════════════════════════════════════════════
// Master strip
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(AudioMixerTest, MasterStripHasLabel)
{
    AudioMixer mixer;
    mixer.rebuildStrips();
    EXPECT_EQ(mixer.masterStrip().nameLabel->text(), "MASTER");
}

TEST_F(AudioMixerTest, MasterFaderExists)
{
    AudioMixer mixer;
    mixer.rebuildStrips();
    EXPECT_NE(mixer.masterStrip().fader, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// Edge cases
// ═════════════════════════════════════════════════════════════════════════════

TEST(ChannelStripConversion, FaderClampNegative)
{
    // Negative fader value should still return 0
    EXPECT_FLOAT_EQ(ChannelStrip::faderToVolume(-10), 0.0f);
}

TEST(ChannelStripConversion, PanDialClamp)
{
    // Values beyond range should clamp
    EXPECT_FLOAT_EQ(ChannelStrip::dialToPan(-200), -1.0f);
    EXPECT_FLOAT_EQ(ChannelStrip::dialToPan(200), 1.0f);
}

TEST(ChannelStripConversion, PanToDialClamp)
{
    EXPECT_EQ(ChannelStrip::panToDial(-5.0f), -100);
    EXPECT_EQ(ChannelStrip::panToDial(5.0f), 100);
}
