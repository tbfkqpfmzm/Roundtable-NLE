/*
 * AudioSyncGapTools.cpp - AudioSync silence trimming and inter-clip gap closing.
 */

#include "panels/audio/AudioSync.h"

#include <spdlog/spdlog.h>

#include <QString>

#include <algorithm>
#include <cmath>
#include <vector>

namespace rt {

void AudioSync::closeInterClipGaps()
{
    if (m_clips.empty()) return;

    int trimCount = 0;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        auto& clip = m_clips[i];
        if (clip.matchState == 2) continue;
        auto it = m_audioSamples.find(clip.sourceFile);
        if (it == m_audioSamples.end()) continue;

        const auto& audioData = it->second;
        const int windowSize = static_cast<int>(0.010 * audioData.sampleRate);
        auto startSample = static_cast<int>(clip.start * audioData.sampleRate);
        auto endSample   = static_cast<int>(clip.end * audioData.sampleRate);
        if (startSample >= static_cast<int>(audioData.samples.size())) continue;
        if (endSample > static_cast<int>(audioData.samples.size()))
            endSample = static_cast<int>(audioData.samples.size());
        if (endSample - startSample < windowSize * 3) continue;

        std::vector<float> rmsValues;
        for (int pos = startSample; pos + windowSize <= endSample; pos += windowSize) {
            float sumSq = 0.0f;
            for (int j = 0; j < windowSize; ++j) {
                float sample = audioData.samples[static_cast<size_t>(pos + j)];
                sumSq += sample * sample;
            }
            rmsValues.push_back(std::sqrt(sumSq / static_cast<float>(windowSize)));
        }
        if (rmsValues.size() < 3) continue;

        auto sorted = rmsValues;
        std::sort(sorted.begin(), sorted.end());
        float noiseFloor  = sorted[sorted.size() / 10];
        float speechLevel = sorted[sorted.size() * 9 / 10];
        float dynRange    = speechLevel - noiseFloor;
        float threshold   = (dynRange > 0.001f)
            ? noiseFloor + dynRange * 0.08f
            : std::max(0.003f, noiseFloor * 1.5f);

        int speechStart = startSample;
        for (int pos = startSample; pos + windowSize * 2 <= endSample; pos += windowSize) {
            auto rmsAt = [&](int samplePos) {
                float sumSq = 0.0f;
                for (int j = 0; j < windowSize; ++j) {
                    float sample = audioData.samples[static_cast<size_t>(samplePos + j)];
                    sumSq += sample * sample;
                }
                return std::sqrt(sumSq / static_cast<float>(windowSize));
            };
            if (rmsAt(pos) > threshold && rmsAt(pos + windowSize) > threshold) {
                speechStart = pos;
                break;
            }
        }

        int speechEnd = endSample;
        for (int pos = endSample - windowSize * 2; pos >= startSample; pos -= windowSize) {
            auto rmsAt = [&](int samplePos) {
                if (samplePos + windowSize > endSample) return 0.0f;
                float sumSq = 0.0f;
                for (int j = 0; j < windowSize; ++j) {
                    float sample = audioData.samples[static_cast<size_t>(samplePos + j)];
                    sumSq += sample * sample;
                }
                return std::sqrt(sumSq / static_cast<float>(windowSize));
            };
            if (rmsAt(pos) > threshold && rmsAt(pos + windowSize) > threshold) {
                speechEnd = pos + windowSize * 2;
                break;
            }
        }

        int prepad  = static_cast<int>(0.060 * audioData.sampleRate);
        int postpad = static_cast<int>(0.040 * audioData.sampleRate);
        speechStart = std::max(startSample, speechStart - prepad);
        speechEnd   = std::min(endSample, speechEnd + postpad);
        double newStart = static_cast<double>(speechStart) / audioData.sampleRate;
        double newEnd   = static_cast<double>(speechEnd) / audioData.sampleRate;
        if (std::abs(newStart - clip.start) > 0.01 || std::abs(newEnd - clip.end) > 0.01) {
            clip.start = newStart;
            clip.end = newEnd;
            ++trimCount;
        }
    }

    if (m_clips.size() < 2) {
        if (m_transcribeStatus)
            m_transcribeStatus->setText(
                trimCount > 0
                    ? QString("Trimmed %1 clips").arg(trimCount)
                    : QString("No changes needed"));
        if (trimCount > 0) populateCards();
        return;
    }

    std::sort(m_clips.begin(), m_clips.end(),
              [](const SyncClip& left, const SyncClip& right) {
                  if (left.sourceFile != right.sourceFile) return left.sourceFile < right.sourceFile;
                  return left.start < right.start;
              });

    int gapsClosed = 0;
    for (size_t i = 0; i + 1 < m_clips.size(); ++i) {
        auto& first = m_clips[i];
        auto& second = m_clips[i + 1];
        if (first.sourceFile != second.sourceFile) continue;
        if (first.matchState == 2 || second.matchState == 2) continue;

        double gap = second.start - first.end;
        if (gap > 0.001 && gap < 0.050) {
            double boundary = (first.end + second.start) / 2.0;
            auto samplesIt = m_audioSamples.find(first.sourceFile);
            if (samplesIt != m_audioSamples.end()) {
                const auto& audioData = samplesIt->second;
                int sampleRate = static_cast<int>(audioData.sampleRate);
                int windowSize = static_cast<int>(0.010 * sampleRate);
                int gapStart = static_cast<int>(first.end * sampleRate);
                int gapEnd = static_cast<int>(second.start * sampleRate);
                if (gapEnd - gapStart > windowSize) {
                    float bestRms = 1e9f;
                    int bestPos = (gapStart + gapEnd) / 2;
                    for (int pos = gapStart; pos + windowSize <= gapEnd; pos += windowSize / 2) {
                        float sumSq = 0.0f;
                        for (int j = 0; j < windowSize && static_cast<size_t>(pos + j) < audioData.samples.size(); ++j) {
                            float sample = audioData.samples[static_cast<size_t>(pos + j)];
                            sumSq += sample * sample;
                        }
                        float rms = std::sqrt(sumSq / static_cast<float>(windowSize));
                        if (rms < bestRms) {
                            bestRms = rms;
                            bestPos = pos;
                        }
                    }
                    boundary = static_cast<double>(bestPos) / sampleRate;
                }
            }
            first.end = boundary;
            second.start = boundary;
            ++gapsClosed;
        }
    }

    if (gapsClosed > 0 || trimCount > 0) {
        spdlog::info("AudioSync: Trimmed {} clips, closed {} gaps", trimCount, gapsClosed);
        populateCards();
    }
    if (m_transcribeStatus)
        m_transcribeStatus->setText(
            QString("Trimmed %1 clips, closed %2 gaps").arg(trimCount).arg(gapsClosed));
}

} // namespace rt
