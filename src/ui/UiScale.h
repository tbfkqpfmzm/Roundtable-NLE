/*
 * UiScale.h - Application-wide UI scaling for cross-DPI parity.
 */

#pragma once

#include <QObject>
#include <QString>

class QScreen;
class QWidget;

namespace rt::UiScale {

class Notifier : public QObject {
    Q_OBJECT
public:
    static Notifier* instance();
signals:
    void factorChanged(double newFactor);
private:
    explicit Notifier(QObject* parent = nullptr) : QObject(parent) {}
};

double factor() noexcept;
int px(int v) noexcept;
int px(double v) noexcept;
void setBaselineDpi(double dpi);
void updateForScreen(QScreen* screen);
void setScaledFixedHeight(QWidget* w, int baseHeight);
void setScaledFixedWidth(QWidget* w, int baseWidth);
void setScaledFixedSize(QWidget* w, int baseW, int baseH);
void setScaledMinimumWidth(QWidget* w, int baseWidth);
void rescaleAllRegisteredWidgets();
QString scaleStyleSheet(const QString& qss);

} // namespace rt::UiScale