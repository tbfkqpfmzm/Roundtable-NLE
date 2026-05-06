/*
 * ViewportLayout — Aspect-ratio-correct layout computation for viewport display.
 *
 * Step 11: Pure math for mapping between:
 *   - Content space (source resolution, e.g. 1920x1080)
 *   - Widget space (actual widget pixel dimensions)
 *   - Normalized space [0,1]
 *
 * Handles:
 *   - Letterboxing/pillarboxing for aspect ratio preservation
 *   - Pan and zoom (camera offset + zoom level)
 *   - Fit-to-window, percentage zoom modes
 *   - Safe area rectangles (title-safe, action-safe)
 *   - Content-space ↔ widget-space coordinate conversion
 *
 * This class has NO Qt dependency — all math uses plain types.
 */

#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <cmath>

namespace rt {

// ── Zoom mode ───────────────────────────────────────────────────────────────

enum class ZoomMode : uint8_t
{
    FitToWindow,   ///< Automatically scale to fit widget (default)
    Percentage,    ///< Fixed percentage (e.g. 100% = 1:1 pixels)
};

// ── Safe area preset ────────────────────────────────────────────────────────

enum class SafeAreaType : uint8_t
{
    TitleSafe,     ///< 80% of frame (10% inset each side) — text should stay inside
    ActionSafe,    ///< 90% of frame (5% inset each side) — action should stay inside
    Custom,        ///< User-defined inset percentage
};

// ── Safe area rectangle ─────────────────────────────────────────────────────

struct SafeAreaRect
{
    float left{0};
    float top{0};
    float right{0};
    float bottom{0};
    float width() const { return right - left; }
    float height() const { return bottom - top; }
};

// ── Viewport rect (in widget pixel coordinates) ─────────────────────────────

struct ViewportRect
{
    float x{0};       ///< Left edge in widget pixels
    float y{0};       ///< Top edge in widget pixels
    float width{0};   ///< Width in widget pixels
    float height{0};  ///< Height in widget pixels

    bool contains(float px, float py) const
    {
        return px >= x && px < x + width &&
               py >= y && py < y + height;
    }
};

// ═════════════════════════════════════════════════════════════════════════════

class ViewportLayout
{
public:
    ViewportLayout() = default;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the content (source) resolution.
    void setContentSize(uint32_t width, uint32_t height);

    /// Set the widget (display) size in pixels.
    void setWidgetSize(uint32_t width, uint32_t height);

    /// Set zoom mode.
    void setZoomMode(ZoomMode mode);

    /// Set zoom percentage (only applies when mode = Percentage).
    /// 1.0 = 100% (1:1 pixels), 2.0 = 200%, 0.5 = 50%, etc.
    void setZoomLevel(float zoom);

    /// Set pan offset in content-space pixels.
    void setPanOffset(float x, float y);

    /// Add delta to pan offset (for dragging).
    void pan(float deltaWidgetX, float deltaWidgetY);

    /// Zoom centered on a widget-space point.
    void zoomAt(float widgetX, float widgetY, float zoomDelta);

    /// Reset pan to center and zoom to fit.
    void resetView();

    // ── Accessors ───────────────────────────────────────────────────────

    [[nodiscard]] uint32_t contentWidth()  const noexcept { return m_contentWidth; }
    [[nodiscard]] uint32_t contentHeight() const noexcept { return m_contentHeight; }
    [[nodiscard]] uint32_t widgetWidth()   const noexcept { return m_widgetWidth; }
    [[nodiscard]] uint32_t widgetHeight()  const noexcept { return m_widgetHeight; }
    [[nodiscard]] ZoomMode zoomMode()      const noexcept { return m_zoomMode; }
    [[nodiscard]] float    zoomLevel()     const noexcept { return m_zoom; }
    [[nodiscard]] glm::vec2 panOffset()    const noexcept { return m_pan; }

    /// Get the effective zoom level (for FitToWindow mode, this is auto-calculated).
    [[nodiscard]] float effectiveZoom() const;

    /// Get the content display rect in widget coordinates (the letterboxed area).
    [[nodiscard]] ViewportRect contentRect() const;

    /// Get the aspect ratio of the content (width / height).
    [[nodiscard]] float contentAspect() const;

    /// Get the aspect ratio of the widget (width / height).
    [[nodiscard]] float widgetAspect() const;

    // ── Coordinate conversion ───────────────────────────────────────────

    /// Convert widget pixel coordinates to content-space coordinates.
    /// Returns the position in the source image (e.g. 0-1920, 0-1080).
    [[nodiscard]] glm::vec2 widgetToContent(float widgetX, float widgetY) const;

    /// Convert content-space coordinates to widget pixel coordinates.
    [[nodiscard]] glm::vec2 contentToWidget(float contentX, float contentY) const;

    /// Convert widget pixel coordinates to normalized [0,1] content coordinates.
    [[nodiscard]] glm::vec2 widgetToNormalized(float widgetX, float widgetY) const;

    /// Convert normalized [0,1] content coordinates to widget pixels.
    [[nodiscard]] glm::vec2 normalizedToWidget(float normX, float normY) const;

    /// Check if a widget coordinate lies within the content display area.
    [[nodiscard]] bool isInsideContent(float widgetX, float widgetY) const;

    // ── Safe areas ──────────────────────────────────────────────────────

    /// Get a safe area rectangle in widget coordinates.
    [[nodiscard]] SafeAreaRect safeArea(SafeAreaType type, float customInset = 0.0f) const;

    /// Get a safe area rectangle in content coordinates.
    [[nodiscard]] SafeAreaRect safeAreaContent(SafeAreaType type, float customInset = 0.0f) const;

    // ── Zoom helpers ────────────────────────────────────────────────────

    /// Get the zoom required to fit content in widget.
    [[nodiscard]] float fitZoom() const;

    /// Get the zoom required to fill widget with content (may crop).
    [[nodiscard]] float fillZoom() const;

    /// Constrain zoom to valid range.
    static float clampZoom(float zoom);

    // ── Constants ───────────────────────────────────────────────────────

    static constexpr float kMinZoom = 0.05f;     ///< 5%
    static constexpr float kMaxZoom = 32.0f;     ///< 3200%
    static constexpr float kZoomStep = 0.1f;     ///< Per scroll notch
    static constexpr float kTitleSafeInset = 0.10f;   ///< 10% each side
    static constexpr float kActionSafeInset = 0.05f;  ///< 5% each side

private:
    uint32_t m_contentWidth{1920};
    uint32_t m_contentHeight{1080};
    uint32_t m_widgetWidth{800};
    uint32_t m_widgetHeight{450};

    ZoomMode m_zoomMode{ZoomMode::FitToWindow};
    float    m_zoom{1.0f};        ///< Zoom level (only for Percentage mode)
    glm::vec2 m_pan{0.0f, 0.0f}; ///< Pan offset in content-space pixels
};

// ═════════════════════════════════════════════════════════════════════════════
// GizmoHandle — Individual handle on the transform gizmo
// ═════════════════════════════════════════════════════════════════════════════

enum class GizmoHandle : uint8_t
{
    None = 0,
    Center,         ///< Move the layer (drag to translate)
    TopLeft,        ///< Scale from top-left corner
    TopRight,       ///< Scale from top-right corner
    BottomLeft,     ///< Scale from bottom-left corner
    BottomRight,    ///< Scale from bottom-right corner
    TopEdge,        ///< Scale vertically from top
    BottomEdge,     ///< Scale vertically from bottom
    LeftEdge,       ///< Scale horizontally from left
    RightEdge,      ///< Scale horizontally from right
    RotateTopLeft,  ///< Rotation handle (outside top-left corner)
    RotateTopRight,
    RotateBottomLeft,
    RotateBottomRight,
};

// ── Gizmo configuration ────────────────────────────────────────────────────

struct GizmoConfig
{
    float handleSize{8.0f};          ///< Handle square size in widget pixels
    float rotateHandleOffset{20.0f}; ///< Distance of rotation handles from corner
    float hitPadding{4.0f};          ///< Extra hit-test padding around handles
    float lineWidth{1.5f};           ///< Gizmo outline width
};

// ── Transform gizmo state ───────────────────────────────────────────────────

struct GizmoTransform
{
    glm::vec2 position{0.0f, 0.0f};  ///< Content-space position (top-left of layer)
    glm::vec2 size{1920.0f, 1080.0f}; ///< Content-space size
    float rotation{0.0f};             ///< Rotation in degrees
    glm::vec2 anchor{0.5f, 0.5f};    ///< Anchor point in normalized coords
};

// ── Gizmo anchor point positions (in widget pixels) ─────────────────────────

struct GizmoHandlePositions
{
    glm::vec2 center;
    glm::vec2 topLeft, topRight, bottomLeft, bottomRight;
    glm::vec2 topEdge, bottomEdge, leftEdge, rightEdge;
    glm::vec2 rotTopLeft, rotTopRight, rotBottomLeft, rotBottomRight;
};

// ═════════════════════════════════════════════════════════════════════════════

class GizmoMath
{
public:
    GizmoMath() = default;

    /// Set the gizmo configuration.
    void setConfig(const GizmoConfig& config) { m_config = config; }

    /// Set the viewport layout (for coordinate conversion).
    void setLayout(const ViewportLayout* layout) { m_layout = layout; }

    // ── Handle positions ────────────────────────────────────────────────

    /// Compute handle positions in widget coordinates for a given transform.
    [[nodiscard]] GizmoHandlePositions computeHandles(const GizmoTransform& transform) const;

    // ── Hit testing ─────────────────────────────────────────────────────

    /// Test which handle (if any) is hit at widget coordinates.
    [[nodiscard]] GizmoHandle hitTest(const GizmoTransform& transform,
                                      float widgetX, float widgetY) const;

    // ── Drag computation ────────────────────────────────────────────────

    /// Compute new transform after dragging a handle.
    /// @param original     The transform before drag started
    /// @param handle       Which handle is being dragged
    /// @param startWidget  Widget coords where drag started
    /// @param currentWidget Current widget coords
    /// @param shiftHeld    True if shift key is held (constrain proportions)
    /// @return Updated transform
    [[nodiscard]] GizmoTransform applyDrag(const GizmoTransform& original,
                                            GizmoHandle handle,
                                            glm::vec2 startWidget,
                                            glm::vec2 currentWidget,
                                            bool shiftHeld = false) const;

    /// Compute rotation angle from drag.
    [[nodiscard]] float computeRotation(const GizmoTransform& original,
                                         glm::vec2 startWidget,
                                         glm::vec2 currentWidget) const;

    // ── Utility ─────────────────────────────────────────────────────────

    /// Check if a point is near a handle (within hit radius).
    [[nodiscard]] bool isNearHandle(glm::vec2 point, glm::vec2 handle) const;

    /// Get the cursor style hint for a handle.
    [[nodiscard]] static int cursorForHandle(GizmoHandle handle);

private:
    GizmoConfig          m_config;
    const ViewportLayout* m_layout{nullptr};
};

} // namespace rt
