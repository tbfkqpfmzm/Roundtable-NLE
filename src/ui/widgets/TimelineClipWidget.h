/*
 * TimelineClipWidget — Visual style/rendering for a single clip.
 *
 * Step 12: Static helper for painting clip visuals. Not a standalone widget —
 * clips are painted by TimelineTrackWidget. This class provides shared
 * painting routines and clip color/style logic.
 */

#pragma once

#include <QColor>
#include <QPainter>
#include <QRectF>
#include <QString>

#include <cstdint>

namespace rt {

enum class ClipType : uint8_t;

/// Visual style for clip rendering (used by TimelineTrackWidget::paintClip).
struct ClipVisualStyle
{
    QColor fillColor;
    QColor borderColor;
    QColor textColor{Qt::white};
    QColor selectedBorderColor{255, 200, 50};
    float  borderRadius{3.0f};
    float  borderWidth{1.0f};
    float  selectedBorderWidth{2.0f};
    int    labelPadding{6};
    int    minWidthForLabel{30};
};

/// Static helpers for clip rendering.
class TimelineClipWidget
{
public:
    TimelineClipWidget() = delete; // All static

    /// Get default visual style for a clip type.
    static ClipVisualStyle defaultStyle(ClipType type);

    /// Get a display-friendly type name.
    static QString typeName(ClipType type);

    /// Get a display-friendly type icon character (Unicode).
    static QChar typeIcon(ClipType type);

    /// Compute clip label: "[icon] label" or just "[icon]" if label is empty.
    static QString displayLabel(ClipType type, const QString& label);

    /// Paint a clip rectangle into a painter.
    /// @param painter Must already be set up with correct transform.
    /// @param rect    The clip rectangle in painter coordinates.
    /// @param style   Visual style.
    /// @param label   Display label.
    /// @param selected Whether this clip is selected.
    /// @param enabled  Whether this clip is enabled.
    static void paint(QPainter& painter,
                      const QRectF& rect,
                      const ClipVisualStyle& style,
                      const QString& label,
                      bool selected,
                      bool enabled);
};

} // namespace rt
