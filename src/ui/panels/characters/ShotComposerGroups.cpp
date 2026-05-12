/*
 * ShotComposerGroups.cpp — Layer group operations for ShotComposer.
 * Extracted from ShotComposer.cpp for maintainability.
 */

#include "panels/characters/ShotComposer.h"

#include "panels/characters/ShotComposerInternal.h"
#include "Theme.h"

#include <QListWidget>

#include <algorithm>
#include <spdlog/spdlog.h>

namespace rt {

// =====================================================================
//  Group operations (UI-only — does not affect ShotPreset data model)
// =====================================================================

void ShotComposer::groupSelectedLayers()
{
    auto sel = m_layerList->selectionModel()->selectedRows();
    if (sel.size() < 1) return;

    pushUndoState();

    // Collect selected rows in order (sorted ascending)
    std::vector<int> rows;
    for (const auto& idx : sel)
        rows.push_back(idx.row());
    std::sort(rows.begin(), rows.end());

    // Build the group info
    LayerGroupInfo grp;
    grp.name = "Group";
    grp.expanded = true;
    grp.firstChild = rows.front();
    grp.lastChild  = rows.back();

    // Give the group a unique name if "Group" is already taken
    int suffix = 2;
    QString baseName = QString::fromStdString(grp.name);
    for (;;) {
        bool conflict = false;
        for (const auto& g : m_layerGroups) {
            if (g.name == grp.name) { conflict = true; break; }
        }
        if (!conflict) break;
        grp.name = QString("Group %1").arg(suffix++).toStdString();
    }

    m_layerGroups.push_back(grp);

    // Update selection to the group (it appears at the group insertion point in the list)
    m_selectedLayer = rows.front();
    refreshLayerList();
    m_layerList->setCurrentRow(m_selectedLayer);
    updatePreview();
    emit shotChanged();

    spdlog::info("ShotComposer: grouped {} layers into '{}'", rows.size(), grp.name);
}

void ShotComposer::ungroupSelectedGroup()
{
    if (m_selectedLayer < 0) return;

    // Find which group the selected layer belongs to
    int grpIdx = -1;
    for (int i = 0; i < static_cast<int>(m_layerGroups.size()); ++i) {
        const auto& g = m_layerGroups[i];
        if (m_selectedLayer >= g.firstChild && m_selectedLayer <= g.lastChild) {
            grpIdx = i;
            break;
        }
    }
    if (grpIdx < 0) return;

    pushUndoState();
    m_layerGroups.erase(m_layerGroups.begin() + grpIdx);
    m_selectedLayer = -1;
    refreshLayerList();
    clearLayerProperties();
    updatePreview();
    emit shotChanged();
}

void ShotComposer::addEmptyGroup()
{
    pushUndoState();
    LayerGroupInfo grp;
    grp.name = "Group";
    grp.expanded = true;
    // Empty group sits at the end of the layer list with no children.
    // We use firstChild = -1 to indicate it's an empty group folder.
    grp.firstChild = -1;
    grp.lastChild  = -1;

    // Make name unique
    int suffix = 2;
    for (;;) {
        bool conflict = false;
        for (const auto& g : m_layerGroups) {
            if (g.name == grp.name) { conflict = true; break; }
        }
        if (!conflict) break;
        grp.name = QString("Group %1").arg(suffix++).toStdString();
    }

    m_layerGroups.push_back(grp);

    // Select the last item in the layer list (or the group if empty shot)
    int selectIdx = m_currentShot.layerCount() > 0 ? m_currentShot.layerCount() - 1 : 0;
    if (selectIdx >= 0) {
        m_selectedLayer = selectIdx;
        refreshLayerList();
        m_layerList->setCurrentRow(m_selectedLayer);
    }
    emit shotChanged();
}

} // namespace rt
