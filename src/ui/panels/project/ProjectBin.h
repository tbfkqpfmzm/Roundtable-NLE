/*
 * ProjectBin — Tabbed media browser panel.
 *
 * Step 16: Project Bin
 *
 * The Project Bin is the central media management panel:
 *   - Tabbed categories: All / Video / Image / Audio / Spine
 *   - Search/filter bar at top
 *   - Zoom slider for thumbnail size
 *   - Import button to add files
 *   - Double-click to load in source monitor
 *   - Drag-to-timeline or source monitor
 *
 * Layout:
 *   ┌─────────────────────────────────────────────┐
 *   │  [All] [Video] [Image] [Audio] [Spine]     │
 *   ├─────────────────────────────────────────────┤
 *   │  [🔍 Search...          ]  [Import] [-][+] │
 *   ├─────────────────────────────────────────────┤
 *   │  ┌──────┐ ┌──────┐ ┌──────┐               │
 *   │  │      │ │      │ │      │  ...           │
 *   │  │thumb │ │thumb │ │thumb │               │
 *   │  ├──────┤ ├──────┤ ├──────┤               │
 *   │  │name  │ │name  │ │name  │               │
 *   │  └──────┘ └──────┘ └──────┘               │
 *   │  ...                                       │
 *   ├─────────────────────────────────────────────┤
 *   │  Items: 42                                  │
 *   └─────────────────────────────────────────────┘
 */

#pragma once

#include "media/ThumbnailGenerator.h"

#include <QComboBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTabBar>
#include <QLabel>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidget>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rt {

class ThumbnailGrid;
class ThumbnailGenerator;
class MediaDragTreeWidget;
class MediaPool;
class MediaSourceService;
class Project;
class CommandStack;

/// Bin folder info for serialization.
struct BinFolderState {
    std::string              name;
    bool                     expanded = true;
    std::vector<std::string> childKeys;
};

/// Project Bin — Premiere Pro-style media browser for the project.
class ProjectBin : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectBin(QWidget* parent = nullptr);
    ~ProjectBin() override;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the ThumbnailGenerator used for thumbnail loading.
    void setThumbnailGenerator(ThumbnailGenerator* gen);

    /// Set the MediaPool for handle management.
    void setMediaPool(MediaPool* pool) noexcept;

    /// Set the shared MediaSourceService for source access.
    void setMediaSourceService(MediaSourceService* service) noexcept;

    /// Set the Project for sequence management.
    void setProject(Project* project) noexcept;

    /// Update the root tab label to the project name.
    void setProjectName(const QString& name);

    /// Set the CommandStack for undo/redo support.
    void setCommandStack(CommandStack* stack) noexcept;

    // ── Import / manage items ───────────────────────────────────────────

    /// Import files via a file dialog (opens QFileDialog).
    void importFiles();

    /// Add files to the bin programmatically.
    void addFiles(const std::vector<std::filesystem::path>& files);

    /// Add files into a specific bin (or root if targetBin is nullptr).
    void addFilesToBin(const std::vector<std::filesystem::path>& files,
                       QTreeWidgetItem* targetBin);

    /// Add files into a named bin (creates the bin if it doesn't exist).
    /// If parentBinName is non-empty, the new bin is created under that parent.
    void addFilesToNamedBin(const std::vector<std::filesystem::path>& files,
                            const QString& binName,
                            const QString& parentBinName = {});

    /// Remove a specific media file from the bin.
    bool removeFile(const std::filesystem::path& filePath);

    /// Clear all items from the bin.
    void clearAll();

    /// Force a full refresh of both list and icon views from current state.
    /// Call after project switch to ensure stale tree items are purged.
    void refreshAllViews();

    /// Create default bin folders (1_SEQUENCES, 2_AUDIO, 3_VIDEO, 4_GFX)
    /// and auto-sort existing items into them.
    void ensureDefaultBins();

    /// Refresh the sequence items in the list from the current project.
    void refreshSequences();

    /// Select all items in the bin (for Ctrl+A).
    void selectAllItems();

    /// Number of items in the bin.
    [[nodiscard]] int itemCount() const noexcept;

    // ── Query ───────────────────────────────────────────────────────────

    /// Get all file paths in the bin.
    [[nodiscard]] std::vector<std::filesystem::path> allFiles() const;

    /// Get files of a specific type.
    [[nodiscard]] std::vector<std::filesystem::path> filesOfType(MediaType type) const;

    /// Get the current bin folder structure (for serialization).
    [[nodiscard]] std::vector<BinFolderState> binFolderState() const;

    /// Restore bin folder structure from saved state (after adding files).
    void restoreBinFolders(const std::vector<BinFolderState>& folders);

    // ── Accessors ───────────────────────────────────────────────────────

    /// Get the internal thumbnail grid widget.
    [[nodiscard]] ThumbnailGrid* grid() const noexcept { return m_grid; }

    /// Get the search/filter line edit.
    [[nodiscard]] QLineEdit* searchField() const noexcept { return m_searchField; }

    /// Get the zoom slider.
    [[nodiscard]] QSlider* zoomSlider() const noexcept { return m_zoomSlider; }

    /// Current active tab type.
    [[nodiscard]] MediaType activeTabType() const noexcept { return m_activeType; }

    // ── View mode ───────────────────────────────────────────────────────

    /// Whether the bin is currently showing list view or icon view.
    [[nodiscard]] bool isListView() const noexcept { return m_listView; }

    /// Create a new bin folder (shows input dialog, adds to tree).
    void createNewBin();

    /// Select and scroll to the item matching the given file path.
    void revealByPath(const QString& filePath);

    /// Open the "New Sequence" dialog and create a sequence with chosen settings.
    void createNewSequence();

    /// Create a sequence from a dropped media file, matching its resolution/fps.
    void createSequenceFromMedia(const std::filesystem::path& filePath);

    /// Create a solid-color matte image (like Premiere Pro's Color Matte).
    /// Prompts for color and name, generates a PNG, and imports it.
    void createColorMatte();

    // ── Size hints ──────────────────────────────────────────────────────

    QSize sizeHint() const override;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

signals:
    /// Emitted when the user double-clicks an item to load it in the source monitor.
    void loadInSourceMonitor(const std::filesystem::path& filePath, uint64_t mediaHandle);

    /// Emitted when items are imported or removed.
    void itemCountChanged(int count);

    /// Emitted when the user double-clicks a sequence to open it.
    void sequenceOpened(size_t sequenceIndex);

    /// Emitted when a sequence is added, removed, or duplicated.
    void sequencesChanged();

    /// Emitted when sequence settings (resolution, frame rate, name) are changed.
    void sequenceSettingsChanged();

    /// Emitted when clips have been removed from (or restored to) the
    /// timeline as a side-effect of deleting/undeleting bin media.
    /// Listeners should rebuild timeline track views and flush caches.
    void timelineClipsMutated();

    /// Emitted when user wants to nest a sequence on the active timeline.
    void nestSequenceRequested(size_t sequenceIndex, const QString& sequenceName);

    /// Emitted when a project needs to be created (e.g. creating a sequence
    /// with no project open). The receiver takes ownership of the Project*.
    void projectCreated(class Project* project);

private slots:
    void onFilterChanged(int index);
    void onSearchChanged(const QString& text);
    void onZoomChanged(int value);
    void onItemDoubleClicked(int index, const std::filesystem::path& filePath);
    void onListItemDoubleClicked(QTreeWidgetItem* item, int column);
    void setListView(bool listMode);
    void onBinTabChanged(int index);
    void onBinTabCloseRequested(int index);

private:
    // ── Drag-and-drop (extracted to ProjectBinDragDrop.cpp) ─────────────

    struct BinSnapshot {
        std::vector<std::filesystem::path> files;
        std::vector<BinFolderState> folders;
    };
    BinSnapshot captureBinSnapshot();
    void applyBinSnapshot(const BinSnapshot& s);

    bool handleDropEvent(QEvent* de);
    bool handleDragEnterEvent(QEvent* de);
    bool handleDragMoveEvent(QEvent* de);
    void handleDragLeave();

    // ── UI setup (extracted to ProjectBinUI.cpp) ────────────────────────

    void setupUI();
    void syncListView();
    void syncIconView();

    /// Open (or switch to) a tab for the given bin path.
    void openBinTab(const QStringList& binPath, const QString& name);

    // Widgets
    QComboBox*     m_filterCombo{nullptr};
    QLineEdit*     m_searchField{nullptr};
    QToolButton*   m_importBtn{nullptr};
    QToolButton*   m_btnCreateSequence{nullptr};
    QToolButton*   m_btnListView{nullptr};
    QToolButton*   m_btnIconView{nullptr};
    QSlider*       m_zoomSlider{nullptr};
    QScrollArea*   m_scrollArea{nullptr};
    ThumbnailGrid* m_grid{nullptr};
    class MediaDragTreeWidget* m_listWidget{nullptr};
    QLabel*        m_emptyLabel{nullptr};
    QLabel*        m_statusLabel{nullptr};
    QWidget*       m_iconNavBar{nullptr};
    QLabel*        m_breadcrumbLabel{nullptr};
    QTabBar*       m_binTabBar{nullptr};

    // State
    bool        m_listView{true};  // Default to list view (Premiere style)
    MediaType   m_activeType{MediaType::Unknown}; // Unknown = All
    QStringList m_iconBinPath;     // Current folder path in icon view
    MediaPool*  m_pool{nullptr};
    MediaSourceService* m_mediaSources{nullptr};
    ThumbnailGenerator* m_generator{nullptr};
    std::unique_ptr<ThumbnailGenerator> m_ownedGenerator;  // owned if created internally
    Project*    m_project{nullptr};
    CommandStack* m_commandStack{nullptr};
    int         m_copiedSequenceIdx{-1}; // index of copied sequence for paste
    QTreeWidgetItem* m_dropHighlightItem{nullptr}; // bin highlighted during drag
    QVector<QStringList> m_binTabPaths;  // bin path for each tab (empty = root)
};

} // namespace rt
