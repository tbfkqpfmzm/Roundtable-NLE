/*
 * SequenceDialog — Step-card sequence creation dialog.
 * Matches Projects→NEW tab style with AR → Resolution → FPS.
 */

#pragma once

#include <QDialog>
#include <vector>
#include <utility>

class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;
class QLabel;
class QButtonGroup;
class QWidget;

namespace rt {

class SequenceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SequenceDialog(QWidget* parent = nullptr);

    QString sequenceName() const;
    uint32_t width() const;
    uint32_t height() const;
    double   frameRate() const;

    void setMediaProperties(uint32_t mediaWidth, uint32_t mediaHeight, double mediaFps);
    void setSequenceName(const QString& name);

    /// Change the accept (OK) button text (e.g. "Create Sequence" → "Save Settings").
    void setAcceptButtonText(const QString& text);

private:
    void onArClicked(int id);
    void onResClicked(int id);
    void onFpsClicked(int id);
    void rebuildResGrid();
    void updatePreview();

    QLineEdit*      m_nameEdit{nullptr};
    QSpinBox*       m_widthSpin{nullptr};
    QSpinBox*       m_heightSpin{nullptr};
    QDoubleSpinBox* m_fpsSpin{nullptr};
    QLabel*         m_previewLabel{nullptr};

    // Aspect ratio
    QButtonGroup*   m_arGroup{nullptr};
    QSpinBox*       m_customArW{nullptr};
    QSpinBox*       m_customArH{nullptr};
    QWidget*        m_customArRow{nullptr};

    // Resolution
    QButtonGroup*   m_resGroup{nullptr};
    QWidget*        m_resGridWidget{nullptr};
    QWidget*        m_customResRow{nullptr};
    /// (width, height) for each resolution preset button (indexed by button ID).
    std::vector<std::pair<int,int>> m_resPresets;

    // Frame rate
    QButtonGroup*   m_fpsGroup{nullptr};
    QWidget*        m_customFpsRow{nullptr};

    // Buttons
    QPushButton*    m_okBtn{nullptr};
};

} // namespace rt
