/*
 * AudioSyncTranscriptionClear.cpp - Clear per-file and all transcription results.
 */

#include "panels/audio/AudioSync.h"

#include "ai/Transcriber.h"

#include <QString>

#include <algorithm>

namespace rt {

void AudioSync::clearTranscriptionForFile(size_t fileIndex)
{
    if (fileIndex >= m_allTranscriptionResults.size()) return;

    m_allTranscriptionResults[fileIndex] = TranscriptionResult{};

    const std::string& path = m_audioPaths[fileIndex];
    auto it = std::remove_if(m_clips.begin(), m_clips.end(),
        [&path](const SyncClip& clip) {
            return clip.sourceFile == path;
        });
    m_clips.erase(it, m_clips.end());

    refreshTranscribeFileList();
    populateClipList();
    populateLeftList();
    updateWorkflowState();
    m_transcribeStatus->setText(
        QString("Cleared transcription for file %1").arg(fileIndex + 1));
}

void AudioSync::clearAllTranscriptions()
{
    if (m_allTranscriptionResults.empty()) return;

    for (auto& result : m_allTranscriptionResults)
        result = TranscriptionResult{};

    m_clips.clear();

    refreshTranscribeFileList();
    populateClipList();
    populateLeftList();
    updateWorkflowState();
    m_transcribeStatus->setText("All transcriptions cleared");
}

} // namespace rt
