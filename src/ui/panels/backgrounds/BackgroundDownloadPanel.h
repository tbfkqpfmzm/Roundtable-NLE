/*
 * BackgroundDownloadPanel — Download Nikke in-game backgrounds.
 *
 * Opens the MEGA folder in the user's browser for manual download.
 * The app scans the target directory and displays downloaded images
 * as a thumbnail grid.
 */

#pragma once

#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QFileSystemWatcher;

namespace rt {

class BackgroundDownloadPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BackgroundDownloadPanel(QWidget* parent = nullptr);
    ~BackgroundDownloadPanel() override = default;

protected:
    void showEvent(QShowEvent* event) override;

signals:
    /// Emitted after new files are detected (so listeners refresh).
    void backgroundsDownloaded();

private slots:
    void onOpenMegaClicked();
    void toggleInstructions();
    void processBatch();
    void onSearchChanged();

private:
    void setupUI();
    void lazyInit();
    void scanLocalFiles();
    void startPopulateGrid();
    void startWatching();
    void setStatus(const QString& text);

    // ── State ──
    QSet<QString> m_localFileNames;
    bool          m_initDone = false;

    // ── Batch thumbnail loading ──
    QStringList m_batchQueue;
    int         m_batchTotal = 0;
    QTimer*     m_batchTimer{nullptr};
    static constexpr int kBatchSize = 40;
    static constexpr int kBatchIntervalMs = 1; // near-instant while yielding to events

    // ── File watching ──
    QFileSystemWatcher* m_fileWatcher{nullptr};
    QTimer*             m_scanDebounce{nullptr};

    // ── UI ──
    QLineEdit*    m_searchEdit{nullptr};
    QPushButton*  m_openMegaBtn{nullptr};
    QPushButton*  m_instructionsToggle{nullptr};
    QWidget*      m_instructionsContainer{nullptr};
    QLabel*       m_instructionsLabel{nullptr};
    QListWidget*  m_backgroundGrid{nullptr};
    QLabel*       m_statusLabel{nullptr};

    static constexpr const char* kTargetDir = "assets/NikkeBKG";
    static constexpr const char* kMegaUrl   = "https://mega.nz/folder/V55C3T6C#02N6WUjiEQ8hQv6PhZuMkg";

    /// Shared thumbnail cache so Timeline + Shot panels don't decode twice.
    static QHash<QString, QIcon> s_thumbnailCache;
    /// Shared file list so both panels see the same files.
    static QSet<QString>         s_sharedFileNames;
    static bool                  s_scanDone;
};

} // namespace rt
