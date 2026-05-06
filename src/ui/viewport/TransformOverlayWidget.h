/*
 * TransformOverlayWidget — Transparent overlay for transform handles + pan.
 *
 * Sits on top of VulkanViewport (which cannot use QPainter) and provides:
 *   - Bounding box + corner handles for selected clip transform / scale
 *   - Middle-mouse-button panning (forwarded to VulkanViewport)
 *   - Cursor hints (resize arrows, move hand, etc.)
 *
 * This widget is fully transparent except for the painted overlay elements.
 * Mouse events that don't hit a handle or the body are ignored so they can
 * fall through to the VulkanViewport/native-window below.
 */

#pragma once

#include <QWidget>
#include <QPointF>
#include <QRectF>

#include <cstdint>
#include <cmath>
#include <vector>

#include "viewport/OverlayMath.h"
#include "timeline/OpacityMask.h"

namespace rt {

class VulkanViewport;

/// Transparent overlay widget for transform gizmo + middle-mouse pan.
class TransformOverlayWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TransformOverlayWidget(VulkanViewport* viewport, QWidget* parent = nullptr);
    ~TransformOverlayWidget() override = default;

    /// Show / update the transform overlay.
    void setTransformOverlay(const TransformOverlayInfo& info);

    /// Hide the overlay.
    void clearTransformOverlay();

    /// Toggle safe area guides overlay.
    void setSafeAreasVisible(bool visible);

    /// Toggle rule-of-thirds grid overlay.
    void setGridVisible(bool visible);

    /// Set the active editing tool (so overlay knows when Text tool is active).
    void setEditTool(uint8_t tool) noexcept;

    /// Set the offset from the overlay's top-left to the viewport's top-left.
    /// When the overlay is clipped to the panel bounds, this offset lets
    /// computeFrameRect() use the viewport's full unclipped dimensions.
    void setViewportOffset(const QPoint& offset) noexcept { m_vpOffset = offset; }

    /// Set mask data for overlay drawing + editing. Call after mask changes.
    void setMasks(std::vector<OpacityMask>* masks) noexcept {
        m_masks = masks;
        if (!m_masks || m_masks->empty()) {
            m_activeMaskIndex = -1;
        }
        update();
    }

    /// Set which mask is actively selected for editing (-1 = all drawn, none focused).
    void setActiveMaskIndex(int idx) noexcept { m_activeMaskIndex = idx; update(); }

    /// Get the currently active mask index (-1 = none).
    [[nodiscard]] int activeMaskIndex() const noexcept { return m_activeMaskIndex; }

    /// Are safe areas currently visible?
    [[nodiscard]] bool safeAreasVisible() const noexcept { return m_showSafeAreas; }

    /// Is the grid overlay currently visible?
    [[nodiscard]] bool gridVisible() const noexcept { return m_showGrid; }

    /// Get current overlay info.
    [[nodiscard]] const TransformOverlayInfo& transformOverlay() const noexcept { return m_overlay; }

signals:
    /// Emitted when the user drags the body to change position (ref-space).
    void transformPositionChanged(float posX, float posY);

    /// Emitted when the user drag-scales via corner handles.
    void transformScaleChanged(float scaleX, float scaleY);

    /// Emitted when the user drag-rotates via outside-corner handles.
    void transformRotationChanged(float rotation);

    /// Emitted when the transform drag completes (for undo recording).
    void transformDragFinished(float oldPosX, float oldPosY, float oldScX, float oldScY, float oldRot,
                               float newPosX, float newPosY, float newScX, float newScY, float newRot);

    /// Emitted when a mask drag completes for undo recording.
    void maskDragFinished(int maskIndex, OpacityMask oldMask, OpacityMask newMask);

    /// Emitted during mask drag to trigger live composite refresh.
    void maskLiveUpdate();

    /// Emitted when the user clicks on empty area (no handle/body hit).
    /// Coordinates are in frame-space (0..outputWidth, 0..outputHeight).
    void emptyAreaClicked(float frameX, float frameY);

    /// Emitted when the eyedropper tool picks a color at frame-space coords.
    void colorPicked(float frameX, float frameY);

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Compute the frame draw-rect (where the frame appears in widget space).
    QRectF computeFrameRect() const;

    /// Map frame-space point to widget-space.
    QPointF frameToWidget(const QPointF& fp) const;

    /// Compute the 4 widget-space corners of the overlay bounding box.
    void computeOverlayCorners(QPointF corners[4]) const;

    /// Hit-test a corner handle; returns 0–3 or -1.
    int hitTestHandle(const QPointF& widgetPos) const;

    /// Hit-test the rotation zone (outside but near a corner). Returns 0–3 or -1.
    int hitTestRotate(const QPointF& widgetPos) const;

    /// Hit-test inside the overlay body.
    bool hitTestBody(const QPointF& widgetPos) const;

    /// Hit-test mask control points. Returns handle index (or -1) via outHandle,
    /// and sets outMaskIndex to the mask that was hit.
    int hitTestMaskHandle(const QPointF& widgetPos, int& outMaskIndex) const;

    /// Hit-test inside a mask shape body. Returns the mask index (or -1).
    int hitTestMaskBody(const QPointF& widgetPos) const;

    /// Draw the transform overlay (bounding box + handles).
    void drawTransformOverlay(class QPainter& painter);

    /// Draw mask shape overlays (blue outlines + control points).
    void drawMaskOverlay(class QPainter& painter);

    /// Add a control point to the closest mask edge near widgetPos.
    /// Returns true if a point was added; sets outMaskIndex.
    bool addPointOnMaskEdge(const QPointF& widgetPos, const QRectF& fr, int& outMaskIndex);

    /// Hit-test whether widgetPos is near a mask edge (for pen cursor hint).
    bool hitTestMaskEdge(const QPointF& widgetPos) const;

    /// Draw safe area guides (action-safe + title-safe).
    void drawSafeAreas(class QPainter& painter);

    /// Draw rule-of-thirds grid.
    void drawGrid(class QPainter& painter);

    /// Set cursor on the native QWindow (which is on top and receives events).
    void applyCursor(Qt::CursorShape shape);
    void applyCursor(const QCursor& cursor);

    /// Build a Premiere-style curved-arrow rotation cursor.
    static QCursor rotateCursor();

    /// Build a pen cursor for add-point-on-edge feedback.
    static QCursor penCursor();

    /// Build a magnifying-glass zoom cursor matching Premiere Pro.
    static QCursor zoomCursor();

    VulkanViewport* m_vulkanVp{nullptr};

    TransformOverlayInfo m_overlay;

    enum class DragMode : uint8_t { None, MoveBody, ScaleCorner, RotateCorner, Pan, DragMaskPoint };
    DragMode m_dragMode{DragMode::None};
    int      m_dragHandle{-1};
    QPointF  m_dragStartWidget;
    float    m_dragStartPosX{0.0f};
    float    m_dragStartPosY{0.0f};
    float    m_dragStartScX{1.0f};
    float    m_dragStartScY{1.0f};
    float    m_dragStartRot{0.0f};
    float    m_dragStartAngle{0.0f}; ///< Angle from center at drag start (for rotation)

    // Pan state (middle mouse)
    QPointF  m_panStartPos;
    float    m_panStartVpX{0.0f};
    float    m_panStartVpY{0.0f};

    // Safe area guides
    bool     m_showSafeAreas{false};

    // Rule-of-thirds grid
    bool     m_showGrid{false};

    // Active editing tool (uses uint8_t to avoid EditTool dependency)
    uint8_t  m_editTool{0};  // 0 = Selection, 6 = Text

    // Mask overlay data (non-owning pointer to clip's masks vector)
    std::vector<OpacityMask>* m_masks{nullptr};

    // Mask drag state
    int m_dragMaskIndex{-1};
    int m_dragMaskHandle{-1};
    OpacityMask m_dragStartMask{};

    // Mask hover state (for handle glow)
    int m_hoverMaskIndex{-1};
    int m_hoverMaskHandle{-1};

    // Active mask index (-1 = all masks visible, none focused)
    int m_activeMaskIndex{-1};

    // Offset from overlay origin to viewport origin (overlay is clipped to panel)
    QPoint m_vpOffset{0, 0};
};

} // namespace rt
