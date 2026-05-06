/*
 * TransportButton.h — Custom-painted transport control button.
 *
 * Renders transport icons (play, pause, stop, step, go-to-start/end)
 * using QPainter geometric shapes instead of Unicode symbols, ensuring
 * reliable rendering regardless of installed fonts.
 *
 * Usage:
 *   auto* btn = new TransportButton(TransportButton::Play, parent);
 *   btn->setFixedSize(28, 28);
 */

#pragma once

#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPushButton>

namespace rt {

class TransportButton : public QPushButton
{
public:
    enum Icon { GoStart, StepBack, Play, Pause, Stop, StepForward, GoEnd, Screenshot };

    explicit TransportButton(Icon icon, QWidget* parent = nullptr)
        : QPushButton(parent), m_icon(icon)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; }"));
    }

    void setTransportIcon(Icon icon) { m_icon = icon; update(); }
    [[nodiscard]] Icon transportIcon() const { return m_icon; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const auto& tc = Theme::colors();

        // Hover / pressed background
        if (isDown()) {
            p.setPen(Qt::NoPen);
            p.setBrush(tc.controlBgActive);
            p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
        } else if (underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(tc.controlBgHover);
            p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
        }

        // Icon color
        QColor color = underMouse() ? tc.textBright : tc.textSecondary;
        p.setPen(Qt::NoPen);
        p.setBrush(color);

        qreal cx = width() * 0.5;
        qreal cy = height() * 0.5;
        qreal s  = qMin(width(), height()) * 0.28;

        switch (m_icon) {
        case GoStart:   drawGoStart(p, cx, cy, s);   break;
        case StepBack:  drawStepBack(p, cx, cy, s);  break;
        case Play:      drawPlay(p, cx, cy, s);      break;
        case Pause:     drawPause(p, cx, cy, s);     break;
        case Stop:      drawStop(p, cx, cy, s);      break;
        case StepForward: drawStepFwd(p, cx, cy, s); break;
        case GoEnd:       drawGoEnd(p, cx, cy, s);       break;
        case Screenshot:  drawScreenshot(p, cx, cy, s);  break;
        }
    }

private:
    // |◀  — vertical bar + left-pointing triangle
    void drawGoStart(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        p.drawRect(QRectF(cx - s - 1, cy - s, 2, s * 2));
        QPainterPath tri;
        tri.moveTo(cx + s,     cy - s);
        tri.lineTo(cx - s + 2, cy);
        tri.lineTo(cx + s,     cy + s);
        tri.closeSubpath();
        p.drawPath(tri);
    }

    // ◀◀  — two left-pointing triangles
    void drawStepBack(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        QPainterPath t1;
        t1.moveTo(cx,     cy - s);
        t1.lineTo(cx - s, cy);
        t1.lineTo(cx,     cy + s);
        t1.closeSubpath();
        p.drawPath(t1);

        QPainterPath t2;
        t2.moveTo(cx + s, cy - s);
        t2.lineTo(cx,     cy);
        t2.lineTo(cx + s, cy + s);
        t2.closeSubpath();
        p.drawPath(t2);
    }

    // ▶  — right-pointing triangle
    void drawPlay(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        QPainterPath tri;
        tri.moveTo(cx - s * 0.65, cy - s);
        tri.lineTo(cx + s,        cy);
        tri.lineTo(cx - s * 0.65, cy + s);
        tri.closeSubpath();
        p.drawPath(tri);
    }

    // ⏸ — two vertical bars
    void drawPause(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        qreal bw = s * 0.35;
        qreal gap = s * 0.2;
        p.drawRect(QRectF(cx - gap - bw, cy - s, bw, s * 2));
        p.drawRect(QRectF(cx + gap,      cy - s, bw, s * 2));
    }

    // ■ — filled square
    void drawStop(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        qreal half = s * 0.8;
        p.drawRect(QRectF(cx - half, cy - half, half * 2, half * 2));
    }

    // ▶▶ — two right-pointing triangles
    void drawStepFwd(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        QPainterPath t1;
        t1.moveTo(cx - s, cy - s);
        t1.lineTo(cx,     cy);
        t1.lineTo(cx - s, cy + s);
        t1.closeSubpath();
        p.drawPath(t1);

        QPainterPath t2;
        t2.moveTo(cx,     cy - s);
        t2.lineTo(cx + s, cy);
        t2.lineTo(cx,     cy + s);
        t2.closeSubpath();
        p.drawPath(t2);
    }

    // ▶| — right-pointing triangle + vertical bar
    void drawGoEnd(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        QPainterPath tri;
        tri.moveTo(cx - s,     cy - s);
        tri.lineTo(cx + s - 2, cy);
        tri.lineTo(cx - s,     cy + s);
        tri.closeSubpath();
        p.drawPath(tri);
        p.drawRect(QRectF(cx + s - 1, cy - s, 2, s * 2));
    }

    // 📷 — camera icon (rounded rect body + circle lens)
    void drawScreenshot(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        // Camera body — rounded rectangle
        qreal bw = s * 1.8;
        qreal bh = s * 1.3;
        p.drawRoundedRect(QRectF(cx - bw / 2, cy - bh / 2 + s * 0.15, bw, bh), 2, 2);
        // Lens — circle in center
        qreal r = s * 0.42;
        p.setBrush(Qt::NoBrush);
        QColor color = underMouse() ? Theme::colors().textBright : Theme::colors().textSecondary;
        p.setPen(QPen(color, 1.5));
        p.drawEllipse(QPointF(cx, cy + s * 0.15), r, r);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        // Viewfinder bump on top
        qreal vw = s * 0.6;
        qreal vh = s * 0.3;
        p.drawRect(QRectF(cx - vw / 2, cy - bh / 2 + s * 0.15 - vh, vw, vh));
    }

    Icon m_icon{Play};
};

} // namespace rt
