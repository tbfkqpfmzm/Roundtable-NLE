/*
 * BackgroundDownloadPanel — Download Nikke in-game backgrounds.
 *
 * Opens the MEGA folder in the user's browser for manual download.
 * The app scans the target directory and displays downloaded images
 * as a thumbnail grid.
 */

#pragma once

#include <QDataStream>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPixmapCache>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWidget>

class QFileSystemWatcher;

namespace rt {

// ── Custom list widget with drag MIME data ───────────────────────────────
// Provides both application/x-roundtable-asset (for ShotComposer) and
// file URLs (for Timeline), so dragging works from any parent context.
class BackgroundGridWidget : public QListWidget
{
    Q_OBJECT
public:
    using QListWidget::QListWidget;

signals:
    /// Emitted when the user double-clicks a background item.
    void backgroundActivated(const QString& filePath);

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
};

class BackgroundDownloadPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BackgroundDownloadPanel(QWidget* parent = nullptr);
    ~BackgroundDownloadPanel() override = default;

    /// Access the grid widget so parent panels can connect signals.
    [[nodiscard]] BackgroundGridWidget* backgroundGrid() const noexcept { return m_backgroundGrid; }

protected:
    void showEvent(QShowEvent* event) override;

signals:
    /// Emitted after new files are detected (so listeners refresh).
    void backgroundsDownloaded();

    /// Emitted when the user double-clicks a background item.
    void backgroundActivated(const QString& filePath);

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
    BackgroundGridWidget* m_backgroundGrid{nullptr};
    QLabel*       m_statusLabel{nullptr};

    static constexpr const char* kTargetDir = "assets/NikkeBKG";
    static constexpr const char* kMegaUrl   = "https://mega.nz/folder/V55C3T6C#02N6WUjiEQ8hQv6PhZuMkg";

    /// Shared file list so both panels see the same files.
    static QSet<QString>         s_sharedFileNames;
    static bool                  s_scanDone;
};

} // namespace rt
