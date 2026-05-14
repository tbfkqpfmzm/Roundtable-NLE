/*
 * DockEdgeDragWatcher.cpp — extracted from DockBehavior.cpp.
 *
 * Detects floating docks near workspace edges and manages drop zones
 * (edge column creation, inner-split, tab-with).
 */

#include "panels/timeline/DockBehavior.h"
#include "panels/timeline/TimelineWorkspace.h"

#include "widgets/DockTitleBar.h"

#include <QDockWidget>
#include <QMainWindow>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QMenu>
#include <QPointer>
#include <QSet>
#include <QApplication>
#include <QPainter>
#include <QMouseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QWidget>
#include <QWindow>
#include <QBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <climits>

// ═════════════════════════════════════════════════════════════════════════════
//  DockEdgeDragWatcher
// ═════════════════════════════════════════════════════════════════════════════

DockEdgeDragWatcher::DockEdgeDragWatcher(QMainWindow* host, QSplitter* edgeSplitter,
                                         QObject* parent)
    : QObject(parent), m_host(host), m_splitter(edgeSplitter)
{
    m_overlay = new DockEdgeOverlay(edgeSplitter);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(30);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() { poll(); });
    QApplication::instance()->installEventFilter(this);
}

void DockEdgeDragWatcher::beginExternalDrag(QDockWidget* dock)
{
    if (!dock) return;
    m_draggedDock = dock;
    m_dragConfirmed = true;
    m_initialCursorPos = QCursor::pos();
    m_dragOffset = dock->pos() - QCursor::pos();
    dock->setAllowedAreas(Qt::NoDockWidgetArea);
    if (!m_pollTimer->isActive())
        m_pollTimer->start();
}

bool DockEdgeDragWatcher::eventFilter(QObject* /*watched*/, QEvent* event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && !m_pollTimer->isActive()) {
            m_dragConfirmed = false;
            m_initialCursorPos = QCursor::pos();
            m_draggedDock = nullptr;
            m_pressTargetDock = nullptr;
            QWidget* w = QApplication::widgetAt(QCursor::pos());
            bool onDragHandle = false;
            QDockWidget* targetDock = nullptr;
            {
                QWidget* check = w;
                while (check) {
                    if (qobject_cast<rt::DockTitleBar*>(check) ||
                        qobject_cast<QTabBar*>(check))
                        onDragHandle = true;
                    if (auto* dock = qobject_cast<QDockWidget*>(check)) {
                        targetDock = dock;
                        break;
                    }
                    check = check->parentWidget();
                }
            }
            if (onDragHandle && targetDock) {
                m_pressTargetDock = targetDock;
                m_pollTimer->start();
            }
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton) break;
        if (m_dragConfirmed && m_draggedDock)
            updateEdgeHighlight();
        if (m_innerTarget && m_draggedDock && m_dragConfirmed)
            commitInnerDrop();
        else if (m_activeEdge != Edge::None && m_draggedDock && m_dragConfirmed)
            commitEdgeDrop();
        else
            endTracking();
        break;
    }
    default:
        break;
    }
    return false;
}

void DockEdgeDragWatcher::setEdge(Edge edge, QRect zone)
{
    if (edge == Edge::None) {
        m_activeEdge = Edge::None;
        if (m_overlay) m_overlay->hideOverlay();
        return;
    }
    m_activeEdge = edge;
    if (m_overlay) {
        QPoint globalTL = m_splitter->mapToGlobal(zone.topLeft());
        m_overlay->setGeometry(QRect(globalTL, zone.size()));
        m_overlay->show();
        m_overlay->raise();
        m_overlay->update();
    }
}

void DockEdgeDragWatcher::endTracking()
{
    m_pollTimer->stop();
    m_overlay->hideOverlay();
    m_activeEdge = Edge::None;
    m_dropMode = DropMode::None;
    m_dropTarget = nullptr;
    m_innerTarget = nullptr;
    m_innerMode = DropMode::None;
    m_draggedDock = nullptr;
    m_pressTargetDock = nullptr;
    m_dragConfirmed = false;
    m_initialCursorPos = QPoint();
    m_dragOffset = QPoint();
    m_committing = false;
}

void DockEdgeDragWatcher::poll()
{
    if (!(QApplication::mouseButtons() & Qt::LeftButton)) {
        if (m_dragConfirmed && m_draggedDock)
            updateEdgeHighlight();
        if (m_innerTarget && m_draggedDock && m_dragConfirmed)
            commitInnerDrop();
        else if (m_activeEdge != Edge::None && m_draggedDock && m_dragConfirmed)
            commitEdgeDrop();
        else
            endTracking();
        return;
    }
    QDockWidget* dock = findDraggedDock();
    if (!dock) dock = m_pressTargetDock;
    if (!dock && m_draggedDock) dock = m_draggedDock;
    if (!dock) {
        if (m_activeEdge != Edge::None) setEdge(Edge::None);
        clearInner();
        m_dragConfirmed = false;
        return;
    }
    m_draggedDock = dock;
    if (m_draggedDock && m_draggedDock->isFloating() && !m_dragOffset.isNull())
        m_draggedDock->move(QCursor::pos() + m_dragOffset);
    if (!m_initialCursorPos.isNull()) {
        if ((QCursor::pos() - m_initialCursorPos).manhattanLength() > 30)
            m_dragConfirmed = true;
    }
    if (m_dragConfirmed)
        updateEdgeHighlight();
    else
        setEdge(Edge::None);
}

QDockWidget* DockEdgeDragWatcher::findDraggedDock() const
{
    QPoint cursor = QCursor::pos();
    QWidget* hostWindow = m_host->window();
    for (auto* w : QApplication::topLevelWidgets()) {
        if (!w->isVisible()) continue;
        if (w == hostWindow || w == m_host) continue;
        QRect geom = w->frameGeometry();
        if (!geom.adjusted(-40, -40, 40, 40).contains(cursor)) continue;
        if (auto* dock = qobject_cast<QDockWidget*>(w)) return dock;
        if (auto* mw = qobject_cast<QMainWindow*>(w)) {
            auto docks = mw->findChildren<QDockWidget*>();
            if (!docks.isEmpty()) return docks.first();
        }
    }
    return nullptr;
}

QMainWindow* DockEdgeDragWatcher::findEdgeColumn(bool isLeft) const
{
    int hostIndex = m_splitter->indexOf(m_host);
    if (isLeft && hostIndex > 0) {
        auto* w = qobject_cast<QMainWindow*>(m_splitter->widget(0));
        if (w && w != m_host && w->isVisible()) return w;
    } else if (!isLeft && hostIndex >= 0 && hostIndex < m_splitter->count() - 1) {
        auto* w = qobject_cast<QMainWindow*>(
            m_splitter->widget(m_splitter->count() - 1));
        if (w && w != m_host && w->isVisible()) return w;
    }
    return nullptr;
}

QRect DockEdgeDragWatcher::computeColumnDropZone(QMainWindow* edgeCol, bool /*isLeft*/)
{
    QPoint globalCursor = QCursor::pos();
    QRect colGeoInSplitter = edgeCol->geometry();
    QList<QDockWidget*> docks;
    for (auto* d : edgeCol->findChildren<QDockWidget*>()) {
        if (!d->isFloating() && d != m_draggedDock)
            docks.append(d);
    }
    if (docks.isEmpty()) {
        m_dropMode = DropMode::TabWith;
        m_dropTarget = nullptr;
        return colGeoInSplitter;
    }
    QDockWidget* hoveredDock = nullptr;
    for (auto* d : docks) {
        QPoint dockTL = d->mapToGlobal(QPoint(0, 0));
        QRect dockGlobal(dockTL, d->size());
        if (dockGlobal.adjusted(0, -4, 0, 4).contains(globalCursor)) {
            hoveredDock = d;
            break;
        }
    }
    if (!hoveredDock) {
        int minDist = INT_MAX;
        for (auto* d : docks) {
            QPoint dockTL = d->mapToGlobal(QPoint(0, 0));
            int cy = dockTL.y() + d->height() / 2;
            int dist = std::abs(globalCursor.y() - cy);
            if (dist < minDist) { minDist = dist; hoveredDock = d; }
        }
    }
    if (!hoveredDock) {
        m_dropMode = DropMode::TabWith;
        m_dropTarget = docks.first();
        return colGeoInSplitter;
    }
    QPoint dockGlobalTL = hoveredDock->mapToGlobal(QPoint(0, 0));
    QPoint dockInSplitter = m_splitter->mapFromGlobal(dockGlobalTL);
    int dockH = hoveredDock->height();
    int localY = globalCursor.y() - dockGlobalTL.y();
    double yFrac = static_cast<double>(localY) / std::max(1, dockH);
    int colX = colGeoInSplitter.x();
    int colW = colGeoInSplitter.width();
    if (yFrac < 0.25) {
        m_dropMode = DropMode::SplitAbove;
        m_dropTarget = hoveredDock;
        int stripH = std::max(24, dockH / 4);
        return QRect(colX, dockInSplitter.y(), colW, stripH);
    } else if (yFrac > 0.75) {
        m_dropMode = DropMode::SplitBelow;
        m_dropTarget = hoveredDock;
        int stripH = std::max(24, dockH / 4);
        return QRect(colX, dockInSplitter.y() + dockH - stripH, colW, stripH);
    } else {
        m_dropMode = DropMode::TabWith;
        m_dropTarget = hoveredDock;
        return QRect(colX, dockInSplitter.y(), colW, dockH);
    }
}

void DockEdgeDragWatcher::updateEdgeHighlight()
{
    QPoint cursor = m_splitter->mapFromGlobal(QCursor::pos());
    QRect splitterRect = m_splitter->rect();
    if (cursor.y() < -20 || cursor.y() > splitterRect.height() + 20) {
        setEdge(Edge::None);
        return;
    }
    int kEdgeTrigger = std::max(48, splitterRect.width() / 30);
    static constexpr int kNewColStrip = 40;
    int h = splitterRect.height();
    int hostIndex = m_splitter->indexOf(m_host);
    bool leftExisting  = (hostIndex > 0);
    bool rightExisting = (hostIndex >= 0 && hostIndex < m_splitter->count() - 1);
    int leftTrigger  = kEdgeTrigger;
    int rightTrigger = splitterRect.width() - kEdgeTrigger;
    if (leftExisting) {
        QWidget* leftCol = m_splitter->widget(0);
        leftTrigger = leftCol->geometry().right() + 1;
    }
    if (rightExisting) {
        QWidget* rightCol = m_splitter->widget(m_splitter->count() - 1);
        rightTrigger = rightCol->geometry().left();
    }
    auto previewWidth = [&](bool isLeft) -> int {
        if (isLeft && leftExisting) return std::max(200, m_splitter->widget(0)->width());
        if (!isLeft && rightExisting) return std::max(200, m_splitter->widget(m_splitter->count() - 1)->width());
        return std::max(200, splitterRect.width() / 5);
    };
    if (cursor.x() >= 0 && cursor.x() < leftTrigger) {
        QMainWindow* edgeCol = leftExisting ? findEdgeColumn(true) : nullptr;
        int pw = previewWidth(true);
        m_previewWidth = pw;
        if (edgeCol && cursor.x() < kNewColStrip) {
            m_dropMode = DropMode::NewColumn;
            m_dropTarget = nullptr;
            setEdge(Edge::Left, QRect(0, 0, pw, h));
        } else if (edgeCol) {
            QRect zone = computeColumnDropZone(edgeCol, true);
            setEdge(Edge::Left, zone);
        } else {
            m_dropMode = DropMode::NewColumn;
            m_dropTarget = nullptr;
            setEdge(Edge::Left, QRect(0, 0, pw, h));
        }
    } else if (cursor.x() >= rightTrigger && cursor.x() <= splitterRect.width()) {
        QMainWindow* edgeCol = rightExisting ? findEdgeColumn(false) : nullptr;
        int pw = previewWidth(false);
        m_previewWidth = pw;
        if (edgeCol && cursor.x() >= splitterRect.width() - kNewColStrip) {
            m_dropMode = DropMode::NewColumn;
            m_dropTarget = nullptr;
            setEdge(Edge::Right, QRect(splitterRect.width() - pw, 0, pw, h));
        } else if (edgeCol) {
            QRect zone = computeColumnDropZone(edgeCol, false);
            setEdge(Edge::Right, zone);
        } else {
            m_dropMode = DropMode::NewColumn;
            m_dropTarget = nullptr;
            setEdge(Edge::Right, QRect(splitterRect.width() - pw, 0, pw, h));
        }
    } else {
        setEdge(Edge::None);
        updateInnerHighlight();
    }
}

void DockEdgeDragWatcher::clearInner()
{
    if (m_innerTarget) {
        m_innerTarget = nullptr;
        m_innerMode = DropMode::None;
        if (m_draggedDock)
            m_draggedDock->setAllowedAreas(Qt::AllDockWidgetAreas);
        m_overlay->hideOverlay();
    }
}

void DockEdgeDragWatcher::updateInnerHighlight()
{
    QPoint globalCursor = QCursor::pos();
    QDockWidget* hoveredDock = nullptr;
    for (int i = 0; i < m_splitter->count(); ++i) {
        auto* mw = qobject_cast<QMainWindow*>(m_splitter->widget(i));
        if (!mw) continue;
        for (auto* d : mw->findChildren<QDockWidget*>()) {
            if (d->isFloating() || !d->isVisible() || d == m_draggedDock) continue;
            QPoint tl = d->mapToGlobal(QPoint(0, 0));
            QRect globalRect(tl, d->size());
            if (globalRect.contains(globalCursor)) { hoveredDock = d; break; }
        }
        if (hoveredDock) break;
    }
    if (!hoveredDock) { clearInner(); return; }
    QPoint dockTL = hoveredDock->mapToGlobal(QPoint(0, 0));
    int localX = globalCursor.x() - dockTL.x();
    int localY = globalCursor.y() - dockTL.y();
    int dockW  = hoveredDock->width();
    int dockH  = hoveredDock->height();
    double xFrac = static_cast<double>(localX) / std::max(1, dockW);
    double yFrac = static_cast<double>(localY) / std::max(1, dockH);
    QPoint dockInSplitter = m_splitter->mapFromGlobal(dockTL);
    DropMode mode;
    QRect zone;
    if (yFrac < 0.25) {
        mode = DropMode::SplitAbove;
        int stripH = std::max(24, dockH / 4);
        zone = QRect(dockInSplitter.x(), dockInSplitter.y(), dockW, stripH);
    } else if (yFrac > 0.75) {
        mode = DropMode::SplitBelow;
        int stripH = std::max(24, dockH / 4);
        zone = QRect(dockInSplitter.x(), dockInSplitter.y() + dockH - stripH, dockW, stripH);
    } else if (xFrac < 0.25) {
        mode = DropMode::SplitLeft;
        int stripW = std::max(24, dockW / 4);
        zone = QRect(dockInSplitter.x(), dockInSplitter.y(), stripW, dockH);
    } else if (xFrac > 0.75) {
        mode = DropMode::SplitRight;
        int stripW = std::max(24, dockW / 4);
        zone = QRect(dockInSplitter.x() + dockW - stripW, dockInSplitter.y(), stripW, dockH);
    } else {
        mode = DropMode::TabWith;
        zone = QRect(dockInSplitter.x(), dockInSplitter.y(), dockW, dockH);
    }
    m_innerTarget = hoveredDock;
    m_innerMode = mode;
    m_overlay->showZone(zone, mode == DropMode::TabWith);
    if (m_draggedDock)
        m_draggedDock->setAllowedAreas(Qt::NoDockWidgetArea);
}

void DockEdgeDragWatcher::commitEdgeDrop()
{
    if (m_committing) return;
    m_committing = true;
    QPointer<QDockWidget> dock = m_draggedDock;
    QPointer<QSplitter>   splitter = m_splitter;
    QPointer<QMainWindow> host = m_host;
    DropMode dropMode = m_dropMode;
    QPointer<QDockWidget> dropTarget = m_dropTarget;
    int savedPreviewWidth = m_previewWidth;
    bool isLeft = (m_activeEdge == Edge::Left);

    if (dock) {
        dock->setProperty("_rt_edgeDrop", true);
        dock->setAllowedAreas(Qt::NoDockWidgetArea);
    }
    m_pollTimer->stop();
    m_overlay->hideOverlay();
    m_activeEdge = Edge::None;
    m_dropMode = DropMode::None;
    m_dropTarget = nullptr;
    m_draggedDock = nullptr;
    m_pressTargetDock = nullptr;
    m_dragConfirmed = false;
    m_initialCursorPos = QPoint();

    QTimer::singleShot(0, this, [this, dock, splitter, host, isLeft,
                                  dropMode, dropTarget, savedPreviewWidth]() {
        m_committing = false;
        if (!dock || !splitter || !host) {
            if (dock) {
                dock->setAllowedAreas(Qt::AllDockWidgetAreas);
                dock->setProperty("_rt_edgeDrop", QVariant());
            }
            return;
        }
        int hostIndex = splitter->indexOf(host);
        QMainWindow* existingEdge = nullptr;
        if (isLeft && hostIndex > 0) {
            auto* first = qobject_cast<QMainWindow*>(splitter->widget(0));
            if (first && first != host.data() && first->isVisible())
                existingEdge = first;
        } else if (!isLeft && hostIndex >= 0 && hostIndex < splitter->count() - 1) {
            auto* last = qobject_cast<QMainWindow*>(
                splitter->widget(splitter->count() - 1));
            if (last && last != host.data() && last->isVisible())
                existingEdge = last;
        }

        if (existingEdge && dropMode != DropMode::NewColumn) {
            if (!dock->isFloating()) {
                QMainWindow* currentMW = nullptr;
                QWidget* p = dock->parentWidget();
                while (p) {
                    if (auto* mw = qobject_cast<QMainWindow*>(p)) { currentMW = mw; break; }
                    p = p->parentWidget();
                }
                if (currentMW && currentMW != existingEdge)
                    currentMW->removeDockWidget(dock);
            }
            dock->setAllowedAreas(Qt::AllDockWidgetAreas);
            if (dropTarget && dropMode == DropMode::TabWith) {
                existingEdge->tabifyDockWidget(dropTarget, dock);
            } else if (dropTarget && dropMode == DropMode::SplitAbove) {
                existingEdge->removeDockWidget(dropTarget);
                existingEdge->addDockWidget(Qt::TopDockWidgetArea, dock);
                existingEdge->addDockWidget(Qt::BottomDockWidgetArea, dropTarget);
                dropTarget->show();
            } else if (dropTarget && dropMode == DropMode::SplitBelow) {
                existingEdge->addDockWidget(Qt::BottomDockWidgetArea, dock);
            } else {
                existingEdge->addDockWidget(Qt::TopDockWidgetArea, dock);
            }
            dock->setFloating(false);
            dock->show();
            dock->raise();
            dock->setAllowedAreas(Qt::AllDockWidgetAreas);
            dock->setProperty("_rt_edgeDrop", QVariant());
            auto* guard = EdgeColumnGuard::forColumn(existingEdge);
            if (!guard) guard = new EdgeColumnGuard(existingEdge, host);
            guard->watchDock(dock);
            return;
        }

        // No existing edge column — create one.
        if (!dock->isFloating()) dock->setFloating(true);
        auto* edgeMW = new QMainWindow(splitter);
        edgeMW->setObjectName(QStringLiteral("EdgeColumn"));
        edgeMW->setWindowFlags(Qt::Widget);
        edgeMW->setDockNestingEnabled(true);
        edgeMW->setAnimated(false);
        edgeMW->setMinimumWidth(150);
        edgeMW->setTabPosition(Qt::TopDockWidgetArea, QTabWidget::North);
        edgeMW->setTabPosition(Qt::BottomDockWidgetArea, QTabWidget::North);
        edgeMW->setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::North);
        edgeMW->setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);
        edgeMW->setDockOptions(
            QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks |
            QMainWindow::GroupedDragging);
        auto* placeholder = new QWidget(edgeMW);
        placeholder->setMaximumSize(0, 0);
        edgeMW->setCentralWidget(placeholder);
        edgeMW->setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
        edgeMW->setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
        edgeMW->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
        edgeMW->setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
        if (isLeft) splitter->insertWidget(0, edgeMW);
        else        splitter->addWidget(edgeMW);
        int totalW = splitter->width();
        int colW   = std::max(200, savedPreviewWidth);
        QList<int> sizes;
        for (int i = 0; i < splitter->count(); ++i) {
            if (splitter->widget(i) == host.data())
                sizes.append(totalW - colW * (splitter->count() - 1));
            else
                sizes.append(colW);
        }
        splitter->setSizes(sizes);
        edgeMW->show();
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        edgeMW->addDockWidget(Qt::TopDockWidgetArea, dock);
        dock->setProperty("_rt_edgeDrop", QVariant());
        dock->setVisible(true);
        dock->show();
        auto* guard = new EdgeColumnGuard(edgeMW, host);
        guard->watchDock(dock);
    });
}

void DockEdgeDragWatcher::commitInnerDrop()
{
    if (m_committing) return;
    m_committing = true;
    QPointer<QDockWidget> dock = m_draggedDock;
    QPointer<QDockWidget> target = m_innerTarget;
    QPointer<QMainWindow> host = m_host;
    DropMode mode = m_innerMode;

    if (dock) {
        dock->setProperty("_rt_edgeDrop", true);
        dock->setAllowedAreas(Qt::NoDockWidgetArea);
    }
    m_pollTimer->stop();
    m_overlay->hideOverlay();
    m_innerTarget = nullptr;
    m_innerMode = DropMode::None;
    m_activeEdge = Edge::None;
    m_dropMode = DropMode::None;
    m_dropTarget = nullptr;
    m_draggedDock = nullptr;
    m_pressTargetDock = nullptr;
    m_dragConfirmed = false;
    m_initialCursorPos = QPoint();

    QTimer::singleShot(0, this, [this, dock, target, host, mode]() {
        m_committing = false;
        if (!dock || !target || !host) {
            if (dock) {
                dock->setAllowedAreas(Qt::AllDockWidgetAreas);
                dock->setProperty("_rt_edgeDrop", QVariant());
            }
            return;
        }
        QMainWindow* targetMW = nullptr;
        QWidget* p = target->parentWidget();
        while (p) {
            if (auto* mw = qobject_cast<QMainWindow*>(p)) { targetMW = mw; break; }
            p = p->parentWidget();
        }
        if (!targetMW) targetMW = host;
        if (!dock->isFloating()) {
            QMainWindow* currentMW = nullptr;
            QWidget* p2 = dock->parentWidget();
            while (p2) {
                if (auto* mw = qobject_cast<QMainWindow*>(p2)) { currentMW = mw; break; }
                p2 = p2->parentWidget();
            }
            if (currentMW && currentMW != targetMW)
                currentMW->removeDockWidget(dock);
        }
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        switch (mode) {
        case DropMode::SplitAbove:
            targetMW->tabifyDockWidget(target, dock);
            targetMW->splitDockWidget(dock, target, Qt::Vertical);
            break;
        case DropMode::SplitBelow:
            targetMW->splitDockWidget(target, dock, Qt::Vertical);
            break;
        case DropMode::SplitLeft:
            targetMW->tabifyDockWidget(target, dock);
            targetMW->splitDockWidget(dock, target, Qt::Horizontal);
            break;
        case DropMode::SplitRight:
            targetMW->splitDockWidget(target, dock, Qt::Horizontal);
            break;
        case DropMode::TabWith:
            targetMW->tabifyDockWidget(target, dock);
            break;
        default:
            targetMW->addDockWidget(Qt::TopDockWidgetArea, dock);
            break;
        }
        dock->setFloating(false);
        dock->show();
        dock->raise();
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        dock->setProperty("_rt_edgeDrop", QVariant());
        if (targetMW != host) {
            auto* guard = EdgeColumnGuard::forColumn(targetMW);
            if (guard) guard->watchDock(dock);
        }
    });
}

// ═════════════════════════════════════════════════════════════════════════════
