/*
 * ProjectBinQueries.cpp — Query methods for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: allFiles, filesOfType, binFolderState, restoreBinFolders
 */

#include "panels/project/ProjectBin.h"
#include "panels/project/ProjectBinInternal.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"

#include <QTreeWidgetItem>

#include <algorithm>
#include <functional>
#include <map>

namespace rt {

std::vector<std::filesystem::path> ProjectBin::allFiles() const
{
    std::vector<std::filesystem::path> result;
    const auto& items = m_grid->items();
    result.reserve(items.size());
    for (const auto& item : items) {
        // Skip synthetic items (sequences, bin folders) introduced by
        // syncIconView() — they have empty file paths and saving them
        // would create spurious "Unknown"-type entries on reload.
        if (item.isFolder || item.filePath.empty())
            continue;
        result.push_back(item.filePath);
    }
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
                    folder.childKeys.push_back(
                        projectBinItemKey(child).toStdString());
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

} // namespace rt
