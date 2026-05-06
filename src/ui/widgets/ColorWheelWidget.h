/*
 * ColorWheelWidget — Premiere Pro–style color wheel control.
 *
 * Circular color wheel with a draggable center dot for selecting
 * hue (angle) and saturation (distance from center). Includes a
 * vertical slider on the right for luminance/master offset.
 *
 * Used for Lift/Gamma/Gain controls in the Lumetri Color Wheels section.
 */

#pragma once

#include <QWidget>
#include <QColor>
#include <QImage>
#include <QPointF>

namespace rt {

class ColorWheelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ColorWheelWidget(QWidget* parent = nullptr);
    ~ColorWheelWidget() override = default;

    /// Set the color offset (r, g, b each in [-1, 1]).
    void setOffset(float r, float g, float b);

    /// Get current offsets.
    [[nodiscard]] float offsetR() const noexcept { return m_offsetR; }
    [[nodiscard]] float offsetG() const noexcept { return m_offsetG; }
    [[nodiscard]] float offsetB() const noexcept { return m_offsetB; }

    /// Set the master (luminance) slider value [-1, 1].
    void setMaster(float val);
    [[nodiscard]] float master() const noexcept { return m_master; }

    /// Label displayed below the wheel (e.g. "Shadows", "Midtones", "Highlights").
    void setLabel(const QString& label);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// Emitted during drag with current offsets.
    void offsetChanged(float r, float g, float b);

    /// Emitted when drag finishes.
    void offsetCommitted(float oldR, float oldG, float oldB,
                         float newR, float newG, float newB);

    /// Emitted during master slider drag.
    void masterChanged(float value);

    /// Emitted when master slider drag finishes.
    void masterCommitted(float oldVal, float newVal);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildWheelImage();
    QPointF offsetToPos() const;
    void posToOffset(const QPointF& pos);
    QRectF wheelRect() const;
    QRectF masterSliderRect() const;

    QImage  m_wheelImage;
    QString m_label;

    float m_offsetR{0.0f};
    float m_offsetG{0.0f};
    float m_offsetB{0.0f};
    float m_master{0.0f};

    // Drag state
    bool  m_draggingWheel{false};
    bool  m_draggingMaster{false};
    float m_dragStartR{0.0f};
    float m_dragStartG{0.0f};
    float m_dragStartB{0.0f};
    float m_dragStartMaster{0.0f};

    bool m_wheelDirty{true};
};

} // namespace rt
