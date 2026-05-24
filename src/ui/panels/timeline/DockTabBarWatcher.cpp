/*
 * DockTabBarWatcher.cpp — extracted from DockBehavior.cpp.
 *
 * Forces no-elide, scroll-buttons on dock tab bars, enables tab
 * context menus (Close Tab), and watches for newly-added QTabBars
 * in dock containers.
 */

#include "panels/timeline/DockBehavior.h"
#include "panels/timeline/TimelineWorkspace.h"

#include <QDockWidget>
#include <QMainWindow>
#include <QTabBar>
#include <QMenu>
#include <QPointer>
#include <QCoreApplication>
#include <QChildEvent>
#include <QContextMenuEvent>
#include <QEvent>

#include <spdlog/spdlog.h>

// ═════════════════════════════════════════════════════════════════════════════
//  DockTabBarWatcher
// ═════════════════════════════════════════════════════════════════════════════

DockTabBarWatcher::DockTabBarWatcher(QMainWindow* host, QObject* parent)
    : QObject(parent), m_host(host)
{
    QCoreApplication::instance()->installEventFilter(this);
}

void DockTabBarWatcher::watchTabBar(QTabBar* tabBar)
{
    forceSettings(tabBar);
    tabBar->installEventFilter(this);
    setupTabBar(tabBar);
}

bool DockTabBarWatcher::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::ChildAdded
        && qobject_cast<QMainWindow*>(watched)) {
        auto* ce = static_cast<QChildEvent*>(event);
        if (auto* tabBar = qobject_cast<QTabBar*>(ce->child())) {
            watchTabBar(tabBar);
        }
    }
    if (auto* tabBar = qobject_cast<QTabBar*>(watched)) {
        switch (event->type()) {
        case QEvent::Paint:
        case QEvent::LayoutRequest:
        case QEvent::Show:
        case QEvent::Resize:
        case QEvent::StyleChange:
            forceSettings(tabBar);
            break;
        case QEvent::ContextMenu: {
            if (tabBar->documentMode()) break;
            auto* ce = static_cast<QContextMenuEvent*>(event);
            int tabIdx = tabBar->tabAt(tabBar->mapFromGlobal(ce->globalPos()));
            if (tabIdx >= 0) {
                showTabContextMenu(tabBar, tabIdx, ce->globalPos());
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return false;
}

void DockTabBarWatcher::showTabContextMenu(QTabBar* tabBar, int tabIdx,
                                            const QPoint& globalPos)
{
    QString tabTitle = tabBar->tabText(tabIdx);
    QDockWidget* foundDock = nullptr;
    if (m_workspace)
        foundDock = m_workspace->dockForPanel(tabTitle);
    if (!foundDock) {
        auto* mw = qobject_cast<QMainWindow*>(tabBar->window());
        if (!mw) mw = m_host;
        if (mw) {
            for (auto* d : mw->findChildren<QDockWidget*>()) {
                if (d->windowTitle() == tabTitle) {
                    foundDock = d;
                    break;
                }
            }
        }
    }
    QMenu menu(tabBar);
    QPointer<QDockWidget> dock = foundDock;
    QPointer<QTabBar> tb = tabBar;
    menu.addAction(QObject::tr("Close Tab"), [dock, tb, tabIdx]() {
        if (dock) {
            dock->close();
        } else if (tb) {
            // Non-dock tab bar (e.g. ProjectBin) — emit the standard
            // tabCloseRequested signal so the owning widget handles it.
            emit tb->tabCloseRequested(tabIdx);
        }
    });
    menu.exec(globalPos);
}

void DockTabBarWatcher::forceSettings(QTabBar* tabBar)
{
    if (tabBar->documentMode()) return;
    if (m_configuring) return;
    m_configuring = true;
    tabBar->setElideMode(Qt::ElideNone);
    tabBar->setExpanding(false);
    tabBar->setUsesScrollButtons(true);
    tabBar->setMaximumWidth(QWIDGETSIZE_MAX);
    tabBar->setMinimumWidth(0);
    tabBar->setVisible(true);
    m_configuring = false;
}

void DockTabBarWatcher::setupTabBar(QTabBar* tabBar)
{
    if (!tabBar || tabBar->documentMode()) return;
    if (tabBar->property("_rt_ctx_menu").toBool()) return;
    tabBar->setProperty("_rt_ctx_menu", true);
    tabBar->setMovable(true);
    tabBar->setTabsClosable(false);
    if (m_dragFilter)
        tabBar->installEventFilter(m_dragFilter);
}

// ═════════════════════════════════════════════════════════════════════════════
