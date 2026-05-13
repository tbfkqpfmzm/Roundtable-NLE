/*
 * DockBehavior.cpp — Dock behavior coordinator.
 *
 * Thin coordinator after extracting each class to its own .cpp file.
 *
 * Sub-files (all in panels/timeline/):
 *   DockTabBarWatcher.cpp       — DockTabBarWatcher class
 *   DockTabDragFilter.cpp       — DockTabDragFilter class
 *                                + FloatingResizeFallbackFilter
 *                                + installDockResizeSubclass()
 *   DockEdgeOverlay.cpp         — DockEdgeOverlay class
 *   EdgeColumnGuard.cpp         — EdgeColumnGuard class
 *   DockEdgeDragWatcher.cpp     — DockEdgeDragWatcher class
 */

#include "panels/timeline/DockBehavior.h"
