/*
 * TransformOverlayDraw.cpp - Painting & drawing for TransformOverlayWidget.
 */

#include "viewport/TransformOverlayWidget.h"
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"
#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>

#include <cmath>

#include <spdlog/spdlog.h>

namespace rt {

void TransformOverlayWidget::paintEvent(QPaintEvent* /*event*/)
{
    // Skip painting entirely when there's nothing to draw.
    // On a WA_TranslucentBackground window, even a no-op QPainter triggers
    // DWM per-pixel alpha composition, so avoid it when possible.
    bool hasMasks = (m_masks && !m_masks->empty());
    if (!m_overlay.visible && !m_showSafeAreas && !m_showGrid && !hasMasks)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Draw rule-of-thirds grid (behind safe areas)
    if (m_showGrid)
        drawGrid(painter);

    // Draw safe area guides (always if enabled, even without selected clip)
    if (m_showSafeAreas)
        drawSafeAreas(painter);

    // Draw mask overlays (blue outlines + control points)
    if (hasMasks)
        drawMaskOverlay(painter);

    // Draw transform overlay (bounding box + handles)
    if (m_overlay.visible)
        drawTransformOverlay(painter);
}

void TransformOverlayWidget::drawTransformOverlay(QPainter& painter)
{
    QPointF corners[4];
    computeOverlayCorners(corners);

    // ── Bounding box (dashed cyan line) ─────────────────────────────────
    const auto& tc = Theme::colors();
    QColor boxColor = tc.accent; boxColor.setAlpha(200);
    QPen boxPen(boxColor, 1.5, Qt::DashLine);
    painter.setPen(boxPen);
    painter.setBrush(Qt::NoBrush);

    QPolygonF poly;
    for (int i = 0; i < 4; ++i)
        poly << corners[i];
    poly << corners[0];
    painter.drawPolyline(poly);

    // ── Corner handles (filled squares) ─────────────────────────────────
    constexpr double HANDLE_SIZE = 8.0;
    QColor handleBorder = tc.textBright; handleBorder.setAlpha(220);
    painter.setPen(QPen(handleBorder, 1));
    QColor handleFill = tc.accent; handleFill.setAlpha(180);
    painter.setBrush(handleFill);

    for (int i = 0; i < 4; ++i) {
        QRectF handle(corners[i].x() - HANDLE_SIZE / 2,
                      corners[i].y() - HANDLE_SIZE / 2,
                      HANDLE_SIZE, HANDLE_SIZE);
        painter.drawRect(handle);
    }

    // ── Center crosshair ────────────────────────────────────────────────
    QPointF center = (corners[0] + corners[2]) * 0.5;
    QColor crossColor = tc.accent; crossColor.setAlpha(160);
    painter.setPen(QPen(crossColor, 1));
    constexpr double CROSS_SIZE = 6.0;
    painter.drawLine(QPointF(center.x() - CROSS_SIZE, center.y()),
                     QPointF(center.x() + CROSS_SIZE, center.y()));
    painter.drawLine(QPointF(center.x(), center.y() - CROSS_SIZE),
                     QPointF(center.x(), center.y() + CROSS_SIZE));
}

void TransformOverlayWidget::drawSafeAreas(QPainter& painter)
{
    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return;

    QColor safeColor = Theme::colors().textPrimary; safeColor.setAlpha(80);
    QPen safePen(safeColor, 1, Qt::DashLine);
    painter.setPen(safePen);
    painter.setBrush(Qt::NoBrush);

    // Action safe (90% — 5% inset on each side)
    QRectF actionSafe = fr;
    actionSafe.adjust(fr.width() * 0.05, fr.height() * 0.05,
                     -fr.width() * 0.05, -fr.height() * 0.05);
    painter.drawRect(actionSafe);

    // Title safe (80% — 10% inset on each side)
    QRectF titleSafe = fr;
    titleSafe.adjust(fr.width() * 0.1, fr.height() * 0.1,
                    -fr.width() * 0.1, -fr.height() * 0.1);
    painter.drawRect(titleSafe);
}

void TransformOverlayWidget::drawGrid(QPainter& painter)
{
    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return;

    QColor gridColor = Theme::colors().textPrimary; gridColor.setAlpha(60);
    QPen gridPen(gridColor, 1, Qt::SolidLine);
    painter.setPen(gridPen);
    painter.setBrush(Qt::NoBrush);

    // Rule-of-thirds: 2 vertical + 2 horizontal lines
    double x1 = fr.left() + fr.width() / 3.0;
    double x2 = fr.left() + fr.width() * 2.0 / 3.0;
    double y1 = fr.top() + fr.height() / 3.0;
    double y2 = fr.top() + fr.height() * 2.0 / 3.0;

    painter.drawLine(QPointF(x1, fr.top()), QPointF(x1, fr.bottom()));
    painter.drawLine(QPointF(x2, fr.top()), QPointF(x2, fr.bottom()));
    painter.drawLine(QPointF(fr.left(), y1), QPointF(fr.right(), y1));
    painter.drawLine(QPointF(fr.left(), y2), QPointF(fr.right(), y2));

    // Center crosshair
    double cx = fr.center().x();
    double cy = fr.center().y();
    constexpr double CROSS = 8.0;
    painter.drawLine(QPointF(cx - CROSS, cy), QPointF(cx + CROSS, cy));
    painter.drawLine(QPointF(cx, cy - CROSS), QPointF(cx, cy + CROSS));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Mask overlay
// ═════════════════════════════════════════════════════════════════════════════

void TransformOverlayWidget::drawMaskOverlay(QPainter& painter)
{
    if (!m_masks || !m_vulkanVp) return;

    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return;

    float srcW = static_cast<float>(m_vulkanVp->srcWidth());
    float srcH = static_cast<float>(m_vulkanVp->srcHeight());
    if (srcW <= 0.0f || srcH <= 0.0f) return;

    // Source-pixel to widget-pixel conversion factors
    double pxToWX = fr.width()  / static_cast<double>(srcW);
    double pxToWY = fr.height() / static_cast<double>(srcH);

    // Mask outline: Premiere-style blue
    QColor activeMaskColor(0, 120, 255, 200);
    QColor dimMaskColor(0, 120, 255, 80);
    QColor featherColor(0, 120, 255, 120);
    QColor dimFeatherColor(0, 120, 255, 50);
    QColor handleColor(255, 255, 255, 220);
    QColor glowColor(100, 180, 255, 90);
    constexpr double CTRL_SIZE = 12.0;
    constexpr double GLOW_EXTRA = 4.0; // extra radius for hover glow

    for (size_t maskLoopIdx = 0; maskLoopIdx < m_masks->size(); ++maskLoopIdx) {
        const auto& mask = (*m_masks)[maskLoopIdx];
        int mi = static_cast<int>(maskLoopIdx);
        bool isActive = (m_activeMaskIndex < 0 || m_activeMaskIndex == mi);
        bool isMaskHovered = (mi == m_hoverMaskIndex);

        QColor maskColor = isActive ? activeMaskColor : dimMaskColor;
        QPen maskPen(maskColor, isActive ? 1.5 : 1.0, Qt::SolidLine);
        QPen featherPen(isActive ? featherColor : dimFeatherColor, 1.0, Qt::DashLine);

        painter.setPen(maskPen);
        painter.setBrush(Qt::NoBrush);

        // Map normalized mask coords to widget space
        auto toWidget = [&](float nx, float ny) -> QPointF {
            return QPointF(fr.x() + static_cast<double>(nx) * fr.width(),
                           fr.y() + static_cast<double>(ny) * fr.height());
        };

        // Expansion offset in widget pixels (averaged for non-square pixels)
        double expW = static_cast<double>(mask.expansion) * pxToWX;
        double expH = static_cast<double>(mask.expansion) * pxToWY;
        // Feather offset (additional, outside the expanded boundary)
        double feathW = static_cast<double>(mask.feather) * pxToWX;
        double feathH = static_cast<double>(mask.feather) * pxToWY;

        if (mask.shape == MaskShape::Ellipse) {
            QPointF center = toWidget(mask.centerX, mask.centerY);
            double rw = static_cast<double>(mask.width)  * fr.width()  * 0.5 + expW;
            double rh = static_cast<double>(mask.height) * fr.height() * 0.5 + expH;
            rw = std::max(rw, 0.0);
            rh = std::max(rh, 0.0);

            painter.save();
            painter.translate(center);
            painter.rotate(static_cast<double>(mask.rotation));

            // Main expanded boundary
            painter.setPen(maskPen);
            painter.drawEllipse(QPointF(0, 0), rw, rh);

            // Feather boundary (dotted, outside the main boundary)
            if (mask.feather > 0.01f) {
                double frw = rw + feathW;
                double frh = rh + feathH;
                painter.setPen(featherPen);
                painter.drawEllipse(QPointF(0, 0), std::max(frw, 0.0), std::max(frh, 0.0));
            }

            // Control handles at expanded cardinal positions + center
            if (isActive) {
                int hIdx = 0;
                for (auto [dx, dy] : {std::pair{rw, 0.0}, {-rw, 0.0}, {0.0, rh}, {0.0, -rh}, {0.0, 0.0}}) {
                    bool hovered = isMaskHovered && (m_hoverMaskHandle == hIdx);
                    if (hovered) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(glowColor);
                        painter.drawEllipse(QPointF(dx, dy), CTRL_SIZE / 2 + GLOW_EXTRA, CTRL_SIZE / 2 + GLOW_EXTRA);
                    }
                    painter.setPen(QPen(handleColor, hovered ? 2 : 1));
                    painter.setBrush(maskColor);
                    painter.drawEllipse(QPointF(dx, dy), CTRL_SIZE / 2, CTRL_SIZE / 2);
                    ++hIdx;
                }
            }
            painter.restore();
        }
        else if (mask.shape == MaskShape::Rectangle) {
            QPointF center = toWidget(mask.centerX, mask.centerY);
            double hw = static_cast<double>(mask.width)  * fr.width()  * 0.5 + expW;
            double hh = static_cast<double>(mask.height) * fr.height() * 0.5 + expH;
            hw = std::max(hw, 0.0);
            hh = std::max(hh, 0.0);

            painter.save();
            painter.translate(center);
            painter.rotate(static_cast<double>(mask.rotation));

            // Main expanded boundary
            painter.setPen(maskPen);
            QRectF rect(-hw, -hh, hw * 2, hh * 2);
            painter.drawRect(rect);

            // Feather boundary (dotted)
            if (mask.feather > 0.01f) {
                double fhw = hw + feathW;
                double fhh = hh + feathH;
                painter.setPen(featherPen);
                QRectF feathRect(-fhw, -fhh, fhw * 2, fhh * 2);
                painter.drawRect(feathRect);
            }

            // Corner handles + center + mid-edge handles (at expanded dims)
            if (isActive) {
                int hIdx = 0;
                // Corners (square handles) + center
                for (auto [cx2, cy2] : {std::pair{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}, {0.0, 0.0}}) {
                    bool hovered = isMaskHovered && (m_hoverMaskHandle == hIdx);
                    if (hovered) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(glowColor);
                        painter.drawRect(QRectF(cx2 - CTRL_SIZE / 2 - GLOW_EXTRA, cy2 - CTRL_SIZE / 2 - GLOW_EXTRA,
                                                CTRL_SIZE + GLOW_EXTRA * 2, CTRL_SIZE + GLOW_EXTRA * 2));
                    }
                    painter.setPen(QPen(handleColor, hovered ? 2 : 1));
                    painter.setBrush(maskColor);
                    painter.drawRect(QRectF(cx2 - CTRL_SIZE / 2, cy2 - CTRL_SIZE / 2,
                                            CTRL_SIZE, CTRL_SIZE));
                    ++hIdx;
                }
                // Mid-edge handles (round handles for distinction) indices 5-8
                for (auto [ex, ey] : {std::pair{0.0, -hh}, {hw, 0.0}, {0.0, hh}, {-hw, 0.0}}) {
                    bool hovered = isMaskHovered && (m_hoverMaskHandle == hIdx);
                    if (hovered) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(glowColor);
                        painter.drawEllipse(QPointF(ex, ey), CTRL_SIZE / 2 + GLOW_EXTRA, CTRL_SIZE / 2 + GLOW_EXTRA);
                    }
                    painter.setPen(QPen(handleColor, hovered ? 2 : 1));
                    painter.setBrush(maskColor);
                    painter.drawEllipse(QPointF(ex, ey), CTRL_SIZE / 2, CTRL_SIZE / 2);
                    ++hIdx;
                }
            }
            painter.restore();
        }
        else if (mask.shape == MaskShape::FreeDrawBezier && mask.vertices.size() >= 2) {
            // Draw bezier path
            QPainterPath path;
            const auto& verts = mask.vertices;
            QPointF first = toWidget(verts[0].x, verts[0].y);
            path.moveTo(first);
            for (size_t vi = 0; vi < verts.size(); ++vi) {
                size_t ni = (vi + 1) % verts.size();
                QPointF p0 = toWidget(verts[vi].x, verts[vi].y);
                QPointF cp1(p0.x() + static_cast<double>(verts[vi].outTanX) * fr.width(),
                            p0.y() + static_cast<double>(verts[vi].outTanY) * fr.height());
                QPointF p1 = toWidget(verts[ni].x, verts[ni].y);
                QPointF cp2(p1.x() + static_cast<double>(verts[ni].inTanX) * fr.width(),
                            p1.y() + static_cast<double>(verts[ni].inTanY) * fr.height());
                path.cubicTo(cp1, cp2, p1);
            }
            painter.drawPath(path);

            // Feather outline for bezier (stroked expansion of path)
            if (mask.feather > 0.01f) {
                QPainterPathStroker stroker;
                double avgFeath = (feathW + feathH) * 0.5;
                stroker.setWidth(avgFeath * 2.0);
                QPainterPath feathOutline = stroker.createStroke(path);
                painter.setPen(featherPen);
                painter.drawPath(feathOutline);
            }

            // Vertex control points
            if (isActive) {
                int hIdx = 0;
                for (const auto& v : verts) {
                    QPointF pt = toWidget(v.x, v.y);
                    bool hovered = isMaskHovered && (m_hoverMaskHandle == hIdx);
                    if (hovered) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(glowColor);
                        painter.drawEllipse(pt, CTRL_SIZE / 2 + GLOW_EXTRA, CTRL_SIZE / 2 + GLOW_EXTRA);
                    }
                    painter.setPen(QPen(handleColor, hovered ? 2 : 1));
                    painter.setBrush(maskColor);
                    painter.drawEllipse(pt, CTRL_SIZE / 2, CTRL_SIZE / 2);
                    ++hIdx;
                }
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Mask hit-testing
// ═════════════════════════════════════════════════════════════════════════════


} // namespace rt
