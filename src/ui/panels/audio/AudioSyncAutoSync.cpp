/*
 * AudioSyncAutoSync.cpp - Auto-sync matching and cleanup helpers.
 * Split from AudioSyncData.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "panels/audio/AudioSyncFileName.h"
#include "ai/ScriptMatcher.h"

#include <spdlog/spdlog.h>

#include <QCheckBox>
#include <QCoreApplication>
#include <QProgressDialog>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace rt {

void AudioSync::updateSyncProgress(int value, const QString& text)
{
    if (m_syncProgress) {
        m_syncProgress->setValue(value);
        m_syncProgress->setLabelText(text);
        QCoreApplication::processEvents();
    }
}

void AudioSync::recoverAutoSyncClipCharacters()
{
    int recoveredCharacterCount = 0;
    for (auto& clip : m_clips) {
        if (!clip.character.empty()) continue;
        clip.character = extractCharacterName(clip.sourceFile);
        if (!clip.character.empty())
            ++recoveredCharacterCount;
    }
    if (recoveredCharacterCount > 0) {
        spdlog::warn("AudioSync: Recovered character names for {} clips before matching",
                     recoveredCharacterCount);
    }
}

int AudioSync::matchAutoSyncClipsToScriptLines()
{
    auto normalizeChar = [](const std::string& name) -> std::string {
        std::string lower;
        lower.reserve(name.size());
        for (char c : name) {
            if (c != ' ' || (!lower.empty() && lower.back() != ' '))
                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        while (!lower.empty() && lower.back() == ' ') lower.pop_back();
        if (lower == "marian") return "modernia";
        return lower;
    };

    auto norm = [](const std::string& text) {
        return ScriptMatcher::normalize(text);
    };

    auto wordOverlap = [](const std::string& a, const std::string& b) -> float {
        std::unordered_set<std::string> aWords, bWords;
        { std::istringstream ss(a); std::string w; while (ss >> w) aWords.insert(w); }
        { std::istringstream ss(b); std::string w; while (ss >> w) bWords.insert(w); }
        if (aWords.empty()) return 0.0f;
        size_t common = 0;
        for (const auto& w : bWords)
            if (aWords.count(w)) ++common;
        return static_cast<float>(common) / static_cast<float>(std::max(aWords.size(), size_t{1}));
    };

    std::unordered_set<int> confirmedClipIds;
    std::unordered_set<int> confirmedLineNumbers;
    for (const auto& clip : m_clips) {
        if (clip.matchState == 2) {
            confirmedClipIds.insert(clip.id);
            if (clip.scriptLineNumber >= 0)
                confirmedLineNumbers.insert(clip.scriptLineNumber);
        }
    }

    std::unordered_map<std::string, std::vector<size_t>> clipsByChar;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        if (confirmedClipIds.count(m_clips[i].id)) continue;
        clipsByChar[normalizeChar(m_clips[i].character)].push_back(i);
    }

    std::unordered_map<std::string, std::vector<size_t>> linesByChar;
    for (size_t i = 0; i < m_script->lines.size(); ++i) {
        if (confirmedLineNumbers.count(m_script->lines[i].lineNumber)) continue;
        linesByChar[normalizeChar(m_script->lines[i].character)].push_back(i);
    }

    for (const auto& [ch, idx] : clipsByChar)
        spdlog::info("AudioSync: Clip group '{}' has {} clips", ch, idx.size());
    for (const auto& [ch, idx] : linesByChar)
        spdlog::info("AudioSync: Script group '{}' has {} lines", ch, idx.size());

    updateSyncProgress(30, "Matching clips to script lines...");

    for (auto& clip : m_clips) {
        if (confirmedClipIds.count(clip.id)) continue;
        clip.scriptLineNumber = -1;
        clip.confidence       = 0.0f;
        clip.matchState       = 0;
        clip.scriptSegment.clear();
    }

    int matchCount = 0;
    bool allowRetakes = m_retakesCheck && m_retakesCheck->isChecked();

    for (auto& [charKey, clipIndices] : clipsByChar) {
        auto lineIt = linesByChar.find(charKey);
        if (lineIt == linesByChar.end() || lineIt->second.empty()) {
            spdlog::debug("AudioSync: No script lines for character '{}'", charKey);
            continue;
        }
        const auto& lineIndices = lineIt->second;

        std::sort(clipIndices.begin(), clipIndices.end(),
                  [&](size_t a, size_t b) { return m_clips[a].start < m_clips[b].start; });

        size_t N = clipIndices.size();
        size_t M = lineIndices.size();

        std::vector<std::string> clipTexts(N), lineTexts(M);
        for (size_t i = 0; i < N; ++i) {
            const auto& c = m_clips[clipIndices[i]];
            clipTexts[i] = norm(c.editedText.empty() ? c.transcript : c.editedText);
        }
        for (size_t j = 0; j < M; ++j)
            lineTexts[j] = norm(m_script->lines[lineIndices[j]].dialogue);

        struct ScoreEntry {
            size_t clipIdx;
            size_t lineIdx;
            float  score;
            float  textScore;
        };
        std::vector<ScoreEntry> allPairs;
        allPairs.reserve(N * M);

        for (size_t ci = 0; ci < N; ++ci) {
            for (size_t li = 0; li < M; ++li) {
                float textScore = ScriptMatcher::sequenceRatio(clipTexts[ci], lineTexts[li]);
                float wo = wordOverlap(lineTexts[li], clipTexts[ci]);

                float phoneticScore = 0.0f;
                {
                    std::vector<std::string> cWords, lWords;
                    { std::istringstream s(clipTexts[ci]); std::string w; while (s >> w) cWords.push_back(w); }
                    { std::istringstream s(lineTexts[li]); std::string w; while (s >> w) lWords.push_back(w); }
                    if (!lWords.empty() && !cWords.empty()) {
                        size_t matches = 0;
                        for (const auto& lw : lWords) {
                            std::string lm = ScriptMatcher::metaphone(lw);
                            for (const auto& cw : cWords)
                                if (ScriptMatcher::metaphone(cw) == lm) { ++matches; break; }
                        }
                        phoneticScore = static_cast<float>(matches) / static_cast<float>(lWords.size());
                    }
                }

                float expectedLinePos = (N > 1)
                    ? static_cast<float>(ci) / static_cast<float>(N - 1)
                    : 0.5f;
                float actualLinePos = (M > 1)
                    ? static_cast<float>(li) / static_cast<float>(M - 1)
                    : 0.5f;
                float orderDist = std::abs(expectedLinePos - actualLinePos);
                float orderBonus = std::max(0.0f, 0.15f * (1.0f - orderDist * 2.0f));

                float combined = textScore * 0.40f + phoneticScore * 0.20f
                               + wo * 0.25f + orderBonus;

                {
                    size_t clipWordCount = 0;
                    { std::istringstream s(clipTexts[ci]); std::string w; while (s >> w) ++clipWordCount; }
                    size_t lineWordCount = 0;
                    { std::istringstream s(lineTexts[li]); std::string w; while (s >> w) ++lineWordCount; }
                    size_t minWords = std::min(clipWordCount, lineWordCount);
                    if (minWords <= 2) {
                        combined = textScore * 0.70f + phoneticScore * 0.05f
                                 + wo * 0.10f + orderBonus * 0.5f;
                    } else if (minWords <= 4) {
                        combined = textScore * 0.55f + phoneticScore * 0.12f
                                 + wo * 0.18f + orderBonus * 0.75f;
                    }
                }

                if (combined >= 0.20f)
                    allPairs.push_back({ci, li, combined, textScore});
            }
        }

        std::sort(allPairs.begin(), allPairs.end(),
                  [](const ScoreEntry& a, const ScoreEntry& b) {
                      return a.score > b.score;
                  });

        std::unordered_set<size_t> assignedClips;
        std::unordered_set<size_t> assignedLines;

        for (const auto& entry : allPairs) {
            if (assignedClips.count(entry.clipIdx)) continue;
            if (!allowRetakes && assignedLines.count(entry.lineIdx)) continue;

            size_t clipGlobalIdx = clipIndices[entry.clipIdx];
            size_t lineGlobalIdx = lineIndices[entry.lineIdx];
            const auto& line = m_script->lines[lineGlobalIdx];

            auto& clip = m_clips[clipGlobalIdx];
            clip.confidence       = entry.textScore;
            clip.scriptLineNumber = line.lineNumber;
            clip.matchState       = 1;
            clip.scriptSegment    = line.character + ": " + line.dialogue;
            ++matchCount;

            assignedClips.insert(entry.clipIdx);
            if (!allowRetakes)
                assignedLines.insert(entry.lineIdx);

            spdlog::debug("AudioSync: Matched clip {} -> line {} (score={:.2f} text={:.2f}) '{}'",
                          clipGlobalIdx, line.lineNumber, entry.score, entry.textScore,
                          line.dialogue.substr(0, 40));
        }
    }

    return matchCount;
}

} // namespace rt