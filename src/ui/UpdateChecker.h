/*
 * UpdateChecker — Checks for new releases on GitHub and handles seamless update.
 *
 * On app launch, queries the GitHub Releases API in the background.
 * If a newer version is found, offers to download and install silently.
 * The installer runs with /VERYSILENT /SUPPRESSMSGBOXES /NORESTART so the
 * user barely notices — just a brief "Updating…" splash.
 *
 * Usage:
 *   auto* updater = new UpdateChecker(this);
 *   updater->check("YourGitHubUser", "roundtable");
 */

#pragma once

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QElapsedTimer>

namespace rt {

class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    /// Start checking for updates. Runs asynchronously.
    void check(const QString &githubUser, const QString &repoName);

signals:
    /// Emitted when the check completes (whether or not an update was found).
    void checkComplete(bool updateAvailable, const QString &latestVersion);

private slots:
    void onVersionFetched(QNetworkReply *reply);
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished(QNetworkReply *reply);

public:
    /// Start downloading and installing the latest version.
    /// The download URL was already captured during check().
    void doUpdate();

    QString m_githubUser;
    QString m_repoName;
    QString m_latestVersion;
    QString m_downloadUrl;
    QNetworkAccessManager *m_nam{nullptr};
    QProgressDialog *m_progress{nullptr};
};

} // namespace rt
