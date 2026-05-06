/*
 * LibraryPanel — tabbed reusable-asset browser.
 *
 * Tabs:
 *   - Characters   (delegates to existing CharactersPanel, unchanged)
 *   - Backgrounds  (scans assets/backgrounds/)
 *   - Videos       (scans assets/videos/)
 *   - Audio        (scans assets/audio/)
 *
 * Each non-character tab uses a MediaDragTreeWidget so the timeline's
 * existing drop handler (which checks qobject_cast<QTreeWidget*>) accepts
 * dragged items identically to the Project Bin and Characters panel.
 */

#pragma once

#include <QWidget>

class QTabWidget;
class QTreeWidget;
class QLineEdit;
class QToolButton;

namespace rt {

class CharactersPanel;
class MediaDragTreeWidget;
class ModelManager;
class AnimationVideoCache;
class MediaPool;

class LibraryPanel : public QWidget
{
    Q_OBJECT

public:
    explicit LibraryPanel(QWidget* parent = nullptr);
    ~LibraryPanel() override = default;

    // Forwarded to the embedded CharactersPanel
    void setModelManager(ModelManager* mgr);
    void setAnimVideoCache(AnimationVideoCache* cache);
    void setMediaPool(MediaPool* pool);

    /// Refresh all tabs.
    void refresh();

    /// Refresh only the currently visible tab.
    void refreshCurrentTab();

    /// Access the embedded characters panel (for legacy wiring).
    [[nodiscard]] CharactersPanel* charactersPanel() const noexcept { return m_characters; }

    /// Toggle between detailed list view and large-thumbnail (icon) view.
    void setIconMode(bool iconMode);
    [[nodiscard]] bool iconMode() const noexcept { return m_iconMode; }

signals:
    /// Emitted when the user re-links an asset on disk. Listeners (e.g.
    /// TimelineWorkspace) walk all timelines/presets and update references.
    void mediaRelinkRequested(const QString& oldPath, const QString& newPath);

    /// Emitted when the user double-clicks a media asset in a folder tab.
    /// Listeners should open it via MediaPool and route to the Source Monitor.
    void loadInSourceMonitor(const QString& filePath);

private:
    void buildUI();
    void refreshFolderTree(MediaDragTreeWidget* tree,
                           const QString& dirPath,
                           const QStringList& nameFilters,
                           const QString& searchTerm);
    void setupContextMenu(MediaDragTreeWidget* tree, const QString& fileFilter);
    void setupDoubleClick(MediaDragTreeWidget* tree);
    void applyViewModeToTree(MediaDragTreeWidget* tree);

    QTabWidget*          m_tabs{nullptr};
    CharactersPanel*     m_characters{nullptr};

    bool                 m_iconMode{false};
    QToolButton*         m_btnDetail{nullptr};
    QToolButton*         m_btnIcons{nullptr};

    // Backgrounds tab
    QLineEdit*           m_bgSearch{nullptr};
    MediaDragTreeWidget* m_bgTree{nullptr};

    // Videos tab
    QLineEdit*           m_videoSearch{nullptr};
    MediaDragTreeWidget* m_videoTree{nullptr};

    // Audio tab
    QLineEdit*           m_audioSearch{nullptr};
    MediaDragTreeWidget* m_audioTree{nullptr};
};

} // namespace rt
