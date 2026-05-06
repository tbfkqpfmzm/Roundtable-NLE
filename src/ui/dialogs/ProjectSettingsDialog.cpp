/*
 * ProjectSettingsDialog.cpp — Per-project settings dialog implementation.
 */

#include "ProjectSettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "project/Settings.h"

namespace rt {

ProjectSettingsDialog::ProjectSettingsDialog(Settings& settings, QWidget* parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(tr("Project Settings"));
    setMinimumWidth(400);

    auto* mainLayout = new QVBoxLayout(this);

    // ── Video settings ──────────────────────────────────────────────────
    auto* videoGroup = new QGroupBox(tr("Video"), this);
    auto* videoForm = new QFormLayout(videoGroup);

    m_widthSpin = new QSpinBox(this);
    m_widthSpin->setRange(64, 7680);
    m_widthSpin->setValue(static_cast<int>(settings.resolution().width));
    videoForm->addRow(tr("Width:"), m_widthSpin);

    m_heightSpin = new QSpinBox(this);
    m_heightSpin->setRange(36, 4320);
    m_heightSpin->setValue(static_cast<int>(settings.resolution().height));
    videoForm->addRow(tr("Height:"), m_heightSpin);

    m_fpsSpin = new QDoubleSpinBox(this);
    m_fpsSpin->setRange(1.0, 240.0);
    m_fpsSpin->setDecimals(3);
    m_fpsSpin->setValue(settings.frameRate());
    videoForm->addRow(tr("Frame Rate:"), m_fpsSpin);

    // Resolution presets
    auto* presetCombo = new QComboBox(this);
    presetCombo->addItem(tr("Custom"));
    presetCombo->addItem(tr("1920x1080 (Full HD)"));
    presetCombo->addItem(tr("3840x2160 (4K UHD)"));
    presetCombo->addItem(tr("1280x720 (720p)"));
    presetCombo->addItem(tr("2560x1440 (1440p)"));
    presetCombo->addItem(tr("1080x1920 (Vertical HD)"));
    videoForm->addRow(tr("Preset:"), presetCombo);

    connect(presetCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        struct Preset { int w; int h; };
        static const Preset presets[] = {
            {0, 0}, {1920, 1080}, {3840, 2160}, {1280, 720}, {2560, 1440}, {1080, 1920}
        };
        if (idx > 0 && idx < static_cast<int>(std::size(presets))) {
            m_widthSpin->setValue(presets[idx].w);
            m_heightSpin->setValue(presets[idx].h);
        }
    });

    mainLayout->addWidget(videoGroup);

    // ── Audio settings ──────────────────────────────────────────────────
    auto* audioGroup = new QGroupBox(tr("Audio"), this);
    auto* audioForm = new QFormLayout(audioGroup);

    m_sampleRateCombo = new QComboBox(this);
    m_sampleRateCombo->addItem(tr("44100 Hz"), 44100);
    m_sampleRateCombo->addItem(tr("48000 Hz"), 48000);
    m_sampleRateCombo->addItem(tr("96000 Hz"), 96000);
    int srIdx = m_sampleRateCombo->findData(static_cast<int>(settings.sampleRate()));
    if (srIdx >= 0) m_sampleRateCombo->setCurrentIndex(srIdx);
    audioForm->addRow(tr("Sample Rate:"), m_sampleRateCombo);

    m_channelsCombo = new QComboBox(this);
    m_channelsCombo->addItem(tr("Mono"), 1);
    m_channelsCombo->addItem(tr("Stereo"), 2);
    m_channelsCombo->addItem(tr("5.1 Surround"), 6);
    int chIdx = m_channelsCombo->findData(static_cast<int>(settings.audioChannels()));
    if (chIdx >= 0) m_channelsCombo->setCurrentIndex(chIdx);
    audioForm->addRow(tr("Channels:"), m_channelsCombo);

    mainLayout->addWidget(audioGroup);

    // ── Button box ──────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        applySettings();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ProjectSettingsDialog::applySettings()
{
    m_settings.setResolution(
        static_cast<uint32_t>(m_widthSpin->value()),
        static_cast<uint32_t>(m_heightSpin->value()));
    m_settings.setFrameRate(m_fpsSpin->value());

    AudioFormat af = m_settings.audioFormat();
    af.sampleRate = static_cast<uint32_t>(m_sampleRateCombo->currentData().toInt());
    af.channels   = static_cast<uint32_t>(m_channelsCombo->currentData().toInt());
    m_settings.setAudioFormat(af);
}

} // namespace rt
