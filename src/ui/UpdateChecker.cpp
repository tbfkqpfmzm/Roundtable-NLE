/*
 * UpdateChecker.cpp — Check GitHub Releases, download, and install silently.
 */

#include "UpdateChecker.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QNetworkRequest>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <spdlog/spdlog.h>

namespace rt {

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void UpdateChecker::check(const QString &githubUser, const QString &repoName)
{
    m_githubUser = githubUser;
    m_repoName = repoName;

    QString url = QString(
        "https://api.github.com/repos/%1/%2/releases/latest")
        .arg(githubUser, repoName);

    QNetworkRequest req;
    req.setUrl(QUrl(url));
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(10000);

    auto *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onVersionFetched(reply);
    });
}

void UpdateChecker::onVersionFetched(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        spdlog::warn("UpdateChecker: GitHub API error: {}",
                     reply->errorString().toStdString());
        emit checkComplete(false, QString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject root = doc.object();

    // Extract tag name (e.g. "v0.49")
    m_latestVersion = root["tag_name"].toString().remove('v');

    if (m_latestVersion.isEmpty()) {
        spdlog::warn("UpdateChecker: could not parse latest version from GitHub");
        emit checkComplete(false, QString());
        return;
    }

    // Find the first asset that ends with ".exe" (the installer)
    QJsonArray assets = root["assets"].toArray();
    m_downloadUrl.clear();
    for (const auto &a : assets) {
        QJsonObject asset = a.toObject();
        QString name = asset["name"].toString();
        if (name.endsWith(".exe", Qt::CaseInsensitive)) {
            m_downloadUrl = asset["browser_download_url"].toString();
            break;
        }
    }

    if (m_downloadUrl.isEmpty()) {
        spdlog::warn("UpdateChecker: no installer asset found in latest release");
        emit checkComplete(false, QString());
        return;
    }

    // Compare versions
    QString currentVersion = QApplication::applicationVersion();
    if (m_latestVersion == currentVersion) {
        spdlog::info("UpdateChecker: already at latest version ({})",
                     currentVersion.toStdString());
        emit checkComplete(false, QString());
        return;
    }

    spdlog::info("UpdateChecker: new version {} available (current: {})",
                 m_latestVersion.toStdString(), currentVersion.toStdString());

    emit checkComplete(true, m_latestVersion);
}

void UpdateChecker::onDownloadProgress(qint64 received, qint64 total)
{
    if (m_progress && total > 0) {
        m_progress->setMaximum(static_cast<int>(total));
        m_progress->setValue(static_cast<int>(received));
    }
}

void UpdateChecker::onDownloadFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (m_progress) {
        m_progress->close();
        m_progress->deleteLater();
        m_progress = nullptr;
    }

    if (reply->error() != QNetworkReply::NoError) {
        spdlog::error("UpdateChecker: download failed: {}",
                      reply->errorString().toStdString());
        QMessageBox::warning(nullptr, tr("Update Failed"),
            tr("Failed to download the update:\n%1")
            .arg(reply->errorString()));
        return;
    }

    // Save to temp directory
    QString installerPath = QDir(QStandardPaths::writableLocation(
        QStandardPaths::TempLocation))
        .filePath(QString("roundtable-setup-%1.exe").arg(m_latestVersion));

    QFile file(installerPath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(nullptr, tr("Update Failed"),
            tr("Could not write installer to:\n%1").arg(installerPath));
        return;
    }
    file.write(reply->readAll());
    file.close();

    spdlog::info("UpdateChecker: downloaded installer to {}",
                 installerPath.toStdString());

    // Launch the installer silently, then quit the app
    // The installer is configured to auto-start the app after install.
    if (!QProcess::startDetached(
            QString("\"%1\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART")
                .arg(installerPath))) {
        QMessageBox::warning(nullptr, tr("Update Failed"),
            tr("Could not launch the installer."));
        return;
    }

    // Give the installer a moment to start, then quit
    QTimer::singleShot(500, qApp, &QApplication::quit);
}

void UpdateChecker::doUpdate()
{
    QString downloadUrl = m_downloadUrl;
    if (downloadUrl.isEmpty()) {
        spdlog::error("UpdateChecker: no download URL available");
        return;
    }
    spdlog::info("UpdateChecker: downloading {} ...", downloadUrl.toStdString());

    // Show download progress
    m_progress = new QProgressDialog(
        tr("Downloading update v%1 ...").arg(m_latestVersion),
        tr("Cancel"), 0, 100, nullptr);
    m_progress->setWindowTitle(tr("Updating ROUNDTABLE"));
    m_progress->setWindowFlags(m_progress->windowFlags() &
                               ~Qt::WindowCloseButtonHint);
    m_progress->setMinimumDuration(0);
    m_progress->setValue(0);

    QNetworkRequest req;
    req.setUrl(QUrl(downloadUrl));
    req.setTransferTimeout(300000); // 5 minutes
    auto *reply = m_nam->get(req);

    connect(reply, &QNetworkReply::downloadProgress,
            this, &UpdateChecker::onDownloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onDownloadFinished(reply);
    });
}

} // namespace rt
