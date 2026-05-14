/*
 * MiniTimeline.cpp — Compact scrub bar implementation.
 * Step 15
 */

#include "widgets/MiniTimeline.h"
#include "Theme.h"
#include "UiScale.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

MiniTimeline::MiniTimeline(QWidget* parent)
    : QWidget(parent)
{
    rt::UiScale::setScaledFixedHeight(this, kBarHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

MiniTimeline::~MiniTimeline() = default;

// ═════════════════════════════════════════════════════════════════════════════
//  Range & playhead
// ═════════════════════════════════════════════════════════════════════════════

void MiniTimeline::setDuration(int64_t ticks)
{
    m_duration = std::max(int64_t(0), ticks);
    update();
}

void MiniTimeline::setPlayhead(int64_t tick)
{
    m_playhead = clampTick(tick);
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  In / Out points
// ═════════════════════════════════════════════════════════════════════════════

void MiniTimeline::setInPoint(int64_t tick)
{
    // -1 means "not set" — store as-is, don't clamp to 0
    int64_t clamped = (tick >= 0) ? clampTick(tick) : -1;
    if (m_inPoint == clamped) return;
    m_inPoint = clamped;
    emit inPointChanged(m_inPoint);
    update();
}

void MiniTimeline::setOutPoint(int64_t tick)
{
    int64_t clamped = (tick >= 0) ? clampTick(tick) : -1;
    if (m_outPoint == clamped) return;
    m_outPoint = clamped;
    emit outPointChanged(m_outPoint);
    update();
}

void MiniTimeline::clearInOutPoints()
{
    m_inPoint  = -1;
    m_outPoint = -1;
    update();
}

int64_t MiniTimeline::selectedDuration() const noexcept
{
    if (hasInPoint() && hasOutPoint() && m_outPoint > m_inPoint)
        return m_outPoint - m_inPoint;
    return m_duration;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Frame rate
// ═════════════════════════════════════════════════════════════════════════════

void MiniTimeline::setFrameRate(double fps) noexcept
{
    m_fps = (fps > 0.0) ? fps : 24.0;
}

void MiniTimeline::setMarkers(const std::vector<MarkerCue>& markers)
{
    m_markers = markers;
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Coordinate mapping
// ═════════════════════════════════════════════════════════════════════════════

double MiniTimeline::barWidth() const noexcept
{
    double w = static_cast<double>(width()) - 2.0 * kMarginH;
    return (w > 0.0) ? w : 1.0;
}

int64_t MiniTimeline::positionToTick(double xPixel) const noexcept
{
    if (m_duration <= 0) return 0;
    double frac = (xPixel - kMarginH) / barWidth();
    frac = std::clamp(frac, 0.0, 1.0);
    return clampTick(static_cast<int64_t>(frac * m_duration));
}

double MiniTimeline::tickToPosition(int64_t tick) const noexcept
{
    if (m_duration <= 0) return kMarginH;
    double frac = static_cast<double>(tick) / static_cast<double>(m_duration);
    frac = std::clamp(frac, 0.0, 1.0);
    return kMarginH + frac * barWidth();
}

int64_t MiniTimeline::clampTick(int64_t tick) const noexcept
{
    return std::clamp(tick, int64_t(0), std::max(int64_t(0), m_duration));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Size hints
// ═════════════════════════════════════════════════════════════════════════════

QSize MiniTimeline::sizeHint() const
{
    return QSize(rt::UiScale::px(400), rt::UiScale::px(kBarHeight));
}

QSize MiniTimeline::minimumSizeHint() const
{
    return QSize(rt::UiScale::px(100), rt::UiScale::px(kBarHeight));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Painting
// ═════════════════════════════════════════════════════════════════════════════

void MiniTimeline::paintEvent(QPaintEvent* event)
{
    // Re-entrancy guard. The previous version forgot to decrement
    // s_paintDepth on the normal-paint path, so after 5 paints the widget
    // silently no-op'd every subsequent paintEvent — the stale framebuffer
    // was left in place, producing the "visual echo" between the control
    // bar and transport bar when the monitor was resized vertically.
    static thread_local int s_paintDepth = 0;
    struct DepthGuard {
        int& d; explicit DepthGuard(int& v) : d(v) { ++d; } ~DepthGuard() { --d; }
    } depthGuard(s_paintDepth);
    if (s_paintDepth > 5) {
        QWidget::paintEvent(event);
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int h = height();
    const double bw = barWidth();

    // ── Three-zone layout (Premiere Pro style) ──────────────────────────
    //  Zone 1: Ruler        (top ~16px) — tick marks pointing DOWN
    //  Zone 2: Content bar  (middle)    — clip area with in/out brackets
    //  Zone 3: Scrub track  (bottom ~16px) — thin line + circle handle
    const int rulerH   = 16;
    const int scrubZoneH = 16;
    const int contentY = rulerH;
    const int contentH = h - rulerH - scrubZoneH;
    const int scrubY   = contentY + contentH;

    // ── Colors — VERY high contrast for guaranteed visibility ───────────
    QColor bgColor(20, 20, 24);              // overall dark bg
    QColor contentDark(40, 40, 48);          // content bar outside in/out
    QColor contentActive(72, 72, 82);        // content bar inside in/out
    QColor tickSmall(140, 140, 150);         // small ruler ticks — clearly visible
    QColor tickMajor(220, 220, 230);         // major ruler ticks — near white
    QColor scrubTrackCol(90, 90, 100);       // horizontal scrub line
    QColor scrubHandleCol(230, 230, 235);    // circle scrub handle — near white
    QColor phColor(50, 140, 255);            // playhead blue — vivid
    QColor bracketCol(50, 140, 255);         // in/out bracket blue
    QColor borderCol(8, 8, 10);              // top/bottom border

    // ── Background fill ─────────────────────────────────────────────────
    p.setPen(Qt::NoPen);
    p.setBrush(bgColor);
    p.drawRect(0, 0, w, h);

    // ── Content bar ─────────────────────────────────────────────────────
    if (m_duration <= 0) {
        // No content — draw entire content bar in active shade
        p.setBrush(contentActive);
        p.drawRect(QRectF(kMarginH, contentY, bw, contentH));
    } else {
        // Full bar in dark
        p.setBrush(contentDark);
        p.drawRect(QRectF(kMarginH, contentY, bw, contentH));

        // Active region (between in/out) in lighter shade
        double xIn  = hasInPoint()  ? tickToPosition(m_inPoint)  : static_cast<double>(kMarginH);
        double xOut = hasOutPoint() ? tickToPosition(m_outPoint) : kMarginH + bw;
        p.setBrush(contentActive);
        p.drawRect(QRectF(xIn, contentY, xOut - xIn, contentH));
    }

    // ── Ruler tick marks ────────────────────────────────────────────────
    if (m_duration > 0) {
        const double ticksPerFrame = (m_fps > 0.0) ? (48000.0 / m_fps) : 2000.0;
        const double totalFrames = static_cast<double>(m_duration) / ticksPerFrame;

        // Calculate frame interval so ticks are ≥ 6px apart
        int frameInterval = 1;
        if (totalFrames > 0) {
            double pxPerFrame = bw / totalFrames;
            if (pxPerFrame < 6.0)
                frameInterval = static_cast<int>(std::ceil(6.0 / pxPerFrame));
        }

        // Round to nice values
        if      (frameInterval > 30) frameInterval = static_cast<int>(std::ceil(frameInterval / 30.0)) * 30;
        else if (frameInterval > 10) frameInterval = static_cast<int>(std::ceil(frameInterval / 10.0)) * 10;
        else if (frameInterval > 5)  frameInterval = 10;
        else if (frameInterval > 2)  frameInterval = 5;
        else if (frameInterval > 1)  frameInterval = 2;

        int majorMod = 10;
        if (frameInterval >= 30) majorMod = 5;

        const int numFrames = static_cast<int>(totalFrames);
        for (int f = 0; f <= numFrames; f += frameInterval) {
            double tx = kMarginH + (static_cast<double>(f) / totalFrames) * bw;
            int idx = f / frameInterval;
            bool major = (idx % majorMod == 0);

            int tickH = major ? (rulerH - 3) : (rulerH / 2);
            p.setPen(QPen(major ? tickMajor : tickSmall, major ? 2.0 : 1.0));
            p.drawLine(QPointF(tx, 1), QPointF(tx, 1 + tickH));
        }
    }

    // ── In/Out bracket markers ──────────────────────────────────────────
    if (m_duration > 0) {
        if (hasInPoint()) {
            double xi = tickToPosition(m_inPoint);
            p.setPen(QPen(bracketCol, 2));
            p.drawLine(QPointF(xi, contentY), QPointF(xi, contentY + contentH));
            p.drawLine(QPointF(xi, contentY), QPointF(xi + 5, contentY));
            p.drawLine(QPointF(xi, contentY + contentH - 1), QPointF(xi + 5, contentY + contentH - 1));
        }
        if (hasOutPoint()) {
            double xo = tickToPosition(m_outPoint);
            p.setPen(QPen(bracketCol, 2));
            p.drawLine(QPointF(xo, contentY), QPointF(xo, contentY + contentH));
            p.drawLine(QPointF(xo, contentY), QPointF(xo - 5, contentY));
            p.drawLine(QPointF(xo, contentY + contentH - 1), QPointF(xo - 5, contentY + contentH - 1));
        }
    }

    // ── Marker cue points ────────────────────────────────────────────────
    if (m_duration > 0 && !m_markers.empty()) {
        for (const auto& mk : m_markers) {
            double mx = tickToPosition(mk.tick);
            // Convert RGBA to QColor (stored as 0xRRGGBBAA)
            QColor mc(
                static_cast<int>((mk.color >> 24) & 0xFF),
                static_cast<int>((mk.color >> 16) & 0xFF),
                static_cast<int>((mk.color >> 8)  & 0xFF),
                static_cast<int>(mk.color & 0xFF));
            // Draw small diamond on content bar
            p.setPen(Qt::NoPen);
            p.setBrush(mc);
            QPolygonF diamond;
            diamond << QPointF(mx, contentY + 2)
                    << QPointF(mx + 4, contentY + contentH / 2.0)
                    << QPointF(mx, contentY + contentH - 2)
                    << QPointF(mx - 4, contentY + contentH / 2.0);
            p.drawPolygon(diamond);
        }
    }

    // ── Scrub track (thin horizontal line + circle handle) ──────────────
    double trackCenterY = scrubY + scrubZoneH / 2.0;
    p.setPen(QPen(scrubTrackCol, 2));
    p.drawLine(QPointF(kMarginH, trackCenterY), QPointF(kMarginH + bw, trackCenterY));

    // ── Playhead ────────────────────────────────────────────────────────
    if (m_duration > 0) {
        double px = tickToPosition(m_playhead);

        // Blue vertical line spanning ruler + content bar
        p.setPen(QPen(phColor, 2.0));
        p.drawLine(QPointF(px, 0), QPointF(px, contentY + contentH));

        // Inverted triangle at ruler/content boundary
        p.setPen(Qt::NoPen);
        p.setBrush(phColor);
        QPolygonF tri;
        tri << QPointF(px - 6, contentY)
            << QPointF(px + 6, contentY)
            << QPointF(px, contentY + 8);
        p.drawPolygon(tri);

        // Circle handle on scrub track (larger for visibility)
        p.setPen(Qt::NoPen);
        p.setBrush(scrubHandleCol);
        p.drawEllipse(QPointF(px, trackCenterY), 6.0, 6.0);
    }

    // ── Border lines ────────────────────────────────────────────────────
    p.setPen(QPen(borderCol, 1));
    p.drawLine(0, 0, w, 0);
    p.drawLine(0, h - 1, w, h - 1);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Mouse events (scrubbing)
// ═════════════════════════════════════════════════════════════════════════════

void MiniTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_duration > 0)
    {
        m_dragging = true;
        int64_t tick = positionToTick(event->position().x());
        setPlayhead(tick);
        emit scrubbed(tick);
    }
}

void MiniTimeline::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && m_duration > 0)
    {
        int64_t tick = positionToTick(event->position().x());
        setPlayhead(tick);
        emit scrubbed(tick);
    }
}

void MiniTimeline::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        m_dragging = false;
}

} // namespace rt

