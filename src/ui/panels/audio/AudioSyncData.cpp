/*
 * AudioSyncData.cpp - Data processing, algorithms, and persistence for AudioSync.
 * Split from AudioSync.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "media/AudioEngine.h"
#include "media/AudioFile.h"
#include "spine/ShotPreset.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "widgets/MiniWaveformWidget.h"
#include "widgets/ManualMatchDialog.h"
#include "Theme.h"

#include <spdlog/spdlog.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <filesystem>

static std::string extractCharacterName(const std::string& filePath)
{
    std::filesystem::path p(filePath);
    std::string name = p.stem().string();
    if (name.empty()) return "Unknown";
    static const std::vector<std::string> prefixes = {
        "rvc", "voice", "vo_", "audio_", "rec_", "recording_"
    };
    {
        std::string lower = name;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& prefix : prefixes) {
            if (lower.substr(0, prefix.size()) == prefix) {
                name = name.substr(prefix.size());
                break;
            }
        }
    }
    {
        size_t sep = name.find_first_of(" _-");
        if (sep != std::string::npos && sep >= 2) {
            std::string rest = name.substr(sep + 1);
            std::string restLower = rest;
            for (auto& c : restLower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            static const std::vector<std::string> suffixes = {
                "fix", "final", "new", "old", "alt", "v2", "v3", "redo",
                "done", "fixed", "clean", "raw", "edit", "edited", "master",
                "draft", "wip", "temp", "test", "copy", "backup"
            };
            bool allSuffix = true;
            size_t pos2 = 0;
            while (pos2 < restLower.size() && allSuffix) {
                size_t next = restLower.find_first_of(" _-", pos2);
                if (next == std::string::npos) next = restLower.size();
                std::string word = restLower.substr(pos2, next - pos2);
                if (word.empty()) { pos2 = next + 1; continue; }
                bool isNum = true;
                for (char c : word) if (!std::isdigit(static_cast<unsigned char>(c))) { isNum = false; break; }
                bool isSuffix = std::find(suffixes.begin(), suffixes.end(), word) != suffixes.end();
                bool isTake = (word.substr(0, 4) == "take");
                bool isShort = (word.size() <= 2);
                if (!isNum && !isSuffix && !isTake && !isShort) allSuffix = false;
                pos2 = next + 1;
            }
            if (allSuffix) name = name.substr(0, sep);
        }
    }
    {
        while (!name.empty()) {
            char c = name.back();
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ')
                name.pop_back();
            else break;
        }
        if (name.size() >= 4) {
            std::string tail = name.substr(name.size() - 4);
            for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (tail == "take") name = name.substr(0, name.size() - 4);
        }
        while (!name.empty()) {
            char c = name.back();
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ')
                name.pop_back();
            else break;
        }
    }
    if (name.size() < 2) {
        std::string stem = p.stem().string();
        for (size_t i = 0; i < stem.size(); ++i) {
            if (std::isupper(static_cast<unsigned char>(stem[i]))) {
                size_t j = i + 1;
                while (j < stem.size() && std::islower(static_cast<unsigned char>(stem[j]))) ++j;
                if (j - i >= 2) { name = stem.substr(i, j - i); break; }
            }
        }
    }
    if (name.empty()) name = p.stem().string();
    if (!name.empty()) {
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        for (size_t i = 1; i < name.size(); ++i)
            name[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    }
    return name;
}

namespace rt {

void AudioSync::runAutoSync()
{
    if (!m_script || m_clips.empty()) return;

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

    auto updateSyncProgress = [this](int value, const QString& text) {
        if (m_syncProgress) {
            m_syncProgress->setValue(value);
            m_syncProgress->setLabelText(text);
            QCoreApplication::processEvents();
        }
    };

    updateSyncProgress(5, "Merging short segments...");

    // Pre-pass: merge short segments that match script lines better combined
    mergeSegmentsToMatchScript();

    // Pre-pass 2: script-guided re-segmentation ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â re-cut clips so boundaries
    // better align with script lines (fixes whisper's arbitrary segmentation)
    updateSyncProgress(15, "Re-segmenting by script lines...");

    updateSyncProgress(15, "Re-segmenting by script lines...");

    resegmentByScript();

    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    // GLOBAL OPTIMAL MATCHING
    //
    // Instead of greedy sequential matching with a small window, we build
    // an NxM cost matrix (clips ÃƒÆ’Ã¢â‚¬â€ lines) per character and find the best
    // global assignment.  A sequential-order bonus is included so that
    // in-order recordings are preferred, but out-of-order clips can still
    // match correctly.
    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬

    // --- Helpers ---
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

    // --- Step 1: Collect confirmed matches to preserve ---
    std::unordered_set<int> confirmedClipIds;
    std::unordered_set<int> confirmedLineNumbers;
    for (const auto& clip : m_clips) {
        if (clip.matchState == 2) {
            confirmedClipIds.insert(clip.id);
            if (clip.scriptLineNumber >= 0)
                confirmedLineNumbers.insert(clip.scriptLineNumber);
        }
    }

    // --- Step 2: Group by character (exclude confirmed) ---
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

    updateSyncProgress(30, "Matching clips to script lines...");

    updateSyncProgress(30, "Matching clips to script lines...");

    // --- Step 3: Clear non-confirmed matches ---
    for (auto& clip : m_clips) {
        if (confirmedClipIds.count(clip.id)) continue;
        clip.scriptLineNumber = -1;
        clip.confidence       = 0.0f;
        clip.matchState       = 0;
        clip.scriptSegment.clear();
    }

    // --- Step 4: Global matching within each character group ---
    int matchCount = 0;
    bool allowRetakes = m_retakesCheck && m_retakesCheck->isChecked();

    for (auto& [charKey, clipIndices] : clipsByChar) {
        auto lineIt = linesByChar.find(charKey);
        if (lineIt == linesByChar.end() || lineIt->second.empty()) {
            spdlog::debug("AudioSync: No script lines for character '{}'", charKey);
            continue;
        }
        const auto& lineIndices = lineIt->second;

        // Sort clips by start time
        std::sort(clipIndices.begin(), clipIndices.end(),
                  [&](size_t a, size_t b) { return m_clips[a].start < m_clips[b].start; });

        size_t N = clipIndices.size();
        size_t M = lineIndices.size();

        // Pre-normalize all texts
        std::vector<std::string> clipTexts(N), lineTexts(M);
        for (size_t i = 0; i < N; ++i) {
            const auto& c = m_clips[clipIndices[i]];
            clipTexts[i] = norm(c.editedText.empty() ? c.transcript : c.editedText);
        }
        for (size_t j = 0; j < M; ++j)
            lineTexts[j] = norm(m_script->lines[lineIndices[j]].dialogue);

        // Build NxM score matrix
        struct ScoreEntry {
            size_t clipIdx;  // index into clipIndices
            size_t lineIdx;  // index into lineIndices
            float  score;
            float  textScore;
        };
        std::vector<ScoreEntry> allPairs;
        allPairs.reserve(N * M);

        for (size_t ci = 0; ci < N; ++ci) {
            for (size_t li = 0; li < M; ++li) {
                float textScore = ScriptMatcher::sequenceRatio(clipTexts[ci], lineTexts[li]);
                float wo = wordOverlap(lineTexts[li], clipTexts[ci]);

                // Phonetic similarity
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

                // Sequential order bonus: clips and lines that are in the same
                // relative position get a bonus.  The closer to the "expected"
                // sequential mapping (ci/N ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€  li/M), the larger the bonus.
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

                // Short-text penalty: for very short transcripts (1-2 words),
                // word overlap and phonetic scores are unreliable (a single
                // common word gives 1.0).  Shift weight toward textScore to
                // prefer exact matches and reduce false positives.
                {
                    size_t clipWordCount = 0;
                    { std::istringstream s(clipTexts[ci]); std::string w; while (s >> w) ++clipWordCount; }
                    size_t lineWordCount = 0;
                    { std::istringstream s(lineTexts[li]); std::string w; while (s >> w) ++lineWordCount; }
                    size_t minWords = std::min(clipWordCount, lineWordCount);
                    if (minWords <= 2) {
                        // For 1-2 word texts, heavily weight textScore
                        combined = textScore * 0.70f + phoneticScore * 0.05f
                                 + wo * 0.10f + orderBonus * 0.5f;
                    } else if (minWords <= 4) {
                        // For 3-4 word texts, moderately shift toward textScore
                        combined = textScore * 0.55f + phoneticScore * 0.12f
                                 + wo * 0.18f + orderBonus * 0.75f;
                    }
                }

                if (combined >= 0.20f)
                    allPairs.push_back({ci, li, combined, textScore});
            }
        }

        // Sort by score descending ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â best assignments first
        std::sort(allPairs.begin(), allPairs.end(),
                  [](const ScoreEntry& a, const ScoreEntry& b) {
                      return a.score > b.score;
                  });

        // Greedy global assignment: each clip assigned to at most one line,
        // each line assigned to at most one clip (unless retakes are enabled).
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
            clip.matchState       = 1; // tentative
            clip.scriptSegment    = line.character + ": " + line.dialogue;
            ++matchCount;

            assignedClips.insert(entry.clipIdx);
            if (!allowRetakes)
                assignedLines.insert(entry.lineIdx);

            spdlog::debug("AudioSync: Matched clip {} ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ line {} (score={:.2f} text={:.2f}) '{}'",
                          clipGlobalIdx, line.lineNumber, entry.score, entry.textScore,
                          line.dialogue.substr(0, 40));
        }
    updateSyncProgress(60, "Trimming silence from clips...");

    }

    updateSyncProgress(60, "Trimming silence from clips...");

    updateSyncProgress(60, "Trimming silence from clips...");

    // --- Step 5: Auto-trim silence from all clips ---
    int trimCount = 0;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        auto& clip = m_clips[i];
        auto it = m_audioSamples.find(clip.sourceFile);
        if (it == m_audioSamples.end()) continue;

        const auto& audioData = it->second;
        const int windowSize = static_cast<int>(0.010 * audioData.sampleRate); // 10ms
        auto startSample = static_cast<int>(clip.start * audioData.sampleRate);
        auto endSample   = static_cast<int>(clip.end   * audioData.sampleRate);
        if (startSample >= static_cast<int>(audioData.samples.size())) continue;
        if (endSample > static_cast<int>(audioData.samples.size()))
            endSample = static_cast<int>(audioData.samples.size());
        if (endSample - startSample < windowSize * 3) continue;

        // Measure all window RMS values to compute energy profile
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

        // Dynamic-range threshold: 8% of the way from noise floor to speech peak
        auto sortedRms = rmsValues;
        std::sort(sortedRms.begin(), sortedRms.end());
        float noiseFloor  = sortedRms[sortedRms.size() / 10];       // 10th percentile
        float speechLevel = sortedRms[sortedRms.size() * 9 / 10];   // 90th percentile
        float dynRange    = speechLevel - noiseFloor;
        float trimThreshold = (dynRange > 0.001f)
            ? noiseFloor + dynRange * 0.08f
            : std::max(0.003f, noiseFloor * 1.5f);

        // Scan forward for speech start — require 2 consecutive above-threshold windows
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
        // Scan backward for speech end — require 2 consecutive above-threshold windows
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
        int prepad  = static_cast<int>(0.060 * audioData.sampleRate); // 60ms
        int postpad = static_cast<int>(0.040 * audioData.sampleRate); // 40ms
        speechStart = std::max(startSample, speechStart - prepad);
        speechEnd   = std::min(endSample,   speechEnd + postpad);
        double newStart = static_cast<double>(speechStart) / audioData.sampleRate;
        double newEnd   = static_cast<double>(speechEnd)   / audioData.sampleRate;
        if (std::abs(newStart - clip.start) > 0.01 || std::abs(newEnd - clip.end) > 0.01) {
            clip.start = newStart;
            clip.end   = newEnd;
            ++trimCount;
        }
    updateSyncProgress(80, "Closing inter-clip gaps...");

    }
    if (trimCount > 0)
        spdlog::info("AudioSync: Auto-trimmed silence from {} clips", trimCount);

    updateSyncProgress(80, "Closing inter-clip gaps...");

    updateSyncProgress(80, "Closing inter-clip gaps...");

    // --- Step 5b: Close gaps between consecutive clips from the same file ---
    // When two adjacent clips come from the same audio file, snap the
    // boundary so there's no dead gap between them.  The midpoint of
    // the gap is used as the shared boundary.
    // Sort clips by source file + start time so same-file clips are adjacent.
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
        if (gap > 0.001 && gap < 0.050) { // only close tiny seam gaps (<50ms)
            // Find the quietest point in the gap to place the boundary
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
    updateSyncProgress(90, "Confirming high-confidence matches...");

            ++gapsClosed;
        }
    }
    if (gapsClosed > 0)
        spdlog::info("AudioSync: Closed {} inter-clip gaps", gapsClosed);

    updateSyncProgress(90, "Confirming high-confidence matches...");

    updateSyncProgress(90, "Confirming high-confidence matches...");

    // --- Step 6: Auto-confirm high-confidence matches ---
    int autoConfirmed = 0;
    for (auto& clip : m_clips) {
        if (clip.matchState == 1 && clip.confidence >= 0.90f) {
            clip.matchState = 2;
            ++autoConfirmed;
        }
    }
    if (autoConfirmed > 0)
        spdlog::info("AudioSync: Auto-confirmed {} high-confidence matches", autoConfirmed);

    m_syncDone = true;
    populateClipList();
    updateWorkflowState();

    m_syncStatus->setText(QString("Matched %1/%2 segments")
        .arg(matchCount).arg(m_clips.size()));

    spdlog::info("AudioSync: Auto-sync matched {}/{} segments",
                 matchCount, m_clips.size());
    emit syncCompleted(matchCount, static_cast<int>(m_clips.size()));
}


std::vector<std::pair<double,double>> AudioSync::getEffectiveRanges(const SyncClip& clip)
{
    if (clip.deletedRegions.empty())
        return {{clip.start, clip.end}};

    // Sort deleted regions by start time
    auto deleted = clip.deletedRegions;
    std::sort(deleted.begin(), deleted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<double,double>> ranges;
    double currentStart = clip.start;

    for (const auto& [delStart, delEnd] : deleted) {
        double ds = std::max(delStart, clip.start);
        double de = std::min(delEnd, clip.end);

        // Add range before deleted region (at least 50ms)
        if (currentStart < ds - 0.05)
            ranges.emplace_back(currentStart, ds);

        currentStart = de;
    }

    // Final range after all deleted regions
    if (currentStart < clip.end - 0.05)
        ranges.emplace_back(currentStart, clip.end);

    return ranges;
}

// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Export to Timeline (ported from Python _export_timeline) ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬

void AudioSync::createClipsFromTranscription()
{
    // Legacy single-file version ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â delegates to multi-file
    createClipsFromAllTranscriptions();
}

void AudioSync::createClipsFromAllTranscriptions()
{
    m_clips.clear();

    int globalId = 0;
    for (size_t fileIdx = 0; fileIdx < m_allTranscriptionResults.size(); ++fileIdx) {
        const auto& result = m_allTranscriptionResults[fileIdx];
        std::string sourceFile = (fileIdx < m_audioPaths.size()) ? m_audioPaths[fileIdx] : "";

        // Derive character name from audio filename (like Python's _extract_character_name)
        std::string charName;
        if (!sourceFile.empty()) {
            charName = extractCharacterName(sourceFile);
        }

        for (const auto& seg : result.segments) {
            SyncClip clip;
            clip.id         = globalId++;
            clip.sourceFile = sourceFile;
            clip.start      = seg.start;
            clip.end        = seg.end;
            clip.transcript = seg.text;
            clip.editedText = seg.text;
            // Use segment character if set, otherwise derive from filename
            clip.character  = seg.character.empty() ? charName : seg.character;
            m_clips.push_back(std::move(clip));
        }
    }

    spdlog::info("AudioSync: Created {} clips from {} file(s)",
                 m_clips.size(), m_allTranscriptionResults.size());
}

void AudioSync::appendClipsFromNewTranscriptions()
{
    // Build a set of source files that already have clips so we don't
    // duplicate them.  This preserves existing clips with their
    // matchState, scriptLineNumber, character, confidence, etc.
    std::unordered_set<std::string> existingSourceFiles;
    for (const auto& c : m_clips)
        existingSourceFiles.insert(c.sourceFile);

    int nextId = m_clips.empty() ? 0 : m_clips.back().id + 1;

    size_t added = 0;
    for (size_t fileIdx = 0; fileIdx < m_allTranscriptionResults.size(); ++fileIdx) {
        const auto& result = m_allTranscriptionResults[fileIdx];
        std::string sourceFile = (fileIdx < m_audioPaths.size()) ? m_audioPaths[fileIdx] : "";

        // Skip files that already have clips (already transcribed + synced)
        if (existingSourceFiles.count(sourceFile))
            continue;

        std::string charName;
        if (!sourceFile.empty())
            charName = extractCharacterName(sourceFile);

        for (const auto& seg : result.segments) {
            SyncClip clip;
            clip.id         = nextId++;
            clip.sourceFile = sourceFile;
            clip.start      = seg.start;
            clip.end        = seg.end;
            clip.transcript = seg.text;
            clip.editedText = seg.text;
            clip.character  = seg.character.empty() ? charName : seg.character;
            m_clips.push_back(std::move(clip));
            ++added;
        }
    }

    spdlog::info("AudioSync: Appended {} new clips (preserved {} existing)",
                 added, existingSourceFiles.size());
}

void AudioSync::mergeSegmentsToMatchScript()
{
    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    // Post-transcription merge ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â mirrors Python _merge_segments_to_match_script().
    // Consecutive clips from the same character are merged when the combined
    // text matches a script line significantly better than each clip alone.
    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    if (m_clips.empty() || !m_script || m_script->lines.empty()) return;

    auto normalizeChar = [](const std::string& name) -> std::string {
        std::string lower;
        for (char c : name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // trim
        while (!lower.empty() && lower.back() == ' ') lower.pop_back();
        while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
        if (lower == "marian") return "modernia";
        return lower;
    };

    auto norm = [](const std::string& text) {
        return ScriptMatcher::normalize(text);
    };

    // Group clips by character
    std::unordered_map<std::string, std::vector<size_t>> clipsByChar;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        std::string ch = normalizeChar(m_clips[i].character);
        clipsByChar[ch].push_back(i);
    }

    // Group script lines by character
    std::unordered_map<std::string, std::vector<size_t>> linesByChar;
    for (size_t i = 0; i < m_script->lines.size(); ++i) {
        std::string ch = normalizeChar(m_script->lines[i].character);
        linesByChar[ch].push_back(i);
    }

    std::unordered_set<int> clipsToRemove;
    int mergedCount = 0;

    for (auto& [charKey, clipIndices] : clipsByChar) {
        auto lineIt = linesByChar.find(charKey);
        if (lineIt == linesByChar.end()) continue;
        const auto& lineIndices = lineIt->second;

        // Sort clips by start time
        std::sort(clipIndices.begin(), clipIndices.end(),
                  [&](size_t a, size_t b) { return m_clips[a].start < m_clips[b].start; });

        // Pre-normalize all script lines for this character
        std::vector<std::string> normLines;
        normLines.reserve(lineIndices.size());
        for (size_t li : lineIndices)
            normLines.push_back(norm(m_script->lines[li].dialogue));

        size_t i = 0;
        while (i < clipIndices.size()) {
            size_t ci = clipIndices[i];
            const auto& clip = m_clips[ci];
            std::string clipText = norm(clip.editedText.empty() ? clip.transcript : clip.editedText);

            // Best single-clip score against any script line
            float bestSingleScore = 0.0f;
            for (const auto& lt : normLines) {
                float s = ScriptMatcher::sequenceRatio(clipText, lt);
                if (s > bestSingleScore) bestSingleScore = s;
            }

            // Try merging with next 1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“10 clips (extended for long monologues)
            std::string mergedText = clipText;
            float bestMergeScore = bestSingleScore;
            size_t bestMergeCount = 0;
            size_t lastMergedIdx = ci;

            for (size_t j = 1; j <= 10 && i + j < clipIndices.size(); ++j) {
                size_t nextCi = clipIndices[i + j];
                const auto& nextClip = m_clips[nextCi];

                // Merge if clips are close together ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 2.5s gap tolerance for monologues
                if (nextClip.start - m_clips[lastMergedIdx].end > 2.5)
                    break;

                lastMergedIdx = nextCi;
                std::string nextText = norm(
                    nextClip.editedText.empty() ? nextClip.transcript : nextClip.editedText);
                mergedText = mergedText + " " + nextText;

                // Check if merged text matches any line better
                for (const auto& lt : normLines) {
                    float s = ScriptMatcher::sequenceRatio(mergedText, lt);
                    if (s > bestMergeScore + 0.05f) { // Lower threshold for long passages
                        bestMergeScore = s;
                        bestMergeCount = j;
                    }
                }
            }

            if (bestMergeCount > 0 && bestMergeScore > bestSingleScore + 0.05f) {
                // Merge clips: combine text and time range into first clip
                auto& firstClip = m_clips[ci];
                std::string combined;
                for (size_t j = 0; j <= bestMergeCount; ++j) {
                    size_t idx = clipIndices[i + j];
                    const auto& c = m_clips[idx];
                    std::string t = c.editedText.empty() ? c.transcript : c.editedText;
                    // Trim
                    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                    if (!combined.empty()) combined += ' ';
                    combined += t;
                }
                firstClip.editedText = combined;
                firstClip.transcript = combined;
                firstClip.end = m_clips[clipIndices[i + bestMergeCount]].end;

                // Mark merged clips for removal
                for (size_t j = 1; j <= bestMergeCount; ++j)
                    clipsToRemove.insert(m_clips[clipIndices[i + j]].id);

                spdlog::debug("AudioSync: Merged {} clips for '{}': score {:.2f} -> {:.2f}",
                              bestMergeCount + 1, charKey, bestSingleScore, bestMergeScore);
                ++mergedCount;
                i += bestMergeCount + 1;
            } else {
                ++i;
            }
        }
    }

    // Remove merged clips
    if (!clipsToRemove.empty()) {
        m_clips.erase(
            std::remove_if(m_clips.begin(), m_clips.end(),
                           [&](const SyncClip& c) { return clipsToRemove.count(c.id) > 0; }),
            m_clips.end());
        // Re-number IDs
        for (size_t i = 0; i < m_clips.size(); ++i)
            m_clips[i].id = static_cast<int>(i);

        spdlog::info("AudioSync: Merged {} segment groups, removed {} clips (now {} clips)",
                     mergedCount, clipsToRemove.size(), m_clips.size());
    }
}

void AudioSync::resegmentByScript()
{
    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    // Script-guided re-segmentation: split clips whose transcript spans
    // multiple script lines.  Whisper often produces one long segment that
    // actually covers 2-3 script lines ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â this step re-cuts those clips so
    // each new clip aligns with a single script line.
    //
    // The algorithm iterates up to 3 rounds so that a segment spanning 3+
    // lines is progressively split (first into 2, then the longer half
    // may split again).
    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    if (m_clips.empty() || !m_script || m_script->lines.empty()) return;

    auto normalizeChar = [](const std::string& name) -> std::string {
        std::string lower;
        for (char c : name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!lower.empty() && lower.back() == ' ') lower.pop_back();
        while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
        if (lower == "marian") return "modernia";
        return lower;
    };

    auto norm = [](const std::string& text) {
        return ScriptMatcher::normalize(text);
    };

    int totalSplits = 0;

    // Iterate up to 3 rounds to handle multi-line segments
    for (int round = 0; round < 3; ++round) {
        // Group clips by character
        std::unordered_map<std::string, std::vector<size_t>> clipsByChar;
        for (size_t i = 0; i < m_clips.size(); ++i)
            clipsByChar[normalizeChar(m_clips[i].character)].push_back(i);

        // Group script lines by character
        std::unordered_map<std::string, std::vector<size_t>> linesByChar;
        for (size_t i = 0; i < m_script->lines.size(); ++i)
            linesByChar[normalizeChar(m_script->lines[i].character)].push_back(i);

        // Collect splits to apply
        struct SplitOp {
            size_t clipIdx;     // index in m_clips
            size_t splitWord;   // split after this many words
        };
        std::vector<SplitOp> splits;

        for (auto& [charKey, clipIndices] : clipsByChar) {
            auto lineIt = linesByChar.find(charKey);
            if (lineIt == linesByChar.end() || lineIt->second.empty()) continue;
            const auto& lineIndices = lineIt->second;

            // Pre-normalize script lines
            std::vector<std::string> normLines;
            normLines.reserve(lineIndices.size());
            for (size_t li : lineIndices)
                normLines.push_back(norm(m_script->lines[li].dialogue));

            for (size_t ci : clipIndices) {
                const auto& clip = m_clips[ci];
                if (clip.matchState == 2) continue; // confirmed ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â don't touch

                std::string clipText = norm(
                    clip.editedText.empty() ? clip.transcript : clip.editedText);

                // Split clip text into words
                std::vector<std::string> words;
                { std::istringstream ss(clipText); std::string w; while (ss >> w) words.push_back(w); }

                if (words.size() < 4) continue; // too short to split meaningfully

                // Best single-line match score
                float bestSingleScore = 0.0f;
                for (const auto& lt : normLines) {
                    float s = ScriptMatcher::sequenceRatio(clipText, lt);
                    bestSingleScore = std::max(bestSingleScore, s);
                }
                if (bestSingleScore >= 0.85f) continue; // already matches well

                // Try every split point: find the split that produces two halves
                // where the LONGER half matches a script line better than the
                // whole clip does.  We weight each half's score by its proportion
                // of the total word count so short garbage prefixes/suffixes
                // don't dominate.
                float bestSplitScore = -1.0f;
                size_t bestSplitPoint = 0;

                for (size_t sp = 2; sp <= words.size() - 2; ++sp) {
                    // Build both halves
                    std::string half1, half2;
                    for (size_t w = 0; w < sp; ++w) {
                        if (!half1.empty()) half1 += ' ';
                        half1 += words[w];
                    }
                    for (size_t w = sp; w < words.size(); ++w) {
                        if (!half2.empty()) half2 += ' ';
                        half2 += words[w];
                    }

                    // Find best matching script line for each half
                    float bestH1 = 0.0f, bestH2 = 0.0f;
                    size_t bestL1 = SIZE_MAX, bestL2 = SIZE_MAX;
                    for (size_t li = 0; li < normLines.size(); ++li) {
                        float s1 = ScriptMatcher::sequenceRatio(half1, normLines[li]);
                        float s2 = ScriptMatcher::sequenceRatio(half2, normLines[li]);
                        if (s1 > bestH1) { bestH1 = s1; bestL1 = li; }
                        if (s2 > bestH2) { bestH2 = s2; bestL2 = li; }
                    }

                    // At least the better half must match well (> 0.50)
                    float bestHalf = std::max(bestH1, bestH2);
                    if (bestHalf < 0.50f) continue;

                    // Weight-averaged score: each half weighted by word proportion
                    float frac1 = static_cast<float>(sp) / static_cast<float>(words.size());
                    float weighted = bestH1 * frac1 + bestH2 * (1.0f - frac1);

                    // Accept if weighted score beats single AND the better half
                    // improves over the single-clip match
                    if (weighted > bestSingleScore && bestHalf > bestSingleScore + 0.02f
                        && weighted > bestSplitScore) {
                        bestSplitScore = weighted;
                        bestSplitPoint = sp;
                    }
                }

                if (bestSplitPoint > 0) {
                    splits.push_back({ci, bestSplitPoint});
                    spdlog::debug("AudioSync: Splitting clip {} at word {} "
                                  "(single={:.2f} split={:.2f})",
                                  ci, bestSplitPoint, bestSingleScore, bestSplitScore);
                }
            }
        }

        if (splits.empty()) break; // no more splits needed

        // Apply splits in reverse order so indices stay valid
        std::sort(splits.begin(), splits.end(),
                  [](const SplitOp& a, const SplitOp& b) {
                      return a.clipIdx > b.clipIdx;
                  });

        for (const auto& op : splits) {
            auto& clip = m_clips[op.clipIdx];
            std::string text = clip.editedText.empty() ? clip.transcript : clip.editedText;

            // Split the text by words
            std::vector<std::string> words;
            { std::istringstream ss(ScriptMatcher::normalize(text)); std::string w; while (ss >> w) words.push_back(w); }

            // Build first half text (use original-ish words from normalized)
            std::string h1, h2;
            for (size_t w = 0; w < op.splitWord; ++w) {
                if (!h1.empty()) h1 += ' ';
                h1 += words[w];
            }
            for (size_t w = op.splitWord; w < words.size(); ++w) {
                if (!h2.empty()) h2 += ' ';
                h2 += words[w];
            }

            // Estimate split time proportionally, then refine to nearest silence
            double totalWords = static_cast<double>(words.size());
            double splitFrac = static_cast<double>(op.splitWord) / totalWords;
            double splitTimeEst = clip.start + (clip.end - clip.start) * splitFrac;
            double splitTime = splitTimeEst;

            // Refine: search ±0.5s around the estimate for the quietest
            // 20ms window in the audio — that's the natural breath/pause.
            auto sampIt = m_audioSamples.find(clip.sourceFile);
            if (sampIt != m_audioSamples.end()) {
                const auto& ad = sampIt->second;
                int sr = static_cast<int>(ad.sampleRate);
                int winSz = static_cast<int>(0.020 * sr);
                int center = static_cast<int>(splitTimeEst * sr);
                int searchRadius = static_cast<int>(0.5 * sr);
                int lo = std::max(static_cast<int>(clip.start * sr) + winSz, center - searchRadius);
                int hi = std::min(static_cast<int>(clip.end * sr) - winSz, center + searchRadius);
                float bestRms = 1e9f;
                int bestPos = center;
                for (int pos = lo; pos + winSz <= hi; pos += winSz / 2) {
                    float sumSq = 0.0f;
                    for (int j = 0; j < winSz && static_cast<size_t>(pos + j) < ad.samples.size(); ++j) {
                        float s = ad.samples[static_cast<size_t>(pos + j)];
                        sumSq += s * s;
                    }
                    float rms = std::sqrt(sumSq / static_cast<float>(winSz));
                    if (rms < bestRms) { bestRms = rms; bestPos = pos; }
                }
                splitTime = static_cast<double>(bestPos) / sr;
                if (splitTime < clip.start + 0.1) splitTime = clip.start + 0.1;
                if (splitTime > clip.end - 0.1) splitTime = clip.end - 0.1;
            }

            // Create second clip (insert after current)
            SyncClip clip2;
            clip2.id         = 0; // will renumber later
            clip2.sourceFile = clip.sourceFile;
            clip2.character  = clip.character;
            clip2.start      = splitTime;
            clip2.end        = clip.end;
            clip2.transcript = h2;
            clip2.editedText = h2;
            clip2.matchState = 0;
            clip2.confidence = 0.0f;
            clip2.scriptLineNumber = -1;

            // Trim current clip
            clip.end        = splitTime;
            clip.transcript = h1;
            clip.editedText = h1;

            // Insert clip2 after current clip
            m_clips.insert(m_clips.begin() + static_cast<int>(op.clipIdx) + 1, clip2);
            ++totalSplits;
        }

        // Re-number IDs
        for (size_t i = 0; i < m_clips.size(); ++i)
            m_clips[i].id = static_cast<int>(i);
    }

    if (totalSplits > 0)
        spdlog::info("AudioSync: Re-segmentation split {} clips (now {} clips)",
                     totalSplits, m_clips.size());
}

void AudioSync::openManualMatch(int lineNumber)
{
    if (!m_script || m_audioPaths.empty()) return;

    // Find the script line
    std::string character, dialogue;
    for (const auto& sl : m_script->lines) {
        if (sl.lineNumber == lineNumber) {
            character = sl.character;
            dialogue  = sl.dialogue;
            break;
        }
    }

    // Collect per-file confirmed & tentative regions
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> confirmedByFile;
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> tentativeByFile;
    for (const auto& clip : m_clips) {
        if (clip.scriptLineNumber < 0) continue;
        if (clip.matchState == 2)
            confirmedByFile[clip.sourceFile].emplace_back(clip.start, clip.end);
        else if (clip.matchState == 1)
            tentativeByFile[clip.sourceFile].emplace_back(clip.start, clip.end);
    }

    // Build audio data map using same type as ManualMatchDialog expects
    std::unordered_map<std::string, ManualMatchDialog::AudioData> audioDataMap;
    for (const auto& [path, sampleData] : m_audioSamples) {
        audioDataMap[path] = { sampleData.samples, sampleData.sampleRate };
    }

    // Pre-select audio file: user-assigned file takes priority,
    // otherwise fall back to character name matching.
    std::string preselectedFile;
    auto assignIt = m_lineAudioFile.find(lineNumber);
    if (assignIt != m_lineAudioFile.end() && !assignIt->second.empty()) {
        preselectedFile = assignIt->second;
    } else {
        for (const auto& p : m_audioPaths) {
            std::string charFromFile = extractCharacterName(p);
            std::string charLower = character;
            std::transform(charLower.begin(), charLower.end(), charLower.begin(), ::tolower);
            std::transform(charFromFile.begin(), charFromFile.end(), charFromFile.begin(), ::tolower);
            if (charLower == charFromFile) {
                preselectedFile = p;
                break;
            }
        }
    }
    if (preselectedFile.empty() && !m_audioPaths.empty())
        preselectedFile = m_audioPaths[0];

    ManualMatchDialog dlg(
        character, dialogue, lineNumber,
        m_audioPaths, audioDataMap,
        {}, {},  // global regions not used ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â per-file via m_confirmedByFile
        preselectedFile,
        m_audioEngine,
        this);

    // Set per-file region maps, then load initial file (must be after regions)
    dlg.m_confirmedByFile = confirmedByFile;
    dlg.m_tentativeByFile = tentativeByFile;
    dlg.loadInitialFile();

    m_manualMatchOpen = true;
    int dialogResult = dlg.exec();
    m_manualMatchOpen = false;

    if (dialogResult == QDialog::Accepted) {
        auto result = dlg.result();

        // Check if this line already has a clip ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â update it
        bool found = false;
        for (auto& clip : m_clips) {
            if (clip.scriptLineNumber == lineNumber) {
                clip.sourceFile = result.audioFile;
                clip.start      = result.start;
                clip.end        = result.end;
                clip.matchState = 2;
                clip.confidence = 1.0f;
                found = true;
                break;
            }
        }

        // Otherwise create a new clip
        if (!found) {
            SyncClip newClip;
            newClip.id              = static_cast<int>(m_clips.size());
            newClip.sourceFile      = result.audioFile;
            newClip.character       = character;
            newClip.start           = result.start;
            newClip.end             = result.end;
            newClip.matchState      = 2;
            newClip.confidence      = 1.0f;
            newClip.scriptLineNumber = lineNumber;
            newClip.scriptSegment   = character + ": " + dialogue;
            m_clips.push_back(std::move(newClip));
        }

        spdlog::info("AudioSync: Manual match for line {} ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ {}  {:.3f}-{:.3f}",
                      lineNumber, result.audioFile, result.start, result.end);
        // Defer heavy UI rebuild so the dialog closes instantly
        // For existing clips, do lightweight in-place UI update;
        // for new clips, full rebuild since card count changed.
        if (found) {
            QTimer::singleShot(0, this, [this, lineNumber]() {
                // Find the clip index for this line
                for (size_t ci = 0; ci < m_clips.size(); ++ci) {
                    if (m_clips[ci].scriptLineNumber == lineNumber) {
                        updateCardMatchStyle(ci);
                        // Update waveform widget if it exists
                        for (size_t wi = 0; wi < m_cardScriptLineNums.size(); ++wi) {
                            if (m_cardScriptLineNums[wi] == lineNumber && wi < m_cardWaveforms.size()) {
                                if (auto* wv = m_cardWaveforms[wi]) {
                                    auto sit = m_audioSamples.find(m_clips[ci].sourceFile);
                                    if (sit != m_audioSamples.end()) {
                                        wv->setAudioShared(
                                            &sit->second.samples,
                                            sit->second.sampleRate,
                                            m_clips[ci].start,
                                            m_clips[ci].end);
                                        wv->update();
                                    }
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
                populateLeftList();
                updateSmartBar();
            });
        } else {
            QTimer::singleShot(0, this, [this]() {
                populateCards();
            });
        }
    }
}


// Split/merge + persistence methods moved to AudioSyncState.cpp


// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
//  Script history (recent URLs dropdown)
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â


} // namespace rt
