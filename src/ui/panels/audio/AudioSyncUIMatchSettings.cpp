/*
 * AudioSyncUIMatchSettings.cpp - Match and settings side-panel pages for AudioSync.
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
namespace {
class UnmatchedDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        if (index.data(Qt::UserRole).toString() == "unmatched") {
            painter->save();
            QColor bg(0xCC, 0x33, 0x33);
            if (option.state & QStyle::State_Selected)
                bg = bg.lighter(120);
            else if (option.state & QStyle::State_MouseOver)
                bg = bg.lighter(110);
            painter->fillRect(option.rect, bg);
            QFont f = option.font;
            f.setWeight(QFont::Bold);
            painter->setFont(f);
            painter->setPen(Qt::white);
            QRect textRect = option.rect.adjusted(14, 0, -14, 0);
            painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                              index.data(Qt::DisplayRole).toString());
            painter->restore();
        } else {
            QStyledItemDelegate::paint(painter, option, index);
        }
    }
};
} // namespace
void AudioSync::setupMatchSettingsPages()
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
    m_matchPage = new QWidget;
    auto* matchPageLayout = new QVBoxLayout(m_matchPage);
    matchPageLayout->setContentsMargins(m.spacingMd, m.spacingLg,
                                         m.spacingMd, m.spacingLg);
    matchPageLayout->setSpacing(m.spacingMd);

    // Title row
    auto* matchTitleRow = new QHBoxLayout;
    auto* matchTitle = new QLabel("Characters");
    matchTitle->setStyleSheet(QStringLiteral(
        "font-size: 16px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    matchTitleRow->addWidget(matchTitle, 1);

    auto* matchCloseBtn = new QPushButton("\u2715");
    matchCloseBtn->setFixedSize(28, 28);
    matchCloseBtn->setCursor(Qt::PointingHandCursor);
    matchCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(matchCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    matchTitleRow->addWidget(matchCloseBtn);
    matchPageLayout->addLayout(matchTitleRow);

    // Character filter list
    m_charFilterList = new QListWidget;
    m_charFilterList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_charFilterList->addItem("ALL");
    m_charFilterList->setCurrentRow(0);
    m_charFilterList->setStyleSheet(QStringLiteral(
        "QListWidget {"
        "  background: %1; border: 1px solid %2;"
        "  border-radius: %3px; font-size: 16px; font-weight: 600; outline: none;"
        "}"
        "QListWidget::item {"
        "  padding: 10px 14px; border-bottom: 1px solid %4;"
        "  color: %5;"
        "}"
        "QListWidget::item:last { border-bottom: none; }"
        "QListWidget::item:selected {"
        "  background: %6; color: %7;"
        "  font-weight: 700;"
        "}"
        "QListWidget::item:hover:!selected {"
        "  background: %8;"
        "}")
        .arg(Theme::rgb(c.inputBg))
        .arg(Theme::rgb(c.inputBorder))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.borderLight))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent))
        .arg(Theme::rgb(c.surface3)));
    // Use a delegate to paint UNMATCHED items with red bg + white text
    m_charFilterList->setItemDelegate(new class UnmatchedDelegate(m_charFilterList));
    connect(m_charFilterList, &QListWidget::currentRowChanged, this, [this](int) {
        populateCards();
    });
    matchPageLayout->addWidget(m_charFilterList, 1);

    m_audioSidePanelStack->addWidget(m_matchPage);   // index 3

    // --- SETTINGS page (index 4) -----------------------------------------
    m_audioSettingsPage = new QWidget;
    auto* settingsPageLayout = new QVBoxLayout(m_audioSettingsPage);
    settingsPageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                            m.spacingXxl, m.spacingXxl);
    settingsPageLayout->setSpacing(m.spacingXl);

    // Title row
    auto* settingsTitleRow = new QHBoxLayout;
    auto* settingsTitle = new QLabel("Audio Settings");
    settingsTitle->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    settingsTitleRow->addWidget(settingsTitle, 1);

    auto* settingsCloseBtn = new QPushButton("\u2715");
    settingsCloseBtn->setFixedSize(36, 36);
    settingsCloseBtn->setCursor(Qt::PointingHandCursor);
    settingsCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(settingsCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    settingsTitleRow->addWidget(settingsCloseBtn);
    settingsPageLayout->addLayout(settingsTitleRow);

    // Retakes section
    auto* retakesLabel = new QLabel("MATCHING OPTIONS");
    retakesLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    settingsPageLayout->addWidget(retakesLabel);

    m_retakesCheck = new QCheckBox("Allow retakes (multiple audio segments per script line)");
    m_retakesCheck->setStyleSheet(
        QString("QCheckBox { color: %1; font-size: 14px; spacing: 10px; }"
        "QCheckBox::indicator { width: 20px; height: 20px; }").arg(txt1));
    m_retakesCheck->setToolTip("Allow script lines to match multiple audio segments");
    settingsPageLayout->addWidget(m_retakesCheck);

    settingsPageLayout->addStretch();
    m_audioSidePanelStack->addWidget(m_audioSettingsPage);   // index 4
}

} // namespace rt
