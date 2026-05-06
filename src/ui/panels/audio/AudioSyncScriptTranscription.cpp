/*
 * AudioSyncScriptTranscription.cpp - Script loading, audio import, and transcription entry points.
 */

#include "panels/audio/AudioSync.h"

#include "Theme.h"
#include "ai/ScriptMatcher.h"
#include "ai/Transcriber.h"

#include <spdlog/spdlog.h>

#include <QThread>

#include <exception>
#include <utility>

namespace rt {

TranscriptionWorker::TranscriptionWorker(Transcriber* transcriber, QObject* parent)
    : QObject(parent)
    , m_transcriber(transcriber)
{
}

void TranscriptionWorker::process()
{
    if (!m_transcriber) {
        emit errorOccurred("Transcriber not initialized");
        return;
    }

    auto progress = [this](float pct, const std::string& status) {
        emit progressChanged(pct, QString::fromStdString(status));
    };

    m_result = m_transcriber->transcribe(m_audioPath, m_language, progress);

    if (m_result.segments.empty() && !m_transcriber->lastError().empty()) {
        emit errorOccurred(QString::fromStdString(m_transcriber->lastError()));
        emit finished(false);
    } else {
        emit finished(true);
    }
}

bool AudioSync::loadScript(const std::string& pathOrContent)
{
    try {
        auto parsed = Script::load(pathOrContent);
        if (parsed.isEmpty()) {
            spdlog::warn("AudioSync: Script is empty");
            m_scriptStatus->setText("No lines found");
            return false;
        }

        m_script = std::make_unique<Script>(std::move(parsed));
        m_scriptLoaded = true;

        populateScriptFilter();
        if (!m_restoring) {
            populateScriptList();
            if (!m_clips.empty())
                populateClipList();
            updateWorkflowState();
        }

        m_scriptStatus->setText(QString("%1 lines, %2 characters")
            .arg(m_script->lineCount())
            .arg(m_script->characters.size()));

        spdlog::info("AudioSync: Loaded script with {} lines", m_script->lineCount());
        emit scriptLoaded(static_cast<int>(m_script->lineCount()));
        return true;
    } catch (const std::exception& e) {
        spdlog::error("AudioSync: Failed to load script: {}", e.what());
        m_scriptStatus->setText(QString("Error: %1").arg(e.what()));
        return false;
    }
}

bool AudioSync::importAudio(const std::string& audioPath)
{
    if (audioPath.empty()) return false;

    m_audioPath = audioPath;
    m_audioPaths.push_back(audioPath);
    m_audioImported = true;
    if (m_audioPathEdit) m_audioPathEdit->setText(QString::fromStdString(audioPath));
    m_audioStatus->setText(QString("%1 file(s)").arg(m_audioPaths.size()));

    if (m_audioFileList)
        addAudioFileListItem(QString::fromStdString(audioPath));

    loadAudioSamples();
    updateWorkflowState();

    spdlog::info("AudioSync: Imported audio: {}", audioPath);
    emit audioImported(QString::fromStdString(audioPath));
    return true;
}

void AudioSync::importAudioFiles(const QStringList& paths)
{
    for (const auto& path : paths)
        importAudio(path.toStdString());
}

void AudioSync::startTranscription()
{
    if (m_audioPaths.empty()) {
        m_transcribeStatus->setText("No audio files imported");
        return;
    }

    if (!m_transcriber->isModelLoaded()) {
        auto modelName = m_modelCombo->currentText().toStdString();
        auto modelSize = whisperModelFromName(modelName);
        if (!m_transcriber->loadModel(modelSize)) {
            m_transcribeStatus->setText("Failed to load model");
            return;
        }
    }

    if (m_allTranscriptionResults.size() < m_audioPaths.size())
        m_allTranscriptionResults.resize(m_audioPaths.size());

    m_pendingTranscriptionIndices.clear();
    for (size_t i = 0; i < m_audioPaths.size(); ++i) {
        if (m_allTranscriptionResults[i].segments.empty())
            m_pendingTranscriptionIndices.push_back(i);
    }

    if (m_pendingTranscriptionIndices.empty()) {
        m_transcribeStatus->setText("All files already transcribed");
        m_transcribeStatus->setStyleSheet(QString("color: %1;").arg(Theme::hex(Theme::colors().success)));
        return;
    }

    m_currentTranscriptionIndex = m_pendingTranscriptionIndices.front();

    m_transcribeStatus->setText(QString("Transcribing file 1/%1 (%2 already done)...")
        .arg(m_pendingTranscriptionIndices.size())
        .arg(m_audioPaths.size() - m_pendingTranscriptionIndices.size()));
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_transcribeBtn->setEnabled(false);

    emit transcriptionStarted();
    startTranscriptionForFile(m_currentTranscriptionIndex);
}

void AudioSync::startTranscriptionForFile(size_t index)
{
    if (index >= m_audioPaths.size()) return;

    m_audioPath = m_audioPaths[index];

    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
        delete m_workerThread;
    }
    delete m_worker;

    m_workerThread = new QThread(this);
    m_worker = new TranscriptionWorker(m_transcriber.get());
    m_worker->setAudioPath(m_audioPath);
    m_worker->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::started, m_worker, &TranscriptionWorker::process);
    connect(m_worker, &TranscriptionWorker::progressChanged,
            this, &AudioSync::onTranscriptionProgress);
    connect(m_worker, &TranscriptionWorker::finished,
            this, &AudioSync::onTranscriptionFinished);
    connect(m_worker, &TranscriptionWorker::errorOccurred,
            this, &AudioSync::onTranscriptionError);
    connect(m_worker, &TranscriptionWorker::finished,
            m_workerThread, &QThread::quit);

    m_workerThread->start();
}

int AudioSync::scriptLineCount() const
{
    return m_script ? static_cast<int>(m_script->lineCount()) : 0;
}

QStringList AudioSync::scriptCharacters() const
{
    QStringList result;
    if (m_script) {
        for (const auto& character : m_script->characters)
            result.append(QString::fromStdString(character));
    }
    return result;
}

} // namespace rt