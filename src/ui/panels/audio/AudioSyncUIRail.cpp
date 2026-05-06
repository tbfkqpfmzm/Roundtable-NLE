/*
 * AudioSyncUIRail.cpp - Icon rail setup for AudioSync.
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
void AudioSync::setupAudioIconRail(QHBoxLayout* rootLayout)
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
    m_audioIconRail = new QWidget;
    m_audioIconRail->setObjectName("AudioIconRail");
    m_audioIconRail->setFixedWidth(150);
    m_audioIconRail->setStyleSheet(QStringLiteral(
        "#AudioIconRail { background: %1;"
        "  border-right: 1px solid %2; }")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.border)));

    auto* railLayout = new QVBoxLayout(m_audioIconRail);
    railLayout->setContentsMargins(8, m.spacingXl, 8, m.spacingXl);
    railLayout->setSpacing(0);

    // Rail button style (matches main nav rail exactly)
    QString railBtnStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; color: %2; font-size: 42px;"
        "  padding: 12px 0; }"
        "QPushButton:hover { background: %3; color: %4; }"
        "QPushButton:pressed { background: %5; color: white; }"
        "QPushButton:checked { background: %6; color: %7; }")
        .arg(m.radiusXl)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent));

    auto makeRailBtn = [&](const QString& icon, const QString& label,
                           const QString& tip, bool checkable = false) -> QPushButton*
    {
        auto* btn = new QPushButton(icon);
        btn->setToolTip(tip);
        btn->setFixedSize(128, 84);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setCheckable(checkable);
        btn->setStyleSheet(railBtnStyle);
        railLayout->addWidget(btn, 0, Qt::AlignHCenter);

        railLayout->addSpacing(4);

        auto* lbl = new QLabel(label);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setFixedHeight(20);
        lbl->setStyleSheet(QStringLiteral(
            "font-size: 15px; color: %1; font-weight: 800;")
            .arg(Theme::rgb(c.textPrimary)));
        railLayout->addWidget(lbl, 0, Qt::AlignHCenter);

        return btn;
    };

    // Helper: add a divider between rail entries — single container
    // widget wrapping a 1px line to prevent DPI drift.
    auto addRailDivider = [&]() {
        auto* div = new QWidget;
        div->setFixedHeight(17);
        div->setAutoFillBackground(false);
        div->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* lay = new QVBoxLayout(div);
        lay->setContentsMargins(16, 0, 16, 0);
        lay->setSpacing(0);
        lay->addStretch();
        auto* line = new QFrame;
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::rgb(c.border)));
        lay->addWidget(line);
        lay->addStretch();
        railLayout->addWidget(div);
    };

    m_scriptRailBtn = makeRailBtn("\U0001F4DC", "SCRIPT", "Load / manage script", true);
    connect(m_scriptRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(0); });

    addRailDivider();

    m_importRailBtn = makeRailBtn("\U0001F3B5", "IMPORT", "Import audio files", true);
    connect(m_importRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(1); });

    addRailDivider();

    m_transcribeRailBtn = makeRailBtn("\u26A1", "TRANSCRIBE", "Transcribe audio", true);
    connect(m_transcribeRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(2); });

    addRailDivider();

    m_matchRailBtn = makeRailBtn("\U0001F517", "MATCH", "Character filter & matching", true);
    connect(m_matchRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(3); });

    addRailDivider();

    m_audioSettingsRailBtn = makeRailBtn("\u2699", "SETTINGS", "Audio sync settings", true);
    connect(m_audioSettingsRailBtn, &QPushButton::clicked,
            this, [this]() { toggleAudioSidePanel(4); });

    railLayout->addStretch();
    rootLayout->addWidget(m_audioIconRail);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //  SIDE PANEL  (inline expanding column, like ProjectPanel)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
}

} // namespace rt
