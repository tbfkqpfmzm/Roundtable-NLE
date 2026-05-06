/*
 * NLEScrollBar — Premiere-style horizontal zoom/scroll bar.
 *
 * Step 12: A scrollbar with two draggable handles that control both
 * scroll position and zoom level. Dragging the body scrolls, dragging
 * the handles zooms in/out.
 */

#pragma once

#include <QWidget>
#include <QColor>

namespace rt {

class TimelineLayoutEngine;

/// Premiere-style NLE scrollbar with zoom handles.
class NLEScrollBar : public QWidget
{
    Q_OBJECT

public:
    explicit NLEScrollBar(QWidget* parent = nullptr);
    ~NLEScrollBar() override = default;

    /// Set the layout engine (non-owning).
    void setLayoutEngine(TimelineLayoutEngine* engine);

    /// Bar height constant.
    static constexpr int kBarHeight = 16;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// Emitted when scrollbar state changes (user dragged/resized).
    void scrollChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    enum class DragMode { None, Body, LeftHandle, RightHandle };

    TimelineLayoutEngine* m_engine{nullptr};
    DragMode m_dragMode{DragMode::None};
    int      m_dragStartX{0};
    double   m_dragStartHandleStart{0};
    double   m_dragStartHandleEnd{0};

    static constexpr int kHandleWidth = 8;

    QColor m_trackColor;
    QColor m_handleColor;
    QColor m_handleHoverColor;

    /// Convert normalized [0,1] position to widget pixel X.
    int normalizedToPixel(double norm) const;

    /// Convert widget pixel X to normalized [0,1] position.
    double pixelToNormalized(int px) const;

    /// Determine what's under a pixel X.
    DragMode hitTest(int x) const;
};

} // namespace rt
