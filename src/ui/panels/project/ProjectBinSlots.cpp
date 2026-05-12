/*
 * ProjectBinSlots.cpp — Slot implementations for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: onFilterChanged, onSearchChanged, onZoomChanged,
 * onItemDoubleClicked, onListItemDoubleClicked, setListView,
 * openBinTab, onBinTabChanged, onBinTabCloseRequested, revealByPath
 */

#include "panels/project/ProjectBin.h"
#include "panels/project/ProjectBinInternal.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"

#include <QFileInfo>
#include <QLineEdit>
#include <QTreeWidgetItem>
#include <QHeaderView>

#include <cmath>
#include <functional>

namespace rt {

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

} // namespace rt
