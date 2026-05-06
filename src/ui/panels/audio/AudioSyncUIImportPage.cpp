/*
 * AudioSyncUIImportPage.cpp - Import side-panel page for AudioSync.
 * Split from AudioSyncUI.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "Theme.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <spdlog/spdlog.h>
#include <algorithm>
namespace rt {
void AudioSync::setupImportPage()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();
    const auto& t = Theme::typography();
    const auto surf0 = Theme::hex(c.surface0);
    const auto surf1 = Theme::hex(c.surface1);
    const auto surf2 = Theme::hex(c.surface2);
    const auto surf3 = Theme::hex(c.surface3);
    const auto brd   = Theme::hex(c.border);
    const auto brdL  = Theme::hex(c.borderLight);
    const auto txt1  = Theme::hex(c.textPrimary);
    const auto txt2  = Theme::hex(c.textSecondary);
    const auto txt3  = Theme::hex(c.textTertiary);
    const auto txtD  = Theme::hex(c.textDisabled);
    const auto acc   = Theme::hex(c.accent);
    const auto accH  = Theme::hex(c.accentHover);
    const auto accS  = Theme::hex(c.accentSubtle);
    const auto accDm = Theme::hex(c.accentDim);
    const auto inp   = Theme::hex(c.inputBg);
    const auto inpB  = Theme::hex(c.inputBorder);
    const auto sucBg = Theme::hex(c.successBtnBg);
    const auto sucBH = Theme::hex(c.successBtnHover);
    const auto sucTx = Theme::hex(c.success);
    const auto danBg = Theme::hex(c.dangerBg);
    const auto danBH = Theme::hex(c.dangerBgHover);
    const auto danTx = Theme::hex(c.dangerText);
    const auto errC  = Theme::hex(c.error);
    const auto errBg = Theme::hex(c.errorBg);
    const auto warnC = Theme::hex(c.warning);
    const auto ctrlBd = Theme::hex(c.controlBorder);
    const auto scrTr = Theme::hex(c.scrollbarTrack);
    const auto scrTh = Theme::hex(c.scrollbarThumb);
    const auto rad   = QString::number(m.radiusSm);
    const auto radM  = QString::number(m.radiusMd);
    m_importPage = new QWidget;
    auto* importPageLayout = new QVBoxLayout(m_importPage);
    importPageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                          m.spacingXxl, m.spacingXxl);
    importPageLayout->setSpacing(m.spacingXl);

    // Title row
    auto* importTitleRow = new QHBoxLayout;
    auto* importTitle = new QLabel("Import Audio");
    importTitle->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    importTitleRow->addWidget(importTitle, 1);

    auto* importCloseBtn = new QPushButton("\u2715");
    importCloseBtn->setFixedSize(36, 36);
    importCloseBtn->setCursor(Qt::PointingHandCursor);
    importCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(importCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    importTitleRow->addWidget(importCloseBtn);
    importPageLayout->addLayout(importTitleRow);

    // Import button
    m_importAudioBtn = new QPushButton("\U0001F4C1  Import Audio Files...");
    m_importAudioBtn->setMinimumHeight(48);
    m_importAudioBtn->setCursor(Qt::PointingHandCursor);
    m_importAudioBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: white; border: none;"
        "  border-radius: %2px; font-size: 16px;"
        "  font-weight: 700; padding: 12px 24px; }"
        "QPushButton:hover { background: %3; }"
        "QPushButton:pressed { background: %4; }")
        .arg(Theme::rgb(c.primaryBtnBg))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.primaryBtnHover))
        .arg(Theme::rgb(c.accent)));
    connect(m_importAudioBtn, &QPushButton::clicked, this, &AudioSync::onImportAudioClicked);
    importPageLayout->addWidget(m_importAudioBtn);

    // Audio status
    m_audioStatus = new QLabel("No files imported");
    m_audioStatus->setStyleSheet(QStringLiteral(
        "font-size: 14px; color: %1;")
        .arg(Theme::rgb(c.textTertiary)));
    importPageLayout->addWidget(m_audioStatus);

    // Audio file list header with sort
    auto* filesHeaderRow = new QHBoxLayout;
    auto* filesLabel = new QLabel("IMPORTED FILES");
    filesLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    filesHeaderRow->addWidget(filesLabel, 1);

    m_audioSortCombo = new QComboBox;
    m_audioSortCombo->addItems({"Name", "Date", "Size"});
    m_audioSortCombo->setFixedHeight(28);
    m_audioSortCombo->setStyleSheet(QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3;"
        "  padding: 2px 8px; border-radius: %4px; font-size: 12px; }"
        "QComboBox::drop-down { width: 20px; border: none; }"
        "QComboBox::down-arrow { image: url(none); width: 0; height: 0;"
        "  border-left: 4px solid transparent; border-right: 4px solid transparent;"
        "  border-top: 5px solid %5; }"
        "QComboBox QAbstractItemView {"
        "  background: %6; color: %7; border: 1px solid %8;"
        "  selection-background-color: %9; outline: none; }")
        .arg(Theme::rgb(c.surface2), Theme::rgb(c.textSecondary),
             Theme::rgb(c.border), QString::number(m.radiusSm),
             Theme::rgb(c.textTertiary), Theme::rgb(c.surface2),
             Theme::rgb(c.textPrimary), Theme::rgb(c.border),
             Theme::rgb(c.accentDim)));
    connect(m_audioSortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) { sortAudioFileList(idx); });
    filesHeaderRow->addWidget(m_audioSortCombo);
    importPageLayout->addLayout(filesHeaderRow);

    m_audioFileList = new QListWidget;
    m_audioFileList->setTextElideMode(Qt::ElideNone);
    m_audioFileList->setWordWrap(true);
    m_audioFileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_audioFileList->setStyleSheet(
        QString("QListWidget { background: %1; border: 1px solid %2; "
        "border-radius: %3px; font-size: 14px; }"
        "QListWidget::item { padding: 10px 12px; border-bottom: 1px solid %4; }"
        "QListWidget::item:last { border-bottom: none; }"
        "QListWidget::item:selected { background: %5; }")
        .arg(inp, inpB, radM, Theme::rgb(c.borderLight), accS));
    m_audioFileList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_audioFileList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        auto* item = m_audioFileList->itemAt(pos);
        if (!item) return;
        int row = m_audioFileList->row(item);
        QMenu menu(m_audioFileList);
        QAction* relinkAct = menu.addAction(tr("Re-link..."));
        if (menu.exec(m_audioFileList->viewport()->mapToGlobal(pos)) == relinkAct)
            relinkAudioFile(row);
    });
    importPageLayout->addWidget(m_audioFileList, 1);

    // Remove button
    m_removeAudioBtn = new QPushButton("\U0001F5D1  Remove Selected");
    m_removeAudioBtn->setMinimumHeight(44);
    m_removeAudioBtn->setCursor(Qt::PointingHandCursor);
    m_removeAudioBtn->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: 14px; font-weight: 600;"
        "  padding: 8px 16px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.dangerText))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.surface3)));
    connect(m_removeAudioBtn, &QPushButton::clicked, this, [this]() {
        if (!m_audioFileList) return;
        auto selected = m_audioFileList->selectedItems();
        if (selected.isEmpty()) return;

        // Collect rows in descending order so removal doesn't shift indices
        std::vector<int> rows;
        rows.reserve(static_cast<size_t>(selected.size()));
        for (auto* item : selected)
            rows.push_back(m_audioFileList->row(item));
        std::sort(rows.begin(), rows.end(), std::greater<int>());

        QPointer<QListWidget> safeList = m_audioFileList;
        for (int row : rows) {
            if (row < 0 || row >= static_cast<int>(m_audioPaths.size())) continue;
            std::string removed = m_audioPaths[static_cast<size_t>(row)];
            m_audioPaths.erase(m_audioPaths.begin() + row);
            m_audioSamples.erase(removed);
            if (safeList) {
                QListWidgetItem* item = safeList->takeItem(row);
                if (item) {
                    spdlog::debug("AudioSync: Deleting audio file list item at row {} (remove)", row);
                    delete item;
                }
            }
            spdlog::info("AudioSync: Removed audio: {}", removed);
        }

        m_audioStatus->setText(m_audioPaths.empty() ? "No files imported"
            : QString("%1 file(s) imported").arg(m_audioPaths.size()));
        m_clips.clear();
        m_allTranscriptionResults.clear();
        m_transcriptionDone = false;
        m_syncDone = false;
        populateCards();
        if (m_audioPaths.empty()) {
            m_audioImported = false;
            m_audioPath.clear();
        }
        updateWorkflowState();
    });
    importPageLayout->addWidget(m_removeAudioBtn);

    m_audioSidePanelStack->addWidget(m_importPage);   // index 1

    // --- TRANSCRIBE page (index 2) ---------------------------------------
}

} // namespace rt
