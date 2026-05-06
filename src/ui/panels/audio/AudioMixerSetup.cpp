/*
 * AudioMixerSetup.cpp - Mixer panel construction and dependency wiring.
 */

#include "AudioMixer.h"

#include "Theme.h"
#include "media/AudioEngine.h"
#include "timeline/Timeline.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace rt {

AudioMixer::AudioMixer(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

AudioMixer::~AudioMixer() = default;

QSize AudioMixer::sizeHint() const { return {520, 440}; }

void AudioMixer::setTimeline(Timeline* timeline)
{
    m_timeline = timeline;
    rebuildStrips();
}

void AudioMixer::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;

    if (engine && !m_meterTimer) {
        m_meterTimer = new QTimer(this);
        connect(m_meterTimer, &QTimer::timeout, this, &AudioMixer::updateMeters);
        m_meterTimer->start(kMeterRefreshMs);
    } else if (!engine && m_meterTimer) {
        m_meterTimer->stop();
        delete m_meterTimer;
        m_meterTimer = nullptr;
    }
}

void AudioMixer::setCommandStack(CommandStack* stack)
{
    m_commandStack = stack;
}

void AudioMixer::setupUI()
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    toolbarLayout->setSpacing(m.spacingXs);

    auto* titleLabel = new QLabel(tr("Audio Mixer"), toolbar);
    titleLabel->setStyleSheet(
        QString("QLabel { color: %1; font-weight: bold; font-size: 12px; "
                "background: transparent; border: none; }")
            .arg(Theme::hex(tc.textPrimary)));
    toolbarLayout->addWidget(titleLabel);
    toolbarLayout->addStretch();
    mainLayout->addWidget(toolbar);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName("MixerScroll");
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(
        QString("QScrollArea { background: %1; border: none; }")
            .arg(Theme::hex(tc.surface0)));

    m_stripContainer = new QWidget();
    m_stripContainer->setObjectName("StripContainer");
    m_stripContainer->setStyleSheet(
        QString("QWidget#StripContainer { background: %1; }")
            .arg(Theme::hex(tc.surface0)));
    m_stripLayout = new QHBoxLayout(m_stripContainer);
    m_stripLayout->setContentsMargins(m.spacingMd, m.spacingMd, m.spacingMd, m.spacingMd);
    m_stripLayout->setSpacing(m.spacingSm);
    m_stripLayout->addStretch();

    m_scrollArea->setWidget(m_stripContainer);
    mainLayout->addWidget(m_scrollArea);

    m_emptyLabel = new QLabel(tr("No audio tracks.\nAdd audio tracks to the timeline to mix."), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setObjectName("EmptyStateLabel");
    m_emptyLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 12px; background: transparent;")
        .arg(Theme::hex(tc.textDisabled)));
    m_emptyLabel->setVisible(true);
    m_scrollArea->setVisible(false);
    mainLayout->addWidget(m_emptyLabel, 1);

    auto* statusBar = new QWidget(this);
    statusBar->setFixedHeight(22);
    statusBar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(m.spacingMd, 0, m.spacingMd, 0);
    statusLayout->setSpacing(0);

    m_statusLabel = new QLabel(tr("0 tracks"), statusBar);
    m_statusLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 12px; background: transparent; border: none; }")
            .arg(Theme::hex(tc.textSecondary)));
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addStretch();
    mainLayout->addWidget(statusBar);

    setMinimumHeight(360);
}

} // namespace rt
