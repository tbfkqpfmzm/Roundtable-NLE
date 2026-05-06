/*
 * ViewportLayout.cpp — Aspect-ratio-correct layout + gizmo math.
 * Step 11: Viewport Widget
 */

#include "ViewportLayout.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// ViewportLayout
// ═════════════════════════════════════════════════════════════════════════════

// ── Configuration ───────────────────────────────────────────────────────────

void ViewportLayout::setContentSize(uint32_t width, uint32_t height)
{
    m_contentWidth  = std::max(width, 1u);
    m_contentHeight = std::max(height, 1u);
}

void ViewportLayout::setWidgetSize(uint32_t width, uint32_t height)
{
    m_widgetWidth  = std::max(width, 1u);
    m_widgetHeight = std::max(height, 1u);
}

void ViewportLayout::setZoomMode(ZoomMode mode)
{
    m_zoomMode = mode;
}

void ViewportLayout::setZoomLevel(float zoom)
{
    m_zoom = clampZoom(zoom);
}

void ViewportLayout::setPanOffset(float x, float y)
{
    m_pan = glm::vec2(x, y);
}

void ViewportLayout::pan(float deltaWidgetX, float deltaWidgetY)
{
    // Convert widget-space delta to content-space delta
    float z = effectiveZoom();
    if (z > 0.0f)
    {
        m_pan.x += deltaWidgetX / z;
        m_pan.y += deltaWidgetY / z;
    }
}

void ViewportLayout::zoomAt(float widgetX, float widgetY, float zoomDelta)
{
    // Get content point under cursor before zoom
    glm::vec2 contentPoint = widgetToContent(widgetX, widgetY);

    // Apply zoom
    float oldZoom = effectiveZoom();
    m_zoomMode = ZoomMode::Percentage;
    m_zoom = clampZoom(oldZoom * (1.0f + zoomDelta * kZoomStep));

    // Adjust pan so the content point stays under the cursor
    // After zoom: contentToWidget(contentPoint) should equal (widgetX, widgetY)
    // contentToWidget = (contentPos + pan) * newZoom + offset
    // We need to find new pan such that this holds

    float newZoom = effectiveZoom();
    ViewportRect rect = contentRect();

    // Widget position of content origin after zoom (without pan adjustment)
    float newWidgetX = rect.x + (contentPoint.x * newZoom);
    float newWidgetY = rect.y + (contentPoint.y * newZoom);

    // Adjust pan to keep cursor at the same content point
    float diffX = widgetX - newWidgetX;
    float diffY = widgetY - newWidgetY;

    if (newZoom > 0.0f)
    {
        m_pan.x += diffX / newZoom;
        m_pan.y += diffY / newZoom;
    }
}

void ViewportLayout::resetView()
{
    m_zoomMode = ZoomMode::FitToWindow;
    m_pan = glm::vec2(0.0f);
}

// ── Accessors ───────────────────────────────────────────────────────────────

float ViewportLayout::effectiveZoom() const
{
    if (m_zoomMode == ZoomMode::FitToWindow)
        return fitZoom();
    return m_zoom;
}

ViewportRect ViewportLayout::contentRect() const
{
    float z = effectiveZoom();
    float displayW = static_cast<float>(m_contentWidth) * z;
    float displayH = static_cast<float>(m_contentHeight) * z;

    // Center in widget
    float x = (static_cast<float>(m_widgetWidth) - displayW) * 0.5f;
    float y = (static_cast<float>(m_widgetHeight) - displayH) * 0.5f;

    // Apply pan (in content-space, scaled to widget-space)
    x += m_pan.x * z;
    y += m_pan.y * z;

    return {x, y, displayW, displayH};
}

float ViewportLayout::contentAspect() const
{
    return static_cast<float>(m_contentWidth) / static_cast<float>(m_contentHeight);
}

float ViewportLayout::widgetAspect() const
{
    return static_cast<float>(m_widgetWidth) / static_cast<float>(m_widgetHeight);
}

// ── Coordinate conversion ───────────────────────────────────────────────────

glm::vec2 ViewportLayout::widgetToContent(float widgetX, float widgetY) const
{
    ViewportRect rect = contentRect();
    float z = effectiveZoom();
    if (z <= 0.0f) return glm::vec2(0.0f);

    float contentX = (widgetX - rect.x) / z;
    float contentY = (widgetY - rect.y) / z;
    return glm::vec2(contentX, contentY);
}

glm::vec2 ViewportLayout::contentToWidget(float contentX, float contentY) const
{
    ViewportRect rect = contentRect();
    float z = effectiveZoom();

    float widgetX = rect.x + contentX * z;
    float widgetY = rect.y + contentY * z;
    return glm::vec2(widgetX, widgetY);
}

glm::vec2 ViewportLayout::widgetToNormalized(float widgetX, float widgetY) const
{
    glm::vec2 content = widgetToContent(widgetX, widgetY);
    return glm::vec2(
        content.x / static_cast<float>(m_contentWidth),
        content.y / static_cast<float>(m_contentHeight)
    );
}

glm::vec2 ViewportLayout::normalizedToWidget(float normX, float normY) const
{
    return contentToWidget(
        normX * static_cast<float>(m_contentWidth),
        normY * static_cast<float>(m_contentHeight)
    );
}

bool ViewportLayout::isInsideContent(float widgetX, float widgetY) const
{
    glm::vec2 content = widgetToContent(widgetX, widgetY);
    return content.x >= 0.0f && content.x < static_cast<float>(m_contentWidth) &&
           content.y >= 0.0f && content.y < static_cast<float>(m_contentHeight);
}

// ── Safe areas ──────────────────────────────────────────────────────────────

SafeAreaRect ViewportLayout::safeArea(SafeAreaType type, float customInset) const
{
    float inset = 0.0f;
    switch (type)
    {
        case SafeAreaType::TitleSafe:  inset = kTitleSafeInset; break;
        case SafeAreaType::ActionSafe: inset = kActionSafeInset; break;
        case SafeAreaType::Custom:     inset = customInset; break;
    }

    // Compute in content space first
    SafeAreaRect contentSafe = safeAreaContent(type, customInset);

    // Convert corners to widget space
    glm::vec2 topLeft     = contentToWidget(contentSafe.left, contentSafe.top);
    glm::vec2 bottomRight = contentToWidget(contentSafe.right, contentSafe.bottom);

    return {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

SafeAreaRect ViewportLayout::safeAreaContent(SafeAreaType type, float customInset) const
{
    float inset = 0.0f;
    switch (type)
    {
        case SafeAreaType::TitleSafe:  inset = kTitleSafeInset; break;
        case SafeAreaType::ActionSafe: inset = kActionSafeInset; break;
        case SafeAreaType::Custom:     inset = customInset; break;
    }

    float w = static_cast<float>(m_contentWidth);
    float h = static_cast<float>(m_contentHeight);

    return {
        w * inset,           // left
        h * inset,           // top
        w * (1.0f - inset),  // right
        h * (1.0f - inset)   // bottom
    };
}

// ── Zoom helpers ────────────────────────────────────────────────────────────

float ViewportLayout::fitZoom() const
{
    float scaleX = static_cast<float>(m_widgetWidth)  / static_cast<float>(m_contentWidth);
    float scaleY = static_cast<float>(m_widgetHeight) / static_cast<float>(m_contentHeight);
    return std::min(scaleX, scaleY);
}

float ViewportLayout::fillZoom() const
{
    float scaleX = static_cast<float>(m_widgetWidth)  / static_cast<float>(m_contentWidth);
    float scaleY = static_cast<float>(m_widgetHeight) / static_cast<float>(m_contentHeight);
    return std::max(scaleX, scaleY);
}

float ViewportLayout::clampZoom(float zoom)
{
    return std::clamp(zoom, kMinZoom, kMaxZoom);
}

// ═════════════════════════════════════════════════════════════════════════════
// GizmoMath
// ═════════════════════════════════════════════════════════════════════════════

GizmoHandlePositions GizmoMath::computeHandles(const GizmoTransform& transform) const
{
    GizmoHandlePositions handles;

    if (!m_layout) return handles;

    // Get the four corners in content space
    float l = transform.position.x;
    float t = transform.position.y;
    float r = l + transform.size.x;
    float b = t + transform.size.y;
    float cx = (l + r) * 0.5f;
    float cy = (t + b) * 0.5f;

    // Convert to widget space
    handles.topLeft     = m_layout->contentToWidget(l, t);
    handles.topRight    = m_layout->contentToWidget(r, t);
    handles.bottomLeft  = m_layout->contentToWidget(l, b);
    handles.bottomRight = m_layout->contentToWidget(r, b);

    handles.topEdge    = m_layout->contentToWidget(cx, t);
    handles.bottomEdge = m_layout->contentToWidget(cx, b);
    handles.leftEdge   = m_layout->contentToWidget(l, cy);
    handles.rightEdge  = m_layout->contentToWidget(r, cy);

    handles.center = m_layout->contentToWidget(cx, cy);

    // Rotation handles are offset outside the bounding box corners
    float off = m_config.rotateHandleOffset;
    glm::vec2 dirs[4] = {
        glm::normalize(handles.topLeft - handles.center),
        glm::normalize(handles.topRight - handles.center),
        glm::normalize(handles.bottomLeft - handles.center),
        glm::normalize(handles.bottomRight - handles.center),
    };

    // Avoid NaN for degenerate (zero-size) transforms
    for (auto& d : dirs)
    {
        if (std::isnan(d.x) || std::isnan(d.y))
            d = glm::vec2(0.0f);
    }

    handles.rotTopLeft     = handles.topLeft + dirs[0] * off;
    handles.rotTopRight    = handles.topRight + dirs[1] * off;
    handles.rotBottomLeft  = handles.bottomLeft + dirs[2] * off;
    handles.rotBottomRight = handles.bottomRight + dirs[3] * off;

    return handles;
}

GizmoHandle GizmoMath::hitTest(const GizmoTransform& transform,
                                float widgetX, float widgetY) const
{
    if (!m_layout) return GizmoHandle::None;

    auto handles = computeHandles(transform);
    glm::vec2 p(widgetX, widgetY);

    // Test rotation handles first (outermost)
    if (isNearHandle(p, handles.rotTopLeft))     return GizmoHandle::RotateTopLeft;
    if (isNearHandle(p, handles.rotTopRight))    return GizmoHandle::RotateTopRight;
    if (isNearHandle(p, handles.rotBottomLeft))  return GizmoHandle::RotateBottomLeft;
    if (isNearHandle(p, handles.rotBottomRight)) return GizmoHandle::RotateBottomRight;

    // Test corner handles
    if (isNearHandle(p, handles.topLeft))     return GizmoHandle::TopLeft;
    if (isNearHandle(p, handles.topRight))    return GizmoHandle::TopRight;
    if (isNearHandle(p, handles.bottomLeft))  return GizmoHandle::BottomLeft;
    if (isNearHandle(p, handles.bottomRight)) return GizmoHandle::BottomRight;

    // Test edge handles
    if (isNearHandle(p, handles.topEdge))    return GizmoHandle::TopEdge;
    if (isNearHandle(p, handles.bottomEdge)) return GizmoHandle::BottomEdge;
    if (isNearHandle(p, handles.leftEdge))   return GizmoHandle::LeftEdge;
    if (isNearHandle(p, handles.rightEdge))  return GizmoHandle::RightEdge;

    // Test center (inside the bounding box)
    // Check if point is inside the axis-aligned bounding box in widget space
    float minX = std::min({handles.topLeft.x, handles.topRight.x,
                           handles.bottomLeft.x, handles.bottomRight.x});
    float maxX = std::max({handles.topLeft.x, handles.topRight.x,
                           handles.bottomLeft.x, handles.bottomRight.x});
    float minY = std::min({handles.topLeft.y, handles.topRight.y,
                           handles.bottomLeft.y, handles.bottomRight.y});
    float maxY = std::max({handles.topLeft.y, handles.topRight.y,
                           handles.bottomLeft.y, handles.bottomRight.y});

    if (widgetX >= minX && widgetX <= maxX &&
        widgetY >= minY && widgetY <= maxY)
    {
        return GizmoHandle::Center;
    }

    return GizmoHandle::None;
}

GizmoTransform GizmoMath::applyDrag(const GizmoTransform& original,
                                      GizmoHandle handle,
                                      glm::vec2 startWidget,
                                      glm::vec2 currentWidget,
                                      bool shiftHeld) const
{
    if (!m_layout) return original;

    GizmoTransform result = original;

    // Convert widget delta to content delta
    glm::vec2 startContent   = m_layout->widgetToContent(startWidget.x, startWidget.y);
    glm::vec2 currentContent = m_layout->widgetToContent(currentWidget.x, currentWidget.y);
    glm::vec2 delta = currentContent - startContent;

    switch (handle)
    {
        case GizmoHandle::Center:
        {
            // Translate
            result.position = original.position + delta;
            break;
        }

        case GizmoHandle::TopLeft:
        {
            // Move top-left, anchor bottom-right
            result.position.x = original.position.x + delta.x;
            result.position.y = original.position.y + delta.y;
            result.size.x = original.size.x - delta.x;
            result.size.y = original.size.y - delta.y;

            if (shiftHeld)
            {
                // Constrain aspect ratio
                float aspect = original.size.x / original.size.y;
                if (std::abs(delta.x) > std::abs(delta.y))
                {
                    result.size.y = result.size.x / aspect;
                    result.position.y = original.position.y + original.size.y - result.size.y;
                }
                else
                {
                    result.size.x = result.size.y * aspect;
                    result.position.x = original.position.x + original.size.x - result.size.x;
                }
            }
            break;
        }

        case GizmoHandle::TopRight:
        {
            result.position.y = original.position.y + delta.y;
            result.size.x = original.size.x + delta.x;
            result.size.y = original.size.y - delta.y;

            if (shiftHeld)
            {
                float aspect = original.size.x / original.size.y;
                result.size.y = result.size.x / aspect;
                result.position.y = original.position.y + original.size.y - result.size.y;
            }
            break;
        }

        case GizmoHandle::BottomLeft:
        {
            result.position.x = original.position.x + delta.x;
            result.size.x = original.size.x - delta.x;
            result.size.y = original.size.y + delta.y;

            if (shiftHeld)
            {
                float aspect = original.size.x / original.size.y;
                result.size.x = result.size.y * aspect;
                result.position.x = original.position.x + original.size.x - result.size.x;
            }
            break;
        }

        case GizmoHandle::BottomRight:
        {
            result.size.x = original.size.x + delta.x;
            result.size.y = original.size.y + delta.y;

            if (shiftHeld)
            {
                float aspect = original.size.x / original.size.y;
                if (std::abs(delta.x) > std::abs(delta.y))
                    result.size.y = result.size.x / aspect;
                else
                    result.size.x = result.size.y * aspect;
            }
            break;
        }

        case GizmoHandle::TopEdge:
        {
            result.position.y = original.position.y + delta.y;
            result.size.y = original.size.y - delta.y;
            break;
        }

        case GizmoHandle::BottomEdge:
        {
            result.size.y = original.size.y + delta.y;
            break;
        }

        case GizmoHandle::LeftEdge:
        {
            result.position.x = original.position.x + delta.x;
            result.size.x = original.size.x - delta.x;
            break;
        }

        case GizmoHandle::RightEdge:
        {
            result.size.x = original.size.x + delta.x;
            break;
        }

        case GizmoHandle::RotateTopLeft:
        case GizmoHandle::RotateTopRight:
        case GizmoHandle::RotateBottomLeft:
        case GizmoHandle::RotateBottomRight:
        {
            result.rotation = computeRotation(original, startWidget, currentWidget);
            break;
        }

        case GizmoHandle::None:
            break;
    }

    return result;
}

float GizmoMath::computeRotation(const GizmoTransform& original,
                                   glm::vec2 startWidget,
                                   glm::vec2 currentWidget) const
{
    if (!m_layout) return original.rotation;

    // Compute rotation around center
    float cx = original.position.x + original.size.x * 0.5f;
    float cy = original.position.y + original.size.y * 0.5f;
    glm::vec2 centerWidget = m_layout->contentToWidget(cx, cy);

    glm::vec2 startDir   = startWidget - centerWidget;
    glm::vec2 currentDir = currentWidget - centerWidget;

    float startAngle   = std::atan2(startDir.y, startDir.x);
    float currentAngle = std::atan2(currentDir.y, currentDir.x);

    float deltaAngle = glm::degrees(currentAngle - startAngle);
    return original.rotation + deltaAngle;
}

bool GizmoMath::isNearHandle(glm::vec2 point, glm::vec2 handle) const
{
    float radius = m_config.handleSize * 0.5f + m_config.hitPadding;
    glm::vec2 diff = point - handle;
    return (diff.x * diff.x + diff.y * diff.y) <= (radius * radius);
}

int GizmoMath::cursorForHandle(GizmoHandle handle)
{
    // Returns cursor shape hints (matching Qt::CursorShape values)
    // 0 = Arrow, 8 = SizeVer, 9 = SizeHor, 10 = SizeBDiag,
    // 11 = SizeFDiag, 13 = SizeAll, 17 = ClosedHand
    switch (handle)
    {
        case GizmoHandle::Center:      return 13; // SizeAll
        case GizmoHandle::TopLeft:     return 11; // SizeFDiag
        case GizmoHandle::BottomRight: return 11; // SizeFDiag
        case GizmoHandle::TopRight:    return 10; // SizeBDiag
        case GizmoHandle::BottomLeft:  return 10; // SizeBDiag
        case GizmoHandle::TopEdge:     return 8;  // SizeVer
        case GizmoHandle::BottomEdge:  return 8;  // SizeVer
        case GizmoHandle::LeftEdge:    return 9;  // SizeHor
        case GizmoHandle::RightEdge:   return 9;  // SizeHor
        case GizmoHandle::RotateTopLeft:
        case GizmoHandle::RotateTopRight:
        case GizmoHandle::RotateBottomLeft:
        case GizmoHandle::RotateBottomRight:
            return 17; // ClosedHand (rotation)
        case GizmoHandle::None:
        default:
            return 0;  // Arrow
    }
}

} // namespace rt
