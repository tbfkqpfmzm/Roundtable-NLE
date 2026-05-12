/*
 * NLEScrollBar.cpp — Premiere-style zoom/scroll bar.
 * Step 12: Timeline Panel — Core UI
 */

#include "widgets/NLEScrollBar.h"
#include "Theme.h"

#include "timeline/TimelineLayoutEngine.h"

#include <QPainter>
#include <QMouseEvent>

#include <algorithm>

namespace rt {

NLEScrollBar::NLEScrollBar(QWidget* parent)
    : QWidget(parent)
{
    const auto& tc = Theme::colors();
    m_trackColor      = tc.surface1;
    m_handleColor     = tc.surface3;
    m_handleHoverColor = tc.textSecondary;

    setFixedHeight(kBarHeight);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
}

void NLEScrollBar::setLayoutEngine(TimelineLayoutEngine* engine)
{
    m_engine = engine;
    update();
}

QSize NLEScrollBar::sizeHint() const
{
    return {400, kBarHeight};
}

QSize NLEScrollBar::minimumSizeHint() const
{
    return {100, kBarHeight};
}

void NLEScrollBar::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    int h = height();

    // Track background
    painter.fillRect(rect(), m_trackColor);

    if (!m_engine) return;

    auto state = m_engine->computeScrollbar();

    int handleLeft  = normalizedToPixel(state.handleStart);
    int handleRight = normalizedToPixel(state.handleEnd);

    // Handle body
    QRect handleRect(handleLeft, 1, handleRight - handleLeft, h - 2);
    painter.fillRect(handleRect, m_handleColor);

    // Left grip
    QRect leftGrip(handleLeft, 1, kHandleWidth, h - 2);
    painter.fillRect(leftGrip, m_handleHoverColor);

    // Right grip
    QRect rightGrip(handleRight - kHandleWidth, 1, kHandleWidth, h - 2);
    painter.fillRect(rightGrip, m_handleHoverColor);

    // Grip lines
    painter.setPen(Theme::colors().border);
    int midY = h / 2;
    // Left grip lines
    painter.drawLine(handleLeft + 3, midY - 3, handleLeft + 3, midY + 3);
    painter.drawLine(handleLeft + 5, midY - 3, handleLeft + 5, midY + 3);
    // Right grip lines
    painter.drawLine(handleRight - 4, midY - 3, handleRight - 4, midY + 3);
    painter.drawLine(handleRight - 6, midY - 3, handleRight - 6, midY + 3);
    --s_paintDepth;
}

void NLEScrollBar::mousePressEvent(QMouseEvent* event)
{
    if (!m_engine || event->button() != Qt::LeftButton) return;

    m_dragStartX = event->pos().x();
    auto state = m_engine->computeScrollbar();
    m_dragStartHandleStart = state.handleStart;
    m_dragStartHandleEnd = state.handleEnd;
    m_dragMode = hitTest(event->pos().x());

    if (m_dragMode == DragMode::None)
    {
        // Click outside handle — jump to position
        double norm = pixelToNormalized(event->pos().x());
        double handleWidth = state.handleEnd - state.handleStart;
        double newStart = norm - handleWidth * 0.5;
        newStart = std::clamp(newStart, 0.0, 1.0 - handleWidth);
        m_engine->applyScrollbarDrag(newStart);
        m_dragMode = DragMode::Body;
        m_dragStartX = event->pos().x();
        m_dragStartHandleStart = newStart;
        m_dragStartHandleEnd = newStart + handleWidth;
        emit scrollChanged();
        update();
    }
}

void NLEScrollBar::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_engine) return;

    if (m_dragMode == DragMode::None)
    {
        // Update cursor
        auto mode = hitTest(event->pos().x());
        switch (mode)
        {
            case DragMode::LeftHandle:
            case DragMode::RightHandle:
                setCursor(Qt::SizeHorCursor);
                break;
            case DragMode::Body:
                setCursor(Qt::OpenHandCursor);
                break;
            default:
                setCursor(Qt::ArrowCursor);
                break;
        }
        return;
    }

    int deltaX = event->pos().x() - m_dragStartX;
    double deltaNorm = static_cast<double>(deltaX) / width();

    switch (m_dragMode)
    {
        case DragMode::Body:
        {
            double handleWidth = m_dragStartHandleEnd - m_dragStartHandleStart;
            double newStart = std::clamp(m_dragStartHandleStart + deltaNorm,
                                         0.0, 1.0 - handleWidth);
            m_engine->applyScrollbarDrag(newStart);
            break;
        }
        case DragMode::LeftHandle:
        {
            double newStart = std::clamp(m_dragStartHandleStart + deltaNorm,
                                         0.0, m_dragStartHandleEnd - 0.02);
            m_engine->applyScrollbarResize(newStart, m_dragStartHandleEnd);
            break;
        }
        case DragMode::RightHandle:
        {
            double newEnd = std::clamp(m_dragStartHandleEnd + deltaNorm,
                                       m_dragStartHandleStart + 0.02, 1.0);
            m_engine->applyScrollbarResize(m_dragStartHandleStart, newEnd);
            break;
        }
        default:
            break;
    }

    emit scrollChanged();
    update();
}

void NLEScrollBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_dragMode = DragMode::None;
        setCursor(Qt::ArrowCursor);
    }
}

int NLEScrollBar::normalizedToPixel(double norm) const
{
    return static_cast<int>(norm * width());
}

double NLEScrollBar::pixelToNormalized(int px) const
{
    return static_cast<double>(px) / std::max(width(), 1);
}

NLEScrollBar::DragMode NLEScrollBar::hitTest(int x) const
{
    if (!m_engine) return DragMode::None;

    auto state = m_engine->computeScrollbar();
    int handleLeft  = normalizedToPixel(state.handleStart);
    int handleRight = normalizedToPixel(state.handleEnd);

    if (x >= handleLeft && x < handleLeft + kHandleWidth)
        return DragMode::LeftHandle;
    if (x > handleRight - kHandleWidth && x <= handleRight)
        return DragMode::RightHandle;
    if (x >= handleLeft && x <= handleRight)
        return DragMode::Body;

    return DragMode::None;
}

} // namespace rt
