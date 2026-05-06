/*
 * AudioSyncUIContent.cpp - Main content area setup for AudioSync.
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
void AudioSync::setupAudioContentArea(QHBoxLayout* rootLayout)
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
    m_audioContentArea = new QWidget;
    auto* contentLayout = new QVBoxLayout(m_audioContentArea);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    // â”€â”€ Status bar (compact, at top of content) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* statusBar = new QWidget;
    statusBar->setFixedHeight(36);
    statusBar->setStyleSheet(
        QString("QWidget { background: %1; border-bottom: 1px solid %2; }").arg(surf0, brd));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 0, 12, 0);
    statusLayout->setSpacing(8);

    m_smartBarIcon = new QLabel(QStringLiteral("\u25CF"));
    m_smartBarIcon->setFixedWidth(18);
    m_smartBarIcon->setStyleSheet(QString("QLabel { color: %1; font-size: 14px; border: none; }").arg(txtD));
    statusLayout->addWidget(m_smartBarIcon);

    m_smartBarLabel = new QLabel("Load a script to begin");
    m_smartBarLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 13px; border: none; }").arg(txt2));
    statusLayout->addWidget(m_smartBarLabel, 1);

    m_syncStatus = new QLabel("0 / 0");
    m_syncStatus->setStyleSheet(QString("QLabel { color: %1; font-size: 12px; border: none; }").arg(txtD));
    statusLayout->addWidget(m_syncStatus);

    contentLayout->addWidget(statusBar);

    // â”€â”€ MAIN SPLIT PANE  (Left: tabs+list | Right: card scroll) â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setStyleSheet(
        QString("QSplitter { background: %1; }"
        "QSplitter::handle { background: %2; width: 3px; }").arg(surf0, brd));

    // â”€â”€ LEFT PANE â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* leftPane = new QWidget;
    leftPane->setStyleSheet(QString("QWidget { background: %1; }").arg(surf1));
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    // Character tabs removed â€” filter now in MATCH side panel (m_charFilterList)

    // Script line list
    m_leftScriptList = new QListWidget;
    m_leftScriptList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_leftScriptList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_leftScriptList->setWordWrap(false);
    m_leftScriptList->setTextElideMode(Qt::ElideNone);
    m_leftScriptList->setStyleSheet(
        "QListWidget { background: " + surf0 + "; border: none; outline: none; padding-top: 4px; }"
        "QListWidget::item { background: transparent; border: none; padding: 0px; margin: 4px 8px; }"
        "QListWidget::item:selected { background: transparent; }"
        "QListWidget::item:hover { background: transparent; }");
    connect(m_leftScriptList, &QListWidget::currentRowChanged, this, [this](int row) {
        // Restore previous left card style
        if (m_selectedLeftCard) {
            m_selectedLeftCard->setGraphicsEffect(nullptr);
            m_selectedLeftCard = nullptr;
        }
        if (row >= 0 && row < static_cast<int>(m_cardScriptLineNums.size())) {
            scrollToCard(m_cardScriptLineNums[static_cast<size_t>(row)]);
            // Select the corresponding clip for spacebar playback
            auto idx = static_cast<size_t>(row);
            if (idx < m_cardClipIndices.size() && m_cardClipIndices[idx] >= 0)
                m_selectedClipIdx = m_cardClipIndices[idx];
            // Highlight the left card
            auto* item = m_leftScriptList->item(row);
            auto* w = m_leftScriptList->itemWidget(item);
            auto* frame = qobject_cast<QFrame*>(w);
            if (frame) {
                m_selectedLeftCard = frame;
                auto* glow = new QGraphicsDropShadowEffect(frame);
                glow->setBlurRadius(14);
                glow->setColor(Theme::colors().accent);
                glow->setOffset(0, 0);
                frame->setGraphicsEffect(glow);
            }
        }
    });
    m_leftScriptList->installEventFilter(this);
    leftLayout->addWidget(m_leftScriptList, 1);

    // Orphan clips section
    m_leftOrphanLabel = new QLabel("  Orphan Clips");
    m_leftOrphanLabel->setFixedHeight(28);
    m_leftOrphanLabel->setStyleSheet(
        "QLabel { color: " + errC + "; font-size: 12px; font-weight: bold; "
        "background: " + errBg + "; border: none; border-top: 2px solid " + errC + "; }");
    m_leftOrphanLabel->setVisible(false);
    leftLayout->addWidget(m_leftOrphanLabel);

    m_leftOrphanList = new QListWidget;
    m_leftOrphanList->setMaximumHeight(120);
    m_leftOrphanList->setStyleSheet(
        "QListWidget { background: " + surf0 + "; border: none; font-size: 12px; }"
        "QListWidget::item { padding: 4px 8px; }"
        "QListWidget::item:selected { background: " + surf2 + "; }");
    m_leftOrphanList->setVisible(false);
    leftLayout->addWidget(m_leftOrphanList);

    m_splitter->addWidget(leftPane);

    // â”€â”€ RIGHT PANE  (continuous scroll of cards) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_rightScrollArea = new QScrollArea;
    m_rightScrollArea->setWidgetResizable(true);
    m_rightScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rightScrollArea->setStyleSheet(
        "QScrollArea { background: " + surf1 + "; border: none; }"
        "QScrollBar:vertical { background: " + scrTr + "; width: 16px; border: none; }"
        "QScrollBar::handle:vertical { background: " + scrTh + "; border-radius: 7px; min-height: 40px; }"
        "QScrollBar::handle:vertical:hover { background: " + txt3 + "; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    m_rightScrollContent = new QWidget;
    m_rightScrollContent->setStyleSheet("QWidget { background: " + surf1 + "; }");
    m_rightLayout = new QVBoxLayout(m_rightScrollContent);
    m_rightLayout->setContentsMargins(12, 12, 12, 12);
    m_rightLayout->setSpacing(10);
    m_rightLayout->addStretch();

    m_rightScrollArea->setWidget(m_rightScrollContent);

    // Sync right scroll â†’ left list selection
    connect(m_rightScrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this]() { syncLeftListFromScroll(); });

    m_splitter->addWidget(m_rightScrollArea);
    m_splitter->setSizes({420, 480});

    contentLayout->addWidget(m_splitter, 1);

    // â”€â”€ BOTTOM ACTION BAR â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_audioActionBar = new QWidget;
    m_audioActionBar->setFixedHeight(56);
    m_audioActionBar->setStyleSheet(
        QString("QWidget { background: %1; border-top: 1px solid %2; }").arg(surf0, brd));

    auto* actionBarLayout = new QHBoxLayout(m_audioActionBar);
    actionBarLayout->setContentsMargins(12, 0, 12, 0);
    actionBarLayout->setSpacing(10);

    auto makeActionBtn = [&](const QString& text, const QColor& bg,
                             const QColor& hoverBg, const QColor& textColor) -> QPushButton*
    {
        auto* btn = new QPushButton(text);
        btn->setMinimumHeight(36);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: %1; color: %2; border: none;"
            "  border-radius: %3px; font-size: 13px;"
            "  font-weight: 600; padding: 6px 16px; }"
            "QPushButton:hover { background: %4; }"
            "QPushButton:disabled { background: %5; color: %6; }")
            .arg(Theme::rgb(bg))
            .arg(Theme::rgb(textColor))
            .arg(m.radiusMd)
            .arg(Theme::rgb(hoverBg))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.textDisabled)));
        return btn;
    };

    m_syncActionBtn = makeActionBtn(
        "\U0001F504  Auto-Sync", c.accentDim, c.accentHover, c.textPrimary);
    connect(m_syncActionBtn, &QPushButton::clicked, this, &AudioSync::onAutoSyncClicked);
    actionBarLayout->addWidget(m_syncActionBtn);

    m_confirmAllActionBtn = makeActionBtn(
        "\u2713  Confirm All", c.successBtnBg, c.successBtnHover, c.textPrimary);
    connect(m_confirmAllActionBtn, &QPushButton::clicked, this, [this]() {
        for (auto& clip : m_clips) {
            if (clip.matchState == 1)
                clip.matchState = 2;
        }
        populateCards();
        updateWorkflowState();
    });
    actionBarLayout->addWidget(m_confirmAllActionBtn);

    m_unconfirmAllActionBtn = makeActionBtn(
        "\u21A9  Unconfirm All", c.surface3, c.surface2, c.textPrimary);
    connect(m_unconfirmAllActionBtn, &QPushButton::clicked, this, [this]() {
        for (auto& clip : m_clips) {
            if (clip.matchState == 2)
                clip.matchState = 1;
        }
        populateCards();
        updateWorkflowState();
    });
    actionBarLayout->addWidget(m_unconfirmAllActionBtn);

    m_clearActionBtn = makeActionBtn(
        "\u2715  Clear Matches", c.surface3, c.surface2, c.textPrimary);
    connect(m_clearActionBtn, &QPushButton::clicked, this, [this]() {
        for (auto& clip : m_clips) {
            clip.matchState = 0;
            clip.confidence = 0.0f;
            clip.scriptLineNumber = -1;
            clip.scriptSegment.clear();
        }
        populateCards();
        updateWorkflowState();
    });
    actionBarLayout->addWidget(m_clearActionBtn);

    auto* closeGapsBtn = makeActionBtn(
        "\u2194  Close Gaps", c.surface3, c.surface2, c.textPrimary);
    connect(closeGapsBtn, &QPushButton::clicked, this, &AudioSync::closeInterClipGaps);
    actionBarLayout->addWidget(closeGapsBtn);

    actionBarLayout->addStretch();

    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 12px; border: none; }").arg(txtD));
    actionBarLayout->addWidget(m_statusLabel);

    m_exportActionBtn = makeActionBtn(
        "\U0001F4E4  Export \u2192", c.successBtnBg, c.successBtnHover, c.textPrimary);
    connect(m_exportActionBtn, &QPushButton::clicked, this, &AudioSync::onExportClicked);
    actionBarLayout->addWidget(m_exportActionBtn);

    contentLayout->addWidget(m_audioActionBar);

    rootLayout->addWidget(m_audioContentArea, 1);

}

} // namespace rt
