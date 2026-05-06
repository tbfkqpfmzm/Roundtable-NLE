/*
 * UiScale.cpp -- see header.
 */

#include "UiScale.h"

#include <QApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

namespace rt::UiScale {

namespace {

constexpr const char* kBaseFixedHeight = "rt_uiscale_baseFixedHeight";
constexpr const char* kBaseFixedWidth  = "rt_uiscale_baseFixedWidth";
constexpr const char* kBaseMinWidth    = "rt_uiscale_baseMinWidth";

double g_baselineDpi = 96.0;
double g_factor      = 1.0;

std::vector<QPointer<QWidget>>& trackedWidgets()
{
    static std::vector<QPointer<QWidget>> v;
    return v;
}

void trackWidget(QWidget* w)
{
    if (!w) return;
    auto& v = trackedWidgets();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const QPointer<QWidget>& p) { return p.isNull(); }),
            v.end());
    v.emplace_back(w);
}

inline int roundScaled(double v) noexcept
{
    return static_cast<int>(v + (v >= 0 ? 0.5 : -0.5));
}

} // namespace

Notifier* Notifier::instance()
{
    static Notifier* inst = nullptr;
    if (!inst) inst = new Notifier(qApp);
    return inst;
}

double factor() noexcept { return g_factor; }

int px(int v) noexcept    { return (v == 0) ? 0 : roundScaled(v * g_factor); }
int px(double v) noexcept { return roundScaled(v * g_factor); }

void setBaselineDpi(double dpi)
{
    if (dpi > 1.0) g_baselineDpi = dpi;
}

void updateForScreen(QScreen* screen)
{
    if (!screen) return;
    // Compute factor as the *largest* of the available signals so we get a
    // real scale-up on every Windows configuration:
    //   - logicalDpi/baseline  : works on Linux/macOS where Qt scales fonts
    //   - devicePixelRatio     : Windows PassThrough exposes scaling here
    //   - physicalDpi/baseline : fallback when user runs 4K at 100% scaling
    //                            so that high-PPI screens still get bigger UI
    const double logicalDpi  = screen->logicalDotsPerInch();
    const double physicalDpi = screen->physicalDotsPerInch();
    const double dpr         = screen->devicePixelRatio();
    const double fromLogical = (logicalDpi  > 0.0) ? (logicalDpi  / g_baselineDpi) : 1.0;
    const double fromPhys    = (physicalDpi > 0.0) ? (physicalDpi / g_baselineDpi) : 1.0;
    const double newFactor   = std::max({1.0, fromLogical, dpr, fromPhys});
    if (std::abs(newFactor - g_factor) < 0.01) return;
    g_factor = newFactor;
    rescaleAllRegisteredWidgets();
    emit Notifier::instance()->factorChanged(newFactor);
}

void setScaledFixedHeight(QWidget* w, int baseHeight)
{
    if (!w) return;
    w->setProperty(kBaseFixedHeight, baseHeight);
    w->setFixedHeight(px(baseHeight));
    trackWidget(w);
}

void setScaledFixedWidth(QWidget* w, int baseWidth)
{
    if (!w) return;
    w->setProperty(kBaseFixedWidth, baseWidth);
    w->setFixedWidth(px(baseWidth));
    trackWidget(w);
}

void setScaledFixedSize(QWidget* w, int baseW, int baseH)
{
    if (!w) return;
    w->setProperty(kBaseFixedWidth, baseW);
    w->setProperty(kBaseFixedHeight, baseH);
    w->setFixedSize(px(baseW), px(baseH));
    trackWidget(w);
}

void setScaledMinimumWidth(QWidget* w, int baseWidth)
{
    if (!w) return;
    w->setProperty(kBaseMinWidth, baseWidth);
    w->setMinimumWidth(px(baseWidth));
    trackWidget(w);
}

void rescaleAllRegisteredWidgets()
{
    auto& v = trackedWidgets();
    for (auto& p : v) {
        QWidget* w = p.data();
        if (!w) continue;
        const QVariant fh = w->property(kBaseFixedHeight);
        const QVariant fw = w->property(kBaseFixedWidth);
        const QVariant mw = w->property(kBaseMinWidth);
        if (fh.isValid() && fw.isValid()) {
            w->setFixedSize(px(fw.toInt()), px(fh.toInt()));
        } else if (fh.isValid()) {
            w->setFixedHeight(px(fh.toInt()));
        } else if (fw.isValid()) {
            w->setFixedWidth(px(fw.toInt()));
        }
        if (mw.isValid()) {
            w->setMinimumWidth(px(mw.toInt()));
        }
    }
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const QPointer<QWidget>& p) { return p.isNull(); }),
            v.end());
}

QString scaleStyleSheet(const QString& qss)
{
    if (std::abs(g_factor - 1.0) < 0.01) return qss;

    static const QRegularExpression rx(QStringLiteral("(\\d+)px(?![a-zA-Z0-9_])"));
    QString out;
    out.reserve(qss.size() + 16);
    int last = 0;
    auto it = rx.globalMatch(qss);
    while (it.hasNext()) {
        const auto m = it.next();
        out.append(QStringView{qss}.mid(last, m.capturedStart() - last));
        const int n = m.captured(1).toInt();
        out.append(QString::number(px(n)));
        out.append(QStringLiteral("px"));
        last = m.capturedEnd();
    }
    out.append(QStringView{qss}.mid(last));
    return out;
}

} // namespace rt::UiScale
