/*
 * TimelineWorkspaceToggleMaximize.cpp — Panel maximize/restore (tilde/`).
 *
 * Premiere-style: pressing ` maximizes the panel under the cursor; pressing
 * it again reverts to EXACTLY the prior layout.
 *
 * Design (deliberately minimal):
 *   - Maximize never reparents any widget.  It simply hides every sibling
 *     panel/column so the target dock/panel gets all the space.  This is
 *     critical because the Program Monitor hosts a native Vulkan surface;
 *     reparenting it (the old implementation did) destroys + recreates the
 *     native window and the swapchain, which is why the monitor went blank.
 *   - Restore re-applies an EXACT visibility snapshot captured at maximize
 *     time, plus QMainWindow::restoreState() for the inner dock arrangement.
 *     Panels the user had closed stay closed (the old code force-showed
 *     every panel/dock unconditionally — the reported bug).
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "panels/timeline/TimelinePanel.h"

#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QEvent>
#include <QEventLoop>
#include <QMainWindow>
#include <QPoint>

#include <spdlog/spdlog.h>

namespace rt {

// Walk up to the first QMainWindow ancestor (the dock's owning window —
// either m_innerMainWindow or one of the edge-column QMainWindows).
static QMainWindow* ownerMainWindow(QWidget* w)
{
    for (QWidget* p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget())
        if (auto* mw = qobject_cast<QMainWindow*>(p))
            return mw;
    return nullptr;
}

void TimelineWorkspace::togglePanelMaximize()
{
    if (!m_panelsBuilt || !m_innerMainWindow)
        return;

    // ──────────────────────────────────────────────────────────────────
    // RESTORE — revert to precisely the pre-maximize layout
    // ──────────────────────────────────────────────────────────────────
    if (m_panelMaximized) {
        // ORDER MATTERS.  QMainWindow::restoreState() distributes dock
        // sizes against the inner window's CURRENT size.  While maximized
        // the edge columns are hidden, so the inner window is at its wide
        // full-workspace size.  We must put the columns + splitter back
        // FIRST so the inner window regains its pre-maximize dimensions,
        // then restoreState() so dock proportions are computed against the
        // correct width (otherwise panels come back wider/larger/moved).

        // 1. Edge column (QMainWindow) + central visibility.
        for (auto& [win, vis] : m_edgeVisBeforeMax)
            if (win) win->setVisible(vis);
        if (m_centralBeforeMax)
            m_centralBeforeMax->setVisible(m_centralVisBeforeMax);

        // 2. Splitter pane geometry (saveState/restoreState is far more
        //    reliable than setSizes() across hidden/shown panes).
        if (m_edgeSplitter && !m_edgeSplitterStateBeforeMax.isEmpty())
            m_edgeSplitter->restoreState(m_edgeSplitterStateBeforeMax);

        // 3. Flush pending layout so the inner QMainWindow is back at its
        //    pre-maximize size BEFORE its dock state is restored.
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        // 4. Dock arrangement (areas, sizes, tabs, visibility) for EVERY
        //    dock-hosting window — inner AND each edge column — now
        //    computed against their correct restored sizes.
        for (auto& [win, state] : m_dockStatesBeforeMax)
            if (win && !state.isEmpty())
                win->restoreState(state, 4);

        // 5. Exact per-dock visibility.  Fixes docks that live in edge
        //    columns AND guarantees panels the user had CLOSED are not
        //    force-shown.
        for (auto& [dock, vis] : m_dockVisBeforeMax)
            if (dock) dock->setVisible(vis);

        m_panelMaximized = false;
        m_maximizedWidget = nullptr;
        m_maximizedDock   = nullptr;
        m_dockVisBeforeMax.clear();
        m_edgeVisBeforeMax.clear();
        m_dockStatesBeforeMax.clear();
        m_edgeSplitterStateBeforeMax.clear();
        m_centralBeforeMax = nullptr;
        spdlog::info("[togglePanelMaximize] restored prior layout");
        return;
    }

    // ──────────────────────────────────────────────────────────────────
    // MAXIMIZE — pick the panel under the cursor (then focus fallback)
    // ──────────────────────────────────────────────────────────────────
    QWidget* target = nullptr;
    const QPoint gm = QCursor::pos();

    for (auto it = m_dockWidgets.begin(); it != m_dockWidgets.end(); ++it) {
        QDockWidget* dock = it.value();
        if (dock && dock->isVisible() &&
            dock->rect().contains(dock->mapFromGlobal(gm))) {
            target = dock;
            break;
        }
    }
    if (!target && m_timelinePanel && m_timelinePanel->isVisible() &&
        m_timelinePanel->rect().contains(m_timelinePanel->mapFromGlobal(gm)))
        target = m_timelinePanel;
    if (!target) {
        QWidget* fw = QApplication::focusWidget();
        for (auto it = m_dockWidgets.begin(); it != m_dockWidgets.end(); ++it) {
            QDockWidget* dock = it.value();
            if (dock && dock->isVisible() && fw && dock->isAncestorOf(fw)) {
                target = dock;
                break;
            }
        }
        if (!target && m_timelinePanel && m_timelinePanel->isVisible() &&
            fw && m_timelinePanel->isAncestorOf(fw))
            target = m_timelinePanel;
    }
    if (!target)
        return;

    QDockWidget* targetDock = qobject_cast<QDockWidget*>(target);
    QMainWindow* targetOwner = ownerMainWindow(targetDock ? static_cast<QWidget*>(targetDock)
                                                          : target);

    // ── Snapshot the exact current state for a faithful restore ────────
    m_dockStateBeforeMaximize = m_innerMainWindow->saveState(4);

    m_dockVisBeforeMax.clear();
    for (auto it = m_dockWidgets.begin(); it != m_dockWidgets.end(); ++it)
        if (it.value())
            m_dockVisBeforeMax[it.value()] = it.value()->isVisible();

    // Capture saveState() for EVERY dock-hosting window: the inner window
    // AND each edge-column QMainWindow (each owns the heights of its own
    // stacked panels — restoring only the inner one left the leftmost
    // column's bottom panel taller than the original).
    m_dockStatesBeforeMax.clear();
    m_dockStatesBeforeMax.push_back({m_innerMainWindow,
                                     m_innerMainWindow->saveState(4)});

    m_edgeVisBeforeMax.clear();
    if (m_edgeSplitter) {
        for (int i = 0; i < m_edgeSplitter->count(); ++i)
            if (auto* win = qobject_cast<QMainWindow*>(m_edgeSplitter->widget(i))) {
                m_edgeVisBeforeMax.push_back({win, win->isVisible()});
                if (win != m_innerMainWindow)
                    m_dockStatesBeforeMax.push_back({win, win->saveState(4)});
            }
        m_edgeSplitterStateBeforeMax = m_edgeSplitter->saveState();
    }

    m_centralBeforeMax    = m_innerMainWindow->centralWidget();
    m_centralVisBeforeMax = m_centralBeforeMax ? m_centralBeforeMax->isVisible()
                                               : true;

    // ── Hide everything except the target (no reparenting) ─────────────
    // Sibling docks.
    for (auto it = m_dockWidgets.begin(); it != m_dockWidgets.end(); ++it) {
        QDockWidget* dock = it.value();
        if (dock && dock != targetDock)
            dock->setVisible(false);
    }

    // Inner central widget: keep it only when the maximized panel IS it
    // (the timeline panel lives inside the central container).
    if (m_centralBeforeMax) {
        const bool targetInCentral =
            (target == m_centralBeforeMax) ||
            m_centralBeforeMax->isAncestorOf(target);
        m_centralBeforeMax->setVisible(targetInCentral);
    }

    // Splitter columns: keep only the one that owns the target so it
    // fills the whole workspace; hide the rest.
    for (auto& [win, vis] : m_edgeVisBeforeMax)
        if (win)
            win->setVisible(win == targetOwner);

    if (targetDock) {
        targetDock->setVisible(true);
        targetDock->raise();
    } else {
        target->setVisible(true);
    }

    m_maximizedWidget = target;
    m_maximizedDock   = targetDock;
    m_panelMaximized  = true;
    spdlog::info("[togglePanelMaximize] maximized '{}'",
                 target->objectName().toStdString());
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
