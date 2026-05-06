/*
 * ProjectSettingsDialog — Dialog for editing per-project settings
 * (resolution, frame rate, audio format).
 */

#pragma once

#include <QDialog>

class QComboBox;
class QSpinBox;
class QDoubleSpinBox;

namespace rt {

class Settings;

class ProjectSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProjectSettingsDialog(Settings& settings, QWidget* parent = nullptr);

private:
    void applySettings();

    Settings& m_settings;

    QSpinBox*       m_widthSpin{nullptr};
    QSpinBox*       m_heightSpin{nullptr};
    QDoubleSpinBox* m_fpsSpin{nullptr};
    QComboBox*      m_sampleRateCombo{nullptr};
    QComboBox*      m_channelsCombo{nullptr};
};

} // namespace rt
