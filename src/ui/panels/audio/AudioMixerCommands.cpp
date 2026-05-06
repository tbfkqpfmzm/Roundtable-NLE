/*
 * AudioMixerCommands.cpp - Mixer commands and channel value conversions.
 */

#include "AudioMixer.h"

#include "command/Command.h"
#include "timeline/Track.h"

#include <algorithm>
#include <cmath>

namespace rt {

static constexpr int kCmdIdSetVolume = 2500;
static constexpr int kCmdIdSetPan    = 2501;

void SetTrackVolumeCommand::execute() { m_track->setVolume(m_newVolume); }
void SetTrackVolumeCommand::undo()    { m_track->setVolume(m_oldVolume); }

std::string SetTrackVolumeCommand::description() const { return "Set Track Volume"; }
int SetTrackVolumeCommand::typeId() const { return kCmdIdSetVolume; }

bool SetTrackVolumeCommand::mergeWith(const Command& next)
{
    if (next.typeId() != kCmdIdSetVolume) return false;
    auto& other = static_cast<const SetTrackVolumeCommand&>(next);
    if (other.m_track != m_track) return false;
    m_newVolume = other.m_newVolume;
    return true;
}

void SetTrackPanCommand::execute() { m_track->setPan(m_newPan); }
void SetTrackPanCommand::undo()    { m_track->setPan(m_oldPan); }

std::string SetTrackPanCommand::description() const { return "Set Track Pan"; }
int SetTrackPanCommand::typeId() const { return kCmdIdSetPan; }

bool SetTrackPanCommand::mergeWith(const Command& next)
{
    if (next.typeId() != kCmdIdSetPan) return false;
    auto& other = static_cast<const SetTrackPanCommand&>(next);
    if (other.m_track != m_track) return false;
    m_newPan = other.m_newPan;
    return true;
}

void SetTrackMuteCommand::execute() { m_track->setMuted(m_muted); }
void SetTrackMuteCommand::undo()    { m_track->setMuted(!m_muted); }

std::string SetTrackMuteCommand::description() const
{
    return m_muted ? "Mute Track" : "Unmute Track";
}

void SetTrackSoloCommand::execute() { m_track->setSoloed(m_soloed); }
void SetTrackSoloCommand::undo()    { m_track->setSoloed(!m_soloed); }

std::string SetTrackSoloCommand::description() const
{
    return m_soloed ? "Solo Track" : "Unsolo Track";
}

float ChannelStrip::faderToVolume(int faderValue) noexcept
{
    if (faderValue <= 0) return 0.0f;

    constexpr float maxDb  =  6.0f;
    constexpr float minDb  = -60.0f;
    constexpr float range  = maxDb - minDb;
    constexpr int   maxVal = 1000;

    const float normalized = static_cast<float>(faderValue) / static_cast<float>(maxVal);
    const float db = minDb + normalized * range;
    return std::pow(10.0f, db / 20.0f);
}

int ChannelStrip::volumeToFader(float volume) noexcept
{
    if (volume <= 0.0f) return 0;

    constexpr float maxDb  =  6.0f;
    constexpr float minDb  = -60.0f;
    constexpr float range  = maxDb - minDb;
    constexpr int   maxVal = 1000;

    const float db = 20.0f * std::log10(std::max(volume, 1e-10f));
    const float normalized = (db - minDb) / range;
    return std::clamp(static_cast<int>(normalized * maxVal), 0, maxVal);
}

QString ChannelStrip::volumeToDbString(float volume)
{
    if (volume <= 0.0f) return QStringLiteral("-inf dB");
    const float db = 20.0f * std::log10(std::max(volume, 1e-10f));
    return QString::number(static_cast<double>(db), 'f', 1) + QStringLiteral(" dB");
}

float ChannelStrip::dialToPan(int dialValue) noexcept
{
    return std::clamp(static_cast<float>(dialValue) / 100.0f, -1.0f, 1.0f);
}

int ChannelStrip::panToDial(float pan) noexcept
{
    return std::clamp(static_cast<int>(std::round(pan * 100.0f)), -100, 100);
}

QString ChannelStrip::panToString(float pan)
{
    if (std::abs(pan) < 0.01f) return QStringLiteral("C");
    if (pan < 0.0f)
        return QStringLiteral("L") + QString::number(static_cast<int>(std::abs(pan) * 100));
    return QStringLiteral("R") + QString::number(static_cast<int>(pan * 100));
}

} // namespace rt
