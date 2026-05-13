/*
 * ProjectPanel.cpp — V5 "Inline Stage" project manager coordinator.
 *
 * Thin coordinator (~160 lines) after extracting helpers, table, new-panel,
 * side-panel, context-menus, events, and thumbnails into sub-files.
 *
 * Sub-files (all in panels/project/):
 *   ProjectPanelUI.cpp          — setupUI() (1297 lines)
 *   ProjectPanelHelpers.cpp     — formatFileSize, formatDate, createProjectThumb,
 *                                 addRecentSaveLocation, rebuildRecentPathButtons
 *   ProjectPanelTable.cpp       — rebuildTable, selectedProjectName,
 *                                 updateActionButtons, applyResponsiveLayout
 *   ProjectPanelNewPanel.cpp    — rebuildResGrid, updateSummaryLabels,
 *                                 applyNewPanelResponsiveLayout
 *   ProjectPanelSidePanel.cpp   — toggleSidePanel, showSidePanel, hideSidePanel,
 *                                 onCreateClicked, onSearchTextChanged,
 *                                 onSortChanged, onFileTreeDoubleClicked,
 *                                 onOpenListItemDoubleClicked, populateOpenList
 *   ProjectPanelContextMenu.cpp — showContextMenu, showOpenListContextMenu
 *   ProjectPanelEvents.cpp      — keyPressEvent, mousePressEvent, eventFilter,
 *                                 resizeEvent
 *   ProjectPanelThumbnails.cpp  — thumbnailPathForProject, removeThumbnailForProject,
 *                                 setThumbnailForProject, setThumbnailFromPixels,
 *                                 setThumbnailFromImage
 */

#include "panels/project/ProjectPanel.h"

#include "Theme.h"
#include "Settings.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTreeView>

namespace rt {

// =============================================================================
// Construction
// =============================================================================

ProjectPanel::ProjectPanel(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setupUI();
    setFocusPolicy(Qt::StrongFocus);

    auto settings = rt::appSettings();
    m_recentSaveLocations = settings.value("ProjectPanel/RecentSaveLocations").toStringList();
    rebuildRecentPathButtons();
}

ProjectPanel::~ProjectPanel()
{
    m_destroying.store(true);

    auto settings = rt::appSettings();
    if (m_projectTable) {
        settings.setValue("ProjectPanel/HeaderState",
                          m_projectTable->horizontalHeader()->saveState());
    }
}

// =============================================================================
// Public API
// =============================================================================

void ProjectPanel::setProjects(const QVector<ProjectInfo>& projects)
{
    m_allProjects = projects;
    for (auto& p : m_allProjects)
        p.isCurrent = (!m_currentProjectName.isEmpty()
                       && p.name == m_currentProjectName);
    rebuildTable();
}

void ProjectPanel::setCurrentProjectName(const QString& name)
{
    m_currentProjectName = name;
    for (auto& p : m_allProjects)
        p.isCurrent = (!name.isEmpty() && p.name == name);
    rebuildTable();
}

void ProjectPanel::setRecentProjects(const QStringList& paths)
{
    m_recentPaths = paths;
}

QString ProjectPanel::projectFilePath(const QString& name) const
{
    for (const auto& p : m_allProjects) {
        if (p.name == name)
            return p.filePath;
    }
    return {};
}

void ProjectPanel::setProjectsDirectory(const QString& dir)
{
    m_projectsDir = dir;
    if (m_fileModel)
        m_fileModel->setRootPath(dir);
    if (m_fileTree && m_fileModel)
        m_fileTree->setRootIndex(m_fileModel->index(dir));
    if (m_sidePanelMode == SidePanelMode::Open)
        populateOpenList();
}

uint32_t ProjectPanel::customResWidth()  const
{
    if (m_customResRow && m_customResRow->isVisible())
        return static_cast<uint32_t>(m_customResW->value());
    if (m_resGroup) {
        auto* checked = m_resGroup->checkedButton();
        if (checked) {
            bool ok = false;
            uint32_t w = checked->property("resW").toUInt(&ok);
            if (ok && w > 0)
                return w;
        }
    }
    return 1920;
}

uint32_t ProjectPanel::customResHeight() const
{
    if (m_customResRow && m_customResRow->isVisible())
        return static_cast<uint32_t>(m_customResH->value());
    if (m_resGroup) {
        auto* checked = m_resGroup->checkedButton();
        if (checked) {
            bool ok = false;
            uint32_t h = checked->property("resH").toUInt(&ok);
            if (ok && h > 0)
                return h;
        }
    }
    return 1080;
}

double ProjectPanel::customFps() const
{
    if (m_customFpsRow && m_customFpsRow->isVisible())
        return m_customFps->value();
    if (m_fpsGroup) {
        auto* checked = m_fpsGroup->checkedButton();
        if (checked) {
            QString txt = checked->text().trimmed();
            if (txt == "Custom")
                return m_customFps ? m_customFps->value() : 30.0;
            return txt.toDouble();
        }
    }
    return 30.0;
}

} // namespace rt
