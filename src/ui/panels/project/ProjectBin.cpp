/*
 * ProjectBin.cpp � Premiere Pro-style media browser panel.
 * Step 16
 */

#include "QtHelpers.h"
#include "panels/project/ProjectBin.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "media/MediaPool.h"
#include "media/MediaSourceService.h"
#include "dialogs/SequenceDialog.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QColorDialog>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPainterPath>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTreeWidgetItem>
#include <QImage>
#include <QRegularExpression>

#include <map>
#include <set>

#include "panels/project/ProjectBinInternal.h"

namespace rt {

namespace {

QString projectBinItemKey(QTreeWidgetItem* item)
{
    QString key = item->data(0, Qt::UserRole).toString();
    if (key.isEmpty()) key = item->text(0);
    return key;
}

QTreeWidgetItem* projectBinFindChildBin(QTreeWidgetItem* parent, const QString& name)
{
    if (!parent) return nullptr;
    for (int i = 0; i < parent->childCount(); ++i) {
        auto* child = parent->child(i);
        if (child->data(0, Qt::UserRole + 2).toBool() && child->text(0) == name)
            return child;
    }
    return nullptr;
}

QTreeWidgetItem* projectBinCreateBinItem(const QString& name)
{
    auto* binItem = new QTreeWidgetItem();
    binItem->setText(0, name);
    binItem->setData(0, Qt::UserRole + 2, true);
    binItem->setIcon(0, makePremiereBinIcon(kLabelBin, "bin"));
    binItem->setData(0, Qt::UserRole + 10, QVariant::fromValue(kLabelBin));
    binItem->setFlags(binItem->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
    return binItem;
}

} // namespace


// -----------------------------------------------------------------------------
//  Configuration
// -----------------------------------------------------------------------------

void ProjectBin::setThumbnailGenerator(ThumbnailGenerator* gen)
{
    m_generator = gen;
    m_grid->setThumbnailGenerator(gen);
}

void ProjectBin::setMediaPool(MediaPool* pool) noexcept
{
    m_pool = pool;

    // Create a ThumbnailGenerator if we don't have one yet
    if (pool && !m_generator) {
        m_ownedGenerator = std::make_unique<ThumbnailGenerator>(2, 160, 120);
        m_ownedGenerator->setMediaPool(pool);
        m_ownedGenerator->setCacheDirectory(
            (rt::userDataDir() + "/cache/thumbnails").toStdString());
        setThumbnailGenerator(m_ownedGenerator.get());
    }

    if (m_grid)
        m_grid->setMediaPool(pool);
}

void ProjectBin::setMediaSourceService(MediaSourceService* service) noexcept
{
    m_mediaSources = service;
}

void ProjectBin::setProject(Project* project) noexcept
{
    m_project = project;
    if (m_listView) syncListView();
}

void ProjectBin::setProjectName(const QString& name)
{
    if (m_binTabBar && m_binTabBar->count() > 0) {
        m_binTabBar->setTabText(0, name.isEmpty() ? QStringLiteral("Project") : name);
    }
}

void ProjectBin::setCommandStack(CommandStack* stack) noexcept
{
    m_commandStack = stack;
}

void ProjectBin::createNewBin()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, "New Bin", "Bin name:",
                                         QLineEdit::Normal, "Bin", &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty())
        return;

    QTreeWidgetItem* targetParentBin = nullptr;
    if (auto* selected = m_listWidget ? m_listWidget->currentItem() : nullptr) {
        if (selected->data(0, Qt::UserRole + 2).toBool())
            targetParentBin = selected;
        else if (selected->parent() && selected->parent()->data(0, Qt::UserRole + 2).toBool())
            targetParentBin = selected->parent();
    }

    auto* binItem = projectBinCreateBinItem(name);
    if (targetParentBin) {
        targetParentBin->addChild(binItem);
        targetParentBin->setExpanded(true);
    } else {
        m_listWidget->addTopLevelItem(binItem);
    }

    m_listWidget->setCurrentItem(binItem);
    m_listWidget->scrollToItem(binItem);
}

// -----------------------------------------------------------------------------
//  Import / manage items
// -----------------------------------------------------------------------------

void ProjectBin::importFiles()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this,
        "Import Media",
        QString(),
        "All Files (*.*);;"
        "Video (*.mp4 *.mkv *.avi *.mov *.webm);;"
        "Images (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp);;"
        "Audio (*.wav *.mp3 *.flac *.ogg *.aac);;"
        "Spine (*.skel *.json)");

    if (files.isEmpty()) return;

    std::vector<std::filesystem::path> paths;
    paths.reserve(files.size());
    for (const auto& f : files)
        paths.emplace_back(f.toStdString());

    addFiles(paths);
}

void ProjectBin::addFiles(const std::vector<std::filesystem::path>& files)
{
    for (const auto& f : files) {
        // Skip duplicates — Premiere Pro silently ignores re-imports
        if (m_grid->hasItem(f))
            continue;
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto result = m_mediaSources->openSource({f, RenderRequestType::Still, false});
            handle = result.handle;
        }
        m_grid->addItem(f, MediaType::Unknown, handle);
    }
    m_grid->loadVisibleThumbnails();
    // Always sync the list view (source of truth for bins)
    syncListView();
    if (!m_listView)
        syncIconView();
}

void ProjectBin::addFilesToBin(const std::vector<std::filesystem::path>& files,
                               QTreeWidgetItem* targetBin)
{
    // Save the target bin's FULL PATH (root→leaf bin-name breadcrumbs)
    // BEFORE syncListView clears the tree. The raw QTreeWidgetItem* pointer
    // becomes dangling after m_listWidget->clear(), and a simple name
    // lookup at top level fails for nested sub-bins.
    QStringList targetBinPath;
    for (auto* node = targetBin; node; node = node->parent()) {
        if (node->data(0, Qt::UserRole + 2).toBool())
            targetBinPath.prepend(node->text(0));
    }

    // Add to the grid (data model), skipping duplicates
    for (const auto& f : files) {
        if (m_grid->hasItem(f))
            continue;
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto result = m_mediaSources->openSource({f, RenderRequestType::Still, false});
            handle = result.handle;
        }
        m_grid->addItem(f, MediaType::Unknown, handle);
    }
    m_grid->loadVisibleThumbnails();

    // Rebuild the list view (creates tree items at top-level)
    syncListView();

    // If a target bin was specified, walk the saved breadcrumb path
    // to locate the same bin again (handles nested sub-bins correctly).
    if (!targetBinPath.isEmpty()) {
        QTreeWidgetItem* bin = nullptr;
        // Find top-level bin matching the first path segment
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, Qt::UserRole + 2).toBool() && it->text(0) == targetBinPath.first()) {
                bin = it;
                break;
            }
        }
        // Descend through remaining path segments
        for (int depth = 1; bin && depth < targetBinPath.size(); ++depth) {
            QTreeWidgetItem* next = nullptr;
            for (int i = 0; i < bin->childCount(); ++i) {
                auto* ch = bin->child(i);
                if (ch->data(0, Qt::UserRole + 2).toBool() && ch->text(0) == targetBinPath[depth]) {
                    next = ch;
                    break;
                }
            }
            bin = next;
        }
        if (bin) {
            for (const auto& f : files) {
                QString key = QString::fromStdString(f.string());
                for (int i = m_listWidget->topLevelItemCount() - 1; i >= 0; --i) {
                    auto* it = m_listWidget->topLevelItem(i);
                    if (it->data(0, Qt::UserRole).toString() == key) {
                        auto* taken = m_listWidget->takeTopLevelItem(i);
                        if (taken) bin->addChild(taken);
                        break;
                    }
                }
            }
            bin->setExpanded(true);
        }
    }

    if (!m_listView)
        syncIconView();
}

void ProjectBin::addFilesToNamedBin(const std::vector<std::filesystem::path>& files,
                                    const QString& binName,
                                    const QString& parentBinName)
{
    if (files.empty() || binName.isEmpty()) return;

    // Add to the grid (data model), skipping duplicates
    for (const auto& f : files) {
        if (m_grid->hasItem(f)) continue;
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto result = m_mediaSources->openSource({f, RenderRequestType::Still, false});
            handle = result.handle;
        }
        m_grid->addItem(f, MediaType::Unknown, handle);
    }
    m_grid->loadVisibleThumbnails();
    syncListView();

    // Find or create the parent bin
    QTreeWidgetItem* parentBin = nullptr;
    if (!parentBinName.isEmpty()) {
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, Qt::UserRole + 2).toBool() && it->text(0) == parentBinName) {
                parentBin = it;
                break;
            }
        }
    }

    // Find or create the target bin
    auto findBinUnder = [](QTreeWidgetItem* parent, const QString& name) -> QTreeWidgetItem* {
        if (!parent) return nullptr;
        for (int i = 0; i < parent->childCount(); ++i) {
            auto* child = parent->child(i);
            if (child->data(0, Qt::UserRole + 2).toBool() && child->text(0) == name)
                return child;
        }
        return nullptr;
    };

    QTreeWidgetItem* targetBin = nullptr;
    if (parentBin) {
        targetBin = findBinUnder(parentBin, binName);
    } else {
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, Qt::UserRole + 2).toBool() && it->text(0) == binName) {
                targetBin = it;
                break;
            }
        }
    }

    if (!targetBin) {
        targetBin = new QTreeWidgetItem();
        targetBin->setText(0, binName);
        targetBin->setData(0, Qt::UserRole + 2, true);
        targetBin->setIcon(0, makePremiereBinIcon(kLabelBin, "bin"));
        targetBin->setData(0, Qt::UserRole + 10, QVariant::fromValue(kLabelBin));
        targetBin->setFlags(targetBin->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
        if (parentBin)
            parentBin->addChild(targetBin);
        else
            m_listWidget->addTopLevelItem(targetBin);
    }

    // Move the newly-added files into the target bin
    for (const auto& f : files) {
        QString key = QString::fromStdString(f.string());
        for (int i = m_listWidget->topLevelItemCount() - 1; i >= 0; --i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, Qt::UserRole).toString() == key) {
                auto* taken = m_listWidget->takeTopLevelItem(i);
                if (taken) targetBin->addChild(taken);
                break;
            }
        }
        // Also check children of other bins (in case syncListView placed them elsewhere)
        if (parentBin) {
            for (int bi = 0; bi < m_listWidget->topLevelItemCount(); ++bi) {
                auto* bin = m_listWidget->topLevelItem(bi);
                if (bin == targetBin || !bin->data(0, Qt::UserRole + 2).toBool()) continue;
                for (int ci = bin->childCount() - 1; ci >= 0; --ci) {
                    auto* child = bin->child(ci);
                    if (child->data(0, Qt::UserRole).toString() == key) {
                        auto* taken = bin->takeChild(ci);
                        if (taken) targetBin->addChild(taken);
                    }
                }
            }
        }
    }

    targetBin->setExpanded(true);
    if (parentBin) parentBin->setExpanded(true);

    if (!m_listView) syncIconView();
}

bool ProjectBin::removeFile(const std::filesystem::path& filePath)
{
    return m_grid->removeItem(filePath);
}

// -----------------------------------------------------------------------------
//  Drag & drop from Windows Explorer
// -----------------------------------------------------------------------------

// Widget-level drag/drop handlers removed.
// All external drops are handled by the viewport eventFilter in eventFilter(),
// which correctly maps coordinates for bin detection.

void ProjectBin::clearAll()
{
    m_grid->clearItems();
    // Also clear the tree/list widget so stale items from a previous
    // project don't linger even if refreshAllViews() isn't called.
    if (m_listWidget) {
        m_listWidget->blockSignals(true);
        m_listWidget->clear();
        m_listWidget->blockSignals(false);
    }
}

void ProjectBin::refreshAllViews()
{
    // Force-sync both views so that stale tree items from a previous
    // project are purged regardless of which view mode is active.
    syncListView();
    syncIconView();
}

void ProjectBin::selectAllItems()
{
    if (m_listWidget)
        m_listWidget->selectAll();
}

void ProjectBin::ensureDefaultBins()
{
    // Collect existing bin names
    std::set<QString> existingBins;
    for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
        auto* item = m_listWidget->topLevelItem(i);
        if (item->data(0, Qt::UserRole + 2).toBool())
            existingBins.insert(item->text(0));
    }

    // Default bin structure -- only auto-create the VO bin.  Other
    // categories (sequences, video, GFX) are no longer auto-generated;
    // the user wants a clean project bin with just VO for synced audio.
    struct DefaultBin {
        QString name;
        std::vector<QString> subBins;
    };
    std::vector<DefaultBin> defaults = {
        {"VO", {}},
    };

    auto makeBinItem = [this](const QString& name) {
        auto* item = new QTreeWidgetItem();
        item->setText(0, name);
        item->setData(0, Qt::UserRole + 2, true);
        item->setIcon(0, makePremiereBinIcon(kLabelBin, "bin"));
        item->setData(0, Qt::UserRole + 10, QVariant::fromValue(kLabelBin));
        item->setFlags(item->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
        return item;
    };

    for (const auto& def : defaults) {
        QTreeWidgetItem* binItem = nullptr;
        if (existingBins.contains(def.name)) {
            // Parent bin exists � find it so we can ensure sub-bins
            for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
                auto* it = m_listWidget->topLevelItem(i);
                if (it->data(0, Qt::UserRole + 2).toBool() && it->text(0) == def.name) {
                    binItem = it;
                    break;
                }
            }
        } else {
            binItem = makeBinItem(def.name);
            m_listWidget->addTopLevelItem(binItem);
        }
        if (!binItem) continue;

        // Ensure sub-bins exist
        for (const auto& sub : def.subBins) {
            bool found = false;
            for (int ci = 0; ci < binItem->childCount(); ++ci) {
                if (binItem->child(ci)->data(0, Qt::UserRole + 2).toBool() &&
                    binItem->child(ci)->text(0) == sub) {
                    found = true;
                    break;
                }
            }
            if (!found)
                binItem->addChild(makeBinItem(sub));
        }
        binItem->setExpanded(true);
    }

    // Auto-sort existing top-level items into bins
    // Sequences ? 1_SEQUENCES, Audio ? 2_AUDIO, Video/Image ? 3_VIDEO, Spine ? 4_GFX
    auto findBin = [this](const QString& name) -> QTreeWidgetItem* {
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* item = m_listWidget->topLevelItem(i);
            if (item->data(0, Qt::UserRole + 2).toBool() && item->text(0) == name)
                return item;
        }
        return nullptr;
    };

    // Helper: find a sub-bin within a parent bin
    [[maybe_unused]] auto findSubBin = [](QTreeWidgetItem* parent, const QString& name) -> QTreeWidgetItem* {
        if (!parent) return nullptr;
        for (int i = 0; i < parent->childCount(); ++i) {
            auto* child = parent->child(i);
            if (child->data(0, Qt::UserRole + 2).toBool() && child->text(0) == name)
                return child;
        }
        return nullptr;
    };

    QTreeWidgetItem* voBin = findBin("VO");

    // Gather items to reparent (can't remove while iterating)
    std::vector<std::pair<QTreeWidgetItem*, QTreeWidgetItem*>> moves; // item, target bin
    for (int i = m_listWidget->topLevelItemCount() - 1; i >= 0; --i) {
        auto* item = m_listWidget->topLevelItem(i);
        if (item->data(0, Qt::UserRole + 2).toBool()) continue; // skip bins

        QString typeStr = item->text(1);
        if (typeStr == "Audio" && voBin) {
            moves.emplace_back(item, voBin);
        }
    }

    for (auto& [item, bin] : moves) {
        int idx = m_listWidget->indexOfTopLevelItem(item);
        if (idx >= 0) {
            m_listWidget->takeTopLevelItem(idx);
            bin->addChild(item);
        }
    }

    spdlog::info("ProjectBin: ensureDefaultBins � {} bins created, {} items sorted",
                 defaults.size(), moves.size());
}

int ProjectBin::itemCount() const noexcept
{
    return m_grid->itemCount();
}

// -----------------------------------------------------------------------------
//  Query
// -----------------------------------------------------------------------------

std::vector<std::filesystem::path> ProjectBin::allFiles() const
{
    std::vector<std::filesystem::path> result;
    const auto& items = m_grid->items();
    result.reserve(items.size());
    for (const auto& item : items)
        result.push_back(item.filePath);
    return result;
}

std::vector<std::filesystem::path> ProjectBin::filesOfType(MediaType type) const
{
    std::vector<std::filesystem::path> result;
    for (const auto& item : m_grid->items())
    {
        if (item.type == type)
            result.push_back(item.filePath);
    }
    return result;
}

std::vector<BinFolderState> ProjectBin::binFolderState() const
{
    std::vector<BinFolderState> folders;
    std::function<void(QTreeWidgetItem*, const QString&)> visitBin =
        [&](QTreeWidgetItem* item, const QString& path) {
            BinFolderState folder;
            folder.name = path.toStdString();
            folder.expanded = item->isExpanded();

            for (int c = 0; c < item->childCount(); ++c) {
                auto* child = item->child(c);
                if (child->data(0, Qt::UserRole + 2).toBool()) {
                    QString childPath = path;
                    if (!childPath.isEmpty()) childPath += '/';
                    childPath += child->text(0);
                    visitBin(child, childPath);
                } else {
                    folder.childKeys.push_back(projectBinItemKey(child).toStdString());
                }
            }

            folders.push_back(std::move(folder));
        };

    for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
        auto* item = m_listWidget->topLevelItem(i);
        if (!item->data(0, Qt::UserRole + 2).toBool()) continue;
        visitBin(item, item->text(0));
    }

    return folders;
}

void ProjectBin::restoreBinFolders(const std::vector<BinFolderState>& folders)
{
    if (folders.empty()) return;

    // Helper to find and reparent an item by key from top-level items
    auto reparentByKey = [this](QTreeWidgetItem* parent, const QString& qKey) {
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* candidate = m_listWidget->topLevelItem(i);
            if (candidate == parent) continue;
            if (candidate->data(0, Qt::UserRole + 2).toBool()) continue;
            QString candidateKey = candidate->data(0, Qt::UserRole).toString();
            if (candidateKey.isEmpty()) candidateKey = candidate->text(0);
            if (candidateKey == qKey) {
                m_listWidget->takeTopLevelItem(i);
                parent->addChild(candidate);
                return;
            }
        }
    };

    std::map<QString, QTreeWidgetItem*> binMap;
    std::vector<const BinFolderState*> sortedFolders;
    sortedFolders.reserve(folders.size());
    for (const auto& bf : folders) {
        sortedFolders.push_back(&bf);
    }
    std::sort(sortedFolders.begin(), sortedFolders.end(),
              [](const BinFolderState* a, const BinFolderState* b) {
                  auto depthOf = [](const std::string& name) {
                      return static_cast<int>(std::count(name.begin(), name.end(), '/'));
                  };
                  return depthOf(a->name) < depthOf(b->name);
              });

    for (const BinFolderState* folder : sortedFolders) {
        const QString path = QString::fromStdString(folder->name);
        const QStringList parts = path.split('/', Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;

        QTreeWidgetItem* parentBin = nullptr;
        QString currentPath;
        for (const QString& part : parts) {
            if (!currentPath.isEmpty()) currentPath += '/';
            currentPath += part;

            auto it = binMap.find(currentPath);
            if (it != binMap.end()) {
                parentBin = it->second;
                continue;
            }

            QTreeWidgetItem* binItem = nullptr;
            if (parentBin) {
                binItem = projectBinFindChildBin(parentBin, part);
                if (!binItem) {
                    binItem = projectBinCreateBinItem(part);
                    parentBin->addChild(binItem);
                }
            } else {
                for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
                    auto* candidate = m_listWidget->topLevelItem(i);
                    if (candidate->data(0, Qt::UserRole + 2).toBool() && candidate->text(0) == part) {
                        binItem = candidate;
                        break;
                    }
                }
                if (!binItem) {
                    binItem = projectBinCreateBinItem(part);
                    m_listWidget->addTopLevelItem(binItem);
                }
            }

            binMap[currentPath] = binItem;
            parentBin = binItem;
        }
    }

    for (const BinFolderState* folder : sortedFolders) {
        const QString path = QString::fromStdString(folder->name);
        auto it = binMap.find(path);
        if (it == binMap.end()) continue;
        auto* binItem = it->second;
        binItem->setExpanded(folder->expanded);
        for (const auto& key : folder->childKeys)
            reparentByKey(binItem, QString::fromStdString(key));
    }
}

// -----------------------------------------------------------------------------
//  Size
// -----------------------------------------------------------------------------

QSize ProjectBin::sizeHint() const
{
    return QSize(320, 450);
}

// -----------------------------------------------------------------------------
//  Event filter � key handling for search bar + list widget
// -----------------------------------------------------------------------------

bool ProjectBin::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_searchField && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            m_searchField->clear();
            // Return focus to whichever ancestor wants it (TimelineWorkspace)
            if (auto* p = parentWidget()) {
                // Walk up to the top-level workspace
                while (p->parentWidget() && p->parentWidget()->parentWidget())
                    p = p->parentWidget();
                p->setFocus();
            }
            return true;
        }
    }

    // Ctrl+F or '/' focuses the search bar (Premiere Pro behavior)
    if (obj == m_listWidget && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Slash ||
            (ke->key() == Qt::Key_F && (ke->modifiers() & Qt::ControlModifier))) {
            m_searchField->setFocus();
            m_searchField->selectAll();
            return true;
        }
    }

    // -- List widget / viewport: intercept drag-drop -- delegated to
    // ProjectBinDragDrop.cpp.
    if (obj == m_listWidget || obj == m_listWidget->viewport()) {
        if (event->type() == QEvent::Drop) {
            if (handleDropEvent(event))
                return true;
        }
        if (event->type() == QEvent::DragEnter) {
            if (handleDragEnterEvent(event))
                return true;
        }
        if (event->type() == QEvent::DragMove) {
            if (handleDragMoveEvent(event))
                return true;
        }
        if (event->type() == QEvent::DragLeave) {
            handleDragLeave();
        }
        // Double-click on empty space in list view → Import Media
        if (obj == m_listWidget->viewport() && event->type() == QEvent::MouseButtonDblClick) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                QTreeWidgetItem* item = m_listWidget->itemAt(me->pos());
                if (!item) {
                    importFiles();
                    return true;
                }
            }
        }
    }

    // -- Create Sequence button: accept drag-and-drop of media files ------
    if (obj == m_btnCreateSequence) {
        if (event->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                // Visual feedback: highlight the button
                m_btnCreateSequence->setStyleSheet(QStringLiteral(
                    "QToolButton { background: %1; border: none; color: %2; "
                    "font-size: 13px; border-radius: 3px; }")
                    .arg(Theme::hex(Theme::colors().accentDim))
                    .arg(Theme::hex(Theme::colors().accent)));
                return true;
            }
            // Also accept internal media drags
            if (de->mimeData()->hasFormat("application/x-roundtable-media")) {
                de->acceptProposedAction();
                m_btnCreateSequence->setStyleSheet(QStringLiteral(
                    "QToolButton { background: %1; border: none; color: %2; "
                    "font-size: 13px; border-radius: 3px; }")
                    .arg(Theme::hex(Theme::colors().accentDim))
                    .arg(Theme::hex(Theme::colors().accent)));
                return true;
            }
        }
        if (event->type() == QEvent::DragMove) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData()->hasUrls() ||
                de->mimeData()->hasFormat("application/x-roundtable-media")) {
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::DragLeave) {
            // Reset button style
            m_btnCreateSequence->setStyleSheet(QStringLiteral(
                "QToolButton { background: transparent; border: none; color: %1; "
                "font-size: 13px; border-radius: 3px; }"
                "QToolButton:hover { background: %2; color: %3; }"
                "QToolButton:pressed { background: %4; color: %5; }")
                .arg(Theme::hex(Theme::colors().textTertiary))
                .arg(Theme::hex(Theme::colors().controlBgHover))
                .arg(Theme::hex(Theme::colors().textPrimary))
                .arg(Theme::hex(Theme::colors().accentDim))
                .arg(Theme::hex(Theme::colors().accent)));
            return true;
        }
        if (event->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(event);
            // Reset button style
            m_btnCreateSequence->setStyleSheet(QStringLiteral(
                "QToolButton { background: transparent; border: none; color: %1; "
                "font-size: 13px; border-radius: 3px; }"
                "QToolButton:hover { background: %2; color: %3; }"
                "QToolButton:pressed { background: %4; color: %5; }")
                .arg(Theme::hex(Theme::colors().textTertiary))
                .arg(Theme::hex(Theme::colors().controlBgHover))
                .arg(Theme::hex(Theme::colors().textPrimary))
                .arg(Theme::hex(Theme::colors().accentDim))
                .arg(Theme::hex(Theme::colors().accent)));

            // External file drop (from Windows Explorer)
            if (de->mimeData()->hasUrls()) {
                for (const QUrl& url : de->mimeData()->urls()) {
                    if (url.isLocalFile()) {
                        std::filesystem::path p(url.toLocalFile().toStdString());
                        createSequenceFromMedia(p);
                        break; // Only create one sequence from the first file
                    }
                }
                de->acceptProposedAction();
                return true;
            }

            // Internal media drop (from the bin itself)
            if (de->mimeData()->hasFormat("application/x-roundtable-media")) {
                QByteArray mediaData = de->mimeData()->data("application/x-roundtable-media");
                // The data contains file paths separated by newlines
                QStringList paths = QString::fromUtf8(mediaData).split('\n', Qt::SkipEmptyParts);
                if (!paths.isEmpty()) {
                    std::filesystem::path p(paths.first().trimmed().toStdString());
                    createSequenceFromMedia(p);
                }
                de->acceptProposedAction();
                return true;
            }
        }
    }

    // -- List widget: Copy / Paste / Delete for sequences ----------------
    if (obj == m_listWidget && event->type() == QEvent::KeyPress && m_project) {
        auto* ke = static_cast<QKeyEvent*>(event);
        auto* selected = m_listWidget->currentItem();
        bool isSeq = selected && selected->data(0, Qt::UserRole + 3).toBool();
        size_t seqIdx = isSeq
            ? selected->data(0, Qt::UserRole + 4).toULongLong()
            : size_t(-1);

        // Ctrl+A � select all items
        if (ke->matches(QKeySequence::SelectAll)) {
            m_listWidget->selectAll();
            return true;
        }

        // Ctrl+C � copy selected sequence
        if (ke->matches(QKeySequence::Copy) && isSeq) {
            m_copiedSequenceIdx = static_cast<int>(seqIdx);
            spdlog::info("ProjectBin: copied sequence {} for paste", seqIdx);
            return true;
        }

        // Ctrl+V � paste (duplicate) copied sequence
        if (ke->matches(QKeySequence::Paste) &&
            m_copiedSequenceIdx >= 0 &&
            static_cast<size_t>(m_copiedSequenceIdx) < m_project->sequenceCount()) {
            size_t srcIdx = static_cast<size_t>(m_copiedSequenceIdx);
            if (m_commandStack) {
                size_t newIdx = m_project->sequenceCount();
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Paste Sequence",
                    [this, srcIdx]() {
                        m_project->duplicateSequence(srcIdx);
                        syncListView();
                        emit sequencesChanged();
                    },
                    [this, newIdx]() {
                        m_project->removeSequence(newIdx);
                        syncListView();
                        emit sequencesChanged();
                    }));
            } else {
                m_project->duplicateSequence(srcIdx);
                syncListView();
                emit sequencesChanged();
            }
            return true;
        }

        // Delete / Backspace � handle deletion
        if (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) {
            // ── Helpers for "propagate delete into timeline" (like Premiere Pro) ──
            struct TimelineRef { size_t seqIdx; size_t trackIdx; uint64_t clipId; };
            auto findTimelineRefs =
                [this](const std::vector<std::filesystem::path>& paths)
                -> std::vector<TimelineRef>
            {
                std::vector<TimelineRef> refs;
                if (!m_project || paths.empty()) return refs;
                auto normalize = [](const std::filesystem::path& p) {
                    std::error_code ec;
                    auto c = std::filesystem::weakly_canonical(p, ec);
                    auto s = (ec ? p : c).generic_string();
                    std::transform(s.begin(), s.end(), s.begin(),
                                   [](unsigned char ch){ return std::tolower(ch); });
                    return s;
                };
                std::set<std::string> needles;
                for (const auto& p : paths)
                    needles.insert(normalize(p));
                for (size_t si = 0; si < m_project->sequenceCount(); ++si) {
                    auto* tl = m_project->sequence(si);
                    if (!tl) continue;
                    for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
                        auto* tr = tl->track(ti);
                        if (!tr) continue;
                        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
                            Clip* c = tr->clip(ci);
                            if (!c) continue;
                            std::string clipPath;
                            if (auto* vc = dynamic_cast<VideoClip*>(c))
                                clipPath = vc->mediaPath();
                            else if (auto* ac = dynamic_cast<AudioClip*>(c))
                                clipPath = ac->mediaPath();
                            else if (auto* ic = dynamic_cast<ImageClip*>(c))
                                clipPath = ic->mediaPath();
                            if (clipPath.empty()) continue;
                            if (needles.count(normalize(clipPath)))
                                refs.push_back({si, ti, c->id()});
                        }
                    }
                }
                return refs;
            };

            // Shows a Premiere-style warning; returns true if deletion should
            // proceed, false if the user cancelled. When true, `refs` is
            // populated (empty when there were no timeline references).
            auto confirmTimelinePropagation =
                [this](const std::vector<TimelineRef>& foundRefs) -> bool
            {
                if (foundRefs.empty()) return true;
                QMessageBox box(this);
                box.setWindowTitle(tr("Clip is in use"));
                box.setIcon(QMessageBox::Warning);
                box.setText(tr("%1 clip(s) on the timeline reference this media.")
                                .arg(foundRefs.size()));
                box.setInformativeText(
                    tr("Deleting from the bin will also remove these clip(s) "
                       "from the timeline.\n\nProceed?"));
                box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
                box.setDefaultButton(QMessageBox::Cancel);
                return box.exec() == QMessageBox::Yes;
            };

            // Extracts clips matched by `refs` from their tracks.  The
            // returned vector owns the extracted clips (for later re-insertion
            // on undo).
            struct ExtractedClip {
                size_t seqIdx;
                size_t trackIdx;
                std::unique_ptr<Clip> clip;
            };
            auto extractTimelineClips =
                [this](const std::vector<TimelineRef>& refs)
                -> std::vector<ExtractedClip>
            {
                std::vector<ExtractedClip> out;
                out.reserve(refs.size());
                for (const auto& r : refs) {
                    auto* tl = m_project ? m_project->sequence(r.seqIdx) : nullptr;
                    if (!tl) continue;
                    auto* tr = tl->track(r.trackIdx);
                    if (!tr) continue;
                    auto removed = tr->removeClipById(r.clipId);
                    if (removed)
                        out.push_back({r.seqIdx, r.trackIdx, std::move(removed)});
                }
                return out;
            };
            auto restoreTimelineClips =
                [this](std::vector<ExtractedClip>& extracted)
            {
                for (auto& e : extracted) {
                    auto* tl = m_project ? m_project->sequence(e.seqIdx) : nullptr;
                    if (!tl) continue;
                    auto* tr = tl->track(e.trackIdx);
                    if (!tr) continue;
                    tr->addClip(std::move(e.clip));
                }
                extracted.clear();
            };

            // Recursively collect descendant media file paths under a bin
            // so that deleting the bin also removes its contents from the
            // grid model (otherwise syncListView() re-adds them at root,
            // "spilling" the deleted folder's contents).
            std::function<void(QTreeWidgetItem*, std::vector<std::filesystem::path>&)>
                collectDescendantFiles =
                [&](QTreeWidgetItem* node, std::vector<std::filesystem::path>& out) {
                    if (!node) return;
                    for (int i = 0; i < node->childCount(); ++i) {
                        QTreeWidgetItem* ch = node->child(i);
                        if (!ch) continue;
                        bool chIsBin = ch->data(0, Qt::UserRole + 2).toBool();
                        bool chIsSeq = ch->data(0, Qt::UserRole + 3).toBool();
                        if (chIsBin) {
                            collectDescendantFiles(ch, out);
                        } else if (!chIsSeq) {
                            QString fp = ch->data(0, Qt::UserRole).toString();
                            if (!fp.isEmpty())
                                out.emplace_back(fp.toStdString());
                        }
                    }
                };

            auto allSelected = m_listWidget->selectedItems();

            // Multi-select: mass-delete selected items
            if (allSelected.size() > 1) {
                auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());
                std::vector<std::filesystem::path> toRemove;
                // Collect bins to delete (process after media so children
                // are reparented correctly)
                std::vector<QTreeWidgetItem*> binsToDelete;
                for (auto* item : allSelected) {
                    bool itemIsSeq = item->data(0, Qt::UserRole + 3).toBool();
                    bool itemIsBin = item->data(0, Qt::UserRole + 2).toBool();
                    if (itemIsSeq) continue; // skip sequences in multi-delete
                    if (itemIsBin) {
                        // Also delete the bin's contents so they don't
                        // spill back to root after syncListView().
                        collectDescendantFiles(item, toRemove);
                        binsToDelete.push_back(item);
                        continue;
                    }
                    QString fp = item->data(0, Qt::UserRole).toString();
                    if (!fp.isEmpty())
                        toRemove.emplace_back(fp.toStdString());
                }
                // Check timeline for references and ask for confirmation
                auto timelineRefs = findTimelineRefs(toRemove);
                if (!confirmTimelinePropagation(timelineRefs))
                    return true;  // user cancelled — abort entire delete
                auto extractedTimelineClips =
                    std::make_shared<std::vector<ExtractedClip>>(
                        extractTimelineClips(timelineRefs));
                for (const auto& path : toRemove)
                    removeFile(path);
                // Delete bins and all their contents
                for (auto* binItem : binsToDelete) {
                    int idx = m_listWidget->indexOfTopLevelItem(binItem);
                    if (idx >= 0) {
                        delete m_listWidget->takeTopLevelItem(idx);
                    } else if (auto* parent = binItem->parent()) {
                        int ci = parent->indexOfChild(binItem);
                        if (ci >= 0) delete parent->takeChild(ci);
                    }
                }
                if (!toRemove.empty() || !binsToDelete.empty())
                    syncListView();

                if (!extractedTimelineClips->empty()) {
                    emit timelineClipsMutated();
                }

                if ((!toRemove.empty() || !binsToDelete.empty()) && m_commandStack) {
                    auto after = std::make_shared<ProjectBin::BinSnapshot>(captureBinSnapshot());
                    // Keep a ref list so redo can re-extract the same clips
                    auto timelineRefsShared =
                        std::make_shared<std::vector<TimelineRef>>(std::move(timelineRefs));
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete Bin Items",
                        [this, after, timelineRefsShared,
                         extractedTimelineClips, extractTimelineClips]() {
                            if (extractedTimelineClips->empty() && !timelineRefsShared->empty())
                                *extractedTimelineClips =
                                    extractTimelineClips(*timelineRefsShared);
                            this->applyBinSnapshot(*after);
                            if (!timelineRefsShared->empty())
                                emit timelineClipsMutated();
                        },
                        [this, before,
                         extractedTimelineClips, restoreTimelineClips]() {
                            this->applyBinSnapshot(*before);
                            if (!extractedTimelineClips->empty()) {
                                restoreTimelineClips(*extractedTimelineClips);
                                emit timelineClipsMutated();
                            }
                        }));
                }
                return true;
            }

            // Single selection: sequence
            if (isSeq && m_project->sequenceCount() > 1) {
                if (m_commandStack) {
                    auto extracted = m_project->extractSequence(seqIdx);
                    if (!extracted) return false;
                    auto shared = std::make_shared<std::unique_ptr<Timeline>>(std::move(extracted));
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete Sequence",
                        [this, seqIdx]() {
                            m_project->removeSequence(seqIdx);
                            syncListView();
                            emit sequencesChanged();
                        },
                        [this, seqIdx, shared]() {
                            m_project->insertSequence(seqIdx, std::move(*shared));
                            syncListView();
                            emit sequencesChanged();
                        }));
                    syncListView();
                    emit sequencesChanged();
                } else {
                    m_project->removeSequence(seqIdx);
                    syncListView();
                    emit sequencesChanged();
                }
                return true;
            }

            // Single selection: bin (delete bin AND its descendants so
            // contents don't spill back to root)
            bool isBin = selected && selected->data(0, Qt::UserRole + 2).toBool();
            if (isBin) {
                auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());
                QString binName = selected->text(0);
                spdlog::info("ProjectBin: deleting bin '{}'", binName.toStdString());

                // Remove media files contained in (or below) the bin first
                std::vector<std::filesystem::path> contents;
                collectDescendantFiles(selected, contents);

                // Check timeline for references and ask before propagating
                auto timelineRefs = findTimelineRefs(contents);
                if (!confirmTimelinePropagation(timelineRefs))
                    return true;  // user cancelled
                auto extractedTimelineClips =
                    std::make_shared<std::vector<ExtractedClip>>(
                        extractTimelineClips(timelineRefs));

                for (const auto& p : contents)
                    removeFile(p);

                int idx = m_listWidget->indexOfTopLevelItem(selected);
                if (idx >= 0) {
                    delete m_listWidget->takeTopLevelItem(idx);
                } else if (auto* parent = selected->parent()) {
                    int ci = parent->indexOfChild(selected);
                    if (ci >= 0) delete parent->takeChild(ci);
                }
                syncListView();

                if (!extractedTimelineClips->empty())
                    emit timelineClipsMutated();

                if (m_commandStack) {
                    auto after = std::make_shared<BinSnapshot>(captureBinSnapshot());
                    auto timelineRefsShared =
                        std::make_shared<std::vector<TimelineRef>>(std::move(timelineRefs));
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete Bin",
                        [this, after, timelineRefsShared,
                         extractedTimelineClips, extractTimelineClips]() {
                            if (extractedTimelineClips->empty() && !timelineRefsShared->empty())
                                *extractedTimelineClips =
                                    extractTimelineClips(*timelineRefsShared);
                            this->applyBinSnapshot(*after);
                            if (!timelineRefsShared->empty())
                                emit timelineClipsMutated();
                        },
                        [this, before,
                         extractedTimelineClips, restoreTimelineClips]() {
                            this->applyBinSnapshot(*before);
                            if (!extractedTimelineClips->empty()) {
                                restoreTimelineClips(*extractedTimelineClips);
                                emit timelineClipsMutated();
                            }
                        }));
                }
                return true;
            }

            // Single selection: media item
            if (!isSeq) {
                QString fp = selected ? selected->data(0, Qt::UserRole).toString() : QString();
                if (!fp.isEmpty()) {
                    auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());

                    std::vector<std::filesystem::path> contents{
                        std::filesystem::path(fp.toStdString())};
                    auto timelineRefs = findTimelineRefs(contents);
                    if (!confirmTimelinePropagation(timelineRefs))
                        return true;  // user cancelled
                    auto extractedTimelineClips =
                        std::make_shared<std::vector<ExtractedClip>>(
                            extractTimelineClips(timelineRefs));

                    removeFile(std::filesystem::path(fp.toStdString()));
                    syncListView();

                    if (!extractedTimelineClips->empty())
                        emit timelineClipsMutated();

                    if (m_commandStack) {
                        auto after = std::make_shared<BinSnapshot>(captureBinSnapshot());
                        auto timelineRefsShared =
                            std::make_shared<std::vector<TimelineRef>>(std::move(timelineRefs));
                        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                            "Delete Bin Item",
                            [this, after, timelineRefsShared,
                             extractedTimelineClips, extractTimelineClips]() {
                                if (extractedTimelineClips->empty() && !timelineRefsShared->empty())
                                    *extractedTimelineClips =
                                        extractTimelineClips(*timelineRefsShared);
                                this->applyBinSnapshot(*after);
                                if (!timelineRefsShared->empty())
                                    emit timelineClipsMutated();
                            },
                            [this, before,
                             extractedTimelineClips, restoreTimelineClips]() {
                                this->applyBinSnapshot(*before);
                                if (!extractedTimelineClips->empty()) {
                                    restoreTimelineClips(*extractedTimelineClips);
                                    emit timelineClipsMutated();
                                }
                            }));
                    }
                }
                return true;
            }
        }
    }

    return QWidget::eventFilter(obj, event);
}

// -----------------------------------------------------------------------------
//  Slots
// -----------------------------------------------------------------------------

void ProjectBin::onFilterChanged(int index)
{
    switch (index)
    {
    case 0: m_activeType = MediaType::Unknown; break; // All
    case 1: m_activeType = MediaType::Video;   break;
    case 2: m_activeType = MediaType::Image;   break;
    case 3: m_activeType = MediaType::Audio;   break;
    case 4: m_activeType = MediaType::Spine;   break;
    default: m_activeType = MediaType::Unknown; break;
    }

    m_grid->setTypeFilter(m_activeType);
    m_grid->loadVisibleThumbnails();
    syncListView();
}

void ProjectBin::onSearchChanged(const QString& text)
{
    m_grid->setFilter(text);
    if (m_listView)
        syncListView();
}

void ProjectBin::onZoomChanged(int value)
{
    float zoom = static_cast<float>(value) / 100.0f;
    m_grid->setZoom(zoom);
    m_grid->loadVisibleThumbnails();

    // In list view, scale the row height
    if (m_listView) {
        int rowH = std::max(16, static_cast<int>(16 * zoom));
        int iconSz = std::max(12, rowH - 4);
        m_listWidget->setIconSize(QSize(iconSz, iconSz));
        m_listWidget->setStyleSheet(
            m_listWidget->styleSheet()
            + QStringLiteral(" QTreeView::item { height: %1px; }").arg(rowH));
    }
}

void ProjectBin::onItemDoubleClicked(int index, const std::filesystem::path& filePath)
{
    // Check if it's a folder item — navigate into it (only if it's a real bin)
    if (index >= 0 && index < static_cast<int>(m_grid->items().size())) {
        const auto& gridItem = m_grid->items()[index];
        if (gridItem.isFolder) {
            // Verify it's an actual bin (not a sequence shown as folder)
            QTreeWidgetItem* container = nullptr;
            for (const auto& seg : m_iconBinPath) {
                int cnt = container ? container->childCount()
                                    : m_listWidget->topLevelItemCount();
                for (int i = 0; i < cnt; ++i) {
                    auto* c = container ? container->child(i)
                                        : m_listWidget->topLevelItem(i);
                    if (c->data(0, Qt::UserRole + 2).toBool() && c->text(0) == seg) {
                        container = c;
                        break;
                    }
                }
            }
            int cnt = container ? container->childCount()
                                : m_listWidget->topLevelItemCount();
            bool isBin = false;
            for (int i = 0; i < cnt; ++i) {
                auto* c = container ? container->child(i)
                                    : m_listWidget->topLevelItem(i);
                if (c->data(0, Qt::UserRole + 2).toBool() &&
                    c->text(0) == gridItem.folderName) {
                    isBin = true;
                    break;
                }
            }
            if (isBin) {
                QStringList binPath = m_iconBinPath;
                binPath.append(gridItem.folderName);
                openBinTab(binPath, gridItem.folderName);
                return;
            }
            // Sequence — try to open it
            if (m_project) {
                for (size_t si = 0; si < m_project->sequenceCount(); ++si) {
                    if (QString::fromStdString(m_project->sequence(si)->name()) == gridItem.folderName) {
                        emit sequenceOpened(si);
                        return;
                    }
                }
            }
            return;
        }
    }

    // Media item — load in source monitor
    uint64_t handle = 0;
    const auto* item = m_grid->selectedItem();
    if (item)
        handle = item->mediaHandle;

    emit loadInSourceMonitor(filePath, handle);
}

void ProjectBin::onListItemDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;

    // Bins: open in a new tab
    if (item->data(0, Qt::UserRole + 2).toBool()) {
        // Build the full bin path by walking up the tree
        QStringList binPath;
        for (auto* p = item; p; p = p->parent()) {
            if (p->data(0, Qt::UserRole + 2).toBool())
                binPath.prepend(p->text(0));
        }
        openBinTab(binPath, item->text(0));
        return;
    }

    // Sequences: open in timeline
    if (item->data(0, Qt::UserRole + 3).toBool()) {
        size_t seqIdx = item->data(0, Qt::UserRole + 4).toULongLong();
        emit sequenceOpened(seqIdx);
        return;
    }

    // Media items: load in source monitor
    std::filesystem::path filePath(item->data(0, Qt::UserRole).toString().toStdString());
    uint64_t handle = item->data(0, Qt::UserRole + 1).toULongLong();
    emit loadInSourceMonitor(filePath, handle);
}

void ProjectBin::setListView(bool listMode)
{
    m_listView = listMode;
    m_btnListView->setChecked(listMode);
    m_btnIconView->setChecked(!listMode);
    m_listWidget->setVisible(listMode);
    m_scrollArea->setVisible(!listMode);
    m_iconNavBar->setVisible(!listMode);

    // Apply current zoom to whichever view is active
    onZoomChanged(m_zoomSlider->value());

    if (listMode) {
        syncListView();
    } else {
        // Respect the current bin tab's path
        if (m_binTabBar) {
            int idx = m_binTabBar->currentIndex();
            if (idx >= 0 && idx < m_binTabPaths.size())
                m_iconBinPath = m_binTabPaths[idx];
            else
                m_iconBinPath.clear();
        } else {
            m_iconBinPath.clear();
        }
        syncIconView();
    }
}

void ProjectBin::openBinTab(const QStringList& binPath, const QString& name)
{
    // Check if a tab for this bin already exists
    for (int i = 0; i < m_binTabPaths.size(); ++i) {
        if (m_binTabPaths[i] == binPath) {
            m_binTabBar->setCurrentIndex(i);
            return;
        }
    }
    // Create a new tab
    int idx = m_binTabBar->addTab(name);
    m_binTabPaths.append(binPath);
    m_binTabBar->setCurrentIndex(idx);
}

void ProjectBin::onBinTabChanged(int index)
{
    if (index < 0 || index >= m_binTabPaths.size())
        return;

    m_iconBinPath = m_binTabPaths[index];

    // Non-root tabs always show icon view for that bin
    if (index > 0 && m_listView)
        setListView(false);
    else if (!m_listView)
        syncIconView();
}

void ProjectBin::onBinTabCloseRequested(int index)
{
    // Never close the root "Project" tab
    if (index <= 0)
        return;

    m_binTabBar->removeTab(index);
    m_binTabPaths.removeAt(index);

    // If we closed the active tab, switch to root
    if (m_binTabBar->currentIndex() < 0)
        m_binTabBar->setCurrentIndex(0);
}

void ProjectBin::revealByPath(const QString& filePath)
{
    if (!m_listWidget) return;

    // Normalize the incoming path for comparison
    QString normalized = QFileInfo(filePath).absoluteFilePath();

    // Recursively search all items (top-level and children of bin folders)
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findItem;
    findItem = [&](QTreeWidgetItem* parent) -> QTreeWidgetItem* {
        int count = parent ? parent->childCount()
                          : m_listWidget->topLevelItemCount();
        for (int i = 0; i < count; ++i) {
            auto* item = parent ? parent->child(i)
                                : m_listWidget->topLevelItem(i);
            // Skip bin folders
            if (item->data(0, Qt::UserRole + 2).toBool()) {
                if (auto* found = findItem(item))
                    return found;
                continue;
            }
            QString itemPath = QFileInfo(item->data(0, Qt::UserRole).toString()).absoluteFilePath();
            if (itemPath == normalized)
                return item;
        }
        return nullptr;
    };

    if (auto* item = findItem(nullptr)) {
        // Expand parent bins so the item is visible
        for (auto* p = item->parent(); p; p = p->parent())
            p->setExpanded(true);
        m_listWidget->setCurrentItem(item);
        m_listWidget->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
}


// =============================================================================
//  Sequence creation
// =============================================================================

void ProjectBin::createNewSequence()
{
    // Show dialog FIRST — no project is created until user confirms
    SequenceDialog dlg(this);
    dlg.setWindowTitle(tr("New Sequence"));

    if (m_project) {
        dlg.setMediaProperties(
            m_project->settings().resolution().width,
            m_project->settings().resolution().height,
            m_project->settings().frameRate());
        dlg.setSequenceName(QString::fromStdString(m_project->nextSequenceName()));
    } else {
        dlg.setMediaProperties(1920, 1080, 30.0);
        dlg.setSequenceName(QStringLiteral("Sequence 1"));
    }

    if (dlg.exec() != QDialog::Accepted)
        return;

    QString seqName = dlg.sequenceName();
    uint32_t w = dlg.width();
    uint32_t h = dlg.height();
    double fps = dlg.frameRate();
    std::string name = seqName.toStdString();

    // Auto-create project if none exists, with the chosen settings
    if (!m_project) {
        auto* newProj = new Project();
        newProj->setName("Untitled");
        newProj->settings().setResolution(w, h);
        newProj->settings().setFrameRate(fps);
        // Name the default sequence what the user chose
        if (newProj->sequenceCount() > 0 && newProj->sequence(0))
            newProj->sequence(0)->setName(name);
        emit projectCreated(newProj);
        if (!m_project) { delete newProj; return; }
        // Bin already reflects the project — just signal the sequence
        emit sequencesChanged();
        emit sequenceOpened(0);
    } else {
        // Existing project: update settings and add a new sequence
        m_project->settings().setResolution(w, h);
        m_project->settings().setFrameRate(fps);
        if (m_commandStack) {
            size_t newIdx = m_project->sequenceCount();
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Add Sequence '" + name + "'",
                [this, name, newIdx]() {
                    m_project->addSequence(name);
                    syncListView();
                    emit sequencesChanged();
                    emit sequenceOpened(newIdx);
                },
                [this, newIdx]() {
                    m_project->removeSequence(newIdx);
                    syncListView();
                    emit sequencesChanged();
                }));
        } else {
            m_project->addSequence(name);
            syncListView();
            emit sequencesChanged();
            emit sequenceOpened(m_project->sequenceCount() - 1);
        }
    }
}

// -----------------------------------------------------------------------------
//  Color Matte (Premiere Pro-style)
// -----------------------------------------------------------------------------

void ProjectBin::createColorMatte()
{
    // 1. Pick a color
    QColor color = QColorDialog::getColor(Qt::white, this,
                                          tr("Choose Color Matte Color"),
                                          QColorDialog::ShowAlphaChannel);
    if (!color.isValid())
        return;

    // 2. Ask for a name
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New Color Matte"),
                                         tr("Matte name:"),
                                         QLineEdit::Normal,
                                         QStringLiteral("Color Matte"), &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty())
        return;

    // 3. Determine output directory
    std::filesystem::path matteDir;
    if (m_project && !m_project->filePath().empty()) {
        // Place alongside the project file
        matteDir = m_project->filePath().parent_path() / "Mattes";
    } else {
        // Fallback to user data directory
        matteDir = std::filesystem::path(userDataDir().toStdString()) / "Mattes";
    }
    std::filesystem::create_directories(matteDir);

    // 4. Generate a unique filename
    QString safeName = name;
    safeName.replace(QRegularExpression(R"([<>:"/\\|?*])"), QStringLiteral("_"));
    std::filesystem::path mattePath = matteDir / (safeName.toStdString() + ".png");
    {
        int counter = 1;
        while (std::filesystem::exists(mattePath)) {
            mattePath = matteDir / (safeName.toStdString() + "_" + std::to_string(counter++) + ".png");
        }
    }

    // 5. Create the solid-color PNG (1920x1080 like Premiere's default)
    QImage matteImage(1920, 1080, QImage::Format_ARGB32_Premultiplied);
    matteImage.fill(color);
    if (!matteImage.save(QString::fromStdString(mattePath.string()), "PNG")) {
        spdlog::error("Failed to save color matte: {}", mattePath.string());
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to save color matte image."));
        return;
    }

    // 6. Import the generated matte into the project bin
    addFiles({mattePath});
}

void ProjectBin::createSequenceFromMedia(const std::filesystem::path& filePath)
{
    if (!m_project) return;

    // Determine media properties from the MediaPool
    uint32_t mediaW = 0, mediaH = 0;
    double mediaFps = 30.0;
    double mediaDurationSec = 0.0;
    bool mediaHasAudio = false;

    if (m_pool) {
        uint64_t handle = m_pool->open(filePath.string());
        if (handle != 0) {
            const auto* info = m_pool->getInfo(handle);
            if (info) {
                mediaW = info->width;
                mediaH = info->height;
                if (info->fps > 0.0) mediaFps = info->fps;
                mediaDurationSec = info->duration;
                mediaHasAudio = info->hasAudio;
            }
        }
    }

    // Default to 1920x1080 30fps if no media info
    if (mediaW == 0 || mediaH == 0) {
        mediaW = 1920;
        mediaH = 1080;
    }

    // Update project settings to match media
    m_project->settings().setResolution(mediaW, mediaH);
    m_project->settings().setFrameRate(mediaFps);

    // Create a sequence named after the media file
    QString stem = QFileInfo(QString::fromStdString(filePath.string())).completeBaseName();
    if (stem.isEmpty()) stem = QStringLiteral("Sequence");
    std::string seqName = stem.toStdString();

    // Compute clip duration in ticks
    int64_t clipDuration = secondsToTicks(mediaDurationSec);
    if (clipDuration <= 0)
        clipDuration = secondsToTicks(5.0); // default 5 seconds

    // Determine media type from extension
    std::string ext = filePath.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool isVideo = (ext == ".mp4" || ext == ".mov" || ext == ".mkv" ||
                          ext == ".webm" || ext == ".avi" || ext == ".m4v");
    const bool isAudio = (ext == ".wav" || ext == ".mp3" || ext == ".flac" ||
                          ext == ".ogg" || ext == ".m4a" || ext == ".aac" || ext == ".opus");
    const bool isImage = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                          ext == ".bmp" || ext == ".gif" || ext == ".tga" ||
                          ext == ".tiff" || ext == ".webp");

    // Pre-build the timeline so undo/redo swaps it cleanly via
    // insertSequence/extractSequence (no dangling pointer window).
    auto builtTimeline = std::make_unique<Timeline>();
    builtTimeline->setName(seqName);
    std::string fileStr = filePath.string();

    if (isVideo) {
        // Replace default V1+A1 with our populated tracks
        while (builtTimeline->trackCount() > 0)
            builtTimeline->removeTrack(0);

        Track* vTrack = builtTimeline->addVideoTrack("Video 1");
        auto vClip = std::make_unique<VideoClip>(fileStr);
        vClip->setTimelineIn(0);
        vClip->setDuration(clipDuration);
        vClip->setSourceIn(0);
        vClip->setSourceResolution(mediaW, mediaH);
        vClip->setSourceFps(mediaFps);
        vClip->setSourceDuration(clipDuration);
        vClip->setLabel(QFileInfo(QString::fromStdString(fileStr))
                            .fileName().toStdString());
        vTrack->addClip(std::move(vClip));

        if (mediaHasAudio) {
            Track* aTrack = builtTimeline->addAudioTrack("Audio 1");
            auto aClip = std::make_unique<AudioClip>(fileStr);
            aClip->setTimelineIn(0);
            aClip->setDuration(clipDuration);
            aClip->setSourceIn(0);
            aClip->setSourceDuration(clipDuration);
            aClip->setLabel(QFileInfo(QString::fromStdString(fileStr))
                                .fileName().toStdString());
            aTrack->addClip(std::move(aClip));
        }
    } else if (isImage) {
        while (builtTimeline->trackCount() > 0)
            builtTimeline->removeTrack(0);

        Track* vTrack = builtTimeline->addVideoTrack("Video 1");
        auto iClip = std::make_unique<ImageClip>(fileStr);
        iClip->setTimelineIn(0);
        iClip->setDuration(clipDuration);
        iClip->setSourceIn(0);
        iClip->setSourceResolution(mediaW, mediaH);
        iClip->setLabel(QFileInfo(QString::fromStdString(fileStr))
                            .fileName().toStdString());
        vTrack->addClip(std::move(iClip));
    } else if (isAudio) {
        while (builtTimeline->trackCount() > 0)
            builtTimeline->removeTrack(0);

        Track* aTrack = builtTimeline->addAudioTrack("Audio 1");
        auto aClip = std::make_unique<AudioClip>(fileStr);
        aClip->setTimelineIn(0);
        aClip->setDuration(clipDuration);
        aClip->setSourceIn(0);
        aClip->setSourceDuration(clipDuration);
        aClip->setLabel(QFileInfo(QString::fromStdString(fileStr))
                            .fileName().toStdString());
        aTrack->addClip(std::move(aClip));
    }

    // Wrap in shared_ptr so we can move the unique_ptr through std::function captures
    auto sharedTimeline = std::make_shared<std::unique_ptr<Timeline>>(
        std::move(builtTimeline));

    size_t newIdx = m_project->sequenceCount();

    auto addSeqCmd = std::make_unique<LambdaCommand>(
        "Add Sequence '" + seqName + "'",
        [this, newIdx, sharedTimeline]() {
            m_project->insertSequence(newIdx, std::move(*sharedTimeline));
            *sharedTimeline = nullptr;
            syncListView();
            emit sequencesChanged();
            emit sequenceOpened(newIdx);
        },
        [this, newIdx, sharedTimeline]() {
            *sharedTimeline = m_project->extractSequence(newIdx);
            syncListView();
            emit sequencesChanged();
        });

    if (m_commandStack) {
        m_commandStack->execute(std::move(addSeqCmd));
    } else {
        m_project->insertSequence(newIdx, std::move(*sharedTimeline));
        *sharedTimeline = nullptr;
        syncListView();
        emit sequencesChanged();
        emit sequenceOpened(newIdx);
    }
}

} // namespace rt

