/*
 * TransformOverlayInput.cpp - Mouse/keyboard input handling for TransformOverlayWidget.
 */

#include "viewport/TransformOverlayWidget.h"
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Keyframe.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/KeyframeCmds.h"
#include "Theme.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QCoreApplication>
#include <QMenu>
#include <QAction>
#include <QLineEdit>
#include <QFontMetricsF>
#include <QPointer>
#include <QTimer>
#include <algorithm>

#include <cmath>
#include <algorithm>

#include <spdlog/spdlog.h>

namespace rt {

void TransformOverlayWidget::mousePressEvent(QMouseEvent* event)
{
    // ── Right button on a motion-path waypoint → Spatial Interpolation menu
    if (event->button() == Qt::RightButton && m_motionX && m_motionY) {
        int wp = hitTestMotionWaypoint(event->position());
        if (wp >= 0) {
            const int64_t kfTime = m_motionX->keyframe(static_cast<size_t>(wp)).time;

            QMenu menu(this);
            QMenu* sub = menu.addMenu(QStringLiteral("Spatial Interpolation"));
            auto addAction = [&](const QString& label, InterpMode mode) {
                QAction* a = sub->addAction(label);
                connect(a, &QAction::triggered, this, [this, kfTime, mode]() {
                    if (!m_motionX || !m_motionY) return;
                    if (m_motionCmdStack) {
                        m_motionCmdStack->execute(
                            std::make_unique<SetKeyframeSpatialInterpCommand>(
                                m_motionX, m_motionY, kfTime, mode));
                    } else {
                        // No undo stack — still apply the change.
                        for (size_t i = 0; i < m_motionX->keyframeCount(); ++i)
                            if (m_motionX->keyframe(i).time == kfTime) {
                                m_motionX->keyframe(i).spatialInterp = mode;
                                break;
                            }
                        for (size_t i = 0; i < m_motionY->keyframeCount(); ++i)
                            if (m_motionY->keyframe(i).time == kfTime) {
                                m_motionY->keyframe(i).spatialInterp = mode;
                                break;
                            }
                    }
                    update();
                });
            };
            addAction(QStringLiteral("Linear"),            InterpMode::Linear);
            addAction(QStringLiteral("Bezier"),            InterpMode::Bezier);
            addAction(QStringLiteral("Auto Bezier"),       InterpMode::AutoBezier);
            addAction(QStringLiteral("Continuous Bezier"), InterpMode::ContinuousBezier);

            menu.exec(event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }

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

    // ── Motion-path spatial handle drag ─────────────────────────────────
    if (event->button() == Qt::LeftButton && m_motionX && m_motionY) {
        int kfIdx = -1;
        bool isIn = false;
        if (hitTestMotionHandle(event->position(), kfIdx, isIn)) {
            m_dragMode        = DragMode::DragMotionHandle;
            m_dragMotionKfIdx = kfIdx;
            m_dragMotionIsIn  = isIn;
            const auto& kfx   = m_motionX->keyframe(static_cast<size_t>(kfIdx));
            const auto& kfy   = m_motionY->keyframe(static_cast<size_t>(kfIdx));
            m_dragKfTime      = kfx.time;
            m_dragOrigInX     = kfx.spatialInX;
            m_dragOrigInY     = kfy.spatialInY;
            m_dragOrigOutX    = kfx.spatialOutX;
            m_dragOrigOutY    = kfy.spatialOutY;
            applyCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
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
            applyCursor(Qt::ArrowCursor);   // no special move cursor
            event->accept();
            return;
        }
    }

    // ── Left-click on empty area: emit signal for text tool etc. ────────
    if (event->button() == Qt::LeftButton) {
        if (m_vulkanVp) {
            // IMPORTANT: this handler is reached via eventFilter() forwarding
            // mouse events from m_vulkanVp's native QWindow (HWND). So
            // event->position() is in HWND-LOCAL coordinates — NOT the
            // overlay widget's local coords. computeFrameRect() returns a
            // rect in overlay-widget-local coords (it applies
            // `+ hwndOff - vpOffset` to shift FROM HWND space INTO overlay
            // space). Mixing those two spaces produces a constant offset
            // exactly equal to (hwndOff - vpOffset) — historically seen as
            // text/shapes landing ~31 px below the cursor when the panel
            // header clipped the HWND upward. Compute the click in the
            // composite's source pixels directly from the GPU draw rect in
            // HWND space (gpuNorm × surface), which is the SAME space the
            // event coordinates are in.
            QRectF gnorm = m_vulkanVp->gpuFrameRect();
            QSize  surf  = m_vulkanVp->nativeSurfaceSize();
            float srcW = static_cast<float>(m_vulkanVp->srcWidth());
            float srcH = static_cast<float>(m_vulkanVp->srcHeight());
            if (!gnorm.isEmpty() && surf.width() > 0 && surf.height() > 0 &&
                srcW > 0.0f && srcH > 0.0f)
            {
                QPointF wPos = event->position();
                double drawX = gnorm.x() * surf.width();
                double drawY = gnorm.y() * surf.height();
                double drawW = gnorm.width()  * surf.width();
                double drawH = gnorm.height() * surf.height();
                float frameX = static_cast<float>((wPos.x() - drawX) / drawW) * srcW;
                float frameY = static_cast<float>((wPos.y() - drawY) / drawH) * srcH;
                emit emptyAreaClicked(frameX, frameY);
                event->accept();
                return;
            }
        }
    }

    // Not handled — pass through
    event->ignore();
}

void TransformOverlayWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-click → edit the text layer under the cursor (Premiere Pro).
    // The preceding single click already selected the layer and bound it
    // to the panels (see emptyAreaClicked handler), so we only need to
    // signal "enter text editing" with the frame-space coordinates.
    if (event->button() == Qt::LeftButton) {
        if (m_vulkanVp) {
            // Same coord-space caveat as emptyAreaClicked: events forwarded
            // from m_vulkanVp's native QWindow are in HWND-local coords, so
            // map the click directly through the GPU draw rect rather than
            // computeFrameRect (which is in overlay-widget-local coords).
            QRectF gnorm = m_vulkanVp->gpuFrameRect();
            QSize  surf  = m_vulkanVp->nativeSurfaceSize();
            float srcW = static_cast<float>(m_vulkanVp->srcWidth());
            float srcH = static_cast<float>(m_vulkanVp->srcHeight());
            if (!gnorm.isEmpty() && surf.width() > 0 && surf.height() > 0 &&
                srcW > 0.0f && srcH > 0.0f)
            {
                QPointF wPos = event->position();
                double drawX = gnorm.x() * surf.width();
                double drawY = gnorm.y() * surf.height();
                double drawW = gnorm.width()  * surf.width();
                double drawH = gnorm.height() * surf.height();
                float frameX = static_cast<float>((wPos.x() - drawX) / drawW) * srcW;
                float frameY = static_cast<float>((wPos.y() - drawY) / drawH) * srcH;
                emit textEditRequested(frameX, frameY);
                event->accept();
                return;
            }
        }
    }
    event->ignore();
}

bool TransformOverlayWidget::isInlineTextEditing() const noexcept
{
    return m_inlineTextEdit && m_inlineTextEdit->isVisible();
}

void TransformOverlayWidget::beginInlineTextEdit(const QString& initial,
                                                  const QString& fontFamily,
                                                  float fontSizeRef,
                                                  int fontWeight,
                                                  bool italic,
                                                  const QColor& textColor,
                                                  float horizontalStretch)
{
    // Bounding box of the selected layer in widget coords (same corners
    // used to draw the transform box). Falls back to a centred box if the
    // overlay geometry isn't available yet.
    QRectF box;
    {
        QPointF c[4];
        computeOverlayCorners(c);
        double minX = c[0].x(), minY = c[0].y(), maxX = c[0].x(), maxY = c[0].y();
        for (int i = 1; i < 4; ++i) {
            minX = std::min(minX, c[i].x()); maxX = std::max(maxX, c[i].x());
            minY = std::min(minY, c[i].y()); maxY = std::max(maxY, c[i].y());
        }
        box = QRectF(minX, minY, maxX - minX, maxY - minY);
    }
    if (box.width() < 8.0 || box.height() < 8.0) {
        const int w = std::min(width() - 40, 360);
        box = QRectF((width() - w) / 2.0, height() / 2.0 - 18.0,
                     std::max(120, w), 36.0);
    }
    // Keep the editor centered on the rendered text. Earlier code
    // enforced a 120×28 minimum anchored at the box's top-left, which
    // made the editor grow down-and-right of small transform boxes —
    // both off-center and visually much larger than the rendered text.
    // Center any minimum-size expansion around the transform box's
    // centroid, and use a small floor (40×16) that won't dominate the
    // rendered text for typical font sizes.
    constexpr double kMinW = 40.0;
    constexpr double kMinH = 16.0;
    double cx = box.x() + box.width()  * 0.5;
    double cy = box.y() + box.height() * 0.5;
    double w  = std::max(kMinW, box.width());
    double h  = std::max(kMinH, box.height());
    QRect g(static_cast<int>(std::round(cx - w * 0.5)),
            static_cast<int>(std::round(cy - h * 0.5)),
            static_cast<int>(std::round(w)),
            static_cast<int>(std::round(h)));
    QRect clamped = g.intersected(rect().adjusted(2, 2, -2, -2));
    if (clamped.width() < 8 || clamped.height() < 8) {
        // Degenerate (e.g. overlay not laid out yet) — fall back to a
        // small editor centered in the overlay.
        clamped = QRect((width() - 200) / 2, (height() - 32) / 2, 200, 32)
                      .intersected(rect().adjusted(2, 2, -2, -2));
    }

    if (!m_inlineTextEdit) {
        // Independent top-level frameless window. A child of this overlay
        // inherits its WindowDoesNotAcceptFocus, so it could never accept
        // keyboard input; a Tool window parented to the overlay has the
        // same issue plus loses focus when the app deactivates. A real
        // top-level Window with no parent is the only reliable way to get
        // both visibility above the native Vulkan surface AND keyboard
        // focus. Ownership: we delete it in the overlay destructor.
        m_inlineTextEdit = new QLineEdit(nullptr);
        m_inlineTextEdit->setWindowFlags(Qt::Window
                                         | Qt::FramelessWindowHint
                                         | Qt::WindowStaysOnTopHint
                                         | Qt::NoDropShadowWindowHint);
        // WA_TranslucentBackground is THE attribute that lets rgba(...)
        // in the stylesheet actually composite over the screen. Without
        // it the top-level widget has an opaque backing surface and the
        // rgba alpha just blends within that surface — i.e. you get a
        // solid grey/black box no matter what alpha you specify.
        m_inlineTextEdit->setAttribute(Qt::WA_TranslucentBackground, true);
        m_inlineTextEdit->setAttribute(Qt::WA_NoSystemBackground, true);
        m_inlineTextEdit->setAttribute(Qt::WA_ShowWithoutActivating, false);
        m_inlineTextEdit->setAttribute(Qt::WA_DeleteOnClose, false);
        m_inlineTextEdit->setObjectName(QStringLiteral("inlineTextEdit"));
        m_inlineTextEdit->setAlignment(Qt::AlignCenter);
        auto commit = [this]() {
            if (!m_inlineTextEdit || !m_inlineTextEdit->isVisible()) return;
            if (m_committingInlineText) return;
            m_committingInlineText = true;
            const QString t = m_inlineTextEdit->text();
            m_inlineTextEdit->hide();
            emit inlineTextCommitted(t);
            m_committingInlineText = false;
            setFocus();
        };
        connect(m_inlineTextEdit, &QLineEdit::returnPressed, this, commit);
        connect(m_inlineTextEdit, &QLineEdit::editingFinished, this, commit);

        // Auto-grow as the user types: QLineEdit doesn't expand to fit its
        // content, it just scrolls. Recompute the screen geometry from the
        // current text's pixel width and keep it anchored on the original
        // transform-box center so the edit area grows symmetrically.
        connect(m_inlineTextEdit, &QLineEdit::textChanged, this,
                [this](const QString& t) {
            if (!m_inlineTextEdit) return;
            QFontMetricsF fm(m_inlineTextEdit->font());
            // Slack of one average char-width keeps the caret visible past
            // the last glyph and avoids a one-frame scroll on the next
            // keystroke. Border (1 px each side) + padding 0 + caret slack.
            double slack  = std::max(8.0, fm.averageCharWidth());
            double inkW   = fm.horizontalAdvance(t);
            double wantW  = inkW + 2.0 /*border*/ + slack;
            int    minW   = 40;
            int    newW   = std::max(minW, static_cast<int>(std::ceil(wantW)));
            int    newH   = (m_inlineEditHeight > 0) ? m_inlineEditHeight
                                                     : m_inlineTextEdit->height();
            QRect r(m_inlineEditCenter.x() - newW / 2,
                    m_inlineEditCenter.y() - newH / 2,
                    newW, newH);
            if (r != m_inlineTextEdit->geometry())
                m_inlineTextEdit->setGeometry(r);
        });
    }

    // Position in GLOBAL screen coordinates: a top-level window's geometry
    // is screen-relative, not parent-relative.
    QPoint globalTL = mapToGlobal(clamped.topLeft());
    QRect screenRect(globalTL, clamped.size());

    // Match the renderer's font sizing exactly. renderGraphicClip()
    // builds its QFont with POINT size and rasterises into a canvas at
    // the PROJECT/sequence resolution, which is then downscaled to the
    // composite output and displayed in the on-screen frame rect. So
    // the on-screen point size = layer pointSize × frameRect.height /
    // projectHeight. The reference height here MUST be the project
    // resolution (m_seqH, e.g. 2160 for a 4K project), NOT a hardcoded
    // 1080 — that hardcode made the inline editor's text ~2× too big
    // during edit for non-1080 projects, then snap back on commit.
    QRectF fr = computeFrameRect();
    double scaleHeight = (fr.height() > 1.0) ? fr.height()
                                              : double(height());
    double refH = (m_seqH > 0) ? static_cast<double>(m_seqH) : 1080.0;
    double fontPt = std::max(6.0, double(fontSizeRef) * scaleHeight / refH);

    // Force opaque alpha on the text color (the layer's fill might be
    // semi-transparent and would otherwise be unreadable while editing).
    QColor visibleColor = textColor;
    visibleColor.setAlpha(255);

    // Bake the font into the stylesheet too — setFont alone can be
    // overridden by stylesheet font defaults on some platforms. Background
    // uses rgba alpha now that WA_TranslucentBackground is enabled, so it
    // actually composites over the underlying video instead of looking
    // like a solid black box.
    const QString styleSheet = QStringLiteral(
        "QLineEdit { "
        "font-family: \"%1\"; "
        "font-size: %2pt; "
        "font-weight: %3; "
        "font-style: %4; "
        "background: rgba(20,20,20,110); "
        "color: %5; "
        "border: 1px solid rgba(77,158,255,200); "
        "selection-background-color: rgba(77,158,255,180); "
        "selection-color: white; "
        "padding: 0px; }")
        .arg(fontFamily)
        .arg(fontPt, 0, 'f', 1)
        .arg(std::clamp(fontWeight, 1, 1000))
        .arg(italic ? "italic" : "normal")
        .arg(visibleColor.name(QColor::HexRgb));
    QFont qf(fontFamily, -1, std::clamp(fontWeight, 1, 1000), italic);
    qf.setPointSizeF(fontPt);
    // Anisotropic-scale support: the renderer applies painter.scale(sx, sy)
    // which stretches glyph WIDTHS by sx/sy relative to height. QFont has
    // no general anisotropic transform, but setStretch() is exactly this:
    // a percentage applied to glyph advance/width. QSS has no font-stretch
    // property so setStyleSheet below won't override it. QFontMetricsF
    // honours it too, so the textChanged auto-grow stays accurate.
    if (std::isfinite(horizontalStretch) && horizontalStretch > 0.0f) {
        const int stretchPct = std::clamp(
            static_cast<int>(std::round(horizontalStretch * 100.0f)), 1, 4000);
        qf.setStretch(stretchPct);
    }
    m_inlineTextEdit->setFont(qf);
    m_inlineTextEdit->setStyleSheet(styleSheet);

    // Remember the editor's screen-space anchor so the textChanged handler
    // can grow/shrink the geometry symmetrically as the user types.
    m_inlineEditCenter = screenRect.center();
    m_inlineEditHeight = screenRect.height();

    m_inlineTextEdit->setGeometry(screenRect);
    m_inlineTextEdit->setText(initial);
    m_inlineTextEdit->show();
    m_inlineTextEdit->raise();
    // Defer activation to the next event-loop tick so the window is fully
    // realised before Windows is asked to give it foreground focus. Same
    // pattern PropertiesPanel::focusGraphicTextField uses, for the same
    // "setFocus is ignored on not-yet-shown widget" reason.
    QPointer<QLineEdit> edit(m_inlineTextEdit);
    QTimer::singleShot(0, edit, [edit]() {
        if (!edit) return;
        edit->activateWindow();
        edit->setFocus(Qt::MouseFocusReason);
        edit->selectAll();
    });
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

    // ── Motion-path spatial handle drag ─────────────────────────────────
    if (m_dragMode == DragMode::DragMotionHandle && m_motionX && m_motionY &&
        (event->buttons() & Qt::LeftButton))
    {
        const QRectF fr = computeFrameRect();
        if (fr.isEmpty()) { event->accept(); return; }
        if (m_dragMotionKfIdx < 0 ||
            m_dragMotionKfIdx >= static_cast<int>(m_motionX->keyframeCount())) {
            event->accept();
            return;
        }
        auto& kfx = m_motionX->keyframe(static_cast<size_t>(m_dragMotionKfIdx));
        auto& kfy = m_motionY->keyframe(static_cast<size_t>(m_dragMotionKfIdx));

        // Convert widget pos to REF-1920 px and subtract the waypoint
        // position to get the handle offset.
        const QPointF refPos = widgetToRef(wPos, fr);
        const float newHX = static_cast<float>(refPos.x()) - kfx.value;
        const float newHY = static_cast<float>(refPos.y()) - kfy.value;

        if (m_dragMotionIsIn) {
            kfx.spatialInX = newHX;
            kfy.spatialInY = newHY;
            // Continuous Bezier: mirror the out handle collinearly with same length.
            if (kfx.spatialInterp == InterpMode::ContinuousBezier) {
                kfx.spatialOutX = -newHX;
                kfy.spatialOutY = -newHY;
            }
        } else {
            kfx.spatialOutX = newHX;
            kfy.spatialOutY = newHY;
            if (kfx.spatialInterp == InterpMode::ContinuousBezier) {
                kfx.spatialInX = -newHX;
                kfy.spatialInY = -newHY;
            }
        }
        update();
        emit motionPathLiveUpdate();
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

    // Text/Type tool: always show the I-beam over the monitor (Premiere
    // Pro behavior) — even when a clip is selected and its transform
    // handles are visible.
    if (m_editTool == 6 && m_dragMode == DragMode::None) {
        applyCursor(Qt::IBeamCursor);
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
        else
            applyCursor(Qt::ArrowCursor);   // body = plain arrow (no hand)
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

    if (m_dragMode == DragMode::DragMotionHandle) {
        // Record an undoable command capturing the spatial-handle change.
        if (m_motionCmdStack && m_motionX && m_motionY &&
            m_dragMotionKfIdx >= 0)
        {
            auto* tx = m_motionX;
            auto* ty = m_motionY;
            const int64_t kfTime = m_dragKfTime;
            const float oldInX  = m_dragOrigInX,  oldInY  = m_dragOrigInY;
            const float oldOutX = m_dragOrigOutX, oldOutY = m_dragOrigOutY;
            const float newInX  = (m_dragMotionKfIdx < static_cast<int>(tx->keyframeCount())
                                   ? tx->keyframe(static_cast<size_t>(m_dragMotionKfIdx)).spatialInX  : 0.0f);
            const float newInY  = (m_dragMotionKfIdx < static_cast<int>(ty->keyframeCount())
                                   ? ty->keyframe(static_cast<size_t>(m_dragMotionKfIdx)).spatialInY  : 0.0f);
            const float newOutX = (m_dragMotionKfIdx < static_cast<int>(tx->keyframeCount())
                                   ? tx->keyframe(static_cast<size_t>(m_dragMotionKfIdx)).spatialOutX : 0.0f);
            const float newOutY = (m_dragMotionKfIdx < static_cast<int>(ty->keyframeCount())
                                   ? ty->keyframe(static_cast<size_t>(m_dragMotionKfIdx)).spatialOutY : 0.0f);

            auto applyHandles = [tx, ty, kfTime](float ix, float iy, float ox, float oy) {
                for (size_t i = 0; i < tx->keyframeCount(); ++i)
                    if (tx->keyframe(i).time == kfTime) {
                        tx->keyframe(i).spatialInX  = ix;
                        tx->keyframe(i).spatialOutX = ox;
                        break;
                    }
                for (size_t i = 0; i < ty->keyframeCount(); ++i)
                    if (ty->keyframe(i).time == kfTime) {
                        ty->keyframe(i).spatialInY  = iy;
                        ty->keyframe(i).spatialOutY = oy;
                        break;
                    }
            };
            m_motionCmdStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                "Move Motion-Path Handle",
                [applyHandles, newInX, newInY, newOutX, newOutY]() {
                    applyHandles(newInX, newInY, newOutX, newOutY);
                },
                [applyHandles, oldInX, oldInY, oldOutX, oldOutY]() {
                    applyHandles(oldInX, oldInY, oldOutX, oldOutY);
                }));
        }
        m_dragMode        = DragMode::None;
        m_dragMotionKfIdx = -1;
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

    case QEvent::MouseButtonDblClick:
        // Without this, Qt's auto-generated double-click never reaches our
        // mouseDoubleClickEvent — the user double-clicks a text layer, the
        // event fires on the native Vulkan window, and the overlay filter
        // drops it. Forwarding here is what enables the in-place text
        // editor flow.
        mouseDoubleClickEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    case QEvent::MouseMove:
        mouseMoveEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    case QEvent::MouseButtonRelease:
        mouseReleaseEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    case QEvent::Leave:
        // Pointer left the Vulkan surface — drop any override cursor so it
        // doesn't persist application-wide outside the viewport.
        if (m_dragMode == DragMode::None)
            clearCursorOverride();
        break;

    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}


} // namespace rt
