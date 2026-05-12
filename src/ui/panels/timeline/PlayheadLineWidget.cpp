/*
 * PlayheadLineWidget.cpp — Draws the playhead line overlay.
 */

#include "panels/timeline/PlayheadLineWidget.h"
#include "Theme.h"

#include <QPainter>

PlayheadLineWidget::PlayheadLineWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(3);
}

void PlayheadLineWidget::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor phCol = rt::Theme::colors().playhead;
    phCol.setAlpha(220);
    p.setPen(QPen(phCol, 1.5));
    const double cx = width() / 2.0;
    p.drawLine(QPointF(cx, 0), QPointF(cx, height()));
    --s_paintDepth;
}
