/*
 * AudioSyncHandlers.cpp - UI event handlers and transcription callbacks for AudioSync.
 * Split from AudioSync.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "ai/Transcriber.h"
#include "ai/ScriptMatcher.h"
#include "media/AudioEngine.h"
#include "Theme.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <QCoreApplication>
#include <QDir>
#include <functional>
#include <QFileDialog>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSettings>

namespace rt {

void AudioSync::onLoadScriptClicked()
{
    QString text = m_scriptUrlCombo->currentText().trimmed();
    if (text.isEmpty()) {
        // Open file dialog
        text = QFileDialog::getOpenFileName(this, "Load Script",
            QString(), "Script Files (*.txt *.json *.html);;All Files (*)");
        if (text.isEmpty()) return;
        m_scriptUrlCombo->setEditText(text);
    }

    m_lastScriptSource = text;

    // Check if it's a URL
    if (text.startsWith("http://") || text.startsWith("https://")) {
        addToScriptHistory(text);
        fetchScriptFromUrl(text);
    } else {
        loadScript(text.toStdString());
    }
}

void AudioSync::fetchScriptFromUrl(const QString& url)
{
    QString docId;
    bool isGoogleDocs = url.contains("docs.google.com/document");
    if (isGoogleDocs) {
        QRegularExpression re(R"(/d/([^/\?]+))");
        auto match = re.match(url);
        if (match.hasMatch())
            docId = match.captured(1);
    }

    // Build a list of URLs to try in order
    QStringList urlsToTry;
    if (isGoogleDocs && !docId.isEmpty()) {
        // Try HTML export first (best for parsing), then plain text
        urlsToTry << QString("https://docs.google.com/document/d/%1/export?format=html").arg(docId)
                  << QString("https://docs.google.com/document/d/%1/export?format=txt").arg(docId);
    } else {
        urlsToTry << url;
    }

    m_scriptStatus->setText("Fetching script...");
    m_scriptStatus->setStyleSheet(QString("color: %1;").arg(Theme::hex(Theme::colors().textSecondary)));
    m_loadScriptBtn->setEnabled(false);

    struct FetchState {
        int attemptIndex = 0;
        QNetworkAccessManager* manager = nullptr;
    };
    auto* state = new FetchState;

    // Use std::function so the lambda can call itself recursively
    std::function<void()> tryNextUrl;
    tryNextUrl = [this, state, &urlsToTry, &tryNextUrl, docId, isGoogleDocs]() {
        if (state->attemptIndex >= urlsToTry.size()) {
            delete state;
            m_loadScriptBtn->setEnabled(true);
            return;
        }

        QString currentUrl = urlsToTry[state->attemptIndex];
        spdlog::info("Fetching script (attempt {}/{}): {}", state->attemptIndex + 1,
                     urlsToTry.size(), currentUrl.toStdString());

        state->manager = new QNetworkAccessManager(this);
        QNetworkRequest request{QUrl(currentUrl)};
        request.setRawHeader("User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36");

        auto* reply = state->manager->get(request);
        reply->setProperty("attemptIndex", state->attemptIndex);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, state, &urlsToTry, &tryNextUrl, isGoogleDocs]() {
            int attempt = reply->property("attemptIndex").toInt();
            reply->deleteLater();
            state->manager->deleteLater();
            state->manager = nullptr;

            if (reply->error() == QNetworkReply::NoError) {
                QByteArray data = reply->readAll();
                std::string content = data.toStdString();
                spdlog::info("Downloaded script: {} bytes", content.size());
                m_loadScriptBtn->setEnabled(true);
                loadScript(content);
                delete state;
                return;
            }

            // Authentication error for Google Docs — try next format
            if (reply->error() == QNetworkReply::AuthenticationRequiredError &&
                isGoogleDocs && attempt < urlsToTry.size() - 1)
            {
                spdlog::warn("Google Docs export requires auth, trying next format");
                state->attemptIndex = attempt + 1;
                tryNextUrl();
                return;
            }

            QString errMsg = reply->errorString();
            spdlog::error("Failed to fetch script from URL: {}", errMsg.toStdString());
            m_scriptStatus->setText(QString("Error: %1").arg(errMsg));
            m_scriptStatus->setStyleSheet(QString("color: %1;").arg(Theme::hex(Theme::colors().error)));
            m_loadScriptBtn->setEnabled(true);
            delete state;
        });
    };

    tryNextUrl();
}

void AudioSync::onImportAudioClicked()
{
    // Restore last used directory from QSettings
    QSettings settings("ROUNDTABLE", "NLE");
    QString lastDir = settings.value("AudioSync/lastImportDir", QString()).toString();

    // Fall back to member variable if QSettings is empty
    if (lastDir.isEmpty() && !m_lastImportDir.isEmpty())
        lastDir = m_lastImportDir;

    // Fall back to user's home directory if still empty
    if (lastDir.isEmpty())
        lastDir = QDir::homePath();

    QStringList paths = QFileDialog::getOpenFileNames(this, "Import Audio Files",
        lastDir, "Audio Files (*.wav *.mp3 *.flac *.ogg *.m4a);;All Files (*)");
    if (!paths.isEmpty()) {
        // Save the directory for next time
        QString dir = QFileInfo(paths.first()).absolutePath();
        m_lastImportDir = dir;
        settings.setValue("AudioSync/lastImportDir", dir);
        settings.sync();
        importAudioFiles(paths);
    }
}

void AudioSync::onTranscribeClicked()
{
    startTranscription();
}

void AudioSync::onAutoSyncClicked()
{
    // Show a progress dialog so the user knows sync is working
    m_syncProgress = new QProgressDialog("Syncing...", QString(), 0, 100, this);
    m_syncProgress->setWindowTitle("Auto-Sync");
    m_syncProgress->setWindowModality(Qt::WindowModal);
    m_syncProgress->setMinimumDuration(0);
    m_syncProgress->setValue(0);
    m_syncProgress->show();
    QCoreApplication::processEvents();

    runAutoSync();

    if (m_syncProgress) {
        m_syncProgress->setValue(100);
        m_syncProgress->close();
        m_syncProgress->deleteLater();
        m_syncProgress = nullptr;
    }
}

void AudioSync::onExportClicked()
{
    emit exportRequested();
}

// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Effective ranges (ported from Python _get_effective_ranges) ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬


void AudioSync::onTranscriptionProgress(float percent, const QString& status)
{
    float clampedPercent = std::clamp(percent, 0.0f, 100.0f);
    size_t runTotal = std::max<size_t>(m_transcriptionRunTotal, 1);
    double overall = (static_cast<double>(m_transcriptionRunCompleted)
                    + static_cast<double>(clampedPercent) / 100.0)
                    / static_cast<double>(runTotal) * 100.0;
    m_progressBar->setValue(static_cast<int>(std::clamp(overall, 0.0, 99.0)));

    size_t fileNumber = std::min(m_transcriptionRunCompleted + 1, runTotal);
    m_transcribeStatus->setText(QString("File %1/%2: %3")
        .arg(fileNumber)
        .arg(runTotal)
        .arg(status));
    emit transcriptionProgress(percent, status);
}

void AudioSync::onTranscriptionFinished(bool success)
{
    if (success && m_worker) {
        // Store the result for this file
        if (m_currentTranscriptionIndex < m_allTranscriptionResults.size())
            m_allTranscriptionResults[m_currentTranscriptionIndex] = m_worker->result();
        refreshTranscribeFileList();
    }

    if (m_transcriptionRunCompleted < m_transcriptionRunTotal)
        ++m_transcriptionRunCompleted;

    // Remove the just-completed index from the pending list
    auto it = std::find(m_pendingTranscriptionIndices.begin(),
                        m_pendingTranscriptionIndices.end(),
                        m_currentTranscriptionIndex);
    if (it != m_pendingTranscriptionIndices.end())
        m_pendingTranscriptionIndices.erase(it);

    if (!m_pendingTranscriptionIndices.empty()) {
        // More files to transcribe ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â pick the next pending one
        m_currentTranscriptionIndex = m_pendingTranscriptionIndices.front();
        m_transcribeStatus->setText(QString("Transcribing file %1/%2...")
            .arg(std::min(m_transcriptionRunCompleted + 1, m_transcriptionRunTotal))
            .arg(std::max<size_t>(m_transcriptionRunTotal, 1)));
        startTranscriptionForFile(m_currentTranscriptionIndex);
        return;
    }

    // All files done ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â finalize
    m_progressBar->setValue(100);
    m_progressBar->setVisible(false);
    m_transcribeBtn->setEnabled(true);
    m_transcriptionRunCompleted = 0;
    m_transcriptionRunTotal = 0;

    if (success) {
        // Only create clips for NEWLY transcribed files — preserve existing
        // clips (which may have matchState, scriptLineNumber, character, etc.)
        appendClipsFromNewTranscriptions();

        // Post-process: merge short segments that match script lines better combined
        if (m_script && !m_script->lines.empty())
            mergeSegmentsToMatchScript();

        if (m_clips.empty()) {
            m_transcribeStatus->setText(
                "Transcription unavailable ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â whisper.cpp is not compiled in.\n"
                "To enable: install whisper.cpp, set ROUNDTABLE_HAS_WHISPER=ON in CMake,\n"
                "and place ggml-base.bin (or another model) in the models/ directory.");
            m_transcribeStatus->setStyleSheet(QString("color: %1; font-size: 12px;").arg(Theme::hex(Theme::colors().warning)));
            m_transcribeStatus->setWordWrap(true);
            spdlog::warn("AudioSync: Transcription returned 0 segments "
                         "(whisper may not be compiled in)");
        } else {
            m_transcriptionDone = true;
            populateClipList();
            updateWorkflowState();

            m_transcribeStatus->setText(
                QString("Transcribed %1 segments from %2 file(s)")
                    .arg(m_clips.size()).arg(m_audioPaths.size()));
            m_transcribeStatus->setStyleSheet(QString("color: %1;").arg(Theme::hex(Theme::colors().success)));
            emit transcriptionFinished(static_cast<int>(m_clips.size()));
        }
    }
}

void AudioSync::onTranscriptionError(const QString& error)
{
    m_progressBar->setVisible(false);
    m_transcribeBtn->setEnabled(true);
    m_transcriptionRunCompleted = 0;
    m_transcriptionRunTotal = 0;
    m_transcribeStatus->setText("Error: " + error);
    emit transcriptionFailed(error);
}

// ─── Clear transcription ──────────────────────────────────────────────────────

void AudioSync::clearTranscriptionForFile(size_t fileIndex)
{
    if (fileIndex >= m_allTranscriptionResults.size()) return;

    m_allTranscriptionResults[fileIndex] = TranscriptionResult{};

    // Remove clips that came from this file
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

    for (auto& r : m_allTranscriptionResults)
        r = TranscriptionResult{};

    m_clips.clear();

    refreshTranscribeFileList();
    populateClipList();
    populateLeftList();
    updateWorkflowState();
    m_transcribeStatus->setText("All transcriptions cleared");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Standalone gap closing
// ═════════════════════════════════════════════════════════════════════════════

void AudioSync::closeInterClipGaps()
{
    if (m_clips.empty()) return;

    // First pass: auto-trim silence from every clip (skip confirmed)
    int trimCount = 0;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        auto& clip = m_clips[i];
        if (clip.matchState == 2) continue; // confirmed — don't touch
        auto it = m_audioSamples.find(clip.sourceFile);
        if (it == m_audioSamples.end()) continue;

        const auto& audioData = it->second;
        const int windowSize = static_cast<int>(0.010 * audioData.sampleRate); // 10ms
        auto startSample = static_cast<int>(clip.start * audioData.sampleRate);
        auto endSample   = static_cast<int>(clip.end * audioData.sampleRate);
        if (startSample >= static_cast<int>(audioData.samples.size())) continue;
        if (endSample > static_cast<int>(audioData.samples.size()))
            endSample = static_cast<int>(audioData.samples.size());
        if (endSample - startSample < windowSize * 3) continue;

        // Measure RMS per window to build energy profile
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
        auto sorted = rmsValues;
        std::sort(sorted.begin(), sorted.end());
        // Dynamic-range threshold: 8% of the way from noise floor to speech peak
        float noiseFloor  = sorted[sorted.size() / 10];       // 10th percentile
        float speechLevel = sorted[sorted.size() * 9 / 10];   // 90th percentile
        float dynRange    = speechLevel - noiseFloor;
        float threshold   = (dynRange > 0.001f)
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
            if (rmsAt(pos) > threshold && rmsAt(pos + windowSize) > threshold) {
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
            if (rmsAt(pos) > threshold && rmsAt(pos + windowSize) > threshold) {
                speechEnd = pos + windowSize * 2; break;
            }
        }

        int prepad  = static_cast<int>(0.060 * audioData.sampleRate); // 60ms
        int postpad = static_cast<int>(0.040 * audioData.sampleRate); // 40ms
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

    // Second pass: close gaps between adjacent same-file clips
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
        if (a.matchState == 2 || b.matchState == 2) continue; // skip confirmed
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
