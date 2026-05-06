/*
 * KeyframeEditorRender.cpp - Painting and event handling for KeyframeEditor.
 * Split from KeyframeEditor.cpp.
 */

#include "panels/effects/KeyframeEditor.h"
#include "Theme.h"

#include "Constants.h"
#include "timeline/Clip.h"
#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"
#include "command/CommandStack.h"
#include "command/commands/KeyframeCmds.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <limits>


namespace rt {

//  Constants
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

static constexpr int    kGridLineAlpha       = 40;
static constexpr int    kGridTextAlpha       = 140;
static constexpr int    kCurvePenWidth       = 2;
static constexpr double kKeyframeRadius      = 5.0;
static constexpr double kTangentRadius       = 4.0;
static constexpr double kTangentLineLen      = 50.0; // pixels
static constexpr double kZoomFactor          = 1.15;
static constexpr int    kBezierSubdivisions  = 64;  // segments per curve
static constexpr double kMinViewSpan         = 100.0; // minimum view span (ticks or value units)
static constexpr int    kMarginLeft          = 50;
static constexpr int    kMarginRight         = 10;
static constexpr int    kMarginTop           = 10;
static constexpr int    kMarginBottom        = 25;

// Curve colors for the 6 base clip properties + extras
static const QColor kCurveColors[] = {
    QColor(255, 100, 100),  // opacity    - red
    QColor(100, 200, 255),  // positionX  - blue
    QColor(100, 255, 100),  // positionY  - green
    QColor(255, 200, 100),  // scaleX     - orange
    QColor(255, 255, 100),  // scaleY     - yellow
    QColor(200, 100, 255),  // rotation   - purple
    QColor(255, 150, 200),  // extra 1
    QColor(150, 255, 200),  // extra 2
};
static constexpr int kNumCurveColors = sizeof(kCurveColors) / sizeof(kCurveColors[0]);

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Paint
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void KeyframeEditor::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto& tc = Theme::colors();

    // Background
    p.fillRect(rect(), tc.surface0);

    // Empty state: no clip selected
    if (!m_clip || m_curves.empty()) {
        p.setPen(tc.textDisabled);
        QFont f = p.font();
        f.setPixelSize(12);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter,
                   tr("Select a clip with keyframes to edit curves."));
        return;
    }

    drawGrid(p);

    // Draw curves in order
    for (int ci = 0; ci < static_cast<int>(m_curves.size()); ++ci) {
        if (!m_curves[ci].visible) continue;
        drawCurve(p, ci);
        drawBezierHandles(p, ci);
        drawKeyframeHandles(p, ci);
    }

    if (m_boxSelecting)
        drawBoxSelection(p);
}

void KeyframeEditor::drawGrid(QPainter& p)
{
    const auto& tc = Theme::colors();
    QColor gridColor = tc.textBright; gridColor.setAlpha(kGridLineAlpha);
    QColor textColor = tc.textPrimary; textColor.setAlpha(kGridTextAlpha);
    QPen gridPen(gridColor, 1);
    p.setPen(gridPen);

    double plotW = width()  - kMarginLeft - kMarginRight;
    double plotH = height() - kMarginTop  - kMarginBottom;
    if (plotW <= 0 || plotH <= 0) return;

    // Time grid Ã¢â‚¬â€ choose nice intervals
    double timeRange = m_viewTimeMax - m_viewTimeMin;
    double timeStep  = std::pow(10.0, std::floor(std::log10(timeRange / 4.0)));
    if (timeRange / timeStep > 8) timeStep *= 2;
    if (timeRange / timeStep < 3) timeStep *= 0.5;

    double t0 = std::floor(m_viewTimeMin / timeStep) * timeStep;
    for (double t = t0; t <= m_viewTimeMax; t += timeStep) {
        QPointF px = graphToPixel(t, 0);
        p.drawLine(QPointF(px.x(), kMarginTop),
                   QPointF(px.x(), height() - kMarginBottom));

        // Label (convert ticks to seconds)
        p.setPen(textColor);
        double sec = t / kTicksPerSecond;
        p.drawText(QPointF(px.x() - 15, height() - 5),
                   QString::number(sec, 'f', 2) + "s");
        p.setPen(gridPen);
    }

    // Value grid
    double valRange = m_viewValueMax - m_viewValueMin;
    double valStep  = std::pow(10.0, std::floor(std::log10(valRange / 4.0)));
    if (valRange / valStep > 8) valStep *= 2;
    if (valRange / valStep < 3) valStep *= 0.5;

    double v0 = std::floor(m_viewValueMin / valStep) * valStep;
    for (double v = v0; v <= m_viewValueMax; v += valStep) {
        QPointF px = graphToPixel(0, v);
        p.drawLine(QPointF(kMarginLeft, px.y()),
                   QPointF(width() - kMarginRight, px.y()));

        p.setPen(textColor);
        p.drawText(QPointF(2, px.y() + 4), QString::number(v, 'f', 2));
        p.setPen(gridPen);
    }

    // Zero lines (thicker)
    QColor zeroColor = tc.textBright; zeroColor.setAlpha(80);
    QPen zeroPen(zeroColor, 1);
    p.setPen(zeroPen);
    QPointF zeroX = graphToPixel(0, 0);
    if (zeroX.x() >= kMarginLeft && zeroX.x() <= width() - kMarginRight)
        p.drawLine(QPointF(zeroX.x(), kMarginTop),
                   QPointF(zeroX.x(), height() - kMarginBottom));
    if (zeroX.y() >= kMarginTop && zeroX.y() <= height() - kMarginBottom)
        p.drawLine(QPointF(kMarginLeft, zeroX.y()),
                   QPointF(width() - kMarginRight, zeroX.y()));
}

void KeyframeEditor::drawCurve(QPainter& p, int curveIdx)
{
    auto& ce = m_curves[curveIdx];
    if (!ce.track || ce.track->keyframeCount() == 0) return;

    QPen pen(ce.color, kCurvePenWidth);
    p.setPen(pen);

    auto& track = *ce.track;
    auto count = track.keyframeCount();

    if (count == 1) {
        // Single keyframe: draw horizontal line across view
        const auto& kf = track.keyframe(0);
        QPointF l = graphToPixel(m_viewTimeMin, kf.value);
        QPointF r = graphToPixel(m_viewTimeMax, kf.value);
        p.drawLine(l, r);
        return;
    }

    // Draw line before first keyframe (extend left)
    {
        const auto& kf0 = track.keyframe(0);
        QPointF from = graphToPixel(m_viewTimeMin, kf0.value);
        QPointF to   = graphToPixel(kf0.time, kf0.value);
        QPen dashPen(ce.color, 1, Qt::DashLine);
        p.setPen(dashPen);
        p.drawLine(from, to);
        p.setPen(pen);
    }

    // Draw each segment
    for (size_t i = 0; i + 1 < count; ++i) {
        const auto& kf0 = track.keyframe(i);
        const auto& kf1 = track.keyframe(i + 1);

        switch (kf0.interp) {
        case InterpMode::Hold: {
            QPointF a = graphToPixel(kf0.time, kf0.value);
            QPointF b = graphToPixel(kf1.time, kf0.value);
            QPointF c = graphToPixel(kf1.time, kf1.value);
            p.drawLine(a, b);
            p.drawLine(b, c);
            break;
        }
        case InterpMode::Linear: {
            QPointF a = graphToPixel(kf0.time, kf0.value);
            QPointF b = graphToPixel(kf1.time, kf1.value);
            p.drawLine(a, b);
            break;
        }
        case InterpMode::Bezier: {
            // Draw subdivided bezier curve
            QPainterPath path;
            QPointF first = graphToPixel(kf0.time, kf0.value);
            path.moveTo(first);

            double dt = static_cast<double>(kf1.time - kf0.time);
            double dv = static_cast<double>(kf1.value - kf0.value);

            for (int s = 1; s <= kBezierSubdivisions; ++s) {
                float t = static_cast<float>(s) / kBezierSubdivisions;
                float bt = solveBezierT(0.0f, kf0.bezierOutX, kf1.bezierInX, 1.0f, t);
                float progress = evalCubicBezier(0.0f, kf0.bezierOutY, kf1.bezierInY, 1.0f, bt);

                double time  = kf0.time + t * dt;
                double value = kf0.value + progress * dv;
                path.lineTo(graphToPixel(time, value));
            }
            p.drawPath(path);
            break;
        }
        }
    }

    // Draw line after last keyframe (extend right)
    {
        const auto& kfLast = track.keyframe(count - 1);
        QPointF from = graphToPixel(kfLast.time, kfLast.value);
        QPointF to   = graphToPixel(m_viewTimeMax, kfLast.value);
        QPen dashPen(ce.color, 1, Qt::DashLine);
        p.setPen(dashPen);
        p.drawLine(from, to);
        p.setPen(pen);
    }
}

void KeyframeEditor::drawKeyframeHandles(QPainter& p, int curveIdx)
{
    auto& ce = m_curves[curveIdx];
    if (!ce.track) return;
    auto& track = *ce.track;

    for (size_t i = 0; i < track.keyframeCount(); ++i) {
        const auto& kf = track.keyframe(i);
        QPointF px = graphToPixel(kf.time, kf.value);

        bool selected = m_selection.count({curveIdx, static_cast<int>(i)}) > 0;

        // Draw diamond for keyframe
        QPainterPath diamond;
        diamond.moveTo(px.x(), px.y() - kKeyframeRadius);
        diamond.lineTo(px.x() + kKeyframeRadius, px.y());
        diamond.lineTo(px.x(), px.y() + kKeyframeRadius);
        diamond.lineTo(px.x() - kKeyframeRadius, px.y());
        diamond.closeSubpath();

        if (selected) {
            p.fillPath(diamond, Theme::colors().textBright);
            p.setPen(QPen(ce.color, 2));
        } else {
            p.fillPath(diamond, ce.color);
            p.setPen(QPen(ce.color.darker(150), 1));
        }
        p.drawPath(diamond);
    }
}

void KeyframeEditor::drawBezierHandles(QPainter& p, int curveIdx)
{
    auto& ce = m_curves[curveIdx];
    if (!ce.track) return;
    auto& track = *ce.track;

    const auto& tc = Theme::colors();
    QPen handlePen(tc.textPrimary, 1);

    for (auto& sk : m_selection) {
        if (sk.curveIndex != curveIdx) continue;
        if (sk.keyIndex < 0 || sk.keyIndex >= static_cast<int>(track.keyframeCount())) continue;

        const auto& kf = track.keyframe(sk.keyIndex);

        if (kf.interp == InterpMode::Bezier && sk.keyIndex + 1 < static_cast<int>(track.keyframeCount())) {
            const auto& kfNext = track.keyframe(sk.keyIndex + 1);
            double dt = static_cast<double>(kfNext.time - kf.time);
            double dv = static_cast<double>(kfNext.value - kf.value);

            QPointF kfPx  = graphToPixel(kf.time, kf.value);
            QPointF tanPx = graphToPixel(
                kf.time + kf.bezierOutX * dt,
                kf.value + kf.bezierOutY * dv);

            p.setPen(handlePen);
            p.drawLine(kfPx, tanPx);
            p.setBrush(tc.warning);
            p.drawEllipse(tanPx, kTangentRadius, kTangentRadius);
        }

        // In tangent from previous keyframe
        if (sk.keyIndex > 0) {
            const auto& kfPrev = track.keyframe(sk.keyIndex - 1);
            if (kfPrev.interp == InterpMode::Bezier) {
                double dt = static_cast<double>(kf.time - kfPrev.time);
                double dv = static_cast<double>(kf.value - kfPrev.value);

                QPointF kfPx  = graphToPixel(kf.time, kf.value);
                QPointF tanPx = graphToPixel(
                    kfPrev.time + kfPrev.bezierInX * dt,
                    kfPrev.value + kfPrev.bezierInY * dv);

                p.setPen(handlePen);
                p.drawLine(kfPx, tanPx);
                p.setBrush(tc.accent);
                p.drawEllipse(tanPx, kTangentRadius, kTangentRadius);
            }
        }
    }
}

void KeyframeEditor::drawBoxSelection(QPainter& p)
{
    QRectF box(m_boxStart, m_boxCurrent);
    const auto& tc = Theme::colors();
    p.setPen(QPen(tc.accent, 1, Qt::DashLine));
    QColor boxFill = tc.accent; boxFill.setAlpha(30);
    p.setBrush(boxFill);
    p.drawRect(box.normalized());
}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Mouse events
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void KeyframeEditor::mousePressEvent(QMouseEvent* event)
{
    setFocus();

    if (event->button() == Qt::MiddleButton) {
        // Pan
        m_panStart = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    QPointF pos = event->position();
    auto hit = hitTest(pos);

    bool shift = event->modifiers() & Qt::ShiftModifier;

    if (hit.isTangentIn || hit.isTangentOut) {
        // Start tangent drag
        m_draggingTangent = true;
        m_tangentIsIn     = hit.isTangentIn;
        m_dragCurveIdx    = hit.curveIndex;
        m_dragKeyIdx      = hit.keyIndex;
        m_dragStartPos    = pos;
        event->accept();
        return;
    }

    if (hit.curveIndex >= 0) {
        // Clicked on a keyframe
        SelectedKey sk{hit.curveIndex, hit.keyIndex};
        if (!shift && m_selection.count(sk) == 0)
            m_selection.clear();
        m_selection.insert(sk);

        m_dragging     = true;
        m_dragCurveIdx = hit.curveIndex;
        m_dragKeyIdx   = hit.keyIndex;
        m_dragStartPos = pos;

        emit selectionChanged();
        update();
        event->accept();
        return;
    }

    // Start box selection
    if (!shift) m_selection.clear();
    m_boxSelecting = true;
    m_boxStart     = pos;
    m_boxCurrent   = pos;
    emit selectionChanged();
    update();
    event->accept();
}

void KeyframeEditor::mouseMoveEvent(QMouseEvent* event)
{
    QPointF pos = event->position();

    // Middle-button pan
    if (event->buttons() & Qt::MiddleButton) {
        QPointF delta = pos - m_panStart;
        double plotW = width()  - kMarginLeft - kMarginRight;
        double plotH = height() - kMarginTop  - kMarginBottom;
        if (plotW <= 0) plotW = 1;
        if (plotH <= 0) plotH = 1;

        double timeRange  = m_viewTimeMax  - m_viewTimeMin;
        double valueRange = m_viewValueMax - m_viewValueMin;

        double dtTime  = -delta.x() / plotW  * timeRange;
        double dtValue =  delta.y() / plotH  * valueRange;

        m_viewTimeMin  += dtTime;
        m_viewTimeMax  += dtTime;
        m_viewValueMin += dtValue;
        m_viewValueMax += dtValue;

        m_panStart = pos;
        emit viewChanged();
        update();
        event->accept();
        return;
    }

    // Tangent dragging
    if (m_draggingTangent) {
        if (m_dragCurveIdx < 0 || m_dragCurveIdx >= static_cast<int>(m_curves.size())) return;
        auto* track = m_curves[m_dragCurveIdx].track;
        if (!track || m_dragKeyIdx < 0 || m_dragKeyIdx >= static_cast<int>(track->keyframeCount()))
            return;

        auto& kf = track->keyframe(m_dragKeyIdx);
        QPointF gp = pixelToGraph(pos.x(), pos.y());

        if (m_tangentIsIn) {
            // In tangent Ã¢â‚¬â€ relates to next keyframe
            int nextIdx = m_dragKeyIdx + 1;
            if (nextIdx < static_cast<int>(track->keyframeCount())) {
                const auto& kfNext = track->keyframe(nextIdx);
                double dt = static_cast<double>(kfNext.time - kf.time);
                double dv = static_cast<double>(kfNext.value - kf.value);
                if (std::abs(dt) > 1e-6 && std::abs(dv) > 1e-6) {
                    kf.bezierInX = std::clamp(static_cast<float>((gp.x() - kf.time) / dt), 0.0f, 1.0f);
                    kf.bezierInY = static_cast<float>((gp.y() - kf.value) / dv);
                }
            }
        } else {
            // Out tangent
            int nextIdx = m_dragKeyIdx + 1;
            if (nextIdx < static_cast<int>(track->keyframeCount())) {
                const auto& kfNext = track->keyframe(nextIdx);
                double dt = static_cast<double>(kfNext.time - kf.time);
                double dv = static_cast<double>(kfNext.value - kf.value);
                if (std::abs(dt) > 1e-6 && std::abs(dv) > 1e-6) {
                    kf.bezierOutX = std::clamp(static_cast<float>((gp.x() - kf.time) / dt), 0.0f, 1.0f);
                    kf.bezierOutY = static_cast<float>((gp.y() - kf.value) / dv);
                }
            }
        }
        emit keyframeChanged();
        update();
        event->accept();
        return;
    }

    // Keyframe dragging
    if (m_dragging) {
        if (m_dragCurveIdx < 0 || m_dragCurveIdx >= static_cast<int>(m_curves.size())) return;
        auto* track = m_curves[m_dragCurveIdx].track;
        if (!track || m_dragKeyIdx < 0 || m_dragKeyIdx >= static_cast<int>(track->keyframeCount()))
            return;

        auto& kf = track->keyframe(m_dragKeyIdx);
        QPointF gp = pixelToGraph(pos.x(), pos.y());

        auto oldTime = kf.time;
        auto newTime = static_cast<int64_t>(gp.x());
        auto newVal  = static_cast<float>(gp.y());

        if (m_commandStack) {
            m_commandStack->execute(
                std::make_unique<MoveKeyframeCommand>(track, oldTime, newTime, newVal));
            // Find the moved keyframe's new index
            for (int ki = 0; ki < static_cast<int>(track->keyframeCount()); ++ki) {
                if (track->keyframe(ki).time == newTime) {
                    m_dragKeyIdx = ki;
                    // Update selection
                    m_selection.clear();
                    m_selection.insert({m_dragCurveIdx, ki});
                    break;
                }
            }
        } else {
            kf.time  = newTime;
            kf.value = newVal;
        }

        emit keyframeChanged();
        emit selectionChanged();
        update();
        event->accept();
        return;
    }

    // Box selection
    if (m_boxSelecting) {
        m_boxCurrent = pos;
        update();
        event->accept();
        return;
    }

    // Hovering Ã¢â‚¬â€ change cursor over keyframes
    auto hit = hitTest(pos);
    if (hit.curveIndex >= 0)
        setCursor(Qt::PointingHandCursor);
    else
        setCursor(Qt::ArrowCursor);
}

void KeyframeEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }

    if (m_draggingTangent) {
        m_draggingTangent = false;
        event->accept();
        return;
    }

    if (m_boxSelecting) {
        m_boxSelecting = false;
        // Convert pixel box to graph coords and select
        QPointF g1 = pixelToGraph(m_boxStart.x(), m_boxStart.y());
        QPointF g2 = pixelToGraph(m_boxCurrent.x(), m_boxCurrent.y());
        QRectF gr(g1, g2);
        bool shift = event->modifiers() & Qt::ShiftModifier;
        boxSelect(gr.normalized(), shift);
        event->accept();
        return;
    }
}

void KeyframeEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;

    QPointF pos = event->position();
    auto hit = hitTest(pos);

    if (hit.curveIndex >= 0) {
        // Double-click on keyframe Ã¢â‚¬â€ cycle interpolation
        auto* track = m_curves[hit.curveIndex].track;
        if (track && hit.keyIndex >= 0 && hit.keyIndex < static_cast<int>(track->keyframeCount())) {
            auto& kf = track->keyframe(hit.keyIndex);
            int next = (static_cast<int>(kf.interp) + 1) % 3;
            kf.interp = static_cast<InterpMode>(next);
            emit keyframeChanged();
            update();
        }
    } else {
        // Double-click on empty space Ã¢â‚¬â€ add keyframe to first visible curve
        QPointF gp = pixelToGraph(pos.x(), pos.y());
        for (int i = 0; i < static_cast<int>(m_curves.size()); ++i) {
            if (m_curves[i].visible) {
                addKeyframe(i, static_cast<int64_t>(gp.x()), static_cast<float>(gp.y()));
                break;
            }
        }
    }
    event->accept();
}

void KeyframeEditor::wheelEvent(QWheelEvent* event)
{
    double factor = (event->angleDelta().y() > 0) ? (1.0 / kZoomFactor) : kZoomFactor;

    QPointF anchor = pixelToGraph(event->position().x(), event->position().y());

    bool ctrlHeld = event->modifiers() & Qt::ControlModifier;
    bool shiftHeld = event->modifiers() & Qt::ShiftModifier;

    if (ctrlHeld && !shiftHeld) {
        // Zoom time only
        m_viewTimeMin = anchor.x() + (m_viewTimeMin - anchor.x()) * factor;
        m_viewTimeMax = anchor.x() + (m_viewTimeMax - anchor.x()) * factor;
    } else if (shiftHeld && !ctrlHeld) {
        // Zoom value only
        m_viewValueMin = anchor.y() + (m_viewValueMin - anchor.y()) * factor;
        m_viewValueMax = anchor.y() + (m_viewValueMax - anchor.y()) * factor;
    } else {
        // Zoom both axes
        m_viewTimeMin = anchor.x() + (m_viewTimeMin - anchor.x()) * factor;
        m_viewTimeMax = anchor.x() + (m_viewTimeMax - anchor.x()) * factor;
        m_viewValueMin = anchor.y() + (m_viewValueMin - anchor.y()) * factor;
        m_viewValueMax = anchor.y() + (m_viewValueMax - anchor.y()) * factor;
    }

    emit viewChanged();
    update();
    event->accept();
}

void KeyframeEditor::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        deleteSelectedKeyframes();
        break;
    case Qt::Key_A:
        if (event->modifiers() & Qt::ControlModifier)
            selectAll();
        break;
    case Qt::Key_C:
        if (event->modifiers() & Qt::ControlModifier)
            copySelectedKeyframes();
        break;
    case Qt::Key_V:
        if (event->modifiers() & Qt::ControlModifier) {
            // Paste at center of view
            auto centerTime = static_cast<int64_t>((m_viewTimeMin + m_viewTimeMax) / 2.0);
            pasteKeyframes(centerTime);
        }
        break;
    case Qt::Key_F:
        if (event->modifiers() & Qt::ShiftModifier)
            fitViewToSelection();
        else
            fitViewToAll();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void KeyframeEditor::contextMenuEvent(QContextMenuEvent* event)
{
    m_contextMenuGraphPos = pixelToGraph(event->pos().x(), event->pos().y());

    // Enable/disable actions based on state
    m_actDeleteKeyframes->setEnabled(!m_selection.empty());
    m_actLinear->setEnabled(!m_selection.empty());
    m_actBezier->setEnabled(!m_selection.empty());
    m_actHold->setEnabled(!m_selection.empty());
    m_actCopy->setEnabled(!m_selection.empty());
    m_actPaste->setEnabled(!m_clipboard.empty());

    m_contextMenu->exec(event->globalPos());
    event->accept();
}

} // namespace rt
