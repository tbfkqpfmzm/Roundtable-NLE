/*
 * AudioSyncUISidePanel.cpp - Side panel container setup for AudioSync.
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
void AudioSync::setupAudioSidePanel(QHBoxLayout* rootLayout)
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
    m_audioSidePanel = new QWidget;
    m_audioSidePanel->setObjectName("AudioSidePanel");
    m_audioSidePanel->setMinimumWidth(0);
    m_audioSidePanel->setMaximumWidth(0);
    m_audioSidePanel->setVisible(false);
    m_audioSidePanel->setStyleSheet(QStringLiteral(
        "#AudioSidePanel {"
        "  background: %1;"
        "  border-right: 1px solid %2;"
        "}")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.borderLight)));

    auto* sidePanelLayout = new QVBoxLayout(m_audioSidePanel);
    sidePanelLayout->setContentsMargins(0, 0, 0, 0);
    sidePanelLayout->setSpacing(0);

    m_audioSidePanelStack = new QStackedWidget;
    sidePanelLayout->addWidget(m_audioSidePanelStack);

    // --- SCRIPT page (index 0) -------------------------------------------
    setupScriptPage();
    setupImportPage();
    setupTranscribePage();
    setupMatchSettingsPages();
    rootLayout->addWidget(m_audioSidePanel);
}

} // namespace rt
