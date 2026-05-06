/*
 * AudioSyncFilePreview.cpp - Imported audio file preview playback for AudioSync.
 * Split from AudioSyncPlayback.cpp for maintainability.
 */

#include "panels/audio/AudioSync.h"

#include "media/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"

#include <QTimer>

#include <algorithm>

namespace rt {

void AudioSync::playAudioFile(size_t fileIndex)
{
    if (!m_audioEngine || fileIndex >= m_audioPaths.size()) return;

    // If already playing this file, toggle off
    if (m_previewFileIdx == static_cast<int>(fileIndex)) {
        stopFilePlayback();
        return;
    }

    // Stop any clip playback first
    stopPlayback();
    stopFilePlayback();

    const auto& path = m_audioPaths[fileIndex];
    auto it = m_audioSamples.find(path);
    if (it == m_audioSamples.end()) return;

    const auto& audioData = it->second;
    if (audioData.samples.empty()) return;

    m_previewFileIdx = static_cast<int>(fileIndex);

    // Determine start position from waveform playhead (if user clicked to seek)
    int64_t seekFrame = 0;
    for (auto* map : {&m_fileWaveforms, &m_transcribeWaveforms}) {
        auto wfIt = map->find(fileIndex);
        if (wfIt != map->end() && wfIt->second && wfIt->second->isPlayheadVisible()) {
            double ph = wfIt->second->playhead();
            seekFrame = static_cast<int64_t>(ph * audioData.sampleRate);
            seekFrame = std::clamp(seekFrame, int64_t{0},
                                   static_cast<int64_t>(audioData.samples.size()) - 1);
            break;
        }
    }

    // Build a simple track source - startFrame=0 means file starts at timeline origin
    AudioTrackSource src{};
    src.trackId     = 9998;
    src.samples     = audioData.samples.data();
    src.totalFrames = static_cast<int64_t>(audioData.samples.size());
    src.startFrame  = 0;
    src.channels    = 1;
    src.sampleRate  = audioData.sampleRate;
    src.volume      = 1.0f;

    // Detach timeline sync clock during preview
    m_savedSyncClock = nullptr;
    if (m_audioEngine->syncClock()) {
        m_savedSyncClock = m_audioEngine->syncClock();
        m_audioEngine->setSyncClock(nullptr);
    }

    m_audioEngine->setPlaybackSpeed(1.0);
    m_audioEngine->setTrackSources({src});
    m_audioEngine->seekToFrame(seekFrame);
    m_audioEngine->play();

    // Update play button appearance
    updateFilePlayButton(fileIndex, true);

    // Timer to update waveform playhead and detect end
    if (!m_previewTimer) {
        m_previewTimer = new QTimer(this);
        m_previewTimer->setInterval(30);
        connect(m_previewTimer, &QTimer::timeout, this, [this]() {
            if (!m_audioEngine || m_previewFileIdx < 0) {
                stopFilePlayback();
                return;
            }
            auto fidx = static_cast<size_t>(m_previewFileIdx);
            if (fidx >= m_audioPaths.size()) { stopFilePlayback(); return; }

            auto sampIt = m_audioSamples.find(m_audioPaths[fidx]);
            if (sampIt == m_audioSamples.end()) { stopFilePlayback(); return; }

            double sr = sampIt->second.sampleRate;
            int64_t frame = m_audioEngine->currentFrame();
            double currentTime = static_cast<double>(frame) / sr;
            double duration = static_cast<double>(sampIt->second.samples.size()) / sr;

            if (currentTime >= duration ||
                m_audioEngine->transportState() != TransportState::Playing) {
                // Park playhead at final position
                double t = std::min(currentTime, duration);
                for (auto* map : {&m_fileWaveforms, &m_transcribeWaveforms}) {
                    auto it2 = map->find(fidx);
                    if (it2 != map->end() && it2->second) {
                        it2->second->setPlayhead(t);
                        it2->second->setPlayheadVisible(true);
                    }
                }
                stopFilePlayback();
                return;
            }

            // Update waveform playhead (both IMPORT and TRANSCRIBE lists)
            for (auto* map : {&m_fileWaveforms, &m_transcribeWaveforms}) {
                auto it2 = map->find(fidx);
                if (it2 != map->end() && it2->second) {
                    it2->second->setPlayhead(currentTime);
                    it2->second->setPlayheadVisible(true);
                }
            }

            // Update time label
            updateFileTimeLabel(fidx, currentTime);
        });
    }
    m_previewTimer->start();
}

void AudioSync::stopFilePlayback()
{
    if (m_previewTimer)
        m_previewTimer->stop();

    int prevIdx = m_previewFileIdx;

    if (m_previewFileIdx >= 0 && m_audioEngine) {
        m_audioEngine->pause();
        m_audioEngine->clearTrackSources();
        m_audioEngine->setPlaybackSpeed(1.0);
        if (m_savedSyncClock) {
            m_audioEngine->setSyncClock(m_savedSyncClock);
            m_savedSyncClock = nullptr;
        }
    }

    m_previewFileIdx = -1;

    // Reset play button
    if (prevIdx >= 0)
        updateFilePlayButton(static_cast<size_t>(prevIdx), false);
}

void AudioSync::updateFilePlayButton(size_t fileIndex, bool playing)
{
    for (auto* map : {&m_filePlayBtns, &m_transcribePlayBtns}) {
        auto it = map->find(fileIndex);
        if (it != map->end() && it->second)
            it->second->setText(playing ? QStringLiteral("\u23F8") : QStringLiteral("\u25B6"));
    }
}

void AudioSync::updateFileTimeLabel(size_t fileIndex, double timeSec)
{
    for (auto* map : {&m_fileTimeLabels, &m_transcribeTimeLabels}) {
        auto it = map->find(fileIndex);
        if (it != map->end() && it->second) {
            int mins = static_cast<int>(timeSec) / 60;
            int secs = static_cast<int>(timeSec) % 60;
            int cs   = static_cast<int>(timeSec * 100) % 100;
            it->second->setText(QString("%1:%2.%3")
                .arg(mins).arg(secs, 2, 10, QChar('0')).arg(cs, 2, 10, QChar('0')));
        }
    }
}

} // namespace rt
