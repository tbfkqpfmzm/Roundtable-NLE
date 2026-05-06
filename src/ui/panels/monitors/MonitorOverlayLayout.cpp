#include "panels/monitors/MonitorOverlayLayout.h"

namespace rt {

MonitorOverlayLayoutPlan planMonitorOverlayLayout(
    const MonitorOverlayLayoutRequest& request) noexcept
{
    MonitorOverlayLayoutPlan plan;

    if (!request.viewportVisible ||
        request.viewportGlobalRect.isEmpty() ||
        request.panelGlobalRect.isEmpty()) {
        return plan;
    }

    const QRect visibleViewport = request.viewportGlobalRect.intersected(
        request.panelGlobalRect);
    if (visibleViewport.isEmpty()) {
        return plan;
    }

    plan.overlayGlobalRect = visibleViewport;
    plan.viewportOffset = QPoint(
        visibleViewport.left() - request.viewportGlobalRect.left(),
        visibleViewport.top() - request.viewportGlobalRect.top());
    plan.showOverlay = true;
    plan.clipNativeViewport = true;
    return plan;
}

} // namespace rt
