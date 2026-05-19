/*
 * AudioSyncManualMatch.cpp - Manual script/audio matching dialog integration.
 */

#include "panels/audio/AudioSync.h"

#include "ai/ScriptMatcher.h"
#include "panels/audio/AudioSyncFileName.h"
#include "widgets/MiniWaveformWidget.h"
#include "widgets/ManualMatchDialog.h"

#include <spdlog/spdlog.h>

#include <QDialog>
#include <QTimer>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

void AudioSync::openManualMatch(int lineNumber)
{
    if (!m_script || m_audioPaths.empty()) return;

    std::string character, dialogue;
    for (const auto& scriptLine : m_script->lines) {
        if (scriptLine.lineNumber == lineNumber) {
            character = scriptLine.character;
            dialogue = scriptLine.dialogue;
            break;
        }
    }

    std::unordered_map<std::string, std::vector<std::pair<double, double>>> confirmedByFile;
    std::unordered_map<std::string, std::vector<std::pair<double, double>>> tentativeByFile;
    for (const auto& clip : m_clips) {
        if (clip.scriptLineNumber < 0) continue;
        if (clip.matchState == 2)
            confirmedByFile[clip.sourceFile].emplace_back(clip.start, clip.end);
        else if (clip.matchState == 1)
            tentativeByFile[clip.sourceFile].emplace_back(clip.start, clip.end);
    }

    std::unordered_map<std::string, ManualMatchDialog::AudioData> audioDataMap;
    for (const auto& [path, sampleData] : m_audioSamples)
        audioDataMap[path] = {sampleData.samples, sampleData.sampleRate};

    std::string preselectedFile;
    auto assignIt = m_lineAudioFile.find(lineNumber);
    if (assignIt != m_lineAudioFile.end() && !assignIt->second.empty()) {
        preselectedFile = assignIt->second;
    } else {
        for (const auto& path : m_audioPaths) {
            std::string charFromFile = extractCharacterName(path);
            std::string charLower = character;
            std::transform(charLower.begin(), charLower.end(), charLower.begin(), ::tolower);
            std::transform(charFromFile.begin(), charFromFile.end(), charFromFile.begin(), ::tolower);
            if (charLower == charFromFile) {
                preselectedFile = path;
                break;
            }
        }
    }
    if (preselectedFile.empty() && !m_audioPaths.empty())
        preselectedFile = m_audioPaths[0];

    ManualMatchDialog dialog(
        character, dialogue, lineNumber,
        m_audioPaths, audioDataMap,
        {}, {},
        preselectedFile,
        m_audioEngine,
        this);

    dialog.m_confirmedByFile = confirmedByFile;
    dialog.m_tentativeByFile = tentativeByFile;
    dialog.loadInitialFile();

    m_manualMatchOpen = true;
    int dialogResult = dialog.exec();
    m_manualMatchOpen = false;

    if (dialogResult == QDialog::Accepted) {
        auto result = dialog.result();

        // Always rebuild the full card set on undo/redo so an added-then-
        // removed clip cleanly disappears, and an updated clip's
        // waveform/text both refresh.  The forward path can still use
        // the lighter targeted-refresh below to avoid jank on accept.
        auto rebuild = [this]() {
            QTimer::singleShot(0, this, [this]() {
                if (m_destroying.load(std::memory_order_acquire)) return;
                populateCards();
                updateWorkflowState();
            });
        };

        runClipsMutationWithUndo(
            "Manual audio match",
            [this, lineNumber, result, character, dialogue]() {
                bool found = false;
                for (auto& clip : m_clips) {
                    if (clip.scriptLineNumber == lineNumber) {
                        clip.sourceFile = result.audioFile;
                        clip.start = result.start;
                        clip.end = result.end;
                        clip.matchState = 2;
                        clip.confidence = 1.0f;
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    SyncClip newClip;
                    newClip.id = static_cast<int>(m_clips.size());
                    newClip.sourceFile = result.audioFile;
                    newClip.character = character;
                    newClip.start = result.start;
                    newClip.end = result.end;
                    newClip.matchState = 2;
                    newClip.confidence = 1.0f;
                    newClip.scriptLineNumber = lineNumber;
                    newClip.scriptSegment = character + ": " + dialogue;
                    m_clips.push_back(std::move(newClip));
                }

                spdlog::info("AudioSync: Manual match for line {} -> {}  {:.3f}-{:.3f}",
                             lineNumber, result.audioFile, result.start, result.end);
            },
            rebuild);
    }
}

} // namespace rt