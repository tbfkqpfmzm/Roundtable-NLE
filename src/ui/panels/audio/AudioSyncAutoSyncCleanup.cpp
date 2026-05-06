/*
 * AudioSyncAutoSyncCleanup.cpp - Auto-sync audio cleanup helpers.
 * Split from AudioSyncAutoSync.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace rt {

int AudioSync::trimAutoSyncClipSilence()
{
    int trimCount = 0;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        auto& clip = m_clips[i];
        auto it = m_audioSamples.find(clip.sourceFile);
        if (it == m_audioSamples.end()) continue;

        const auto& audioData = it->second;
        const int windowSize = static_cast<int>(0.010 * audioData.sampleRate);
        auto startSample = static_cast<int>(clip.start * audioData.sampleRate);
        auto endSample   = static_cast<int>(clip.end   * audioData.sampleRate);
        if (startSample >= static_cast<int>(audioData.samples.size())) continue;
        if (endSample > static_cast<int>(audioData.samples.size()))
            endSample = static_cast<int>(audioData.samples.size());
        if (endSample - startSample < windowSize * 3) continue;

        std::vector<float> rmsValues;
        for (int pos = startSample; pos + windowSize <= endSample; pos += windowSize) {
            float sumSq = 0.0f;
            for (int j = 0; j < windowSize; ++j) {
                float s = audioData.samples[static_cast<size_t>(pos + j)];
                sumSq += s * s;
            }
            rmsValues.push_back(std::sqrt(sumSq / static_cast<float>(windowSize)));
        }
        if (rmsValues.size() < 3) continue;

        auto sortedRms = rmsValues;
        std::sort(sortedRms.begin(), sortedRms.end());
        float noiseFloor  = sortedRms[sortedRms.size() / 10];
        float speechLevel = sortedRms[sortedRms.size() * 9 / 10];
        float dynRange    = speechLevel - noiseFloor;
        float trimThreshold = (dynRange > 0.001f)
            ? noiseFloor + dynRange * 0.08f
            : std::max(0.003f, noiseFloor * 1.5f);

        int speechStart = startSample;
        for (int pos = startSample; pos + windowSize * 2 <= endSample; pos += windowSize) {
            auto rmsAt = [&](int p) {
                float sumSq = 0.0f;
                for (int j = 0; j < windowSize; ++j) {
                    float s = audioData.samples[static_cast<size_t>(p + j)];
                    sumSq += s * s;
                }
                return std::sqrt(sumSq / static_cast<float>(windowSize));
            };
            if (rmsAt(pos) > trimThreshold && rmsAt(pos + windowSize) > trimThreshold) {
                speechStart = pos; break;
            }
        }

        int speechEnd = endSample;
        for (int pos = endSample - windowSize * 2; pos >= startSample; pos -= windowSize) {
            auto rmsAt = [&](int p) {
                if (p + windowSize > endSample) return 0.0f;
                float sumSq = 0.0f;
                for (int j = 0; j < windowSize; ++j) {
                    float s = audioData.samples[static_cast<size_t>(p + j)];
                    sumSq += s * s;
                }
                return std::sqrt(sumSq / static_cast<float>(windowSize));
            };
            if (rmsAt(pos) > trimThreshold && rmsAt(pos + windowSize) > trimThreshold) {
                speechEnd = pos + windowSize * 2; break;
            }
        }

        int prepad  = static_cast<int>(0.060 * audioData.sampleRate);
        int postpad = static_cast<int>(0.040 * audioData.sampleRate);
        speechStart = std::max(startSample, speechStart - prepad);
        speechEnd   = std::min(endSample,   speechEnd + postpad);
        double newStart = static_cast<double>(speechStart) / audioData.sampleRate;
        double newEnd   = static_cast<double>(speechEnd)   / audioData.sampleRate;
        if (std::abs(newStart - clip.start) > 0.01 || std::abs(newEnd - clip.end) > 0.01) {
            clip.start = newStart;
            clip.end   = newEnd;
            ++trimCount;
        }
    }
    if (trimCount > 0)
        spdlog::info("AudioSync: Auto-trimmed silence from {} clips", trimCount);
    return trimCount;
}

int AudioSync::closeAutoSyncTinyGaps()
{
    std::sort(m_clips.begin(), m_clips.end(),
              [](const SyncClip& x, const SyncClip& y) {
                  if (x.sourceFile != y.sourceFile) return x.sourceFile < y.sourceFile;
                  return x.start < y.start;
              });

    int gapsClosed = 0;
    for (size_t i = 0; i + 1 < m_clips.size(); ++i) {
        auto& a = m_clips[i];
        auto& b = m_clips[i + 1];
        if (a.sourceFile != b.sourceFile) continue;
        double gap = b.start - a.end;
        if (gap > 0.001 && gap < 0.050) {
            double boundary = (a.end + b.start) / 2.0;
            auto sampIt = m_audioSamples.find(a.sourceFile);
            if (sampIt != m_audioSamples.end()) {
                const auto& ad = sampIt->second;
                int sr = static_cast<int>(ad.sampleRate);
                int winSz = static_cast<int>(0.010 * sr);
                int gapStart = static_cast<int>(a.end * sr);
                int gapEnd = static_cast<int>(b.start * sr);
                if (gapEnd - gapStart > winSz) {
                    float bestRms = 1e9f;
                    int bestPos = (gapStart + gapEnd) / 2;
                    for (int pos = gapStart; pos + winSz <= gapEnd; pos += winSz / 2) {
                        float sumSq = 0.0f;
                        for (int j = 0; j < winSz && static_cast<size_t>(pos + j) < ad.samples.size(); ++j) {
                            float s = ad.samples[static_cast<size_t>(pos + j)];
                            sumSq += s * s;
                        }
                        float rms = std::sqrt(sumSq / static_cast<float>(winSz));
                        if (rms < bestRms) { bestRms = rms; bestPos = pos; }
                    }
                    boundary = static_cast<double>(bestPos) / sr;
                }
            }
            a.end = boundary;
            b.start = boundary;
            ++gapsClosed;
        }
    }
    if (gapsClosed > 0)
        spdlog::info("AudioSync: Closed {} inter-clip gaps", gapsClosed);
    return gapsClosed;
}

int AudioSync::autoConfirmHighConfidenceMatches()
{
    int autoConfirmed = 0;
    for (auto& clip : m_clips) {
        if (clip.matchState == 1 && clip.confidence >= 0.90f) {
            clip.matchState = 2;
            ++autoConfirmed;
        }
    }
    if (autoConfirmed > 0)
        spdlog::info("AudioSync: Auto-confirmed {} high-confidence matches", autoConfirmed);
    return autoConfirmed;
}

} // namespace rt
