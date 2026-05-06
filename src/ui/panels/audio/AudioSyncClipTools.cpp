/*
 * AudioSyncClipTools.cpp - Clip scrub and silence-trim helpers for AudioSync.
 * Split from AudioSyncPlayback.cpp for maintainability.
 */

#include "panels/audio/AudioSync.h"

#include "media/AudioEngine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace rt {

void AudioSync::scrubClipAt(size_t clipIdx, double timeSec)
{
    if (!m_audioEngine || clipIdx >= m_clips.size())
        return;

    const auto& clip = m_clips[clipIdx];
    auto it = m_audioSamples.find(clip.sourceFile);
    if (it == m_audioSamples.end())
        return;

    const auto& audioData = it->second;
    if (audioData.samples.empty())
        return;

    // Detach sync clock temporarily for scrub
    AVSyncClock* savedClock = m_audioEngine->syncClock();
    m_audioEngine->setSyncClock(nullptr);

    // Set up a temporary source for scrub
    AudioTrackSource src;
    src.trackId     = 9999;
    src.samples     = audioData.samples.data();
    src.totalFrames = static_cast<int64_t>(audioData.samples.size());
    src.startFrame  = 0;
    src.channels    = 1;
    src.sampleRate  = audioData.sampleRate;
    src.volume      = 1.0f;
    src.pan         = 0.0f;
    src.muted       = false;
    src.solo        = false;

    m_audioEngine->setTrackSources({src});

    int64_t frame = static_cast<int64_t>(timeSec * audioData.sampleRate);
    m_audioEngine->scrub(frame, 2048);

    // Restore sync clock
    m_audioEngine->setSyncClock(savedClock);
}

void AudioSync::autoTrimClip(size_t clipIdx)
{
    if (clipIdx >= m_clips.size()) return;

    auto& clip = m_clips[clipIdx];
    auto it = m_audioSamples.find(clip.sourceFile);
    if (it == m_audioSamples.end()) return;

    const auto& audioData = it->second;
    const int windowSize  = static_cast<int>(0.020 * audioData.sampleRate); // 20ms

    auto startSample = static_cast<int>(clip.start * audioData.sampleRate);
    auto endSample   = static_cast<int>(clip.end * audioData.sampleRate);
    if (startSample >= static_cast<int>(audioData.samples.size())) return;
    if (endSample > static_cast<int>(audioData.samples.size())) endSample = static_cast<int>(audioData.samples.size());
    if (endSample - startSample < windowSize * 3) return;

    // Measure all window RMS values to compute adaptive noise floor
    std::vector<float> rmsValues;
    for (int pos = startSample; pos + windowSize <= endSample; pos += windowSize) {
        float sumSq = 0.0f;
        for (int j = 0; j < windowSize; ++j) {
            float s = audioData.samples[static_cast<size_t>(pos + j)];
            sumSq += s * s;
        }
        rmsValues.push_back(std::sqrt(sumSq / static_cast<float>(windowSize)));
    }
    if (rmsValues.size() < 3) return;

    // Adaptive threshold: sort RMS values, take the 10th percentile as noise floor,
    // then set threshold at 3x that level (min 0.005)
    auto sorted = rmsValues;
    std::sort(sorted.begin(), sorted.end());
    float noiseFloor = sorted[sorted.size() / 10];
    float threshold = std::max(0.005f, noiseFloor * 3.0f);

    int speechStart = startSample;
    int aboveCount = 0;
    for (int pos = startSample; pos + windowSize <= endSample; pos += windowSize) {
        float sumSq = 0.0f;
        for (int j = 0; j < windowSize; ++j) {
            float s = audioData.samples[static_cast<size_t>(pos + j)];
            sumSq += s * s;
        }
        float rms = std::sqrt(sumSq / static_cast<float>(windowSize));
        if (rms > threshold) {
            ++aboveCount;
            if (aboveCount >= 2) {
                speechStart = pos - windowSize;
                break;
            }
        } else {
            aboveCount = 0;
        }
    }

    int speechEnd = endSample;
    aboveCount = 0;
    int backStart = endSample - windowSize;
    backStart = backStart - (backStart % windowSize) + (startSample % windowSize);
    for (int pos = backStart; pos >= startSample; pos -= windowSize) {
        if (pos + windowSize > endSample) continue;
        float sumSq = 0.0f;
        for (int j = 0; j < windowSize; ++j) {
            float s = audioData.samples[static_cast<size_t>(pos + j)];
            sumSq += s * s;
        }
        float rms = std::sqrt(sumSq / static_cast<float>(windowSize));
        if (rms > threshold) {
            ++aboveCount;
            if (aboveCount >= 2) {
                speechEnd = pos + windowSize + windowSize;
                break;
            }
        } else {
            aboveCount = 0;
        }
    }

    // 120ms pre-padding (breath/plosive room), 150ms post-padding
    int prepad  = static_cast<int>(0.120 * audioData.sampleRate);
    int postpad = static_cast<int>(0.150 * audioData.sampleRate);
    speechStart = std::max(startSample, speechStart - prepad);
    speechEnd   = std::min(endSample, speechEnd + postpad);

    double newStart = static_cast<double>(speechStart) / audioData.sampleRate;
    double newEnd   = static_cast<double>(speechEnd) / audioData.sampleRate;

    // Only apply if change is meaningful (>20ms)
    if (std::abs(newStart - clip.start) > 0.02 || std::abs(newEnd - clip.end) > 0.02) {
        clip.start = newStart;
        clip.end   = newEnd;
        spdlog::info("AudioSync: Auto-trimmed clip {} to {:.3f}-{:.3f}", clipIdx, newStart, newEnd);
        populateClipList();
    }
}

} // namespace rt
