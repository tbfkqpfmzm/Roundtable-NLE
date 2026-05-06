/*
 * SequenceDialog.cpp — Implementation of the new sequence creation dialog.
 */

#include "SequenceDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace rt {

SequenceDialog::SequenceDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New Sequence"));
    setMinimumWidth(420);
    setModal(true);

    auto* mainLayout = new QVBoxLayout(this);

    // ── Sequence Name ───────────────────────────────────────────────────
    auto* nameGroup = new QGroupBox(tr("Sequence"), this);
    auto* nameForm  = new QFormLayout(nameGroup);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("Sequence 1"));
    m_nameEdit->setText(tr("Sequence 1"));
    nameForm->addRow(tr("Name:"), m_nameEdit);

    mainLayout->addWidget(nameGroup);

    // ── Video settings ──────────────────────────────────────────────────
    auto* videoGroup = new QGroupBox(tr("Video"), this);
    auto* videoForm  = new QFormLayout(videoGroup);

    m_widthSpin = new QSpinBox(this);
    m_widthSpin->setRange(64, 7680);
    m_widthSpin->setValue(1920);
    videoForm->addRow(tr("Width:"), m_widthSpin);

    m_heightSpin = new QSpinBox(this);
    m_heightSpin->setRange(36, 4320);
    m_heightSpin->setValue(1080);
    videoForm->addRow(tr("Height:"), m_heightSpin);

    m_fpsSpin = new QDoubleSpinBox(this);
    m_fpsSpin->setRange(1.0, 240.0);
    m_fpsSpin->setDecimals(3);
    m_fpsSpin->setValue(30.0);
    videoForm->addRow(tr("Frame Rate:"), m_fpsSpin);

    // Resolution presets
    m_presetCombo = new QComboBox(this);
    m_presetCombo->addItem(tr("Custom"));
    m_presetCombo->addItem(tr("1920x1080 (Full HD)"));
    m_presetCombo->addItem(tr("3840x2160 (4K UHD)"));
    m_presetCombo->addItem(tr("1280x720 (720p)"));
    m_presetCombo->addItem(tr("2560x1440 (1440p)"));
    m_presetCombo->addItem(tr("1080x1920 (Vertical HD)"));
    m_presetCombo->addItem(tr("640x480 (SD)"));
    videoForm->addRow(tr("Preset:"), m_presetCombo);

    connect(m_presetCombo, &QComboBox::currentIndexChanged,
            this, &SequenceDialog::applyResPreset);

    mainLayout->addWidget(videoGroup);

    // ── Button box ──────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString SequenceDialog::sequenceName() const
{
    QString name = m_nameEdit->text().trimmed();
    return name.isEmpty() ? QStringLiteral("Sequence 1") : name;
}

uint32_t SequenceDialog::width() const
{
    return static_cast<uint32_t>(m_widthSpin->value());
}

uint32_t SequenceDialog::height() const
{
    return static_cast<uint32_t>(m_heightSpin->value());
}

double SequenceDialog::frameRate() const
{
    return m_fpsSpin->value();
}

void SequenceDialog::setMediaProperties(uint32_t mediaWidth, uint32_t mediaHeight, double mediaFps)
{
    if (mediaWidth > 0 && mediaHeight > 0) {
        m_widthSpin->setValue(static_cast<int>(mediaWidth));
        m_heightSpin->setValue(static_cast<int>(mediaHeight));
    }
    if (mediaFps > 0.0) {
        m_fpsSpin->setValue(mediaFps);
    }
    m_presetCombo->setCurrentIndex(0); // "Custom"
}

void SequenceDialog::setSequenceName(const QString& name)
{
    m_nameEdit->setText(name);
}

void SequenceDialog::applyResPreset(int idx)
{
    struct Preset { int w; int h; };
    static const Preset presets[] = {
        {0, 0}, {1920, 1080}, {3840, 2160}, {1280, 720},
        {2560, 1440}, {1080, 1920}, {640, 480}
    };
    if (idx > 0 && idx < static_cast<int>(std::size(presets))) {
        m_widthSpin->setValue(presets[idx].w);
        m_heightSpin->setValue(presets[idx].h);
    }
}

} // namespace rt
