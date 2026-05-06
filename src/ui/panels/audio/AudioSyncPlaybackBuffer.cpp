/*
 * AudioSyncPlaybackBuffer.cpp - Playback buffer construction helpers.
 */

#include "panels/audio/AudioSync.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rt {

void AudioSync::buildClipPlaybackBuffer(const SyncClip& clip,
                                        const std::vector<float>& samples,
                                        double sourceRate,
                                        double engineRate)
{
    auto startSample = static_cast<size_t>(clip.start * sourceRate);
    auto endSample   = static_cast<size_t>(clip.end * sourceRate);

    m_playbackBuffer.clear();
    m_playbackTimeMap.clear();
    m_playbackSourceRate = sourceRate;

    if (startSample >= samples.size()) return;
    if (endSample > samples.size()) endSample = samples.size();

    auto sortedDeleted = clip.deletedRegions;
    std::sort(sortedDeleted.begin(), sortedDeleted.end());

    double cursor = clip.start;
    for (const auto& [delStart, delEnd] : sortedDeleted) {
        if (delStart >= clip.end) break;

        double regionStart = std::max(delStart, clip.start);
        double regionEnd   = std::min(delEnd, clip.end);
        if (regionStart <= cursor && regionEnd > cursor) {
            cursor = regionEnd;
            continue;
        }

        if (regionStart > cursor) {
            auto from = static_cast<size_t>(cursor * sourceRate);
            auto to   = static_cast<size_t>(regionStart * sourceRate);
            from = std::min(from, samples.size());
            to   = std::min(to, samples.size());
            if (to > from) {
                m_playbackTimeMap.push_back({static_cast<int64_t>(m_playbackBuffer.size()), cursor});
                m_playbackBuffer.insert(m_playbackBuffer.end(),
                    samples.begin() + static_cast<ptrdiff_t>(from),
                    samples.begin() + static_cast<ptrdiff_t>(to));
            }
        }
        cursor = regionEnd;
    }

    if (cursor < clip.end) {
        auto from = static_cast<size_t>(cursor * sourceRate);
        auto to   = std::min(endSample, samples.size());
        from = std::min(from, samples.size());
        if (to > from) {
            m_playbackTimeMap.push_back({static_cast<int64_t>(m_playbackBuffer.size()), cursor});
            m_playbackBuffer.insert(m_playbackBuffer.end(),
                samples.begin() + static_cast<ptrdiff_t>(from),
                samples.begin() + static_cast<ptrdiff_t>(to));
        }
    }

    if (std::abs(sourceRate - engineRate) <= 1.0 || m_playbackBuffer.empty()) return;

    double ratio = engineRate / sourceRate;
    auto newSize = static_cast<size_t>(static_cast<double>(m_playbackBuffer.size()) * ratio) + 1;
    std::vector<float> resampled(newSize);
    for (size_t j = 0; j < newSize; ++j) {
        double srcPos = static_cast<double>(j) / ratio;
        auto idx = static_cast<size_t>(srcPos);
        float frac = static_cast<float>(srcPos - static_cast<double>(idx));
        if (idx + 1 < m_playbackBuffer.size()) {
            resampled[j] = m_playbackBuffer[idx] * (1.0f - frac)
                         + m_playbackBuffer[idx + 1] * frac;
        } else if (idx < m_playbackBuffer.size()) {
            resampled[j] = m_playbackBuffer[idx];
        }
    }

    for (auto& [offset, time] : m_playbackTimeMap) {
        offset = static_cast<int64_t>(static_cast<double>(offset) * ratio);
    }

    m_playbackBuffer = std::move(resampled);
    m_playbackSourceRate = engineRate;
}

int64_t AudioSync::frameForPlaybackTime(double timeSec) const
{
    int64_t targetFrame = 0;
    for (size_t i = 0; i < m_playbackTimeMap.size(); ++i) {
        auto [segStart, segTime] = m_playbackTimeMap[i];
        int64_t segEnd = (i + 1 < m_playbackTimeMap.size())
            ? m_playbackTimeMap[i + 1].first
            : static_cast<int64_t>(m_playbackBuffer.size());
        double segDuration = static_cast<double>(segEnd - segStart) / m_playbackSourceRate;
        if (timeSec >= segTime && timeSec < segTime + segDuration) {
            return segStart + static_cast<int64_t>((timeSec - segTime) * m_playbackSourceRate);
        }
        if (timeSec < segTime) {
            return segStart;
        }
        targetFrame = segEnd;
    }
    return targetFrame;
}

} // namespace rt
