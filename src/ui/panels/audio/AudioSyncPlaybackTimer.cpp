/*
 * AudioSyncPlaybackTimer.cpp - Playback playhead timer helper.
 */

#include "panels/audio/AudioSync.h"

#include "media/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"

#include <QTimer>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace rt {

void AudioSync::ensureClipPlayheadTimer()
{
    if (m_playheadTimer) return;

    m_playheadTimer = new QTimer(this);
    m_playheadTimer->setInterval(30);
    connect(m_playheadTimer, &QTimer::timeout, this, [this]() {
        if (m_playingClipIdx < 0 || static_cast<size_t>(m_playingClipIdx) >= m_clips.size()) {
            spdlog::info("AudioSync timer: invalid playingClipIdx={} clips.size={}",
                         m_playingClipIdx, m_clips.size());
            stopPlayback();
            return;
        }
        if (!m_audioEngine) {
            spdlog::warn("AudioSync timer: engine null");
            stopPlayback();
            return;
        }

        auto frame = m_audioEngine->currentFrame();
        const auto& pClip = m_clips[static_cast<size_t>(m_playingClipIdx)];

        double currentTime = pClip.end;
        for (size_t i = 0; i < m_playbackTimeMap.size(); ++i) {
            auto [segStart, segTime] = m_playbackTimeMap[i];
            int64_t segEnd = (i + 1 < m_playbackTimeMap.size())
                ? m_playbackTimeMap[i + 1].first
                : static_cast<int64_t>(m_playbackBuffer.size());
            if (frame >= segStart && frame < segEnd) {
                currentTime = segTime + static_cast<double>(frame - segStart) / m_playbackSourceRate;
                break;
            }
        }

        if (currentTime >= pClip.end || m_audioEngine->transportState() != TransportState::Playing) {
            if (auto* wfEnd = waveformForClip(m_playingClipIdx)) {
                wfEnd->setPlayhead(std::min(currentTime, pClip.end));
                wfEnd->setPlayheadVisible(true);
            }
            stopPlayback();
            return;
        }

        if (auto* wf = waveformForClip(m_playingClipIdx)) {
            wf->setPlayhead(currentTime);
            wf->setPlayheadVisible(true);
        }

        updateTransportBar();
    });
}

} // namespace rt
