/*
 * ExportMiniTimeline.cpp — Scrub bar for the Export panel preview.
 */

#include "ExportMiniTimeline.h"
#include "Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace rt {

// ── Layout constants ────────────────────────────────────────────────────────
static constexpr int kBarMarginH   = 14;  // Left/right padding (increased to avoid timecode cutoff)
static constexpr int kBarMarginTop = 14;  // Top padding (room for timecodes)
static constexpr int kBarHeight    = 16;  // Main bar height
static constexpr int kPlayheadW    = 2;   // Playhead line width
static constexpr int kMarkerTriH   = 7;   // In/Out marker triangle height
static constexpr int kTimecodeH    = 14;  // Height of timecode text area below bar

// ─────────────────────────────────────────────────────────────────────────────

ExportMiniTimeline::ExportMiniTimeline(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(52);
    setMaximumHeight(64);
}

void ExportMiniTimeline::setDuration(int64_t durationTicks)
{
    m_duration = std::max<int64_t>(durationTicks, 1);
    update();
}

void ExportMiniTimeline::setInOutRange(int64_t inTick, int64_t outTick)
{
    m_inPoint  = inTick;
    m_outPoint = outTick;
    update();
}

void ExportMiniTimeline::setPlayhead(int64_t tick)
{
    m_playhead = std::clamp<int64_t>(tick, 0, m_duration);
    update();
}

// ── Coordinate helpers ──────────────────────────────────────────────────────

int64_t ExportMiniTimeline::xToTick(int x) const
{
    int barLeft  = kBarMarginH;
    int barRight = width() - kBarMarginH;
    int barW     = barRight - barLeft;
    if (barW <= 0) return 0;

    double t = static_cast<double>(x - barLeft) / barW;
    t = std::clamp(t, 0.0, 1.0);
    return static_cast<int64_t>(t * m_duration);
}

int ExportMiniTimeline::tickToX(int64_t tick) const
{
    int barLeft  = kBarMarginH;
    int barRight = width() - kBarMarginH;
    int barW     = barRight - barLeft;
    if (m_duration <= 0) return barLeft;

    double t = static_cast<double>(tick) / m_duration;
    t = std::clamp(t, 0.0, 1.0);
    return barLeft + static_cast<int>(t * barW);
}

// ── Painting ────────────────────────────────────────────────────────────────

void ExportMiniTimeline::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto& tc = Theme::colors();
    const int w = width();
    const int barLeft  = kBarMarginH;
    const int barRight = w - kBarMarginH;
    const int barW     = barRight - barLeft;
    const int barTop   = kBarMarginTop;
    const int barBot   = barTop + kBarHeight;

    if (barW <= 0) return;

    // ── 1. Draw the full-range bar background ───────────────────────
    QRect barRect(barLeft, barTop, barW, kBarHeight);
    p.setPen(Qt::NoPen);
    p.setBrush(tc.surface0);
    p.drawRoundedRect(barRect, 3, 3);

    // ── 2. Draw the active export range (brighter) ──────────────────
    int64_t rangeIn  = (m_inPoint >= 0)  ? m_inPoint  : 0;
    int64_t rangeOut = (m_outPoint > 0)   ? m_outPoint : m_duration;

    int xIn  = tickToX(rangeIn);
    int xOut = tickToX(rangeOut);

    // Active range fill
    QRect activeRect(xIn, barTop, xOut - xIn, kBarHeight);
    p.setBrush(tc.accent);
    p.drawRect(activeRect);

    // ── 3. Darken areas outside the export range ────────────────────
    if (xIn > barLeft) {
        QRect leftDim(barLeft, barTop, xIn - barLeft, kBarHeight);
        QColor dimOverlay(0, 0, 0, 140);
        p.setBrush(dimOverlay);
        p.drawRect(leftDim);
    }
    if (xOut < barRight) {
        QRect rightDim(xOut, barTop, barRight - xOut, kBarHeight);
        QColor dimOverlay(0, 0, 0, 140);
        p.setBrush(dimOverlay);
        p.drawRect(rightDim);
    }

    // ── 4. Draw time ticks along the bar ────────────────────────────
    {
        double durationSec = static_cast<double>(m_duration) / 48000.0;
        // Choose a nice tick interval based on duration
        double tickIntervalSec = 1.0;
        if (durationSec > 600) tickIntervalSec = 60.0;
        else if (durationSec > 120) tickIntervalSec = 30.0;
        else if (durationSec > 60)  tickIntervalSec = 10.0;
        else if (durationSec > 20)  tickIntervalSec = 5.0;
        else if (durationSec > 5)   tickIntervalSec = 1.0;
        else                         tickIntervalSec = 0.5;

        p.setPen(QPen(tc.border, 1));
        QFont tickFont = font();
        tickFont.setPixelSize(11);
        p.setFont(tickFont);

        for (double sec = tickIntervalSec; sec < durationSec; sec += tickIntervalSec) {
            int64_t tick = static_cast<int64_t>(sec * 48000.0);
            int x = tickToX(tick);

            // Small tick mark
            p.drawLine(x, barTop, x, barTop + 3);

            // Timecode label above bar (only every other tick to avoid crowding)
            double labelInterval = tickIntervalSec * 2.0;
            if (tickIntervalSec >= 10.0) labelInterval = tickIntervalSec;
            if (std::fmod(sec, labelInterval) < 0.01) {
                int totalSec = static_cast<int>(sec);
                int mins = totalSec / 60;
                int secs = totalSec % 60;
                QString tcStr = QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
                p.setPen(tc.textDisabled);
                QRect tcRect(x - 20, 0, 40, barTop - 1);
                p.drawText(tcRect, Qt::AlignCenter, tcStr);
                p.setPen(QPen(tc.border, 1));
            }
        }
    }

    // ── 5. Draw In/Out point markers as triangles ───────────────────
    auto drawMarkerTriangle = [&](int x, bool isIn) {
        QPainterPath tri;
        if (isIn) {
            // Downward triangle from top of bar, pointing right
            tri.moveTo(x, barTop - 1);
            tri.lineTo(x + kMarkerTriH, barTop - 1);
            tri.lineTo(x, barTop + kMarkerTriH - 1);
        } else {
            // Downward triangle, pointing left
            tri.moveTo(x, barTop - 1);
            tri.lineTo(x - kMarkerTriH, barTop - 1);
            tri.lineTo(x, barTop + kMarkerTriH - 1);
        }
        tri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(tc.accent);
        p.drawPath(tri);

        // Vertical line at marker position
        p.setPen(QPen(tc.accent, 1));
        p.drawLine(x, barTop, x, barBot);
    };

    if (m_inPoint >= 0) {
        drawMarkerTriangle(tickToX(m_inPoint), true);
    }
    if (m_outPoint > 0 && m_outPoint < m_duration) {
        drawMarkerTriangle(tickToX(m_outPoint), false);
    }

    // ── 6. Draw the playhead ────────────────────────────────────────
    {
        int phX = tickToX(m_playhead);
        p.setPen(QPen(tc.textBright, kPlayheadW));
        p.drawLine(phX, barTop - 2, phX, barBot + 2);

        // Small triangle at top of playhead
        QPainterPath phTri;
        phTri.moveTo(phX - 4, barTop - 3);
        phTri.lineTo(phX + 4, barTop - 3);
        phTri.lineTo(phX, barTop + 1);
        phTri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(tc.textBright);
        p.drawPath(phTri);
    }

    // ── 7. Draw timecodes at start/end ──────────────────────────────
    {
        QFont tcFont = font();
        tcFont.setPixelSize(11);
        p.setFont(tcFont);
        p.setPen(tc.textDisabled);
        const int tcAscent = p.fontMetrics().ascent();

        // Start timecode — smart-align: left normally, shift right if
        // the bar margin is too tight for the text to fit.
        {
            QString startTc = QStringLiteral("0:00");
            int tcW = p.fontMetrics().horizontalAdvance(startTc) + 4;
            int tcX = barLeft;
            if (tcX + tcW > w)
                tcX = w - tcW;
            p.drawText(QPointF(std::max(tcX, 0), barBot + 3 + tcAscent), startTc);
        }

        // End timecode — smart-align: right normally, shift left if
        // the bar margin is too tight for the text to fit.
        {
            double durSec = static_cast<double>(m_duration) / 48000.0;
            int endMins = static_cast<int>(durSec) / 60;
            int endSecs = static_cast<int>(durSec) % 60;
            QString endTc = QString("%1:%2").arg(endMins).arg(endSecs, 2, 10, QChar('0'));
            int tcW = p.fontMetrics().horizontalAdvance(endTc) + 4;
            int tcX = barRight - tcW;
            if (tcX < 0)
                tcX = 0;
            p.drawText(QPointF(tcX, barBot + 3 + tcAscent), endTc);
        }

        // Playhead timecode under the playhead — smart-align so the
        // text never bleeds outside the widget edges.
        {
            double phSec = static_cast<double>(m_playhead) / 48000.0;
            int phMins = static_cast<int>(phSec) / 60;
            int phSecs = static_cast<int>(phSec) % 60;
            int phFrames = static_cast<int>((phSec - std::floor(phSec)) * 30.0);
            QString phTc = QString("%1:%2:%3")
                .arg(phMins).arg(phSecs, 2, 10, QChar('0')).arg(phFrames, 2, 10, QChar('0'));
            int phX = tickToX(m_playhead);
            int tcW  = p.fontMetrics().horizontalAdvance(phTc) + 6;
            int tcH  = kTimecodeH;

            // Compute ideal centered rect, then clamp to widget edges
            QRect phTcRect(phX - tcW / 2, barBot + 3, tcW, tcH);
            // Shift right if clipped on the left
            if (phTcRect.left() < 0)
                phTcRect.moveLeft(0);
            // Shift left if clipped on the right
            if (phTcRect.right() >= w)
                phTcRect.moveRight(w - 1);

            // Determine alignment based on actual position relative to playhead
            int alignment = Qt::AlignTop;
            if (phTcRect.left() <= 0)
                alignment |= Qt::AlignLeft;
            else if (phTcRect.right() >= w - 1)
                alignment |= Qt::AlignRight;
            else
                alignment |= Qt::AlignHCenter;

            p.drawText(phTcRect, alignment, phTc);
        }
    }
}

// ── Mouse interaction ───────────────────────────────────────────────────────

void ExportMiniTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_duration > 0) {
        m_dragging = true;
        int64_t tick = xToTick(static_cast<int>(event->position().x()));
        setPlayhead(tick);
        emit scrubbed(m_playhead);
    }
}

void ExportMiniTimeline::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && m_duration > 0) {
        int64_t tick = xToTick(static_cast<int>(event->position().x()));
        setPlayhead(tick);
        emit scrubbed(m_playhead);
    }
}

void ExportMiniTimeline::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
}

} // namespace rt
