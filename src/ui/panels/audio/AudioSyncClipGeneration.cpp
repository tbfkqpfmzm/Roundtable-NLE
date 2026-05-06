/*
 * AudioSyncClipGeneration.cpp - Build AudioSync clips from transcription results.
 */

#include "panels/audio/AudioSync.h"

#include "panels/audio/AudioSyncFileName.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <unordered_set>

namespace rt {

std::vector<std::pair<double, double>> AudioSync::getEffectiveRanges(const SyncClip& clip)
{
    if (clip.deletedRegions.empty())
        return {{clip.start, clip.end}};

    auto deleted = clip.deletedRegions;
    std::sort(deleted.begin(), deleted.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });

    std::vector<std::pair<double, double>> ranges;
    double currentStart = clip.start;

    for (const auto& [delStart, delEnd] : deleted) {
        double clippedStart = std::max(delStart, clip.start);
        double clippedEnd = std::min(delEnd, clip.end);
        if (currentStart < clippedStart - 0.05)
            ranges.emplace_back(currentStart, clippedStart);
        currentStart = clippedEnd;
    }

    if (currentStart < clip.end - 0.05)
        ranges.emplace_back(currentStart, clip.end);
    return ranges;
}

void AudioSync::createClipsFromTranscription()
{
    createClipsFromAllTranscriptions();
}

void AudioSync::createClipsFromAllTranscriptions()
{
    m_clips.clear();

    int globalId = 0;
    for (size_t fileIdx = 0; fileIdx < m_allTranscriptionResults.size(); ++fileIdx) {
        const auto& result = m_allTranscriptionResults[fileIdx];
        std::string sourceFile = (fileIdx < m_audioPaths.size()) ? m_audioPaths[fileIdx] : "";
        std::string charName = sourceFile.empty()
            ? std::string{}
            : extractCharacterName(sourceFile);

        for (const auto& segment : result.segments) {
            SyncClip clip;
            clip.id = globalId++;
            clip.sourceFile = sourceFile;
            clip.start = segment.start;
            clip.end = segment.end;
            clip.transcript = segment.text;
            clip.editedText = segment.text;
            clip.character = segment.character.empty() ? charName : segment.character;
            m_clips.push_back(std::move(clip));
        }
    }

    spdlog::info("AudioSync: Created {} clips from {} file(s)",
                 m_clips.size(), m_allTranscriptionResults.size());
}

void AudioSync::appendClipsFromNewTranscriptions()
{
    std::unordered_set<std::string> existingSourceFiles;
    for (const auto& clip : m_clips)
        existingSourceFiles.insert(clip.sourceFile);

    int nextId = m_clips.empty() ? 0 : m_clips.back().id + 1;
    size_t added = 0;
    for (size_t fileIdx = 0; fileIdx < m_allTranscriptionResults.size(); ++fileIdx) {
        const auto& result = m_allTranscriptionResults[fileIdx];
        std::string sourceFile = (fileIdx < m_audioPaths.size()) ? m_audioPaths[fileIdx] : "";
        if (existingSourceFiles.count(sourceFile))
            continue;

        std::string charName = sourceFile.empty()
            ? std::string{}
            : extractCharacterName(sourceFile);

        for (const auto& segment : result.segments) {
            SyncClip clip;
            clip.id = nextId++;
            clip.sourceFile = sourceFile;
            clip.start = segment.start;
            clip.end = segment.end;
            clip.transcript = segment.text;
            clip.editedText = segment.text;
            clip.character = segment.character.empty() ? charName : segment.character;
            m_clips.push_back(std::move(clip));
            ++added;
        }
    }

    spdlog::info("AudioSync: Appended {} new clips (preserved {} existing)",
                 added, existingSourceFiles.size());
}

} // namespace rt