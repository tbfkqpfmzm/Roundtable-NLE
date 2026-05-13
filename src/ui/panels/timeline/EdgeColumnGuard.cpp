/*
 * EdgeColumnGuard.cpp — extracted from DockBehavior.cpp.
 *
 * Auto-cleanup for edge-column QMainWindows — watches docks for
 * float events and hides empty edge columns.
 */

#include "panels/timeline/DockBehavior.h"

#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QApplication>

// ═════════════════════════════════════════════════════════════════════════════
//  EdgeColumnGuard
// ═════════════════════════════════════════════════════════════════════════════

EdgeColumnGuard::EdgeColumnGuard(QMainWindow* edgeCol, QMainWindow* host)
    : QObject(edgeCol)
    , m_edgeCol(edgeCol)
    , m_host(host)
{
    setObjectName(QStringLiteral("_rt_EdgeColumnGuard"));
}

EdgeColumnGuard* EdgeColumnGuard::forColumn(QMainWindow* edgeCol)
{
    auto* obj = edgeCol->findChild<QObject*>(
        QStringLiteral("_rt_EdgeColumnGuard"), Qt::FindDirectChildrenOnly);
    return static_cast<EdgeColumnGuard*>(obj);
}

void EdgeColumnGuard::watchDock(QDockWidget* dock)
{
    if (!dock || m_watched.contains(dock)) return;
    m_watched.insert(dock);
    connect(dock, &QDockWidget::topLevelChanged,
            this, [this, dockPtr = QPointer<QDockWidget>(dock)](bool floating) {
        if (!floating || !dockPtr) return;
        onDockFloated(dockPtr);
    });
}

void EdgeColumnGuard::onDockFloated(QPointer<QDockWidget> dock)
{
    if (!dock) return;
    disconnect(dock, &QDockWidget::topLevelChanged, this, nullptr);
    m_watched.remove(dock);

    auto* timer = new QTimer(this);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this,
            [this, dock, timer]() {
        if (QApplication::mouseButtons() & Qt::LeftButton) return;
        timer->stop();
        timer->deleteLater();
        if (!dock || !m_host || !m_edgeCol) {
            checkEmpty();
            return;
        }
        if (dock->property("_rt_edgeDrop").toBool()) {
            checkEmpty();
            return;
        }
        if (!dock->isFloating()) {
            bool inOurColumn = false;
            for (auto* d : m_edgeCol->findChildren<QDockWidget*>()) {
                if (d == dock) { inOurColumn = true; break; }
            }
            if (inOurColumn) {
                watchDock(dock);
            }
            checkEmpty();
            return;
        }
        m_host->addDockWidget(Qt::TopDockWidgetArea, dock);
        dock->setFloating(true);
        dock->show();
        dock->raise();
        checkEmpty();
    });
    timer->start();
}

void EdgeColumnGuard::checkEmpty()
{
    if (!m_edgeCol || !m_edgeCol->isVisible()) return;
    for (auto* d : m_edgeCol->findChildren<QDockWidget*>()) {
        if (!d->isFloating()) return;
    }
    m_edgeCol->hide();
    m_edgeCol->deleteLater();
}

// ═════════════════════════════════════════════════════════════════════════════
