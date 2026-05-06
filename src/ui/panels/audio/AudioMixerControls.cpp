/*
 * AudioMixerControls.cpp - Mixer fader, pan, mute, solo, and reset events.
 */

#include "AudioMixer.h"

#include "command/Command.h"
#include "command/CommandStack.h"
#include "media/AudioEngine.h"
#include "timeline/Track.h"

#include <QDial>
#include <QEvent>
#include <QPushButton>
#include <QSlider>

namespace rt {

static constexpr int kCmdIdSetVolume = 2500;

class SetMasterVolumeCommand : public Command
{
public:
    SetMasterVolumeCommand(AudioEngine* engine, float oldVol, float newVol)
        : m_engine(engine), m_oldVolume(oldVol), m_newVolume(newVol) {}

    void execute() override { if (m_engine) m_engine->setMasterVolume(m_newVolume); }
    void undo()    override { if (m_engine) m_engine->setMasterVolume(m_oldVolume); }

    [[nodiscard]] std::string description() const override
    {
        return "Set Master Volume";
    }

    [[nodiscard]] int typeId() const override { return kCmdIdSetVolume + 100; }

    [[nodiscard]] bool mergeWith(const Command& next) override
    {
        if (next.typeId() != typeId()) return false;
        auto& other = static_cast<const SetMasterVolumeCommand&>(next);
        m_newVolume = other.m_newVolume;
        return true;
    }

private:
    AudioEngine* m_engine;
    float        m_oldVolume;
    float        m_newVolume;
};

void AudioMixer::onFaderChanged(int value)
{
    auto* slider = qobject_cast<QSlider*>(sender());
    if (!slider) return;

    const auto idx = slider->property("trackIndex").toUInt();
    const float newVol = ChannelStrip::faderToVolume(value);

    if (idx == 9999) {
        const float oldVol = m_audioEngine ? m_audioEngine->masterVolume() : 1.0f;
        if (m_audioEngine) m_audioEngine->setMasterVolume(newVol);
        if (m_masterStrip.faderLabel)
            m_masterStrip.faderLabel->setText(ChannelStrip::volumeToDbString(newVol));

        if (m_commandStack) {
            m_commandStack->execute(
                std::make_unique<SetMasterVolumeCommand>(m_audioEngine, oldVol, newVol)
            );
        }
        return;
    }

    ChannelStrip* strip = stripForTrack(idx);
    if (!strip || !strip->track) return;

    const float oldVol = strip->track->volume();

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<SetTrackVolumeCommand>(strip->track, oldVol, newVol)
        );
    } else {
        strip->track->setVolume(newVol);
    }

    if (strip->faderLabel)
        strip->faderLabel->setText(ChannelStrip::volumeToDbString(newVol));

    emit volumeChanged(idx, newVol);
}

void AudioMixer::onPanChanged(int value)
{
    auto* dial = qobject_cast<QDial*>(sender());
    if (!dial) return;

    const auto idx = dial->property("trackIndex").toUInt();
    const float newPan = ChannelStrip::dialToPan(value);

    ChannelStrip* strip = stripForTrack(idx);
    if (!strip || !strip->track) return;

    const float oldPan = strip->track->pan();

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<SetTrackPanCommand>(strip->track, oldPan, newPan)
        );
    } else {
        strip->track->setPan(newPan);
    }

    if (strip->panLabel)
        strip->panLabel->setText(ChannelStrip::panToString(newPan));

    emit panChanged(idx, newPan);
}

void AudioMixer::onMuteToggled()
{
    auto* btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;

    const auto idx = btn->property("trackIndex").toUInt();

    if (idx == 9999) {
        const bool muted = btn->isChecked();
        if (m_audioEngine) {
            const float curVol = m_audioEngine->masterVolume();
            const float newVol = muted ? 0.0f : 1.0f;
            if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<SetMasterVolumeCommand>(m_audioEngine, curVol, newVol)
                );
            } else {
                m_audioEngine->setMasterVolume(newVol);
            }
            if (m_masterStrip.fader) {
                m_masterStrip.fader->blockSignals(true);
                m_masterStrip.fader->setValue(ChannelStrip::volumeToFader(newVol));
                m_masterStrip.fader->blockSignals(false);
            }
            if (m_masterStrip.faderLabel)
                m_masterStrip.faderLabel->setText(ChannelStrip::volumeToDbString(newVol));
        }
        return;
    }

    ChannelStrip* strip = stripForTrack(idx);
    if (!strip || !strip->track) return;

    const bool newMuted = !strip->track->isMuted();

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<SetTrackMuteCommand>(strip->track, newMuted)
        );
    } else {
        strip->track->setMuted(newMuted);
    }

    btn->setChecked(newMuted);

    emit muteChanged(idx, newMuted);
}

void AudioMixer::onSoloToggled()
{
    auto* btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;

    const auto idx = btn->property("trackIndex").toUInt();

    ChannelStrip* strip = stripForTrack(idx);
    if (!strip || !strip->track) return;

    const bool newSoloed = !strip->track->isSoloed();

    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<SetTrackSoloCommand>(strip->track, newSoloed)
        );
    } else {
        strip->track->setSoloed(newSoloed);
    }

    btn->setChecked(newSoloed);

    emit soloChanged(idx, newSoloed);
}

bool AudioMixer::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick) {
        if (auto* slider = qobject_cast<QSlider*>(obj)) {
            const auto idx = slider->property("trackIndex").toUInt();
            if (idx == 9999) {
                resetFader(m_masterStrip);
            } else {
                if (auto* strip = stripForTrack(idx))
                    resetFader(*strip);
            }
            return true;
        }
        if (auto* dial = qobject_cast<QDial*>(obj)) {
            const auto idx = dial->property("trackIndex").toUInt();
            if (auto* strip = stripForTrack(idx))
                resetPan(*strip);
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace rt
