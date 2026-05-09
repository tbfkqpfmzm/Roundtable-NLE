/*
 * ConversionPanel — Shows conversion progress for pre-rendering
 *                   Spine character animations to VP9+alpha video cache.
 *
 * Displays a table of all downloaded characters with their conversion
 * status (cached count vs total animations) and allows the user to
 * manually trigger conversion for individual characters or all at once.
 *
 * Layout:
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │  🔄  Animation Conversion                                          │
 * │  Overall: [████████████░░░░] 42 / 98 animations cached             │
 * ├──────────────────────────────────────────────────────────────────────┤
 * │  Character    │ Outfit  │ Cached │ Total │ Status       │ Action    │
 * │──────────────┼─────────┼────────┼───────┼──────────────┼───────────│
 * │  Chime       │ default │   12   │  12   │ ✅ Complete  │           │
 * │  Crown       │ default │    8   │  12   │ ⏳ Partial   │ [Convert] │
 * │  Phantom     │ default │    0   │  14   │ ❌ None      │ [Convert] │
 * ├──────────────────────────────────────────────────────────────────────┤
 * │  [Convert All]  [Refresh]                                          │
 * └──────────────────────────────────────────────────────────────────────┘
 */

#pragma once

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>

namespace rt {

class AnimationVideoCache;
class ModelManager;

class ConversionPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ConversionPanel(QWidget* parent = nullptr);
    ~ConversionPanel() override;

    /// Set the animation video cache (non-const — we call queueRender).
    void setAnimVideoCache(AnimationVideoCache* cache);

    /// Set the model manager for enumerating characters/outfits.
    void setModelManager(ModelManager* mgr);

    /// Refresh the table with current conversion status.
    void refreshTable();

signals:
    void conversionsFinished();

private slots:
    void onConvertAllClicked();
    void onRefreshClicked();
    void onConvertCharacter(const QString& charName, const QString& outfit);
    void onCancelClicked();
    void onTableContextMenu(const QPoint& pos);
    void onTimerTick();

private:
    void setupUI();
    void updateOverallProgress();

    // Data sources
    AnimationVideoCache* m_animVideoCache{nullptr};
    ModelManager*        m_modelManager{nullptr};

    // UI
    QLabel*        m_titleLabel{nullptr};
    QLabel*        m_overallLabel{nullptr};
    QProgressBar*  m_overallProgress{nullptr};
    QLabel*        m_statusLabel{nullptr};
    QTreeWidget*   m_table{nullptr};
    QPushButton*   m_convertAllBtn{nullptr};
    QPushButton*   m_cancelBtn{nullptr};
    QPushButton*   m_refreshBtn{nullptr};
    QComboBox*     m_encoderCombo{nullptr};
    QPushButton*   m_colorSwatchBtn{nullptr};  ///< Opens color picker for chroma key colour
    QCheckBox*     m_hideConvertedCheck{nullptr};

    // Periodic refresh while rendering
    QTimer*        m_refreshTimer{nullptr};

    // Batch conversion tracking — tracks current Convert job progress
    size_t         m_batchTotal{0};
};

} // namespace rt
