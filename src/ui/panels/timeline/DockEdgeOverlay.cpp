/*
 * DockEdgeOverlay.cpp — extracted from DockBehavior.cpp.
 *
 * Semi-transparent drop zone preview overlay for dock drag-and-drop.
 */

#include "panels/timeline/DockBehavior.h"

#include <QPainter>
#include <QWidget>

// ═════════════════════════════════════════════════════════════════════════════
//  DockEdgeOverlay
// ═════════════════════════════════════════════════════════════════════════════

DockEdgeOverlay::DockEdgeOverlay(QWidget* parent)
    : QWidget(nullptr,
              Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
    , m_anchor(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_OpaquePaintEvent);
    hide();
}

void DockEdgeOverlay::showZone(const QRect& zone, bool isTab)
{
    if (!m_anchor) { hide(); return; }
    m_isTab = isTab;
    QPoint globalTL = m_anchor->mapToGlobal(zone.topLeft());
    setGeometry(QRect(globalTL, zone.size()));
    show();
    raise();
    update();
}

void DockEdgeOverlay::hideOverlay() { hide(); }

void DockEdgeOverlay::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    if (m_isTab) {
        p.fillRect(rect(), QColor(80, 180, 120, 100));
        p.setPen(QPen(QColor(80, 180, 120, 220), 2));
    } else {
        p.fillRect(rect(), QColor(60, 130, 220, 100));
        p.setPen(QPen(QColor(60, 130, 220, 220), 2));
    }
    p.drawRect(rect().adjusted(1, 1, -2, -2));

    --s_paintDepth;
}

// ═════════════════════════════════════════════════════════════════════════════
