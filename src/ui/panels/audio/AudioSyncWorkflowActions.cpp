/*
 * AudioSyncWorkflowActions.cpp - Script, import, sync, and export UI actions.
 */

#include "panels/audio/AudioSync.h"

#include "Theme.h"

#include <spdlog/spdlog.h>

#include <QCoreApplication>
#include <QDir>
#include <functional>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
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

    // If user selected an existing history item, use the stored URL
    int currentIdx = m_scriptUrlCombo->currentIndex();
    QString storedUrl;
    if (currentIdx >= 0)
        storedUrl = m_scriptUrlCombo->itemData(currentIdx).toString();

    QString urlToUse;
    if (!storedUrl.isEmpty() && text == m_scriptUrlCombo->itemText(currentIdx)) {
        // User selected an existing history entry — use stored URL
        urlToUse = storedUrl;
    } else if (!text.isEmpty()) {
        // User typed something new — use the text directly
        urlToUse = text;
    }

    if (urlToUse.isEmpty()) {
        urlToUse = QFileDialog::getOpenFileName(this, "Load Script",
            QString(), "Script Files (*.txt *.json *.html);;All Files (*)");
        if (urlToUse.isEmpty()) return;
        m_scriptUrlCombo->setEditText(urlToUse);
    }

    m_lastScriptSource = urlToUse;

    // Prompt for a session name (must provide one)
    QString defaultName = displayNameForScriptUrl(urlToUse);
    // For local files, use the file stem (without extension) if short enough
    if (!urlToUse.startsWith("http://") && !urlToUse.startsWith("https://")) {
        QFileInfo fi(urlToUse);
        QString stem = fi.completeBaseName();
        if (!stem.isEmpty() && stem.length() <= 50)
            defaultName = stem;
    }

    bool ok = false;
    QString chosenName = QInputDialog::getText(
        this, "Script Name",
        "Enter a name for this script session:",
        QLineEdit::Normal, defaultName, &ok);
    if (!ok || chosenName.trimmed().isEmpty()) return;  // User cancelled or empty

    m_pendingSessionName = chosenName.trimmed().toStdString();

    if (urlToUse.startsWith("http://") || urlToUse.startsWith("https://")) {
        addToScriptHistory(urlToUse);
        fetchScriptFromUrl(urlToUse);
    } else {
        loadScript(urlToUse.toStdString(), urlToUse.toStdString());
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

    // Capture the original URL for session key purposes
    QString originalUrl = url;

    std::function<void()> tryNextUrl;
    tryNextUrl = [this, state, &urlsToTry, &tryNextUrl, isGoogleDocs, originalUrl]() {
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
                // Pass the original URL as the session key, not the raw content
                loadScript(content, originalUrl.toStdString());
                delete state;
                return;
            }

            if (reply->error() == QNetworkReply::AuthenticationRequiredError &&
                isGoogleDocs && attempt < urlsToTry.size() - 1)
            {
                spdlog::warn("Google Docs export requires auth, trying next format");
                state->attemptIndex = attempt + 1;
                tryNextUrl();
                return;
            }

            QString errorMessage = reply->errorString();
            spdlog::error("Failed to fetch script from URL: {}", errorMessage.toStdString());
            m_scriptStatus->setText(QString("Error: %1").arg(errorMessage));
            m_scriptStatus->setStyleSheet(QString("color: %1;").arg(Theme::hex(Theme::colors().error)));
            m_loadScriptBtn->setEnabled(true);
            delete state;
        });
    };

    tryNextUrl();
}

void AudioSync::onImportAudioClicked()
{
    QSettings settings("ROUNDTABLE", "NLE");
    QString lastDir = settings.value("AudioSync/lastImportDir", QString()).toString();

    if (lastDir.isEmpty() && !m_lastImportDir.isEmpty())
        lastDir = m_lastImportDir;
    if (lastDir.isEmpty())
        lastDir = QDir::homePath();

    QStringList paths = QFileDialog::getOpenFileNames(this, "Import Audio Files",
        lastDir, "Audio Files (*.wav *.mp3 *.flac *.ogg *.m4a);;All Files (*)");
    if (!paths.isEmpty()) {
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

} // namespace rt
