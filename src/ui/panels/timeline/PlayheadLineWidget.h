#pragma once
/*
 * PlayheadLineWidget — Lightweight widget that draws only the playhead line.
 *
 * Parented to the track scroll viewport so it overlays all track widgets
 * without triggering their paintEvent.
 */

#include <QWidget>

class PlayheadLineWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PlayheadLineWidget(QWidget* parent);

protected:
    void paintEvent(QPaintEvent*) override;
};
