/*
 * TransformOverlayInput.cpp - Mouse/keyboard input handling for TransformOverlayWidget.
 */

#include "viewport/TransformOverlayWidget.h"
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"
#include "Theme.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QCoreApplication>

#include <cmath>
#include <algorithm>

#include <spdlog/spdlog.h>

namespace rt {

void TransformOverlayWidget::mousePressEvent(QMouseEvent* event)
{
    // ── Middle button → pan ─────────────────────────────────────────────
    if (event->button() == Qt::MiddleButton && m_vulkanVp) {
        m_dragMode = DragMode::Pan;
        m_panStartPos = event->position();
        m_panStartVpX = m_vulkanVp->viewPanX();
        m_panStartVpY = m_vulkanVp->viewPanY();
        applyCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    // ── Eyedropper tool: pick color at click location ─────────────────
    if (event->button() == Qt::LeftButton && m_editTool == 8) {
        QRectF fr = computeFrameRect();
        if (!fr.isEmpty() && m_vulkanVp) {
            QPointF wPos = event->position();
            float srcW = static_cast<float>(m_vulkanVp->srcWidth());
            float srcH = static_cast<float>(m_vulkanVp->srcHeight());
            if (srcW > 0.0f && srcH > 0.0f) {
                float frameX = static_cast<float>((wPos.x() - fr.x()) / fr.width()) * srcW;
                float frameY = static_cast<float>((wPos.y() - fr.y()) / fr.height()) * srcH;
                emit colorPicked(frameX, frameY);
                event->accept();
                return;
            }
        }
    }

    // ── Ctrl+Click: add point on mask border ──────────────────────────
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)
        && m_masks && !m_masks->empty()) {
        QRectF fr = computeFrameRect();
        if (!fr.isEmpty()) {
            int maskIdx = -1;
            if (addPointOnMaskEdge(event->position(), fr, maskIdx)) {
                event->accept();
                return;
            }
        }
    }

    // ── Mask control point interaction ──────────────────────────────────
    if (event->button() == Qt::LeftButton && m_masks && !m_masks->empty()) {
        QPointF wPos = event->position();
        int maskIdx = -1;
        int handleIdx = hitTestMaskHandle(wPos, maskIdx);
        if (handleIdx >= 0 && maskIdx >= 0) {
            m_dragMode = DragMode::DragMaskPoint;
            m_dragMaskIndex = maskIdx;
            m_dragMaskHandle = handleIdx;
            m_dragStartMask = (*m_masks)[static_cast<size_t>(maskIdx)];
            m_dragStartWidget = wPos;
            applyCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        // Click inside mask body → move the mask
        maskIdx = hitTestMaskBody(wPos);
        if (maskIdx >= 0) {
            m_dragMode = DragMode::DragMaskPoint;
            m_dragMaskIndex = maskIdx;
            m_dragMaskHandle = INT_MAX; // body drag sentinel
            m_dragStartMask = (*m_masks)[static_cast<size_t>(maskIdx)];
            m_dragStartWidget = wPos;
            applyCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
    }

    // ── Transform overlay interaction ───────────────────────────────────
    if (event->button() == Qt::LeftButton && m_overlay.visible) {
        QPointF wPos = event->position();

        int handle = hitTestHandle(wPos);
        if (handle >= 0) {
            m_dragMode = DragMode::ScaleCorner;
            m_dragHandle = handle;
            m_dragStartWidget = wPos;
            m_dragStartPosX = m_overlay.posX;
            m_dragStartPosY = m_overlay.posY;
            m_dragStartScX  = m_overlay.scaleX;
            m_dragStartScY  = m_overlay.scaleY;
            m_dragStartRot  = m_overlay.rotation;
            applyCursor(Qt::SizeFDiagCursor);
            event->accept();
            return;
        }

        int rotHandle = hitTestRotate(wPos);
        if (rotHandle >= 0) {
            m_dragMode = DragMode::RotateCorner;
            m_dragHandle = rotHandle;
            m_dragStartWidget = wPos;
            m_dragStartPosX = m_overlay.posX;
            m_dragStartPosY = m_overlay.posY;
            m_dragStartScX  = m_overlay.scaleX;
            m_dragStartScY  = m_overlay.scaleY;
            m_dragStartRot  = m_overlay.rotation;
            // Compute starting angle from center to mouse
            QPointF corners[4];
            computeOverlayCorners(corners);
            QPointF center = (corners[0] + corners[2]) * 0.5;
            m_dragStartAngle = static_cast<float>(
                std::atan2(wPos.y() - center.y(), wPos.x() - center.x())
                * 180.0 / 3.14159265358979);
            applyCursor(rotateCursor());
            event->accept();
            return;
        }

        if (hitTestBody(wPos)) {
            // When masks are active, don't let a body click move the layer;
            // body clicks should move the mask instead (handled above).
            if (m_masks && !m_masks->empty()) {
                event->accept();
                return;
            }
            m_dragMode = DragMode::MoveBody;
            m_dragStartWidget = wPos;
            m_dragStartPosX = m_overlay.posX;
            m_dragStartPosY = m_overlay.posY;
            m_dragStartScX  = m_overlay.scaleX;
            m_dragStartScY  = m_overlay.scaleY;
            m_dragStartRot  = m_overlay.rotation;
            applyCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
    }

    // ── Left-click on empty area: emit signal for text tool etc. ────────
    if (event->button() == Qt::LeftButton) {
        QRectF fr = computeFrameRect();
        if (!fr.isEmpty() && m_vulkanVp) {
            QPointF wPos = event->position();
            float srcW = static_cast<float>(m_vulkanVp->srcWidth());
            float srcH = static_cast<float>(m_vulkanVp->srcHeight());
            if (srcW > 0.0f && srcH > 0.0f) {
                float frameX = static_cast<float>((wPos.x() - fr.x()) / fr.width()) * srcW;
                float frameY = static_cast<float>((wPos.y() - fr.y()) / fr.height()) * srcH;
                emit emptyAreaClicked(frameX, frameY);
                event->accept();
                return;
            }
        }
    }

    // Not handled — pass through
    event->ignore();
}

void TransformOverlayWidget::mouseMoveEvent(QMouseEvent* event)
{
    QPointF wPos = event->position();

    // ── Pan drag ────────────────────────────────────────────────────────
    if (m_dragMode == DragMode::Pan && m_vulkanVp) {
        float dx = static_cast<float>(wPos.x() - m_panStartPos.x());
        float dy = static_cast<float>(wPos.y() - m_panStartPos.y());
        m_vulkanVp->setViewPan(m_panStartVpX + dx, m_panStartVpY + dy);
        update(); // redraw overlay at new pan
        event->accept();
        return;
    }

    // ── Mask point drag ─────────────────────────────────────────────────
    if (m_dragMode == DragMode::DragMaskPoint && m_masks &&
        (event->buttons() & Qt::LeftButton))
    {
        QRectF fr = computeFrameRect();
        if (fr.isEmpty()) return;

        auto& mask = (*m_masks)[static_cast<size_t>(m_dragMaskIndex)];
        float dxNorm = static_cast<float>((wPos.x() - m_dragStartWidget.x()) / fr.width());
        float dyNorm = static_cast<float>((wPos.y() - m_dragStartWidget.y()) / fr.height());

        if (mask.shape == MaskShape::Ellipse) {
            if (m_dragMaskHandle == 4 || m_dragMaskHandle == INT_MAX) {
                // Move center
                mask.centerX = m_dragStartMask.centerX + dxNorm;
                mask.centerY = m_dragStartMask.centerY + dyNorm;
            } else if (m_dragMaskHandle == 0 || m_dragMaskHandle == 1) {
                // Right/left cardinal → scale width
                float d = (m_dragMaskHandle == 0) ? dxNorm : -dxNorm;
                mask.width = std::max(0.01f, m_dragStartMask.width + d * 2.0f);
            } else {
                // Bottom/top cardinal → scale height
                float d = (m_dragMaskHandle == 2) ? dyNorm : -dyNorm;
                mask.height = std::max(0.01f, m_dragStartMask.height + d * 2.0f);
            }
        }
        else if (mask.shape == MaskShape::Rectangle) {
            if (m_dragMaskHandle == 4 || m_dragMaskHandle == INT_MAX) {
                mask.centerX = m_dragStartMask.centerX + dxNorm;
                mask.centerY = m_dragStartMask.centerY + dyNorm;
            } else if (m_dragMaskHandle >= 5 && m_dragMaskHandle <= 8) {
                // Mid-edge handles: resize one dimension only
                // 5=top, 6=right, 7=bottom, 8=left
                if (m_dragMaskHandle == 5) {
                    // Top edge: shrink height from top
                    mask.height  = std::max(0.01f, m_dragStartMask.height - dyNorm * 2.0f);
                } else if (m_dragMaskHandle == 7) {
                    // Bottom edge: grow height from bottom
                    mask.height  = std::max(0.01f, m_dragStartMask.height + dyNorm * 2.0f);
                } else if (m_dragMaskHandle == 6) {
                    // Right edge: grow width from right
                    mask.width   = std::max(0.01f, m_dragStartMask.width + dxNorm * 2.0f);
                } else { // 8 = left
                    // Left edge: shrink width from left
                    mask.width   = std::max(0.01f, m_dragStartMask.width - dxNorm * 2.0f);
                }
            } else {
                // Corner drag → scale width/height symmetrically
                float signX = (m_dragMaskHandle == 1 || m_dragMaskHandle == 2) ? 1.0f : -1.0f;
                float signY = (m_dragMaskHandle == 2 || m_dragMaskHandle == 3) ? 1.0f : -1.0f;
                mask.width  = std::max(0.01f, m_dragStartMask.width  + signX * dxNorm * 2.0f);
                mask.height = std::max(0.01f, m_dragStartMask.height + signY * dyNorm * 2.0f);
            }
        }
        else if (mask.shape == MaskShape::FreeDrawBezier) {
            auto vi = static_cast<size_t>(m_dragMaskHandle);
            if (vi < mask.vertices.size()) {
                // Drag single vertex
                mask.vertices[vi].x = m_dragStartMask.vertices[vi].x + dxNorm;
                mask.vertices[vi].y = m_dragStartMask.vertices[vi].y + dyNorm;
            } else {
                // Body drag — translate all vertices
                for (size_t i = 0; i < mask.vertices.size(); ++i) {
                    mask.vertices[i].x = m_dragStartMask.vertices[i].x + dxNorm;
                    mask.vertices[i].y = m_dragStartMask.vertices[i].y + dyNorm;
                }
            }
        }

        emit maskLiveUpdate();
        update();
        event->accept();
        return;
    }

    // ── Move body drag ──────────────────────────────────────────────────
    if (m_dragMode == DragMode::MoveBody && (event->buttons() & Qt::LeftButton)) {
        QRectF fr = computeFrameRect();
        if (fr.isEmpty()) return;

        constexpr float REF_W = 1920.0f;
        constexpr float REF_H = 1080.0f;
        float pxPerRefX = static_cast<float>(fr.width())  / REF_W;
        float pxPerRefY = static_cast<float>(fr.height()) / REF_H;
        if (pxPerRefX < 0.001f || pxPerRefY < 0.001f) return;

        // Account for clip-level scale: layer position is in canvas space,
        // but the clip scale magnifies the whole canvas, so mouse movement
        // needs to be divided by the clip scale to get the correct delta.
        float effScaleX = std::max(0.001f, m_overlay.clipScaleX);
        float effScaleY = std::max(0.001f, m_overlay.clipScaleY);

        float dx = static_cast<float>(wPos.x() - m_dragStartWidget.x()) / (pxPerRefX * effScaleX);
        float dy = static_cast<float>(wPos.y() - m_dragStartWidget.y()) / (pxPerRefY * effScaleY);

        m_overlay.posX = m_dragStartPosX + dx;
        m_overlay.posY = m_dragStartPosY + dy;

        emit transformPositionChanged(m_overlay.posX, m_overlay.posY);
        update();
        event->accept();
        return;
    }

    // ── Scale corner drag ───────────────────────────────────────────────
    if (m_dragMode == DragMode::ScaleCorner && (event->buttons() & Qt::LeftButton)) {
        QPointF corners[4];
        computeOverlayCorners(corners);
        QPointF center = (corners[0] + corners[2]) * 0.5;

        float startDist = std::hypot(
            static_cast<float>(m_dragStartWidget.x() - center.x()),
            static_cast<float>(m_dragStartWidget.y() - center.y()));
        float curDist = std::hypot(
            static_cast<float>(wPos.x() - center.x()),
            static_cast<float>(wPos.y() - center.y()));

        if (startDist > 1.0f) {
            if (event->modifiers() & Qt::ShiftModifier) {
                // Non-uniform (free) scale: separate X and Y ratios
                float startDx = std::abs(static_cast<float>(m_dragStartWidget.x() - center.x()));
                float startDy = std::abs(static_cast<float>(m_dragStartWidget.y() - center.y()));
                float curDx   = std::abs(static_cast<float>(wPos.x() - center.x()));
                float curDy   = std::abs(static_cast<float>(wPos.y() - center.y()));
                float ratioX  = (startDx > 1.0f) ? curDx / startDx : 1.0f;
                float ratioY  = (startDy > 1.0f) ? curDy / startDy : 1.0f;
                m_overlay.scaleX = std::max(0.01f, m_dragStartScX * ratioX);
                m_overlay.scaleY = std::max(0.01f, m_dragStartScY * ratioY);
            } else {
                // Uniform scale (default): both axes get the same value
                float ratio = curDist / startDist;
                float newScale = std::max(0.01f, m_dragStartScX * ratio);
                m_overlay.scaleX = newScale;
                m_overlay.scaleY = newScale;
            }
            emit transformScaleChanged(m_overlay.scaleX, m_overlay.scaleY);
            update();
        }
        event->accept();
        return;
    }

    // ── Rotate corner drag ──────────────────────────────────────────────
    if (m_dragMode == DragMode::RotateCorner && (event->buttons() & Qt::LeftButton)) {
        QPointF corners[4];
        computeOverlayCorners(corners);
        QPointF center = (corners[0] + corners[2]) * 0.5;

        float curAngle = static_cast<float>(
            std::atan2(wPos.y() - center.y(), wPos.x() - center.x())
            * 180.0 / 3.14159265358979);
        float deltaAngle = curAngle - m_dragStartAngle;

        // Normalize to -180..180
        while (deltaAngle >  180.0f) deltaAngle -= 360.0f;
        while (deltaAngle < -180.0f) deltaAngle += 360.0f;

        float newRot = m_dragStartRot + deltaAngle;
        m_overlay.rotation = newRot;
        emit transformRotationChanged(m_overlay.rotation);
        update();
        event->accept();
        return;
    }

    // ── Cursor hint when hovering ───────────────────────────────────────
    if (m_editTool == 8 && m_dragMode == DragMode::None) {
        applyCursor(zoomCursor());
        event->accept();
        return;
    }

    if (m_dragMode == DragMode::None && m_masks && !m_masks->empty()) {
        // Ctrl+hover near mask edge → show pen cursor
        if ((event->modifiers() & Qt::ControlModifier) && hitTestMaskEdge(wPos)) {
            if (m_hoverMaskIndex != -1 || m_hoverMaskHandle != -1) {
                m_hoverMaskIndex = -1;
                m_hoverMaskHandle = -1;
                update();
            }
            applyCursor(penCursor());
            event->accept();
            return;
        }
        int maskIdx = -1;
        int hHandle = hitTestMaskHandle(wPos, maskIdx);
        if (hHandle >= 0) {
            if (m_hoverMaskIndex != maskIdx || m_hoverMaskHandle != hHandle) {
                m_hoverMaskIndex = maskIdx;
                m_hoverMaskHandle = hHandle;
                update(); // repaint to show glow
            }
            applyCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        if (hitTestMaskBody(wPos) >= 0) {
            if (m_hoverMaskIndex != -1 || m_hoverMaskHandle != -1) {
                m_hoverMaskIndex = -1;
                m_hoverMaskHandle = -1;
                update();
            }
            applyCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        // Not hovering any mask element
        if (m_hoverMaskIndex != -1 || m_hoverMaskHandle != -1) {
            m_hoverMaskIndex = -1;
            m_hoverMaskHandle = -1;
            update();
        }
    }
    if (m_overlay.visible && m_dragMode == DragMode::None) {
        if (hitTestHandle(wPos) >= 0)
            applyCursor(Qt::SizeFDiagCursor);
        else if (hitTestRotate(wPos) >= 0)
            applyCursor(rotateCursor());
        else if (hitTestBody(wPos))
            applyCursor(Qt::OpenHandCursor);
        else
            applyCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    event->ignore();
}

void TransformOverlayWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragMode == DragMode::Pan) {
        m_dragMode = DragMode::None;
        applyCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (m_dragMode == DragMode::DragMaskPoint) {
        if (m_masks && m_dragMaskIndex >= 0 &&
            static_cast<size_t>(m_dragMaskIndex) < m_masks->size())
        {
            emit maskDragFinished(m_dragMaskIndex, m_dragStartMask,
                                  (*m_masks)[static_cast<size_t>(m_dragMaskIndex)]);
        }
        m_dragMode = DragMode::None;
        m_dragMaskIndex = -1;
        m_dragMaskHandle = -1;
        applyCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (m_dragMode != DragMode::None) {
        float oldPX = m_dragStartPosX, oldPY = m_dragStartPosY;
        float oldSX = m_dragStartScX,  oldSY = m_dragStartScY;
        float oldRot = m_dragStartRot;
        float newPX = m_overlay.posX,  newPY = m_overlay.posY;
        float newSX = m_overlay.scaleX, newSY = m_overlay.scaleY;
        float newRot = m_overlay.rotation;
        m_dragMode = DragMode::None;
        applyCursor(Qt::ArrowCursor);
        emit transformDragFinished(oldPX, oldPY, oldSX, oldSY, oldRot,
                                   newPX, newPY, newSX, newSY, newRot);
        event->accept();
        return;
    }

    event->ignore();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Event filter — intercept mouse events from the native QWindow
// ═════════════════════════════════════════════════════════════════════════════

void TransformOverlayWidget::wheelEvent(QWheelEvent* event)
{
    // Forward wheel events to VulkanViewport for zoom/scroll.
    if (m_vulkanVp) {
        QCoreApplication::sendEvent(m_vulkanVp, event);
        update(); // redraw overlay at new zoom
    }
}

bool TransformOverlayWidget::eventFilter(QObject* watched, QEvent* event)
{
    // Only intercept events from the VulkanViewport's native QWindow.
    if (!m_vulkanVp || watched != m_vulkanVp->nativeWindow())
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress:
        mousePressEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    case QEvent::MouseMove:
        mouseMoveEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    case QEvent::MouseButtonRelease:
        mouseReleaseEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}


} // namespace rt
