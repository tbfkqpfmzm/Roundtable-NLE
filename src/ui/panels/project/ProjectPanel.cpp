/*
 * ProjectPanel.cpp - V5 "Inline Stage" project manager implementation.
 *
 * Widescreen-optimised layout with icon-rail sidebar, full-width project
 * table (with screenshot thumbnails), and inline expanding side panels
 * for NEW (creation form) and OPEN (file browser) actions.
 *
 * No floating windows - everything is self-contained in the layout.
 */

#include "panels/project/ProjectPanel.h"

#include "Theme.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

#include <spdlog/spdlog.h>

namespace rt {

// =============================================================================
// Helpers
// =============================================================================

static QString formatFileSize(qint64 bytes)
{
    if (bytes < 1024)        return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
}

static QString formatDate(const QDateTime& dt)
{
    if (!dt.isValid()) return "Unknown";
    QDateTime now = QDateTime::currentDateTime();
    int daysAgo = dt.daysTo(now);
    if (daysAgo == 0) return "Today " + dt.toString("h:mm AP");
    if (daysAgo == 1) return "Yesterday " + dt.toString("h:mm AP");
    if (daysAgo < 7)  return dt.toString("dddd h:mm AP");
    return dt.toString("MMM d, yyyy");
}

/// Create a project thumbnail widget for a table cell.
/// Shows actual screenshot if available (projectDir/thumbs/<name>.png or .jpg),
/// otherwise renders a gradient preview with aspect-ratio badge.
/// Includes a small camera button to set a custom thumbnail.
static QWidget* createProjectThumb(const ProjectInfo& info, QWidget* parent,
                                    ProjectPanel* panel)
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    auto* container = new QWidget(parent);
    // Let the container fill the cell naturally instead of fixed size
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* lay = new QHBoxLayout(container);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setAlignment(Qt::AlignCenter);

    // Compute thumbnail dimensions to show the project's aspect ratio
    double aspect = static_cast<double>(info.resW)
                  / std::max(1u, info.resH);
    int thumbH = 160;
    int thumbW = static_cast<int>(thumbH * aspect);
    thumbW = qBound(60, thumbW, 260);

    // Wrapper to hold thumb label + overlay button
    auto* thumbFrame = new QWidget(container);
    thumbFrame->setFixedSize(thumbW, thumbH);
    thumbFrame->setStyleSheet("background: transparent;");

    auto* thumb = new QLabel(thumbFrame);
    thumb->setGeometry(0, 0, thumbW, thumbH);

    // Try to load screenshot thumbnail (next to .rtp in project subfolder)
    QFileInfo fi(info.filePath);
    QString thumbPathPng = fi.absolutePath() + "/" + fi.baseName() + ".png";
    QString thumbPathJpg = fi.absolutePath() + "/" + fi.baseName() + ".jpg";
    QPixmap pix(thumbPathPng);
    if (pix.isNull()) pix.load(thumbPathJpg);

    if (!pix.isNull()) {
        // Actual screenshot available
        thumb->setPixmap(pix.scaled(thumbW, thumbH,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setStyleSheet(QStringLiteral(
            "border-radius: %1px; border: 1px solid %2; background: %3;")
            .arg(m.radiusSm)
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.surface2)));
    } else {
        // No screenshot - render gradient placeholder with aspect badge
        QColor bg = info.isCurrent ? c.accentDim : c.surface3;
        QString tag = (info.resH > info.resW) ? "9:16"
                    : (info.resW >= 3840)     ? "4K"
                    :                           "16:9";
        thumb->setText(tag);
        thumb->setStyleSheet(QStringLiteral(
            "background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
            "stop:0 %1, stop:1 %2);"
            " border-radius: %3px; border: 1px solid %4;"
            " color: %5; font-size: 22px; font-weight: 700;")
            .arg(Theme::rgb(bg))
            .arg(Theme::rgb(info.isCurrent ? c.accent : c.surface2))
            .arg(m.radiusSm)
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(info.isCurrent ? c.textPrimary : c.textTertiary)));
        thumb->setAlignment(Qt::AlignCenter);
    }

    // Camera button overlay (bottom-right corner)
    auto* camBtn = new QPushButton(QStringLiteral("\U0001F4F7"), thumbFrame);
    camBtn->setFixedSize(30, 30);
    camBtn->move(thumbW - 34, thumbH - 34);
    camBtn->setCursor(Qt::PointingHandCursor);
    camBtn->setToolTip("Set thumbnail image...");
    camBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: rgba(0,0,0,160); border: none;"
        "  border-radius: 6px; font-size: 16px; color: white; padding: 0; }"
        "QPushButton:hover { background: rgba(0,0,0,220); }"));

    QString projectName = info.name;
    QObject::connect(camBtn, &QPushButton::clicked, panel,
                     [panel, projectName]() {
                         panel->setThumbnailForProject(projectName);
                     });

    lay->addWidget(thumbFrame);
    return container;
}

// =============================================================================
// Construction
// =============================================================================

ProjectPanel::ProjectPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
    setFocusPolicy(Qt::StrongFocus);
}

ProjectPanel::~ProjectPanel()
{
    // Persist table header layout (column widths, order, hidden state)
    QSettings settings("ROUNDTABLE", "NLE");
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

void ProjectPanel::setProjectsDirectory(const QString& dir)
{
    m_projectsDir = dir;
    if (m_fileModel)
        m_fileModel->setRootPath(dir);
    if (m_fileTree && m_fileModel)
        m_fileTree->setRootIndex(m_fileModel->index(dir));
    // Refresh the thumbnail list if the OPEN page is visible
    if (m_sidePanelMode == SidePanelMode::Open)
        populateOpenList();
}

uint32_t ProjectPanel::customResWidth()  const
{
    if (m_customResRow && m_customResRow->isVisible())
        return static_cast<uint32_t>(m_customResW->value());
    // Read from the selected resolution button
    if (m_resGroup) {
        auto* checked = m_resGroup->checkedButton();
        if (checked) {
            QString txt = checked->text();
            int xIdx = txt.indexOf(QChar(0x00D7));
            if (xIdx > 0)
                return txt.left(xIdx).toUInt();
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
            QString txt = checked->text();
            int xIdx = txt.indexOf(QChar(0x00D7));
            if (xIdx > 0) {
                QString right = txt.mid(xIdx + 1);
                // Strip any trailing tag text (e.g. "HD", "4K")
                for (int i = 0; i < right.size(); ++i) {
                    if (right[i].isLetter() && !right[i].isDigit()) {
                        right = right.left(i);
                        break;
                    }
                }
                return right.toUInt();
            }
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

// =============================================================================
// UI Setup
// =============================================================================
// Table rebuild (filter + sort + populate)
// =============================================================================

void ProjectPanel::rebuildTable()
{
    // Save selection + scroll
    QString prevSelected = selectedProjectName();
    int prevScroll = m_projectTable->verticalScrollBar()
                         ? m_projectTable->verticalScrollBar()->value()
                         : 0;

    m_projectTable->setRowCount(0);

    // Filter
    QVector<ProjectInfo> filtered;
    for (const auto& p : m_allProjects) {
        if (m_searchFilter.isEmpty()
            || p.name.contains(m_searchFilter, Qt::CaseInsensitive))
            filtered.append(p);
    }

    // Sort
    switch (m_sortMode) {
    case 0: std::sort(filtered.begin(), filtered.end(),
                [](const ProjectInfo& a, const ProjectInfo& b)
                { return a.lastModified > b.lastModified; }); break;
    case 1: std::sort(filtered.begin(), filtered.end(),
                [](const ProjectInfo& a, const ProjectInfo& b)
                { return a.lastModified < b.lastModified; }); break;
    case 2: std::sort(filtered.begin(), filtered.end(),
                [](const ProjectInfo& a, const ProjectInfo& b)
                { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; }); break;
    case 3: std::sort(filtered.begin(), filtered.end(),
                [](const ProjectInfo& a, const ProjectInfo& b)
                { return a.name.compare(b.name, Qt::CaseInsensitive) > 0; }); break;
    }

    // Empty state
    bool empty = filtered.isEmpty() && m_searchFilter.isEmpty()
                 && m_allProjects.isEmpty();
    m_projectTable->setVisible(!empty);
    m_emptyStateWidget->setVisible(empty);

    if (filtered.isEmpty()) return;

    const auto& c = Theme::colors();
    const auto& t = Theme::typography();

    m_projectTable->setRowCount(filtered.size());

    for (int row = 0; row < filtered.size(); ++row) {
        const auto& info = filtered[row];

        m_projectTable->setRowHeight(row, 190);

        // Col 0: project thumbnail (with screenshot if available)
        m_projectTable->setCellWidget(row, 0,
            createProjectThumb(info, m_projectTable, this));

        // Col 1: name (+ badge for current)
        QString display = info.name;
        if (info.isCurrent)
            display += QStringLiteral("  \u25CF CURRENT");
        auto* nameItem = new QTableWidgetItem(QStringLiteral("   ") + display);
        nameItem->setData(Qt::UserRole, info.name);  // clean name
        QFont nameFont(t.fontFamily, 26,
                       info.isCurrent ? t.weightBold : t.weightSemiBold);
        nameItem->setFont(nameFont);
        if (info.isCurrent)
            nameItem->setForeground(c.accent);
        m_projectTable->setItem(row, 1, nameItem);

        // Col 2: resolution
        auto* resItem = new QTableWidgetItem(
            QStringLiteral("%1\u00D7%2").arg(info.resW).arg(info.resH));
        resItem->setTextAlignment(Qt::AlignCenter);
        QFont resFont(t.fontFamily, 22);
        resItem->setFont(resFont);
        resItem->setForeground(c.textSecondary);
        m_projectTable->setItem(row, 2, resItem);

        // Col 3: fps
        auto* fpsItem = new QTableWidgetItem(
            QString::number(info.fps, 'g', 4));
        fpsItem->setTextAlignment(Qt::AlignCenter);
        QFont fpsFont(t.fontFamily, 22);
        fpsItem->setFont(fpsFont);
        fpsItem->setForeground(c.textSecondary);
        m_projectTable->setItem(row, 3, fpsItem);

        // Col 4: file size
        auto* sizeItem = new QTableWidgetItem(formatFileSize(info.fileSize));
        sizeItem->setTextAlignment(Qt::AlignCenter);
        QFont sizeFont(t.fontFamily, 22);
        sizeItem->setFont(sizeFont);
        sizeItem->setForeground(c.textSecondary);
        m_projectTable->setItem(row, 4, sizeItem);

        // Col 5: last modified
        auto* dateItem = new QTableWidgetItem(formatDate(info.lastModified));
        dateItem->setTextAlignment(Qt::AlignCenter);
        QFont dateFont(t.fontFamily, 22);
        dateItem->setFont(dateFont);
        dateItem->setForeground(c.textSecondary);
        m_projectTable->setItem(row, 5, dateItem);

        // Accent tint for current project row
        if (info.isCurrent) {
            for (int col = 0; col < 6; ++col) {
                if (auto* it = m_projectTable->item(row, col))
                    it->setBackground(c.accentSubtle);
            }
        }

        // Restore selection
        if (!prevSelected.isEmpty() && info.name == prevSelected)
            m_projectTable->selectRow(row);
    }

    // Restore scroll
    if (auto* sb = m_projectTable->verticalScrollBar())
        sb->setValue(prevScroll);
}

// =============================================================================
// New UI helpers
// =============================================================================

void ProjectPanel::rebuildResGrid(uint32_t arW, uint32_t arH)
{
    auto* outerLay = m_resGridWidget->layout();
    if (!outerLay) return;

    // Remove all existing child widgets from the outer layout
    QLayoutItem* item;
    while ((item = outerLay->takeAt(0)) != nullptr) {
        if (auto* w = item->widget()) {
            // Remove all buttons in this widget from the group
            const auto btns = w->findChildren<QAbstractButton*>();
            for (auto* btn : btns)
                m_resGroup->removeButton(btn);
            w->deleteLater();
        }
        delete item;
    }

    // Create new container widget with its own 2-column grid
    auto* container = new QWidget(m_resGridWidget);
    auto* grid = new QGridLayout(container);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(m_newSizes.gridSpacing);

    // Determine resolution presets based on aspect ratio
    struct ResPreset { uint32_t w; uint32_t h; const char* tag; };
    QVector<ResPreset> presets;

    if (arW == 21 && arH == 9) {
        presets = {{3440,1440,"UW"},{3840,1600,"UW"},{5120,2160,"5K"}};
    } else if (arW >= arH) {
        uint32_t widths[] = {1280, 1920, 3840};
        const char* tags[] = {"HD", "FHD", "4K"};
        for (int i = 0; i < 3; ++i) {
            uint32_t bh = static_cast<uint32_t>(std::round(static_cast<double>(widths[i]) * arH / arW));
            presets.append({widths[i], bh, tags[i]});
        }
    } else {
        uint32_t heights[] = {720, 1080, 2160};
        const char* tags[] = {"HD", "FHD", "4K"};
        for (int i = 0; i < 3; ++i) {
            uint32_t bw = static_cast<uint32_t>(std::round(static_cast<double>(heights[i]) * arW / arH));
            presets.append({bw, heights[i], tags[i]});
        }
    }

    const auto& c = Theme::colors();
    auto chipStyle = [&]() {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: %9px; font-weight: 700; padding: %10px %11px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7; color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(m_newSizes.btnFontSize).arg(m_newSizes.btnPadV).arg(m_newSizes.btnPadH);
    };
    auto dashStyle = [&]() {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px dashed %2;"
            "  color: %3; font-size: %9px; font-weight: 700; padding: %10px %11px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7; color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(m_newSizes.btnFontSize).arg(m_newSizes.btnPadV).arg(m_newSizes.btnPadH);
    };

    for (int i = 0; i < presets.size(); ++i) {
        auto* btn = new QPushButton(
            QString("%1\u00D7%2  %3").arg(presets[i].w).arg(presets[i].h).arg(presets[i].tag));
        btn->setCheckable(true);
        btn->setChecked(i == 0);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(chipStyle());
        m_resGroup->addButton(btn, i);
        grid->addWidget(btn, i / 2, i % 2);
    }

    // "Custom" button
    auto* customBtn = new QPushButton("Custom");
    customBtn->setCheckable(true);
    customBtn->setCursor(Qt::PointingHandCursor);
    customBtn->setStyleSheet(dashStyle());
    int customId = presets.size();
    m_resGroup->addButton(customBtn, customId);
    grid->addWidget(customBtn, customId / 2, customId % 2);

    // Add container to the grid widget's existing layout
    outerLay->addWidget(container);

    // Resolution selection → show/hide custom res row
    connect(m_resGroup, &QButtonGroup::idClicked, this, [this](int id) {
        auto* btn = m_resGroup->button(id);
        m_customResRow->setVisible(btn && btn->text().trimmed() == "Custom");
        updateSummaryLabels();
    });

    updateSummaryLabels();
}

// =============================================================================
// Height-aware responsive layout for the NEW "Create Project" panel
// =============================================================================

void ProjectPanel::applyNewPanelResponsiveLayout()
{
    const int h = height();

    // Height tiers:  Full (≥800),  Medium (600–799),  Compact (<600)
    if (h >= 800) {
        m_newSizes = NewPanelSizes();
    } else if (h >= 600) {
        m_newSizes = NewPanelSizes{
            /*cardMarginTB*/ 10,  /*cardMarginLR*/ 12,  /*cardSpacing*/ 6,
            /*stepFontSize*/ 10,
            /*btnFontSize*/ 15,   /*btnPadV*/ 6,        /*btnPadH*/ 10,
            /*inputFontSize*/ 12, /*inputPadV*/ 6,      /*inputPadH*/ 8,
            /*siFontSize*/ 12,    /*siPadV*/ 5,         /*siPadH*/ 6,
            /*siMinW*/ 45,        /*siMaxW*/ 50,
            /*sumPadTB*/ 10,      /*sumPadLR*/ 10,      /*sumSpacing*/ 8,
            /*sumNameFontSize*/ 11, /*sumSpecFontSize*/ 11, /*sumIconFontSize*/ 10,
            /*createBtnFontSize*/ 12, /*createBtnPadV*/ 6, /*createBtnPadH*/ 14,
            /*createBtnMinH*/ 32,
            /*pageSpacing*/ 6,
            /*gridSpacing*/ 2,
            /*dividerSpacing*/ 8,
            /*headerFontSize*/ 14,
            /*closeBtnSize*/ 24,
            /*browseBtnSize*/ 26,
            /*recentLblFontSize*/ 8,
            /*recentSampleFontSize*/ 9, /*recentSamplePadV*/ 2, /*recentSamplePadH*/ 5,
            /*customRowFontSize*/ 9
        };
    } else {
        m_newSizes = NewPanelSizes{
            /*cardMarginTB*/ 6,   /*cardMarginLR*/ 8,   /*cardSpacing*/ 4,
            /*stepFontSize*/ 9,
            /*btnFontSize*/ 13,   /*btnPadV*/ 4,        /*btnPadH*/ 8,
            /*inputFontSize*/ 11, /*inputPadV*/ 4,      /*inputPadH*/ 6,
            /*siFontSize*/ 11,    /*siPadV*/ 3,         /*siPadH*/ 5,
            /*siMinW*/ 40,        /*siMaxW*/ 45,
            /*sumPadTB*/ 6,       /*sumPadLR*/ 6,       /*sumSpacing*/ 5,
            /*sumNameFontSize*/ 10, /*sumSpecFontSize*/ 10, /*sumIconFontSize*/ 9,
            /*createBtnFontSize*/ 11, /*createBtnPadV*/ 5, /*createBtnPadH*/ 12,
            /*createBtnMinH*/ 28,
            /*pageSpacing*/ 4,
            /*gridSpacing*/ 2,
            /*dividerSpacing*/ 5,
            /*headerFontSize*/ 13,
            /*closeBtnSize*/ 20,
            /*browseBtnSize*/ 22,
            /*recentLblFontSize*/ 8,
            /*recentSampleFontSize*/ 8, /*recentSamplePadV*/ 1, /*recentSamplePadH*/ 4,
            /*customRowFontSize*/ 8
        };
    }

    const auto& c = Theme::colors();

    // Helper: chip button stylesheet
    auto chipSS = [&]() -> QString {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: %9px; font-weight: 700;"
            "  padding: %10px %11px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7;"
            "  color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(m_newSizes.btnFontSize).arg(m_newSizes.btnPadV).arg(m_newSizes.btnPadH);
    };
    auto dashSS = [&]() -> QString {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px dashed %2;"
            "  color: %3; font-size: %9px; font-weight: 700;"
            "  padding: %10px %11px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7;"
            "  color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(m_newSizes.btnFontSize).arg(m_newSizes.btnPadV).arg(m_newSizes.btnPadH);
    };
    auto stepSS = [&]() -> QString {
        return QStringLiteral(
            "font-size: %1px; font-weight: 700; color: %2;"
            " letter-spacing: 0.6px; text-transform: uppercase;")
            .arg(m_newSizes.stepFontSize).arg(Theme::rgb(c.textPrimary));
    };
    auto inputSS = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2;"
            " color: %3; font-size: %4px; padding: %5px %6px;")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(m_newSizes.inputFontSize).arg(m_newSizes.inputPadV).arg(m_newSizes.inputPadH);
    };
    auto smallInputSS = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2;"
            " color: %3; font-size: %4px; padding: %5px %6px;"
            " min-width: %7px; max-width: %8px; text-align: center;")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(m_newSizes.siFontSize).arg(m_newSizes.siPadV).arg(m_newSizes.siPadH)
            .arg(m_newSizes.siMinW).arg(m_newSizes.siMaxW);
    };

    // ── 1. Page layout spacing ──
    if (auto* pageLayout = qobject_cast<QVBoxLayout*>(m_newPage->layout())) {
        pageLayout->setSpacing(m_newSizes.pageSpacing);
    }

    // ── 2. Step cards: margins, spacing, step labels ──
    for (int i = 1; i <= 5; ++i) {
        auto* card = m_newPage->findChild<QWidget*>(QString("NewCard%1").arg(i));
        if (!card || !card->layout()) continue;
        card->layout()->setContentsMargins(
            m_newSizes.cardMarginLR, m_newSizes.cardMarginTB,
            m_newSizes.cardMarginLR, m_newSizes.cardMarginTB);
        card->layout()->setSpacing(m_newSizes.cardSpacing);

        if (auto* stepLbl = card->findChild<QLabel*>(QString("NewStepLbl%1").arg(i)))
            stepLbl->setStyleSheet(stepSS());
    }

    // ── 3. Header ──
    if (auto* title = m_newPage->findChild<QLabel*>("NewHeaderTitle")) {
        title->setStyleSheet(QStringLiteral(
            "font-size: %1px; font-weight: %2; color: %3;")
            .arg(m_newSizes.headerFontSize)
            .arg(Theme::typography().weightBold)
            .arg(Theme::rgb(c.textPrimary)));
    }
    if (auto* closeBtn = m_newPage->findChild<QPushButton*>("NewCloseBtn")) {
        closeBtn->setFixedSize(m_newSizes.closeBtnSize, m_newSizes.closeBtnSize);
    }

    // ── 4. AR grid spacing ──
    if (auto* arGrid = m_newPage->findChild<QWidget*>("NewArGrid")) {
        if (auto* arLay = qobject_cast<QGridLayout*>(arGrid->layout()))
            arLay->setSpacing(m_newSizes.gridSpacing);
    }

    // ── 5. FPS grid spacing ──
    if (auto* fpsGrid = m_newPage->findChild<QWidget*>("NewFpsGrid")) {
        if (auto* fpsLay = qobject_cast<QGridLayout*>(fpsGrid->layout()))
            fpsLay->setSpacing(m_newSizes.gridSpacing);
    }

    // ── 6. AR buttons ──
    QString chip = chipSS();
    QString dash = dashSS();
    for (auto* btn : {m_ar16_9, m_ar9_16, m_ar21_9}) {
        if (btn) btn->setStyleSheet(chip);
    }
    if (m_arCustom) m_arCustom->setStyleSheet(dash);

    // ── 7. FPS buttons ──
    for (auto* btn : {m_fps24, m_fps30, m_fps60}) {
        if (btn) btn->setStyleSheet(chip);
    }
    if (m_fpsCustom) m_fpsCustom->setStyleSheet(dash);

    // ── 8. Resolution grid buttons — rebuilt by rebuildResGrid which reads m_newSizes ──
    // Trigger a rebuild with current AR
    {
        int arId = m_arGroup ? m_arGroup->checkedId() : 0;
        if (arId == 3) {
            rebuildResGrid(static_cast<uint32_t>(m_customArW ? m_customArW->value() : 16),
                           static_cast<uint32_t>(m_customArH ? m_customArH->value() : 9));
        } else if (arId == 1) {
            rebuildResGrid(9, 16);
        } else if (arId == 2) {
            rebuildResGrid(21, 9);
        } else {
            rebuildResGrid(16, 9);
        }
    }

    // ── 9. Inputs (QLineEdit) ──
    QString inSS = inputSS();
    if (m_nameInput) m_nameInput->setStyleSheet(inSS);
    if (m_locationInput) m_locationInput->setStyleSheet(inSS);

    // ── 10. Small inputs (QSpinBox, QDoubleSpinBox) ──
    QString siSS = smallInputSS();
    for (auto* sb : {m_customArW, m_customArH, m_customResW, m_customResH}) {
        if (sb) sb->setStyleSheet(siSS);
    }
    if (m_customFps) m_customFps->setStyleSheet(siSS);

    // ── 11. Custom row labels (W, H, FPS, :) ──
    QString rowLabelSS = QStringLiteral(
        "font-size: %1px; color: %2; font-weight: 600;")
        .arg(m_newSizes.customRowFontSize).arg(Theme::rgb(c.textPrimary));
    for (auto* row : {m_customArRow, m_customResRow, m_customFpsRow}) {
        if (!row) continue;
        const auto labels = row->findChildren<QLabel*>();
        for (auto* lbl : labels)
            lbl->setStyleSheet(rowLabelSS);
    }

    // ── 12. Browse button ──
    if (m_locationBrowseBtn)
        m_locationBrowseBtn->setFixedSize(m_newSizes.browseBtnSize, m_newSizes.browseBtnSize);

    // ── 13. Recent paths ──
    if (auto* recentLbl = m_newPage->findChild<QLabel*>("NewRecentLbl")) {
        recentLbl->setStyleSheet(QStringLiteral(
            "font-size: %1px; font-weight: 600; color: %2; letter-spacing: 0.4px;")
            .arg(m_newSizes.recentLblFontSize).arg(Theme::rgb(c.textPrimary)));
    }
    if (auto* recentSample = m_newPage->findChild<QPushButton*>("NewRecentSample")) {
        recentSample->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: %4px; padding: %5px %6px; }"
            "QPushButton:hover { background: %7; color: %8; }")
            .arg(Theme::rgb(c.surface1)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textTertiary))
            .arg(m_newSizes.recentSampleFontSize)
            .arg(m_newSizes.recentSamplePadV).arg(m_newSizes.recentSamplePadH)
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.textPrimary)));
    }

    // ── 14. Summary bar ──
    if (m_summaryBar) {
        if (auto* sbLay = m_summaryBar->layout()) {
            sbLay->setContentsMargins(
                m_newSizes.sumPadLR, m_newSizes.sumPadTB,
                m_newSizes.sumPadLR, m_newSizes.sumPadTB);
            sbLay->setSpacing(m_newSizes.sumSpacing);
        }
        if (auto* nameLbl = m_newPage->findChild<QLabel*>("NewSummaryName")) {
            nameLbl->setStyleSheet(QStringLiteral(
                "font-size: %1px; font-weight: 700; color: %2;")
                .arg(m_newSizes.sumNameFontSize).arg(Theme::rgb(c.textPrimary)));
        }
    }
    if (m_summaryResLabel) {
        m_summaryResLabel->setStyleSheet(QStringLiteral(
            "font-size: %1px; font-weight: 700; color: %2;")
            .arg(m_newSizes.sumSpecFontSize).arg(Theme::rgb(c.textPrimary)));
    }
    if (m_summaryFpsLabel) {
        m_summaryFpsLabel->setStyleSheet(QStringLiteral(
            "font-size: %1px; font-weight: 700; color: %2;")
            .arg(m_newSizes.sumSpecFontSize).arg(Theme::rgb(c.textPrimary)));
    }

    // ── 15. Summary icon labels ──
    if (m_summaryBar) {
        const auto icons = m_summaryBar->findChildren<QLabel*>();
        for (auto* iconLbl : icons) {
            const auto t = iconLbl->text();
            if (t == QStringLiteral("\U0001F4D0") ||
                t == QStringLiteral("\U0001F39E\uFE0F")) {
                iconLbl->setStyleSheet(QStringLiteral(
                    "font-size: %1px; color: %2;")
                    .arg(m_newSizes.sumIconFontSize).arg(Theme::rgb(c.textTertiary)));
            }
        }
    }

    // ── 16. Create button ──
    if (m_createBtn) {
        m_createBtn->setMinimumHeight(m_newSizes.createBtnMinH);
        m_createBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; color: white; border: none;"
            "  font-size: %2px; font-weight: 700; padding: %3px %4px; }"
            "QPushButton:pressed { background: %6; }")
            .arg(Theme::rgb(c.primaryBtnBg))
            .arg(m_newSizes.createBtnFontSize)
            .arg(m_newSizes.createBtnPadV).arg(m_newSizes.createBtnPadH)
            .arg(Theme::rgb(c.accent)));
    }

    // ── Summary bar border ──
    if (m_summaryBar) {
        m_summaryBar->setStyleSheet(QStringLiteral(
            "background: %1; border: 1px solid %2; padding: %3px %4px;")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(m_newSizes.sumPadTB).arg(m_newSizes.sumPadLR));
    }
}

void ProjectPanel::updateSummaryLabels()
{
    // Project name
    if (m_summaryNameLabel) {
        QString name = m_nameInput ? m_nameInput->text().trimmed() : QString();
        QString display = name.isEmpty() ? QStringLiteral("New Project") : name;
        m_summaryNameLabel->setText(QStringLiteral("\U0001F4C4  ") + display);
    }

    // Resolution
    uint32_t rw = customResWidth();
    uint32_t rh = customResHeight();
    m_summaryResLabel->setText(QString("%1\u00D7%2").arg(rw).arg(rh));

    // FPS
    double fps = customFps();
    m_summaryFpsLabel->setText(QString("%1 fps").arg(fps, 0, 'f', (fps == std::floor(fps)) ? 0 : 2));
}

// =============================================================================
// Side Panel management
// =============================================================================

void ProjectPanel::toggleSidePanel(SidePanelMode mode)
{
    if (m_sidePanelMode == mode) {
        hideSidePanel();
    } else {
        showSidePanel(mode);
    }
}

void ProjectPanel::showSidePanel(SidePanelMode mode)
{
    static constexpr int PANEL_WIDTH = 400;

    m_sidePanelMode = mode;

    // Update rail button checked states
    m_newBtn->setChecked(mode == SidePanelMode::New);
    m_openFileBtn->setChecked(mode == SidePanelMode::Open);
    m_settingsBtn->setChecked(mode == SidePanelMode::Settings);

    // Set the correct page
    if (mode == SidePanelMode::New) {
        m_sidePanelStack->setCurrentWidget(m_newPageScroll ? m_newPageScroll : m_newPage);
        m_nameInput->setFocus();
        m_nameInput->selectAll();
    } else if (mode == SidePanelMode::Open) {
        m_sidePanelStack->setCurrentWidget(m_openPage);
        populateOpenList();
        m_openList->setFocus();
    } else if (mode == SidePanelMode::Settings) {
        m_projDirInput->setText(m_projectsDir);
        m_sidePanelStack->setCurrentWidget(m_settingsPage);
    }

    // Already open — just switch page, no animation needed
    if (m_sidePanel->isVisible() && m_sidePanel->width() > 10)
        return;

    // Reset constraints before animating
    m_sidePanel->setMinimumWidth(0);
    m_sidePanel->setMaximumWidth(0);
    m_sidePanel->setVisible(true);

    // Use parallel animation group for perfectly synchronized min/max
    auto* group = new QParallelAnimationGroup(this);

    auto* animMin = new QPropertyAnimation(m_sidePanel, "minimumWidth");
    animMin->setDuration(150);
    animMin->setStartValue(0);
    animMin->setEndValue(PANEL_WIDTH);
    animMin->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(animMin);

    auto* animMax = new QPropertyAnimation(m_sidePanel, "maximumWidth");
    animMax->setDuration(150);
    animMax->setStartValue(0);
    animMax->setEndValue(PANEL_WIDTH);
    animMax->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(animMax);

    // After animation finishes, lock to target width — prevents overshoot glitch
    connect(group, &QParallelAnimationGroup::finished, this, [this]() {
        m_sidePanel->setMinimumWidth(PANEL_WIDTH);
        m_sidePanel->setMaximumWidth(PANEL_WIDTH);
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void ProjectPanel::hideSidePanel()
{
    if (m_sidePanelMode == SidePanelMode::None) return;
    m_sidePanelMode = SidePanelMode::None;

    // Un-check rail buttons
    m_newBtn->setChecked(false);
    m_openFileBtn->setChecked(false);
    m_settingsBtn->setChecked(false);

    // Use parallel animation group for synchronized collapse
    auto* group = new QParallelAnimationGroup(this);

    auto* animMin = new QPropertyAnimation(m_sidePanel, "minimumWidth");
    animMin->setDuration(120);
    animMin->setStartValue(m_sidePanel->minimumWidth());
    animMin->setEndValue(0);
    animMin->setEasingCurve(QEasingCurve::InCubic);
    group->addAnimation(animMin);

    auto* animMax = new QPropertyAnimation(m_sidePanel, "maximumWidth");
    animMax->setDuration(120);
    animMax->setStartValue(m_sidePanel->maximumWidth());
    animMax->setEndValue(0);
    animMax->setEasingCurve(QEasingCurve::InCubic);
    group->addAnimation(animMax);

    // Hide widget after collapse animation
    connect(group, &QParallelAnimationGroup::finished, this, [this]() {
        m_sidePanel->setVisible(false);
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

// =============================================================================
// Slots
// =============================================================================

void ProjectPanel::onCreateClicked()
{
    QString name = m_nameInput->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, "Cannot Create Project",
            "Please enter a project name.");
        m_nameInput->setFocus();
        return;
    }

    // Validate project name — reject characters invalid in Windows filenames
    static const QString invalidChars = R"(/\:*?"<>|)";
    for (const QChar& ch : name) {
        if (invalidChars.contains(ch)) {
            QMessageBox::warning(this, "Invalid Project Name",
                QString("Project name cannot contain: %1\n\n"
                        "Please remove the invalid character '%2'.")
                    .arg(invalidChars, ch));
            return;
        }
    }

    // Validate save location if provided
    QString saveDir = m_locationInput ? m_locationInput->text().trimmed() : QString();
    if (!saveDir.isEmpty()) {
        QDir dir(saveDir);
        if (!dir.exists()) {
            auto reply = QMessageBox::question(this, "Create Directory",
                QString("The save location does not exist:\n%1\n\n"
                        "Create this folder?").arg(saveDir),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (reply == QMessageBox::No)
                return;
            if (!dir.mkpath(".")) {
                QMessageBox::warning(this, "Cannot Create Directory",
                    "Failed to create the save location folder.\n"
                    "Please choose a different location.");
                return;
            }
        }
    }

    uint32_t resW = customResWidth();
    uint32_t resH = customResHeight();
    double fps = customFps();

    emit createProject(name, resW, resH, fps, saveDir);
    m_nameInput->clear();
    hideSidePanel();
}

void ProjectPanel::onSearchTextChanged(const QString& text)
{
    m_searchFilter = text.trimmed();
    rebuildTable();
}

void ProjectPanel::onSortChanged(int index)
{
    m_sortMode = index;
    rebuildTable();
}

void ProjectPanel::onFileTreeDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;
    QString path = m_fileModel->filePath(index);
    QFileInfo fi(path);
    if (fi.isFile()) {
        hideSidePanel();
        emit openFilePath(path);
    }
    // If it's a directory, the tree view handles expanding it
}

void ProjectPanel::onOpenListItemDoubleClicked(QListWidgetItem* item)
{
    if (!item) return;
    QString path = item->data(Qt::UserRole).toString();
    if (!path.isEmpty() && QFileInfo(path).isFile()) {
        hideSidePanel();
        emit openFilePath(path);
    }
}

void ProjectPanel::populateOpenList()
{
    if (!m_openList) return;
    m_openList->clear();

    const auto& c = Theme::colors();
    constexpr int kThumbW = 160;
    constexpr int kThumbH = 100;

    // Scan the projects directory for matching files
    // Support both subfolder layout (projects/Foo/Foo.rtp) and flat layout (projects/Foo.rtp)
    QDir dir(m_projectsDir);
    QStringList filters;
    filters << "*.rtp";

    QFileInfoList files;
    for (const auto& subDir : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir sub(subDir.absoluteFilePath());
        files.append(sub.entryInfoList(filters, QDir::Files, QDir::Time));
    }
    std::sort(files.begin(), files.end(),
              [](const QFileInfo& a, const QFileInfo& b) {
                  return a.lastModified() > b.lastModified();
              });

    for (const QFileInfo& fi : files) {
        // Skip autosave files
        if (fi.fileName().endsWith(".autosave", Qt::CaseInsensitive))
            continue;

        // Create item widget with thumbnail + info
        auto* itemWidget = new QWidget;
        auto* hlay = new QHBoxLayout(itemWidget);
        hlay->setContentsMargins(4, 4, 4, 4);
        hlay->setSpacing(12);

        // Thumbnail
        auto* thumbLabel = new QLabel;
        thumbLabel->setFixedSize(kThumbW, kThumbH);
        thumbLabel->setAlignment(Qt::AlignCenter);

        // Try to load thumbnail (next to .rtp in project subfolder)
        QString thumbPng = fi.absolutePath() + "/" + fi.baseName() + ".png";
        QString thumbJpg = fi.absolutePath() + "/" + fi.baseName() + ".jpg";
        QPixmap pix(thumbPng);
        if (pix.isNull()) pix.load(thumbJpg);

        if (!pix.isNull()) {
            thumbLabel->setPixmap(pix.scaled(kThumbW, kThumbH,
                                              Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation));
            thumbLabel->setStyleSheet(QStringLiteral(
                "background: %1; border-radius: 4px; border: 1px solid %2;")
                .arg(Theme::rgb(c.surface2))
                .arg(Theme::rgb(c.border)));
        } else {
            // Placeholder gradient
            thumbLabel->setText("\U0001F3AC");
            thumbLabel->setStyleSheet(QStringLiteral(
                "background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
                "stop:0 %1, stop:1 %2);"
                "border-radius: 4px; border: 1px solid %3;"
                "font-size: 32px; color: %4;")
                .arg(Theme::rgb(c.surface3))
                .arg(Theme::rgb(c.surface1))
                .arg(Theme::rgb(c.border))
                .arg(Theme::rgb(c.textTertiary)));
        }
        hlay->addWidget(thumbLabel);

        // Info column (name + date + size)
        auto* infoLayout = new QVBoxLayout;
        infoLayout->setSpacing(4);
        infoLayout->setContentsMargins(0, 4, 0, 4);

        auto* nameLabel = new QLabel(fi.baseName());
        nameLabel->setStyleSheet(QStringLiteral(
            "font-size: 15px; font-weight: 600; color: %1; background: transparent;")
            .arg(Theme::rgb(c.textPrimary)));
        nameLabel->setWordWrap(true);
        infoLayout->addWidget(nameLabel);

        auto* dateLabel = new QLabel(formatDate(fi.lastModified()));
        dateLabel->setStyleSheet(QStringLiteral(
            "font-size: 12px; color: %1; background: transparent;")
            .arg(Theme::rgb(c.textSecondary)));
        infoLayout->addWidget(dateLabel);

        auto* sizeLabel = new QLabel(formatFileSize(fi.size()));
        sizeLabel->setStyleSheet(QStringLiteral(
            "font-size: 12px; color: %1; background: transparent;")
            .arg(Theme::rgb(c.textTertiary)));
        infoLayout->addWidget(sizeLabel);

        infoLayout->addStretch();
        hlay->addLayout(infoLayout, 1);

        // Create list item
        auto* listItem = new QListWidgetItem;
        listItem->setData(Qt::UserRole, fi.absoluteFilePath());
        listItem->setSizeHint(QSize(0, kThumbH + 12));
        m_openList->addItem(listItem);
        m_openList->setItemWidget(listItem, itemWidget);
    }
}

QString ProjectPanel::selectedProjectName() const
{
    auto items = m_projectTable->selectedItems();
    if (items.isEmpty()) return {};
    int row = items.first()->row();
    auto* nameItem = m_projectTable->item(row, 1);
    return nameItem ? nameItem->data(Qt::UserRole).toString() : QString();
}

void ProjectPanel::showContextMenu(const QPoint& pos)
{
    auto* item = m_projectTable->itemAt(pos);
    if (!item) return;
    int row = item->row();
    auto* nameItem = m_projectTable->item(row, 1);
    if (!nameItem) return;

    QString name = nameItem->data(Qt::UserRole).toString();

    QMenu menu(this);
    menu.addAction("Open", this,
                   [this, name]() { emit openProject(name); });
    menu.addSeparator();
    menu.addAction("Rename...", this, [this, name]() {
        bool ok = false;
        QString newName = QInputDialog::getText(
            this, "Rename Project",
            "New name:", QLineEdit::Normal, name, &ok);
        newName = newName.trimmed();
        if (ok && !newName.isEmpty() && newName != name)
            emit renameProject(name, newName);
    });
    menu.addAction("Duplicate", this,
                   [this, name]() { emit duplicateProject(name); });
    menu.addSeparator();
    menu.addAction("Show in Explorer", this,
                   [this, name]() { emit revealInExplorer(name); });
    menu.addAction("Set Thumbnail...", this,
                   [this, name]() { setThumbnailForProject(name); });
    menu.addAction("Remove Thumbnail", this,
                   [this, name]() { removeThumbnailForProject(name); });
    menu.addAction("Export...", this, [this, name]() {
        QString dst = QFileDialog::getSaveFileName(
            this, "Export Project", name + ".rtp",
            "ROUNDTABLE Projects (*.rtp)");
        if (!dst.isEmpty())
            emit exportProject(name, dst);
    });
    menu.addSeparator();
    menu.addAction("Delete", this,
                   [this, name]() { emit deleteProject(name); });

    menu.exec(m_projectTable->viewport()->mapToGlobal(pos));
}

void ProjectPanel::showOpenListContextMenu(const QPoint& pos)
{
    auto* item = m_openList->itemAt(pos);
    if (!item) return;
    QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty()) return;
    QFileInfo fi(path);

    QMenu menu(this);
    menu.addAction("Open", this, [this, path]() {
        hideSidePanel();
        emit openFilePath(path);
    });
    menu.addSeparator();
    menu.addAction("Rename...", this, [this, path, fi]() {
        bool ok = false;
        QString newName = QInputDialog::getText(
            this, "Rename Project File",
            "New name:", QLineEdit::Normal, fi.baseName(), &ok);
        newName = newName.trimmed();
        if (!ok || newName.isEmpty() || newName == fi.baseName()) return;
        QString newPath = fi.absolutePath() + "/" + newName + "." + fi.suffix();
        if (QFile::exists(newPath)) {
            QMessageBox::warning(this, "Rename", "A file with that name already exists.");
            return;
        }
        QFile::rename(path, newPath);
        // Also rename thumbnail if it exists
        QString thumbDir = fi.absolutePath() + "/thumbs/";
        for (const auto& ext : {".png", ".jpg"}) {
            QString oldThumb = thumbDir + fi.baseName() + ext;
            if (QFile::exists(oldThumb))
                QFile::rename(oldThumb, thumbDir + newName + ext);
        }
        populateOpenList();
    });
    menu.addAction("Duplicate", this, [this, path, fi]() {
        QString baseName = fi.baseName() + " Copy";
        QString newPath = fi.absolutePath() + "/" + baseName + "." + fi.suffix();
        int counter = 2;
        while (QFile::exists(newPath)) {
            newPath = fi.absolutePath() + "/" + baseName + " " +
                      QString::number(counter++) + "." + fi.suffix();
        }
        QFile::copy(path, newPath);
        populateOpenList();
    });
    menu.addSeparator();
    menu.addAction("Delete", this, [this, path, fi]() {
        auto reply = QMessageBox::question(this, "Delete Project",
            QString("Delete \"%1\"?\n\nThis cannot be undone.").arg(fi.fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            QFile::remove(path);
            populateOpenList();
        }
    });

    menu.exec(m_openList->viewport()->mapToGlobal(pos));
}

// =============================================================================
// Keyboard shortcuts
// =============================================================================

void ProjectPanel::keyPressEvent(QKeyEvent* event)
{
    // Enter -> open selected
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_projectTable->hasFocus()) {
            QString name = selectedProjectName();
            if (!name.isEmpty()) {
                emit openProject(name);
                event->accept();
                return;
            }
        }
    }
    // Delete / Backspace -> delete selected
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_projectTable->hasFocus()) {
            QString name = selectedProjectName();
            if (!name.isEmpty()) {
                emit deleteProject(name);
                event->accept();
                return;
            }
        }
    }
    // F5 -> refresh
    if (event->key() == Qt::Key_F5) {
        emit m_refreshBtn->clicked();
        event->accept();
        return;
    }
    // F2 -> rename selected
    if (event->key() == Qt::Key_F2) {
        QString name = selectedProjectName();
        if (!name.isEmpty()) {
            bool ok = false;
            QString newName = QInputDialog::getText(
                this, "Rename Project",
                "New name:", QLineEdit::Normal, name, &ok);
            newName = newName.trimmed();
            if (ok && !newName.isEmpty() && newName != name)
                emit renameProject(name, newName);
        }
        event->accept();
        return;
    }
    // Ctrl+N -> toggle NEW side panel
    if (event->key() == Qt::Key_N
        && (event->modifiers() & Qt::ControlModifier)) {
        toggleSidePanel(SidePanelMode::New);
        event->accept();
        return;
    }
    // Ctrl+O -> toggle OPEN side panel
    if (event->key() == Qt::Key_O
        && (event->modifiers() & Qt::ControlModifier)) {
        toggleSidePanel(SidePanelMode::Open);
        event->accept();
        return;
    }
    // Escape -> close side panel
    if (event->key() == Qt::Key_Escape) {
        if (m_sidePanelMode != SidePanelMode::None) {
            hideSidePanel();
            event->accept();
            return;
        }
    }

    QWidget::keyPressEvent(event);
}

// =============================================================================
// Mouse press -> close side panel when clicking on the content area
// =============================================================================

void ProjectPanel::mousePressEvent(QMouseEvent* event)
{
    if (m_sidePanelMode != SidePanelMode::None) {
        // Check if the click is outside the side panel and icon rail
        QPoint pos = event->pos();
        QRect sidePanelRect = m_sidePanel->geometry();
        QRect iconRailRect = m_iconRail->geometry();

        if (!sidePanelRect.contains(pos) && !iconRailRect.contains(pos)) {
            hideSidePanel();
        }
    }

    QWidget::mousePressEvent(event);
}

// =============================================================================
// Event filter -> click on empty table area deselects
// =============================================================================

bool ProjectPanel::eventFilter(QObject* obj, QEvent* event)
{
    // Click on empty table area -> deselect
    if (obj == m_projectTable->viewport()
        && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        QTableWidgetItem* item = m_projectTable->itemAt(me->pos());
        if (!item) {
            m_projectTable->clearSelection();
            m_projectTable->setCurrentItem(nullptr);
            updateActionButtons();
        }
        // Also close side panel if open
        if (m_sidePanelMode != SidePanelMode::None)
            hideSidePanel();
    }

    // Click anywhere in content area -> close side panel
    if (obj == m_contentArea
        && event->type() == QEvent::MouseButtonPress) {
        if (m_sidePanelMode != SidePanelMode::None)
            hideSidePanel();
    }

    return QWidget::eventFilter(obj, event);
}

// =============================================================================
// Enable / disable bottom action buttons based on selection
// =============================================================================

void ProjectPanel::updateActionButtons()
{
    bool hasSelection = !selectedProjectName().isEmpty();
    m_openActionBtn->setEnabled(hasSelection);
    m_dupeActionBtn->setEnabled(hasSelection);
    m_renameActionBtn->setEnabled(hasSelection);
    m_deleteActionBtn->setEnabled(hasSelection);

    const qreal opacity = hasSelection ? 1.0 : 0.4;
    for (auto* btn : {m_openActionBtn, m_dupeActionBtn,
                      m_renameActionBtn, m_deleteActionBtn}) {
        auto* effect = new QGraphicsOpacityEffect(btn);
        effect->setOpacity(opacity);
        btn->setGraphicsEffect(effect);
    }
}

// =============================================================================
// Responsive layout adjustments based on width
// =============================================================================

void ProjectPanel::applyResponsiveLayout()
{
    const int w = width();

    // --- breakpoints ---
    // Compact: < 1400    Medium: 1400-2200    Full: >= 2200
    int thumbW, thumbH, rowH, railW, railBtnW, railBtnH;
    int namePt, dataPt, searchH, refreshSz, sortW;

    if (w < 1400) {
        // compact
        thumbW = 160; thumbH = 100; rowH = 120;
        railW = 100; railBtnW = 84; railBtnH = 56;
        namePt = 16; dataPt = 14;
        searchH = 36; refreshSz = 36; sortW = 180;
    } else if (w < 2200) {
        // medium
        thumbW = 220; thumbH = 140; rowH = 155;
        railW = 128; railBtnW = 106; railBtnH = 70;
        namePt = 21; dataPt = 18;
        searchH = 46; refreshSz = 46; sortW = 230;
    } else {
        // full
        thumbW = 280; thumbH = 180; rowH = 190;
        railW = 150; railBtnW = 128; railBtnH = 84;
        namePt = 26; dataPt = 22;
        searchH = 56; refreshSz = 56; sortW = 280;
    }

    // Icon rail width stays at 150 to match main nav rail for alignment
    // (responsive sizing only affects content area, not the rail)

    // Search bar & sort
    if (m_searchInput) m_searchInput->setFixedHeight(searchH);
    if (m_sortCombo) {
        m_sortCombo->setFixedHeight(searchH);
        m_sortCombo->setFixedWidth(sortW);
    }
    if (m_refreshBtn) {
        m_refreshBtn->setFixedSize(refreshSz, refreshSz);
        m_refreshBtn->setIconSize(QSize(refreshSz / 2, refreshSz / 2));
    }

    // Table row sizes & stylesheet (don't override column widths — user may have resized)
    if (m_projectTable) {
        const auto& c = Theme::colors();
        const auto& m = Theme::metrics();

        m_projectTable->verticalHeader()->setDefaultSectionSize(rowH);

        // Update stylesheet font sizes for table
        QString tableStyle = QString(
            "QTableWidget { background: %1; border: none; gridline-color: %2; "
            "font-size: %3pt; }"
            "QTableWidget::item { padding: %4px; }"
            "QTableWidget::item:selected { background: %6; }"
            "QHeaderView::section { background: %7; color: %8; font-size: %9pt; "
            "font-weight: 600; padding: %10px; border: none; "
            "border-bottom: 2px solid %11; border-right: 1px solid %5; }")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(dataPt)
            .arg(m.spacingMd)
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.accentSubtle))
            .arg(Theme::rgb(c.surface1))
            .arg(Theme::rgb(c.textSecondary))
            .arg(dataPt - 2)
            .arg(m.spacingMd)
            .arg(Theme::rgb(c.accent));
        m_projectTable->setStyleSheet(tableStyle);
    }
}

// =============================================================================
// Resize event -> apply responsive layout
// =============================================================================

void ProjectPanel::resizeEvent(QResizeEvent* event)
{
    applyResponsiveLayout();
    applyNewPanelResponsiveLayout();
    QWidget::resizeEvent(event);
}

// =============================================================================
// Thumbnail management
// =============================================================================

QString ProjectPanel::thumbnailPathForProject(const QString& projectName) const
{
    if (m_projectsDir.isEmpty()) return {};
    return m_projectsDir + "/thumbs/" + projectName + ".png";
}

void ProjectPanel::removeThumbnailForProject(const QString& projectName)
{
    if (m_projectsDir.isEmpty()) return;

    QString projFolder = m_projectsDir + "/" + projectName;
    bool removed = false;
    removed |= QFile::remove(projFolder + "/" + projectName + ".png");
    removed |= QFile::remove(projFolder + "/" + projectName + ".jpg");

    if (removed) {
        spdlog::info("ProjectPanel: Removed thumbnail for '{}'",
                     projectName.toStdString());
        rebuildTable();
    }
}

void ProjectPanel::setThumbnailForProject(const QString& projectName)
{
    QString path = QFileDialog::getOpenFileName(
        this, "Select Thumbnail Image", QString(),
        "Images (*.png *.jpg *.jpeg *.bmp *.webp)");

    if (path.isEmpty()) return;

    QImage img(path);
    if (img.isNull()) return;

    // Ensure project subfolder exists
    QString projFolder = m_projectsDir + "/" + projectName;
    QDir().mkpath(projFolder);

    // Scale to a reasonable thumbnail size (max 480px wide, maintain aspect)
    if (img.width() > 480)
        img = img.scaledToWidth(480, Qt::SmoothTransformation);

    QString dstPath = projFolder + "/" + projectName + ".png";

    // Remove old jpg if it exists (we always save as png)
    QFile::remove(projFolder + "/" + projectName + ".jpg");

    if (img.save(dstPath, "PNG")) {
        spdlog::info("ProjectPanel: Set thumbnail for '{}' from '{}'",
                     projectName.toStdString(), path.toStdString());
        rebuildTable();
    } else {
        spdlog::warn("ProjectPanel: Failed to save thumbnail to '{}'",
                     dstPath.toStdString());
    }
}

void ProjectPanel::setThumbnailFromPixels(const QString& projectName,
                                           const uint8_t* bgra,
                                           uint32_t width, uint32_t height)
{
    if (!bgra || width == 0 || height == 0) return;
    if (m_projectsDir.isEmpty()) return;

    // Ensure project subfolder exists
    QString projFolder = m_projectsDir + "/" + projectName;
    QDir().mkpath(projFolder);

    // Create QImage from BGRA data
    QImage img(bgra, static_cast<int>(width), static_cast<int>(height),
               static_cast<int>(width * 4), QImage::Format_ARGB32);

    // Scale to a reasonable thumbnail size
    if (img.width() > 480)
        img = img.scaledToWidth(480, Qt::SmoothTransformation);

    QString dstPath = projFolder + "/" + projectName + ".png";
    if (img.save(dstPath, "PNG")) {
        spdlog::info("ProjectPanel: Auto-saved thumbnail for '{}'  ({}x{})",
                     projectName.toStdString(), width, height);
        rebuildTable();
    } else {
        spdlog::warn("ProjectPanel: Failed to auto-save thumbnail to '{}'",
                     dstPath.toStdString());
    }
}

void ProjectPanel::setThumbnailFromImage(const QString& projectName, const QImage& image)
{
    if (image.isNull()) return;
    if (m_projectsDir.isEmpty()) return;

    QString projFolder = m_projectsDir + "/" + projectName;
    QDir().mkpath(projFolder);

    QImage img = image;
    if (img.width() > 480)
        img = img.scaledToWidth(480, Qt::SmoothTransformation);

    QString dstPath = projFolder + "/" + projectName + ".png";
    QFile::remove(projFolder + "/" + projectName + ".jpg");
    if (img.save(dstPath, "PNG")) {
        spdlog::info("ProjectPanel: Saved thumbnail for '{}' from viewport grab ({}x{})",
                     projectName.toStdString(), img.width(), img.height());
        rebuildTable();
    } else {
        spdlog::warn("ProjectPanel: Failed to save thumbnail to '{}'",
                     dstPath.toStdString());
    }
}

} // namespace rt
