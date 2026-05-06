/*
 * SequenceDialog — Dialog for creating a new sequence with custom settings
 * (name, resolution, frame rate).
 */

#pragma once

#include <QDialog>

class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;

namespace rt {

class SequenceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SequenceDialog(QWidget* parent = nullptr);

    /// Get the entered sequence name.
    QString sequenceName() const;

    /// Get the chosen width.
    uint32_t width() const;

    /// Get the chosen height.
    uint32_t height() const;

    /// Get the chosen frame rate.
    double frameRate() const;

    /// Pre-populate the dialog with known media dimensions/fps.
    void setMediaProperties(uint32_t mediaWidth, uint32_t mediaHeight, double mediaFps);

    /// Set the sequence name field.
    void setSequenceName(const QString& name);

private:
    void applyResPreset(int idx);

    QLineEdit*      m_nameEdit{nullptr};
    QSpinBox*       m_widthSpin{nullptr};
    QSpinBox*       m_heightSpin{nullptr};
    QDoubleSpinBox* m_fpsSpin{nullptr};
    QComboBox*      m_presetCombo{nullptr};
};

} // namespace rt
