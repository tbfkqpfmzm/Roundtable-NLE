/*
 * AudioSyncTransport.cpp - Clip transport controls for AudioSync.
 * Split from AudioSyncPlayback.cpp for maintainability.
 */

#include "panels/audio/AudioSync.h"

#include "media/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"

#include <spdlog/spdlog.h>

namespace rt {

void AudioSync::stopPlayback()
{
    spdlog::info("AudioSync::stopPlayback: playingClip={}", m_playingClipIdx);

    if (m_audioEngine) {
        m_audioEngine->pause();
        m_audioEngine->setPlaybackSpeed(1.0);
        m_audioEngine->clearTrackSources();
        m_audioEngine->resetStretchers();

        // Restore the timeline's sync clock
        if (m_savedSyncClock) {
            m_audioEngine->setSyncClock(m_savedSyncClock);
            m_savedSyncClock = nullptr;
        }
    }

    if (m_playheadTimer)
        m_playheadTimer->stop();

    // Keep playhead visible at last position (don't hide it)
    m_playingClipIdx = -1;
    updateTransportBar();
}

void AudioSync::togglePlayClip(size_t clipIdx)
{
    if (m_playingClipIdx == static_cast<int>(clipIdx) &&
        m_audioEngine && m_audioEngine->transportState() == TransportState::Playing) {
        pausePlayback();
    } else if (m_playingClipIdx == static_cast<int>(clipIdx) &&
               m_audioEngine && m_audioEngine->transportState() == TransportState::Paused) {
        // Resume from paused
        m_audioEngine->play();
        if (m_playheadTimer) m_playheadTimer->start();
    } else {
        playClip(clipIdx);
    }
}

void AudioSync::pausePlayback()
{
    if (m_audioEngine)
        m_audioEngine->pause();

    if (m_playheadTimer)
        m_playheadTimer->stop();
}

void AudioSync::seekPlayingClip(double timeSec)
{
    if (m_playingClipIdx < 0 || static_cast<size_t>(m_playingClipIdx) >= m_clips.size())
        return;

    const auto& clip = m_clips[static_cast<size_t>(m_playingClipIdx)];

    // Use the effective playback rate (engine rate after resampling)
    double sr = m_playbackSourceRate;

    // If timeSec falls inside a deleted region, snap to end of that region
    for (const auto& [delStart, delEnd] : clip.deletedRegions) {
        if (timeSec >= delStart && timeSec < delEnd) {
            timeSec = delEnd;
            break;
        }
    }

    // Find the buffer frame that corresponds to timeSec using the time map
    int64_t targetFrame = 0;
    for (size_t i = 0; i < m_playbackTimeMap.size(); ++i) {
        auto [segStart, segTime] = m_playbackTimeMap[i];
        int64_t segEnd = (i + 1 < m_playbackTimeMap.size())
            ? m_playbackTimeMap[i + 1].first
            : static_cast<int64_t>(m_playbackBuffer.size());
        double segDuration = static_cast<double>(segEnd - segStart) / sr;
        if (timeSec >= segTime && timeSec < segTime + segDuration) {
            targetFrame = segStart + static_cast<int64_t>((timeSec - segTime) * sr);
            break;
        }
        if (timeSec < segTime) {
            targetFrame = segStart;
            break;
        }
        targetFrame = segEnd;
    }

    m_audioEngine->seekToFrame(targetFrame);

    // Update waveform playhead
    if (auto* wf = waveformForClip(m_playingClipIdx)) {
        wf->setPlayhead(timeSec);
        wf->setPlayheadVisible(true);
    }
}

} // namespace rt
