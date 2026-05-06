/*
 * SourceMonitorUI.cpp -- WaveformDisplayWidget, constructor, destructor, setupUI.
 *
 * Split from SourceMonitor.cpp for maintainability.
 */

#include "panels/monitors/SourceMonitor.h"
#include "panels/monitors/WaveformDisplayWidget.h"

#include "Theme.h"
#include "UiScale.h"

#include "viewport/Viewport.h"
#include "widgets/MiniTimeline.h"
#include "widgets/TransportButton.h"
#include "media/PlaybackController.h"
#include "media/MediaPool.h"
#include "media/AudioFile.h"
#include "media/AudioEngine.h"
#include "media/AVSyncClock.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStackedLayout>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QTreeWidget>
#include <QFrame>
#include <QLineEdit>
#include <QRegularExpressionValidator>

#include <algorithm>
#include <cmath>
#include <thread>



namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

SourceMonitor::SourceMonitor(QWidget* parent)
    : QWidget(parent)
    , m_controller(std::make_unique<PlaybackController>())
{
    setupUI();

    // Polling timer for playback position updates
    m_pollTimer = new QTimer(this);
    m_pollTimer->setTimerType(Qt::PreciseTimer);
    m_pollTimer->setInterval(16); // ~60fps
    connect(m_pollTimer, &QTimer::timeout, this, &SourceMonitor::onPollTimer);

    // Wire mini-timeline scrub events
    connect(m_miniTimeline, &MiniTimeline::scrubbed, this, &SourceMonitor::onScrub);

    // Manage poll timer and audio from controller state changes
    // (covers both button clicks and keyboard shortcuts)
    m_controller->onStateChanged = [this](PlayState state) {
        if (state == PlayState::Playing || state == PlayState::Shuttling) {
            m_pollTimer->start();
            startSourceAudio();
        } else {
            m_pollTimer->stop();
            stopSourceAudio();
            updateFrameDisplay();
        }
    };

    // Update audio engine speed when shuttle speed changes mid-playback
    m_controller->onSpeedChanged = [this](double speed) {
        if (m_sourceAudioActive && m_audioEngine) {
            m_audioEngine->setPlaybackSpeed(speed);
            if (m_audioEngine->transportState() != TransportState::Playing)
                m_audioEngine->play();
        }
    };

    // Accept keyboard focus so JKL/Space route to this monitor
    setFocusPolicy(Qt::ClickFocus);

    // Accept drops from Project Bin
    setAcceptDrops(true);

    // Install event filter on interactive children so any click
    // within the Source Monitor grabs keyboard focus for JKL/Space.
    m_viewport->setAcceptDrops(true);
    m_viewport->installEventFilter(this);
    m_miniTimeline->installEventFilter(this);
    m_waveformWidget->installEventFilter(this);
}

SourceMonitor::~SourceMonitor() = default;

void SourceMonitor::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;
    // NOTE: Do NOT set audio engine on the PlaybackController.
    // The shared AudioEngine is wired to the timeline's sync clock.
    // If the controller calls audioEngine->play()/stop(), it would
    // start/reset the timeline's sync clock, corrupting timeline state.
    // Audio is managed directly by startSourceAudio()/stopSourceAudio().
}

// ═════════════════════════════════════════════════════════════════════════════
//  UI Setup
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    setMinimumWidth(rt::UiScale::px(200));
    setMinimumHeight(rt::UiScale::px(180));

    // ── Clip name label ──────────────────────────────────
    m_clipLabel = new QLabel(tr("No clip loaded"), this);
    rt::UiScale::setScaledFixedHeight(m_clipLabel, 28);
    m_clipLabel->setAlignment(Qt::AlignCenter);
    m_clipLabel->setTextFormat(Qt::PlainText);
    m_clipLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { background: %1; color: %2; "
        "font-size: 12px; padding: 4px 8px; }")
        .arg(Theme::hex(Theme::colors().surface1))
        .arg(Theme::hex(Theme::colors().textSecondary))));
    mainLayout->addWidget(m_clipLabel);

    // ── Viewport / Waveform stacked area ──────────────────────────────
    auto* viewContainer = new QWidget(this);
    m_viewStack = new QStackedLayout(viewContainer);
    m_viewStack->setContentsMargins(0, 0, 0, 0);

    m_viewport = new Viewport(viewContainer);
    m_viewStack->addWidget(m_viewport);           // index 0

    m_waveformWidget = new WaveformDisplayWidget(viewContainer);
    m_viewStack->addWidget(m_waveformWidget);     // index 1

    // Waveform click-to-scrub
    m_waveformWidget->setScrubCallback([this](double ratio) {
        if (!m_hasClip || m_clipDuration <= 0) return;
        int64_t tick = static_cast<int64_t>(ratio * m_clipDuration);
        m_controller->seekTo(tick);
        scrubAudioAt(tick);
        updateFrameDisplay();
        emit playheadChanged(tick);
    });

    m_viewStack->setCurrentIndex(0); // default: video viewport
    mainLayout->addWidget(viewContainer, 1); // stretch=1

    // Spacer between viewport and controls — prevents video spill
    auto* viewSpacer = new QWidget(this);
    rt::UiScale::setScaledFixedHeight(viewSpacer, 4);
    viewSpacer->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; }")
        .arg(Theme::hex(Theme::colors().surface0)));
    mainLayout->addWidget(viewSpacer);

    // ── Mini-timeline scrub bar ─────────────────────────────────────────
    m_miniTimeline = new MiniTimeline(this);

    // ── Info/control bar (Premiere Pro style — above mini timeline) ──────
    auto* controlBar = new QWidget(this);
    controlBar->setObjectName(QStringLiteral("ControlBar"));
    rt::UiScale::setScaledFixedHeight(controlBar, 52);
    controlBar->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "#ControlBar { background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface0))
        .arg(Theme::hex(Theme::colors().border))));

    auto* controlLayout = new QHBoxLayout(controlBar);
    controlLayout->setContentsMargins(rt::UiScale::px(8), rt::UiScale::px(10),
                                      rt::UiScale::px(8), rt::UiScale::px(10));
    controlLayout->setSpacing(rt::UiScale::px(6));

    auto comboStyle = rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QComboBox { background: %1; border: 1px solid %2; "
        "border-radius: 3px; color: %3; font-size: 12px; "
        "padding: 4px 8px 4px 8px; }"
        "QComboBox::drop-down { border: none; width: 20px; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textPrimary)));

    // Timecode display (left side, Premiere Pro green style — click to edit)
    m_timecodeLabel = new QLabel(QStringLiteral("00:00:00:00"), this);
    m_timecodeLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 14px; "
        "font-weight: bold; color: #00CC88; background: transparent; "
        "padding: 0px 6px 0px 0px; }")));
    rt::UiScale::setScaledFixedWidth(m_timecodeLabel, 120);
    m_timecodeLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_timecodeLabel->setCursor(Qt::IBeamCursor);
    m_timecodeLabel->setToolTip(tr("Click to enter timecode"));
    m_timecodeLabel->installEventFilter(this);

    // Hidden editable timecode field (shown on click)
    m_timecodeEdit = new QLineEdit(this);
    m_timecodeEdit->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLineEdit { font-family: 'Consolas', monospace; font-size: 14px; "
        "font-weight: bold; color: #00CC88; background: %1; "
        "border: 1px solid #00CC88; border-radius: 3px; "
        "padding: 0px 6px 0px 0px; }")
        .arg(Theme::hex(Theme::colors().surface2))));
    rt::UiScale::setScaledFixedWidth(m_timecodeEdit, 120);
    m_timecodeEdit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_timecodeEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("\\d{0,2}:?\\d{0,2}:?\\d{0,2}:?\\d{0,2}")), m_timecodeEdit));
    m_timecodeEdit->setPlaceholderText(QStringLiteral("HH:MM:SS:FF"));
    m_timecodeEdit->hide();

    connect(m_timecodeEdit, &QLineEdit::returnPressed, this, [this]() {
        QString text = m_timecodeEdit->text().trimmed();
        QStringList parts = text.split(QChar(':'));
        if (parts.size() == 4 && m_controller && m_hasClip) {
            Timecode tc;
            tc.hours   = parts[0].toInt();
            tc.minutes = parts[1].toInt();
            tc.seconds = parts[2].toInt();
            tc.frames  = parts[3].toInt();
            int64_t tick = timecodeToTick(tc, m_controller->frameRate());
            m_controller->seekTo(tick);
            updateFrameDisplay();
        }
        m_timecodeEdit->hide();
        m_timecodeLabel->show();
    });
    connect(m_timecodeEdit, &QLineEdit::editingFinished, this, [this]() {
        m_timecodeEdit->hide();
        m_timecodeLabel->show();
    });

    controlLayout->addWidget(m_timecodeLabel, 0, Qt::AlignVCenter);
    controlLayout->addWidget(m_timecodeEdit, 0, Qt::AlignVCenter);
    controlLayout->addSpacing(rt::UiScale::px(12));

    // Fit mode / zoom presets combo box
    m_fitModeCombo = new QComboBox(this);
    m_fitModeCombo->addItem(tr("Fit"));     // 0
    m_fitModeCombo->addItem(tr("Fill"));    // 1
    m_fitModeCombo->addItem(tr("25%"));     // 2
    m_fitModeCombo->addItem(tr("50%"));     // 3
    m_fitModeCombo->addItem(tr("75%"));     // 4
    m_fitModeCombo->addItem(tr("100%"));    // 5
    m_fitModeCombo->addItem(tr("150%"));    // 6
    m_fitModeCombo->addItem(tr("200%"));    // 7
    m_fitModeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_fitModeCombo->setFocusPolicy(Qt::NoFocus);
    m_fitModeCombo->setStyleSheet(comboStyle);
    rt::UiScale::setScaledMinimumWidth(m_fitModeCombo, 80);
    rt::UiScale::setScaledFixedHeight(m_fitModeCombo, 24);
    controlLayout->addWidget(m_fitModeCombo, 0, Qt::AlignVCenter);

    controlLayout->addStretch();

    connect(m_fitModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        switch (index) {
        case 0: m_viewport->setFitMode(ViewportFitMode::Fit);  break;
        case 1: m_viewport->setFitMode(ViewportFitMode::Fill); break;
        default: {
            static constexpr float zoomLevels[] = { 0.25f, 0.50f, 0.75f, 1.0f, 1.5f, 2.0f };
            int zi = index - 2;
            if (zi >= 0 && zi < static_cast<int>(std::size(zoomLevels))) {
                m_viewport->setFitMode(ViewportFitMode::Actual);
                m_viewport->setViewZoom(zoomLevels[zi]);
            }
            break;
        }
        }
        if (index <= 1)
            m_viewport->resetZoomPan();
    });

    // Playback resolution dropdown
    m_playbackResCombo = new QComboBox(this);
    m_playbackResCombo->addItem(tr("Full"));
    m_playbackResCombo->addItem(QStringLiteral("1/2"));
    m_playbackResCombo->addItem(QStringLiteral("1/4"));
    m_playbackResCombo->addItem(QStringLiteral("1/8"));
    m_playbackResCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_playbackResCombo->setFocusPolicy(Qt::NoFocus);
    m_playbackResCombo->setToolTip(tr("Playback Resolution"));
    m_playbackResCombo->setStyleSheet(comboStyle);
    rt::UiScale::setScaledMinimumWidth(m_playbackResCombo, 70);
    rt::UiScale::setScaledFixedHeight(m_playbackResCombo, 24);
    m_playbackResCombo->setCurrentIndex(1); // default 1/2
    controlLayout->addWidget(m_playbackResCombo, 0, Qt::AlignVCenter);

    // Safe Area toggle button (Premiere Pro style icon)
    auto checkedBtnStyle = rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; "
        "border-radius: 3px; padding: 3px; } "
        "QPushButton:hover { background: %2; } "
        "QPushButton:checked { background: %3; border-color: %3; }")
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().accent)));

    auto makeSafeIcon = [](QColor fg) -> QIcon {
        QPixmap px(24, 24);
        px.fill(Qt::transparent);
        QPainter ip(&px);
        ip.setRenderHint(QPainter::Antialiasing, false);
        ip.setPen(QPen(fg, 1.5));
        ip.setBrush(Qt::NoBrush);
        ip.drawRect(QRectF(1.5, 1.5, 21, 21));
        ip.drawRect(QRectF(5.5, 5.5, 13, 13));
        ip.setPen(QPen(fg, 1.0, Qt::DashLine));
        ip.drawRect(QRectF(8.5, 8.5, 7, 7));
        ip.end();
        return QIcon(px);
    };
    QIcon safeIcon = makeSafeIcon(Theme::colors().textSecondary);

    m_btnSafeArea = new QPushButton(this);
    m_btnSafeArea->setIcon(safeIcon);
    m_btnSafeArea->setIconSize(QSize(rt::UiScale::px(16), rt::UiScale::px(16)));
    m_btnSafeArea->setCheckable(true);
    rt::UiScale::setScaledFixedSize(m_btnSafeArea, 24, 22);
    m_btnSafeArea->setFocusPolicy(Qt::NoFocus);
    m_btnSafeArea->setToolTip(tr("Toggle Safe Area Overlay"));
    m_btnSafeArea->setStyleSheet(checkedBtnStyle);
    controlLayout->addWidget(m_btnSafeArea, 0, Qt::AlignVCenter);
    connect(m_btnSafeArea, &QPushButton::toggled, this, [this](bool checked) {
        m_viewport->setSafeAreasVisible(checked);
    });

    // Export frame button (large, Premiere Pro style)
    auto exportBtnStyle = rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid %2; "
        "border-radius: 4px; color: %3; font-size: 14px; padding: 1px; } "
        "QPushButton:hover { background: %4; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textBright))
        .arg(Theme::hex(Theme::colors().controlBgHover)));
    m_btnExportFrame = new QPushButton(QStringLiteral("📷"), this);
    m_btnExportFrame->setToolTip(tr("Export Frame (Ctrl+Shift+E)"));
    rt::UiScale::setScaledFixedSize(m_btnExportFrame, 28, 22);
    m_btnExportFrame->setFocusPolicy(Qt::NoFocus);
    m_btnExportFrame->setStyleSheet(exportBtnStyle);
    controlLayout->addWidget(m_btnExportFrame, 0, Qt::AlignVCenter);
    connect(m_btnExportFrame, &QPushButton::clicked, this, [this]() {
        // Source monitor export = screenshot viewport
        // (no exportFrameRequested signal on SourceMonitor, so do it inline)
    });

    // Zoom percentage label (hidden — matches Premiere layout)
    m_zoomLabel = new QLabel(QStringLiteral("100%"), this);
    m_zoomLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 11px; "
        "color: %1; padding: 0 4px; background: transparent; }")
        .arg(Theme::hex(Theme::colors().textSecondary))));
    rt::UiScale::setScaledMinimumWidth(m_zoomLabel, 60);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomLabel->hide();

    connect(m_viewport, &Viewport::viewZoomChanged, this, [this](float zoom) {
        m_zoomLabel->setText(QString::number(static_cast<int>(std::round(zoom * 100))) + QStringLiteral("%"));
    });

    // Duration timecode display (right side)
    m_durationLabel = new QLabel(QStringLiteral("00:00:00:00"), this);
    m_durationLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 14px; "
        "color: %1; background: transparent; padding: 0px 8px 0px 6px; }")
        .arg(Theme::hex(Theme::colors().textSecondary))));
    rt::UiScale::setScaledFixedWidth(m_durationLabel, 120);
    m_durationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    controlLayout->addWidget(m_durationLabel, 0, Qt::AlignVCenter);

    mainLayout->addWidget(controlBar);
    mainLayout->addSpacing(rt::UiScale::px(4));   // gap between control bar and mini-timeline

    // Give the mini-timeline a distinct background
    m_miniTimeline->setMinimumHeight(rt::UiScale::px(56));
    mainLayout->addWidget(m_miniTimeline);

    // ── Transport controls (Premiere Pro style — matches Program Monitor) ──
    auto* transportBar = new QWidget(this);
    transportBar->setObjectName(QStringLiteral("TransportBar"));
    rt::UiScale::setScaledFixedHeight(transportBar, 36);
    transportBar->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "#TransportBar { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface0))
        .arg(Theme::hex(Theme::colors().border))));

    auto* transportLayout = new QHBoxLayout(transportBar);
    transportLayout->setContentsMargins(rt::UiScale::px(8), 0, rt::UiScale::px(8), 0);
    transportLayout->setSpacing(rt::UiScale::px(4));
    transportLayout->addStretch();

    m_btnGoStart     = new TransportButton(TransportButton::GoStart, transportBar);
    m_btnStepBack    = new TransportButton(TransportButton::StepBack, transportBar);
    m_btnPlayPause   = new TransportButton(TransportButton::Play, transportBar);
    m_btnStop        = new TransportButton(TransportButton::Stop, transportBar);
    m_btnStepForward = new TransportButton(TransportButton::StepForward, transportBar);
    m_btnGoEnd       = new TransportButton(TransportButton::GoEnd, transportBar);

    rt::UiScale::setScaledFixedSize(m_btnGoStart, 22, 22);
    m_btnGoStart->setToolTip(tr("Go to Start"));
    rt::UiScale::setScaledFixedSize(m_btnStepBack, 22, 22);
    m_btnStepBack->setToolTip(tr("Step Back"));
    rt::UiScale::setScaledFixedSize(m_btnPlayPause, 26, 26);
    m_btnPlayPause->setToolTip(tr("Play/Pause"));
    rt::UiScale::setScaledFixedSize(m_btnStop, 22, 22);
    m_btnStop->setToolTip(tr("Stop"));
    rt::UiScale::setScaledFixedSize(m_btnStepForward, 22, 22);
    m_btnStepForward->setToolTip(tr("Step Forward"));
    rt::UiScale::setScaledFixedSize(m_btnGoEnd, 22, 22);
    m_btnGoEnd->setToolTip(tr("Go to End"));

    transportLayout->addWidget(m_btnGoStart, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnStepBack, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnPlayPause, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnStop, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnStepForward, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnGoEnd, 0, Qt::AlignVCenter);

    // Vertical divider + screenshot button
    auto* transportDivider = new QFrame(transportBar);
    transportDivider->setFrameShape(QFrame::VLine);
    rt::UiScale::setScaledFixedHeight(transportDivider, 16);
    transportDivider->setStyleSheet(QStringLiteral(
        "QFrame { color: %1; }").arg(Theme::hex(Theme::colors().border)));
    transportLayout->addSpacing(rt::UiScale::px(4));
    transportLayout->addWidget(transportDivider, 0, Qt::AlignVCenter);
    transportLayout->addSpacing(rt::UiScale::px(4));

    m_btnScreenshot = new TransportButton(TransportButton::Screenshot, transportBar);
    m_btnScreenshot->setToolTip(tr("Take Screenshot"));
    rt::UiScale::setScaledFixedSize(m_btnScreenshot, 22, 22);
    transportLayout->addWidget(m_btnScreenshot, 0, Qt::AlignVCenter);
    connect(m_btnScreenshot, &QPushButton::clicked, this, [this]() {
        // Screenshot from source monitor
    });

    // Loop toggle button
    auto loopBtnStyle = rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; "
        "border-radius: 3px; color: %2; font-size: 10px; font-weight: bold; "
        "padding: 2px 6px; } "
        "QPushButton:hover { background: %3; } "
        "QPushButton:checked { background: %4; color: %5; border-color: %4; }")
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().accent))
        .arg(Theme::hex(Theme::colors().textBright)));

    auto* loopDivider = new QFrame(transportBar);
    loopDivider->setFrameShape(QFrame::VLine);
    rt::UiScale::setScaledFixedHeight(loopDivider, 16);
    loopDivider->setStyleSheet(QStringLiteral(
        "QFrame { color: %1; }").arg(Theme::hex(Theme::colors().border)));
    transportLayout->addSpacing(rt::UiScale::px(4));
    transportLayout->addWidget(loopDivider, 0, Qt::AlignVCenter);
    transportLayout->addSpacing(rt::UiScale::px(4));

    m_btnLoop = new QPushButton(tr("Loop"), transportBar);
    m_btnLoop->setCheckable(true);
    rt::UiScale::setScaledFixedHeight(m_btnLoop, 22);
    m_btnLoop->setFocusPolicy(Qt::NoFocus);
    m_btnLoop->setToolTip(tr("Toggle Loop Playback"));
    m_btnLoop->setStyleSheet(loopBtnStyle);
    transportLayout->addWidget(m_btnLoop, 0, Qt::AlignVCenter);
    connect(m_btnLoop, &QPushButton::toggled, this, [this](bool checked) {
        if (m_controller) m_controller->setLoopEnabled(checked);
    });

    // Shuttle speed display label
    m_shuttleSpeedLabel = new QLabel(transportBar);
    m_shuttleSpeedLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 12px; "
        "font-weight: bold; color: #FFD700; background: transparent; "
        "padding: 0px 8px; }")));
    rt::UiScale::setScaledMinimumWidth(m_shuttleSpeedLabel, 50);
    m_shuttleSpeedLabel->setAlignment(Qt::AlignCenter);
    m_shuttleSpeedLabel->hide();
    transportLayout->addWidget(m_shuttleSpeedLabel, 0, Qt::AlignVCenter);

    transportLayout->addStretch();

    // Connect transport buttons
    connect(m_btnGoStart, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->goToStart(); updateFrameDisplay(); }
    });
    connect(m_btnStepBack, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->stepBackward(); updateFrameDisplay(); }
    });
    connect(m_btnPlayPause, &QPushButton::clicked, this, [this]() {
        if (m_hasClip)
            m_controller->togglePlayPause();
    });
    connect(m_btnStop, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->stop(); updateFrameDisplay(); }
    });
    connect(m_btnStepForward, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->stepForward(); updateFrameDisplay(); }
    });
    connect(m_btnGoEnd, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->goToEnd(); updateFrameDisplay(); }
    });

    mainLayout->addWidget(transportBar);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Media loading
// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
