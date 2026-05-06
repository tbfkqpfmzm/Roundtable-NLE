/*
 * AudioSyncRelink.cpp - Re-link imported audio files while preserving sync data.
 */

#include "panels/audio/AudioSync.h"

#include <spdlog/spdlog.h>

#include <QFileDialog>
#include <QFileInfo>
#include <QListWidgetItem>
#include <QScrollBar>
#include <QTimer>

namespace rt {

void AudioSync::relinkAudioFile(int fileIdx)
{
    if (fileIdx < 0 || static_cast<size_t>(fileIdx) >= m_audioPaths.size()) return;

    int importScrollPos = (m_audioFileList && m_audioFileList->verticalScrollBar())
        ? m_audioFileList->verticalScrollBar()->value()
        : 0;
    int transcribeScrollPos = (m_transcribeFileList && m_transcribeFileList->verticalScrollBar())
        ? m_transcribeFileList->verticalScrollBar()->value()
        : 0;

    QList<int> selectedImportRows;
    if (m_audioFileList) {
        const auto selectedItems = m_audioFileList->selectedItems();
        selectedImportRows.reserve(selectedItems.size());
        for (auto* item : selectedItems)
            selectedImportRows.push_back(m_audioFileList->row(item));
    }

    QString oldPath = QString::fromStdString(m_audioPaths[static_cast<size_t>(fileIdx)]);
    QString oldDir = QFileInfo(oldPath).absolutePath();
    QString oldName = QFileInfo(oldPath).fileName();

    QString newPath = QFileDialog::getOpenFileName(
        this,
        tr("Re-link: %1").arg(oldName),
        oldDir,
        tr("Audio Files (*.wav *.mp3 *.flac *.aac *.m4a *.ogg *.aiff *.aif);;All Files (*.*)"));
    if (newPath.isEmpty()) return;

    std::string newPathStr = newPath.toStdString();
    std::string oldPathStr = oldPath.toStdString();
    if (newPathStr == oldPathStr) return;

    m_audioPaths[static_cast<size_t>(fileIdx)] = newPathStr;

    int relinkCount = 0;
    for (auto& clip : m_clips) {
        if (clip.sourceFile == oldPathStr) {
            clip.sourceFile = newPathStr;
            ++relinkCount;
        }
    }

    auto samplesIt = m_audioSamples.find(oldPathStr);
    if (samplesIt != m_audioSamples.end()) {
        m_audioSamples[newPathStr] = std::move(samplesIt->second);
        m_audioSamples.erase(samplesIt);
    }

    spdlog::info("AudioSync: Re-linked file [{}] '{}' -> '{}' ({} clips updated)",
                 fileIdx, oldPathStr, newPathStr, relinkCount);

    m_audioFileList->clear();
    m_fileWaveforms.clear();
    m_filePlayBtns.clear();
    m_fileTimeLabels.clear();
    for (const auto& path : m_audioPaths)
        addAudioFileListItem(QString::fromStdString(path));

    if (m_audioFileList) {
        m_audioFileList->clearSelection();
        for (int row : selectedImportRows) {
            if (auto* item = m_audioFileList->item(row))
                item->setSelected(true);
        }
        if (auto* item = m_audioFileList->item(fileIdx))
            m_audioFileList->setCurrentItem(item);
    }

    refreshTranscribeFileList();
    if (m_script)
        populateCards();

    QTimer::singleShot(0, this, [this, importScrollPos, transcribeScrollPos]() {
        if (m_audioFileList && m_audioFileList->verticalScrollBar())
            m_audioFileList->verticalScrollBar()->setValue(importScrollPos);
        if (m_transcribeFileList && m_transcribeFileList->verticalScrollBar())
            m_transcribeFileList->verticalScrollBar()->setValue(transcribeScrollPos);
    });
}

} // namespace rt
