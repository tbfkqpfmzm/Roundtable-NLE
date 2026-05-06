#pragma once

#include <QPoint>
#include <QRect>

namespace rt {

struct MonitorOverlayLayoutRequest
{
    QRect viewportGlobalRect;
    QRect panelGlobalRect;
    bool viewportVisible{false};
};

struct MonitorOverlayLayoutPlan
{
    QRect overlayGlobalRect;
    QPoint viewportOffset;
    bool showOverlay{false};
    bool clipNativeViewport{false};
};

[[nodiscard]] MonitorOverlayLayoutPlan planMonitorOverlayLayout(
    const MonitorOverlayLayoutRequest& request) noexcept;

} // namespace rt
