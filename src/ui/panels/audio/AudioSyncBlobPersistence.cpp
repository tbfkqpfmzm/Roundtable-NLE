/*
 * AudioSyncBlobPersistence.cpp - Binary blob persistence for AudioSync.
 * Split from AudioSyncPersistence.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "project/BinaryIO.h"

#include <spdlog/spdlog.h>

#include <QFile>

#include <utility>
#include <vector>

namespace rt {

static constexpr uint32_t AUDIO_SYNC_BLOB_VERSION = 1;

std::vector<uint8_t> AudioSync::serializeToBlob() const
{
    BinaryWriter w;
    w.writeU32(AUDIO_SYNC_BLOB_VERSION);

    // Script source
    w.writeString(m_lastScriptSource.isEmpty() ? std::string{}
                  : m_lastScriptSource.toStdString());

    // Audio paths
    w.writeU32(static_cast<uint32_t>(m_audioPaths.size()));
    for (const auto& p : m_audioPaths)
        w.writeString(p);

    // Workflow flags
    w.writeU8(m_scriptLoaded      ? 1 : 0);
    w.writeU8(m_audioImported     ? 1 : 0);
    w.writeU8(m_transcriptionDone ? 1 : 0);
    w.writeU8(m_syncDone          ? 1 : 0);

    // Clips
    w.writeU32(static_cast<uint32_t>(m_clips.size()));
    for (const auto& c : m_clips) {
        w.writeU32(static_cast<uint32_t>(c.id));
        w.writeString(c.sourceFile);
        w.writeString(c.character);
        w.writeF64(c.start);
        w.writeF64(c.end);
        w.writeString(c.transcript);
        w.writeString(c.editedText);
        w.writeU32(static_cast<uint32_t>(c.matchState));
        w.writeF32(c.confidence);
        w.writeU32(static_cast<uint32_t>(c.scriptLineNumber));
        w.writeString(c.scriptSegment);

        // Deleted regions
        w.writeU32(static_cast<uint32_t>(c.deletedRegions.size()));
        for (const auto& [rs, re] : c.deletedRegions) {
            w.writeF64(rs);
            w.writeF64(re);
        }
    }

    spdlog::info("AudioSync::serializeToBlob: {} clips, {} bytes",
                 m_clips.size(), w.data().size());
    return w.data();
}

void AudioSync::deserializeFromBlob(const std::vector<uint8_t>& blob)
{
    if (blob.empty()) return;

    BinaryReader r(blob.data(), blob.size());

    uint32_t version = r.readU32();
    if (version > AUDIO_SYNC_BLOB_VERSION) {
        spdlog::warn("AudioSync::deserializeFromBlob: version {} > supported {}",
                     version, AUDIO_SYNC_BLOB_VERSION);
        return;
    }

    m_restoring = true;

    // Script source
    std::string scriptSource = r.readString();
    if (!scriptSource.empty()) {
        m_lastScriptSource = QString::fromStdString(scriptSource);
        if (m_scriptUrlCombo)
            m_scriptUrlCombo->setEditText(m_lastScriptSource);

        if (m_lastScriptSource.startsWith("http://") || m_lastScriptSource.startsWith("https://"))
            fetchScriptFromUrl(m_lastScriptSource);
        else
            loadScript(scriptSource);
    }

    // Audio paths
    uint32_t pathCount = r.readU32();
    m_audioPaths.clear();
    if (m_audioFileList) m_audioFileList->clear();
    m_fileWaveforms.clear();
    m_filePlayBtns.clear();
    m_fileTimeLabels.clear();
    for (uint32_t i = 0; i < pathCount; ++i) {
        std::string p = r.readString();
        if (!p.empty() && QFile::exists(QString::fromStdString(p)))
            m_audioPaths.push_back(std::move(p));
    }
    if (!m_audioPaths.empty()) {
        m_audioImported = true;
        m_audioPath = m_audioPaths.back();
        if (m_audioPathEdit)
            m_audioPathEdit->setText(QString::fromStdString(m_audioPath));
        m_audioStatus->setText(QString("%1 file(s) imported").arg(m_audioPaths.size()));
        loadAudioSamples();
        if (m_audioFileList) {
            for (const auto& ap : m_audioPaths)
                addAudioFileListItem(QString::fromStdString(ap));
        }
    }

    // Workflow flags
    m_scriptLoaded      = (r.readU8() != 0);
    m_audioImported     = (r.readU8() != 0);
    m_transcriptionDone = (r.readU8() != 0);
    m_syncDone          = (r.readU8() != 0);

    // Clips
    uint32_t clipCount = r.readU32();
    m_clips.clear();
    m_clips.reserve(clipCount);
    for (uint32_t i = 0; i < clipCount; ++i) {
        SyncClip c;
        c.id              = static_cast<int>(r.readU32());
        c.sourceFile      = r.readString();
        c.character       = r.readString();
        c.start           = r.readF64();
        c.end             = r.readF64();
        c.transcript      = r.readString();
        c.editedText      = r.readString();
        c.matchState      = static_cast<int>(r.readU32());
        c.confidence      = r.readF32();
        c.scriptLineNumber = static_cast<int>(r.readU32());
        c.scriptSegment   = r.readString();

        uint32_t delCount = r.readU32();
        c.deletedRegions.reserve(delCount);
        for (uint32_t d = 0; d < delCount; ++d) {
            double rs = r.readF64();
            double re = r.readF64();
            c.deletedRegions.emplace_back(rs, re);
        }
        m_clips.push_back(std::move(c));
    }

    // Rebuild transcription results from clips
    if (m_transcriptionDone && !m_audioPaths.empty()) {
        m_allTranscriptionResults.resize(m_audioPaths.size());
        for (size_t fi = 0; fi < m_audioPaths.size(); ++fi) {
            if (!m_allTranscriptionResults[fi].segments.empty())
                continue;
            std::string combined;
            for (const auto& c : m_clips) {
                if (c.sourceFile == m_audioPaths[fi] && !c.transcript.empty()) {
                    if (!combined.empty()) combined += ' ';
                    combined += c.transcript;
                }
            }
            if (!combined.empty()) {
                TranscriptionSegment seg;
                seg.text = std::move(combined);
                m_allTranscriptionResults[fi].segments.push_back(std::move(seg));
            }
        }
    }

    m_restoring = false;

    spdlog::info("AudioSync::deserializeFromBlob: restored {} clips", m_clips.size());

    updateWorkflowState();
    populateLeftList();
    if (m_script) populateCards();
    if (!m_clips.empty() && m_syncDone) showAudioSidePanel(3);
}

} // namespace rt
