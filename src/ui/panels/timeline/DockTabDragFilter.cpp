/*
 * DockTabDragFilter.cpp — extracted from DockBehavior.cpp.
 *
 * Enables dragging tabs out of tab groups to create floating docks,
 * and provides wide resize borders on floating dock windows (Windows).
 */

#include "panels/timeline/DockBehavior.h"

#include <QDockWidget>
#include <QMainWindow>
#include <QTabBar>
#include <QTimer>
#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QPoint>
#include <QSize>
#include <QWidget>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <commctrl.h>
#pragma comment(lib, "Comctl32.lib")
#endif

#include <spdlog/spdlog.h>

// ═════════════════════════════════════════════════════════════════════════════
//  DockTabDragFilter
// ═════════════════════════════════════════════════════════════════════════════

DockTabDragFilter::DockTabDragFilter(QMainWindow* dockHost, QObject* parent)
    : QObject(parent), m_dockHost(dockHost) {}

bool DockTabDragFilter::eventFilter(QObject* watched, QEvent* event)
{
    auto* tabBar = qobject_cast<QTabBar*>(watched);
    if (!tabBar) return false;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            int idx = tabBar->tabAt(me->pos());
            if (idx >= 0) {
                m_pressedTab    = idx;
                m_pressPos      = me->globalPosition().toPoint();
                m_dragging      = false;
                m_detached      = false;
            }
        }
        break;
    }
    case QEvent::MouseMove: {
        if (m_pressedTab < 0 || m_detached) break;
        auto* me = static_cast<QMouseEvent*>(event);
        if (!(me->buttons() & Qt::LeftButton)) break;
        QPoint delta = me->globalPosition().toPoint() - m_pressPos;
        if (!m_dragging) {
            if (delta.manhattanLength() < 30) break;
            m_dragging = true;
        }
        QPoint localPos = tabBar->mapFromGlobal(me->globalPosition().toPoint());
        QRect  barRect  = tabBar->rect().adjusted(0, -10, 0, 10);
        if (barRect.contains(localPos)) break;

        QDockWidget* dock = dockForTabIndex(tabBar, m_pressedTab);
        if (!dock) break;

        m_detached = true;
        m_pressedTab = -1;
        QPoint cursorPos = QCursor::pos();
        dock->setFloating(true);
        QSize dockSize = dock->size();
        if (dockSize.width() < 200)  dockSize.setWidth(400);
        if (dockSize.height() < 150) dockSize.setHeight(300);
        dock->resize(dockSize);
        dock->move(cursorPos.x() - dockSize.width() / 2,
                   cursorPos.y() - 15);
        dock->show();
        dock->raise();
        notifyEdgeWatcher(dock);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        m_pressedTab = -1;
        m_dragging   = false;
        m_detached   = false;
        break;
    }
    default:
        break;
    }
    return false;
}

QDockWidget* DockTabDragFilter::dockForTabIndex(QTabBar* tabBar, int index) const
{
    if (index < 0 || index >= tabBar->count()) return nullptr;
    QString tabText = tabBar->tabText(index);
    auto* parentMW = qobject_cast<QMainWindow*>(tabBar->window());
    if (parentMW) {
        for (auto* dock : parentMW->findChildren<QDockWidget*>()) {
            if (dock->windowTitle() == tabText)
                return dock;
        }
    }
    for (auto* dock : m_dockHost->findChildren<QDockWidget*>()) {
        if (dock->windowTitle() == tabText)
            return dock;
    }
    return nullptr;
}

void DockTabDragFilter::notifyEdgeWatcher(QDockWidget* dock)
{
    if (m_edgeWatcher)
        m_edgeWatcher->beginExternalDrag(dock);
}

// ═════════════════════════════════════════════════════════════════════════════
//  FloatingResizeFallbackFilter (Windows) & installDockResizeSubclass
// ═════════════════════════════════════════════════════════════════════════════

#ifdef _WIN32
static constexpr int kResizeBorderLogical = 12;

static LRESULT CALLBACK DockResizeSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*subclassId*/, DWORD_PTR /*refData*/)
{
    if (msg == WM_NCHITTEST) {
        LRESULT defResult = DefSubclassProc(hwnd, msg, wParam, lParam);
        switch (defResult) {
        case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
        case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
            return defResult;
        default: break;
        }
        RECT rc;
        GetWindowRect(hwnd, &rc);
        int x = GET_X_LPARAM(lParam) - rc.left;
        int y = GET_Y_LPARAM(lParam) - rc.top;
        int w = rc.right  - rc.left;
        int h = rc.bottom - rc.top;
        UINT dpi = GetDpiForWindow(hwnd);
        int border = MulDiv(kResizeBorderLogical, dpi, 96);
        bool atLeft   = x < border;
        bool atRight  = x >= w - border;
        bool atTop    = y < border;
        bool atBottom = y >= h - border;
        if (atTop    && atLeft)  return HTTOPLEFT;
        if (atTop    && atRight) return HTTOPRIGHT;
        if (atBottom && atLeft)  return HTBOTTOMLEFT;
        if (atBottom && atRight) return HTBOTTOMRIGHT;
        if (atLeft)              return HTLEFT;
        if (atRight)             return HTRIGHT;
        if (atTop)               return HTTOP;
        if (atBottom)            return HTBOTTOM;
        return defResult;
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, DockResizeSubclassProc, 1);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void installDockResizeSubclass(QDockWidget* dock)
{
    WId wid = dock->effectiveWinId();
    if (!wid) {
        QTimer::singleShot(150, dock, [dock]() {
            installDockResizeSubclass(dock);
        });
        return;
    }
    HWND hwnd = reinterpret_cast<HWND>(wid);
    HWND topHwnd = GetAncestor(hwnd, GA_ROOT);
    if (topHwnd && topHwnd != hwnd) hwnd = topHwnd;
    SetWindowSubclass(hwnd, DockResizeSubclassProc, 1, 0);
}

bool FloatingResizeFallbackFilter::nativeEventFilter(
    const QByteArray& eventType, void* message, qintptr* result)
{
    if (eventType != "windows_generic_MSG") return false;
    auto* msg = static_cast<MSG*>(message);
    if (msg->message != WM_NCHITTEST) return false;
    HWND hwnd = msg->hwnd;
    if (hwnd != GetAncestor(hwnd, GA_ROOT)) return false;
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    if (!(style & WS_THICKFRAME)) return false;
    if (!QWidget::find(reinterpret_cast<WId>(hwnd))) return false;
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int x = GET_X_LPARAM(msg->lParam) - rc.left;
    int y = GET_Y_LPARAM(msg->lParam) - rc.top;
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;
    UINT dpi = GetDpiForWindow(hwnd);
    int border = MulDiv(kResizeBorderLogical, dpi, 96);
    bool atLeft   = x < border;
    bool atRight  = x >= w - border;
    bool atTop    = y < border;
    bool atBottom = y >= h - border;
    if (atTop    && atLeft)  { *result = HTTOPLEFT;     return true; }
    if (atTop    && atRight) { *result = HTTOPRIGHT;    return true; }
    if (atBottom && atLeft)  { *result = HTBOTTOMLEFT;  return true; }
    if (atBottom && atRight) { *result = HTBOTTOMRIGHT; return true; }
    if (atLeft)              { *result = HTLEFT;        return true; }
    if (atRight)             { *result = HTRIGHT;       return true; }
    if (atTop)               { *result = HTTOP;         return true; }
    if (atBottom)            { *result = HTBOTTOM;      return true; }
    return false;
}
#endif

// ═════════════════════════════════════════════════════════════════════════════
