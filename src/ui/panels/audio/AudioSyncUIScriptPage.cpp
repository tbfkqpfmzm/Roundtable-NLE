/*
 * AudioSyncUIScriptPage.cpp - Script side-panel page for AudioSync.
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
void AudioSync::setupScriptPage()
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
    m_scriptPage = new QWidget;
    auto* scriptPageLayout = new QVBoxLayout(m_scriptPage);
    scriptPageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                          m.spacingXxl, m.spacingXxl);
    scriptPageLayout->setSpacing(m.spacingXl);

    // Title row
    auto* scriptTitleRow = new QHBoxLayout;
    auto* scriptTitle = new QLabel("Load Script");
    scriptTitle->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    scriptTitleRow->addWidget(scriptTitle, 1);

    auto* scriptCloseBtn = new QPushButton("\u2715");
    scriptCloseBtn->setFixedSize(36, 36);
    scriptCloseBtn->setCursor(Qt::PointingHandCursor);
    scriptCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(scriptCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    scriptTitleRow->addWidget(scriptCloseBtn);
    scriptPageLayout->addLayout(scriptTitleRow);

    // Description
    auto* scriptDesc = new QLabel(
        "Enter a Google Docs URL or choose a local script file.\n"
        "The script defines dialogue lines and characters.");
    scriptDesc->setWordWrap(true);
    scriptDesc->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: %1;")
        .arg(Theme::rgb(c.textTertiary)));
    scriptPageLayout->addWidget(scriptDesc);

    // URL combo
    auto* scriptUrlLabel = new QLabel("SCRIPT SOURCE");
    scriptUrlLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    scriptPageLayout->addWidget(scriptUrlLabel);

    m_scriptUrlCombo = new QComboBox;
    m_scriptUrlCombo->setEditable(true);
    m_scriptUrlCombo->setInsertPolicy(QComboBox::NoInsert);
    m_scriptUrlCombo->lineEdit()->setPlaceholderText("Google Docs URL or file...");
    m_scriptUrlCombo->setMinimumHeight(44);
    m_scriptUrlCombo->setMaxVisibleItems(8);
    m_scriptUrlCombo->setStyleSheet(QStringLiteral(
        "QComboBox {"
        "  background: %1; color: %2; border: 1px solid %3;"
        "  padding: 10px 14px; border-radius: %4px; font-size: 14px;"
        "}"
        "QComboBox:hover { border-color: %5; }"
        "QComboBox:focus { border-color: %6; }"
        "QComboBox::drop-down {"
        "  subcontrol-origin: padding; subcontrol-position: center right;"
        "  width: 32px; border: none;"
        "}"
        "QComboBox::down-arrow {"
        "  image: url(none); width: 0; height: 0;"
        "  border-left: 5px solid transparent; border-right: 5px solid transparent;"
        "  border-top: 6px solid %14;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background: %7; color: %8; border: 1px solid %9;"
        "  border-radius: %10px; padding: 4px;"
        "  selection-background-color: %11;"
        "  selection-color: %12;"
        "  outline: none;"
        "}"
        "QComboBox QAbstractItemView::item {"
        "  padding: 8px 12px; min-height: 28px;"
        "}"
        "QComboBox QAbstractItemView::item:hover {"
        "  background: %13;"
        "}")
        .arg(inp, txt1, inpB, radM,
             Theme::rgb(c.accent), Theme::rgb(c.accent),
             Theme::rgb(c.surface2), txt1, Theme::rgb(c.border))
        .arg(radM, Theme::rgb(c.accentDim), Theme::rgb(c.textPrimary),
             Theme::rgb(c.surface3), Theme::rgb(c.textSecondary)));
    m_scriptUrlCombo->lineEdit()->setStyleSheet(QStringLiteral(
        "QLineEdit { background: transparent; border: none; color: %1;"
        "  padding: 0; font-size: 14px; }")
        .arg(txt1));
    scriptPageLayout->addWidget(m_scriptUrlCombo);
    loadScriptHistory();

    // Load button
    m_loadScriptBtn = new QPushButton("\u25B6  Load Script");
    m_loadScriptBtn->setMinimumHeight(48);
    m_loadScriptBtn->setCursor(Qt::PointingHandCursor);
    m_loadScriptBtn->setStyleSheet(QStringLiteral(
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
    connect(m_loadScriptBtn, &QPushButton::clicked, this, &AudioSync::onLoadScriptClicked);
    scriptPageLayout->addWidget(m_loadScriptBtn);

    // Script status
    m_scriptStatus = new QLabel("No script loaded");
    m_scriptStatus->setStyleSheet(QStringLiteral(
        "font-size: 14px; color: %1;")
        .arg(Theme::rgb(c.textTertiary)));
    scriptPageLayout->addWidget(m_scriptStatus);

    scriptPageLayout->addStretch();
    m_audioSidePanelStack->addWidget(m_scriptPage);   // index 0

    // --- IMPORT page (index 1) -------------------------------------------
}

} // namespace rt
