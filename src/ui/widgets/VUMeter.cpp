/*
 * VUMeter.cpp — Audio level meter widget.
 * Step 19: GPU Waveform Renderer
 */

#include "widgets/VUMeter.h"
#include "Theme.h"

#include <QPainter>
#include <QTimerEvent>

#include <algorithm>
#include <cmath>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

VUMeter::VUMeter(QWidget* parent)
    : QWidget(parent)
{
    m_channels.resize(static_cast<size_t>(m_channelCount));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
}

VUMeter::~VUMeter()
{
    if (m_timerId)
        killTimer(m_timerId);
}

QSize VUMeter::sizeHint() const
{
    int barWidth = 12;
    int spacing  = 2;
    int scaleW   = m_scaleVisible ? 20 : 0;
    int w = m_channelCount * barWidth + (m_channelCount - 1) * spacing + scaleW + 4;
    return (m_orientation == Orientation::Vertical)
        ? QSize(w, 120) : QSize(120, w);
}

QSize VUMeter::minimumSizeHint() const
{
    return (m_orientation == Orientation::Vertical)
        ? QSize(20, 40) : QSize(40, 20);
}

// ═════════════════════════════════════════════════════════════════════════════
// Level input
// ═════════════════════════════════════════════════════════════════════════════

void VUMeter::setLevel(int channel, float lvl)
{
    if (channel < 0 || channel >= m_channelCount) return;
    auto& ch = m_channels[static_cast<size_t>(channel)];
    ch.currentLevel = std::clamp(lvl, 0.0f, 2.0f);

    // Update peak hold
    if (lvl > ch.peakLevel) {
        ch.peakLevel = lvl;
        ch.peakHoldTimer = 0;
    }

    // Clipping
    if (lvl >= 1.0f) {
        ch.clipping = true;
        emit clipping(channel);
    }

    // Start refresh timer if not running
    if (!m_timerId)
        m_timerId = startTimer(33); // ~30 Hz refresh

    update();
}

void VUMeter::setLevels(const std::vector<float>& levels)
{
    int count = std::min(static_cast<int>(levels.size()), m_channelCount);
    for (int i = 0; i < count; ++i)
        setLevel(i, levels[static_cast<size_t>(i)]);
}

float VUMeter::level(int channel) const
{
    if (channel < 0 || channel >= m_channelCount) return 0.0f;
    return m_channels[static_cast<size_t>(channel)].currentLevel;
}

void VUMeter::reset()
{
    for (auto& ch : m_channels) {
        ch.currentLevel = 0.0f;
        ch.peakLevel = 0.0f;
        ch.peakHoldTimer = 0;
        ch.clipping = false;
    }
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
// Configuration
// ═════════════════════════════════════════════════════════════════════════════

void VUMeter::setChannelCount(int channels)
{
    m_channelCount = std::max(channels, 1);
    m_channels.resize(static_cast<size_t>(m_channelCount));
    update();
}

void VUMeter::setOrientation(Orientation orient)
{
    m_orientation = orient;
    updateGeometry();
    update();
}

void VUMeter::setPeakHoldEnabled(bool enabled)
{
    m_peakHoldEnabled = enabled;
    update();
}

void VUMeter::setPeakHoldDecayMs(int ms)
{
    m_peakHoldDecayMs = ms;
}

void VUMeter::setScaleVisible(bool visible)
{
    m_scaleVisible = visible;
    updateGeometry();
    update();
}

void VUMeter::setMinDb(float db)
{
    m_minDb = db;
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
// Appearance
// ═════════════════════════════════════════════════════════════════════════════

void VUMeter::setBackgroundColor(const QColor& color)
{
    m_bgColor = color;
    update();
}

void VUMeter::setGradientColors(const QColor& low, const QColor& mid, const QColor& high)
{
    m_colorLow  = low;
    m_colorMid  = mid;
    m_colorHigh = high;
    update();
}

void VUMeter::setClipColor(const QColor& color)
{
    m_clipColor = color;
    update();
}

void VUMeter::setPeakHoldColor(const QColor& color)
{
    m_peakHoldColor = color;
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
// Utility
// ═════════════════════════════════════════════════════════════════════════════

float VUMeter::linearToDb(float lvl)
{
    if (lvl <= 0.0f) return -120.0f;
    return 20.0f * std::log10(lvl);
}

float VUMeter::dbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

float VUMeter::dbToPosition(float db) const
{
    return std::clamp((db - m_minDb) / (0.0f - m_minDb), 0.0f, 1.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
// Paint
// ═════════════════════════════════════════════════════════════════════════════

void VUMeter::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), m_bgColor);

    if (m_orientation == Orientation::Vertical)
        paintVertical(painter);
    else
        paintHorizontal(painter);

    if (m_scaleVisible)
        paintScale(painter);
}

void VUMeter::paintVertical(QPainter& painter)
{
    int scaleW = m_scaleVisible ? 24 : 0;
    int availW = width() - scaleW - 4;
    int spacing = 3;  // gap between channel bars
    int barW = (availW - spacing * (m_channelCount - 1)) / std::max(m_channelCount, 1);
    barW = std::max(barW, 3);

    int meterH = height() - 6;
    int meterTop = 3;

    // Premiere Pro style: segmented bars with 2px gaps between segments
    constexpr int segmentH  = 3;   // height of each lit segment
    constexpr int segmentGap = 1;  // gap between segments
    int segStep = segmentH + segmentGap;
    int numSegments = meterH / segStep;

    for (int ch = 0; ch < m_channelCount; ++ch) {
        int x = scaleW + 2 + ch * (barW + spacing);
        const auto& state = m_channels[static_cast<size_t>(ch)];

        float db = linearToDb(state.currentLevel);
        float pos = dbToPosition(db);
        int litSegments = static_cast<int>(pos * numSegments);

        // Draw each segment bottom-to-top
        for (int seg = 0; seg < numSegments; ++seg) {
            int y = meterTop + meterH - (seg + 1) * segStep;
            if (y < meterTop) break;

            float segPos = static_cast<float>(seg + 1) / numSegments;
            bool lit = (seg < litSegments);

            QColor color;
            if (lit) {
                color = gradientColor(segPos);
            } else {
                // Unlit segment: very dark version of the gradient color
                QColor base = gradientColor(segPos);
                color = QColor(base.red() / 8, base.green() / 8, base.blue() / 8);
            }

            painter.fillRect(x, y, barW, segmentH, color);
        }

        // Peak hold indicator (bright white line)
        if (m_peakHoldEnabled && state.peakLevel > 0.001f) {
            float peakDb = linearToDb(state.peakLevel);
            float peakPos = dbToPosition(peakDb);
            int peakSeg = static_cast<int>(peakPos * numSegments);
            int peakY = meterTop + meterH - (peakSeg + 1) * segStep;
            if (peakY >= meterTop) {
                painter.fillRect(x, peakY, barW, segmentH, m_peakHoldColor);
            }
        }

        // Clip indicator (top bar glows red when clipping)
        if (state.clipping) {
            painter.fillRect(x, meterTop, barW, segmentH + 1, m_clipColor);
        }
    }

    // Channel separator line (thin dark line between L and R)
    if (m_channelCount >= 2) {
        int sepX = scaleW + 2 + barW + spacing / 2;
        painter.setPen(Theme::colors().surface0);
        painter.drawLine(sepX, meterTop, sepX, meterTop + meterH);
    }
}

void VUMeter::paintHorizontal(QPainter& painter)
{
    int scaleH = m_scaleVisible ? 16 : 0;
    int availH = height() - scaleH - 4;
    int spacing = 2;
    int barH = (availH - spacing * (m_channelCount - 1)) / std::max(m_channelCount, 1);
    barH = std::max(barH, 2);

    int meterW = width() - 4;

    for (int ch = 0; ch < m_channelCount; ++ch) {
        int y = 2 + ch * (barH + spacing);
        const auto& state = m_channels[static_cast<size_t>(ch)];

        float db = linearToDb(state.currentLevel);
        float pos = dbToPosition(db);
        int filledW = static_cast<int>(pos * meterW);

        // Draw filled bar with gradient
        for (int col = 0; col < filledW; ++col) {
            float colPos = static_cast<float>(col) / meterW;
            painter.setPen(gradientColor(colPos));
            int xPos = 2 + col;
            painter.drawLine(xPos, y, xPos, y + barH - 1);
        }

        // Peak hold
        if (m_peakHoldEnabled && state.peakLevel > 0.0f) {
            float peakDb = linearToDb(state.peakLevel);
            float peakPos = dbToPosition(peakDb);
            int peakX = 2 + static_cast<int>(peakPos * meterW);
            painter.setPen(m_peakHoldColor);
            painter.drawLine(peakX, y, peakX, y + barH - 1);
        }

        // Clip indicator
        if (state.clipping) {
            painter.fillRect(meterW - 1, y, 3, barH, m_clipColor);
        }
    }
}

void VUMeter::paintScale(QPainter& painter)
{
    painter.setPen(Theme::colors().textDisabled);
    QFont font = painter.font();
    font.setPixelSize(9);
    painter.setFont(font);

    // Premiere Pro dB markings:  0, -3, -6, -9, -12, -18, -24, -36, -48
    const float dbMarks[] = {0.0f, -3.0f, -6.0f, -9.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f};
    int meterH = height() - 6;
    int meterTop = 3;

    for (float db : dbMarks) {
        if (db < m_minDb) continue;
        float pos = dbToPosition(db);

        if (m_orientation == Orientation::Vertical) {
            int y = meterTop + meterH - static_cast<int>(pos * meterH);
            // Tick mark
            painter.setPen(Theme::colors().border);
            painter.drawLine(0, y, 4, y);
            // Label
            painter.setPen(Theme::colors().textDisabled);
            QString label = (db == 0.0f) ? QStringLiteral("0") : QString::number(static_cast<int>(db));
            painter.drawText(0, y - 5, 22, 10, Qt::AlignRight | Qt::AlignVCenter, label);
        } else {
            int meterW = width() - 4;
            int x = 2 + static_cast<int>(pos * meterW);
            painter.drawLine(x, height() - 3, x, height());
            painter.drawText(x - 10, height() - 14, 20, 12, Qt::AlignCenter, QString::number(static_cast<int>(db)));
        }
    }
}

QColor VUMeter::gradientColor(float normalizedPosition) const
{
    normalizedPosition = std::clamp(normalizedPosition, 0.0f, 1.0f);

    // Green zone: 0.0 - 0.6, Yellow zone: 0.6 - 0.85, Red zone: 0.85 - 1.0
    if (normalizedPosition < 0.6f) {
        float t = normalizedPosition / 0.6f;
        int r = static_cast<int>(m_colorLow.red()   + t * (m_colorMid.red()   - m_colorLow.red()));
        int g = static_cast<int>(m_colorLow.green() + t * (m_colorMid.green() - m_colorLow.green()));
        int b = static_cast<int>(m_colorLow.blue()  + t * (m_colorMid.blue()  - m_colorLow.blue()));
        return QColor(r, g, b);
    } else {
        float t = (normalizedPosition - 0.6f) / 0.4f;
        int r = static_cast<int>(m_colorMid.red()   + t * (m_colorHigh.red()   - m_colorMid.red()));
        int g = static_cast<int>(m_colorMid.green() + t * (m_colorHigh.green() - m_colorMid.green()));
        int b = static_cast<int>(m_colorMid.blue()  + t * (m_colorHigh.blue()  - m_colorMid.blue()));
        return QColor(r, g, b);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Timer (peak hold decay)
// ═════════════════════════════════════════════════════════════════════════════

void VUMeter::timerEvent(QTimerEvent* event)
{
    if (event->timerId() != m_timerId) {
        QWidget::timerEvent(event);
        return;
    }

    bool needsUpdate = false;
    int decayFrames = m_peakHoldDecayMs / 33; // timer fires at ~30 Hz

    for (auto& ch : m_channels) {
        // Decay peak hold
        if (m_peakHoldEnabled && ch.peakLevel > 0.0f) {
            ch.peakHoldTimer++;
            if (ch.peakHoldTimer >= decayFrames) {
                ch.peakLevel *= 0.9f;
                if (ch.peakLevel < 0.001f)
                    ch.peakLevel = 0.0f;
                needsUpdate = true;
            }
        }

        // Smooth level decay (for visual smoothing)
        if (ch.currentLevel > 0.0f) {
            ch.currentLevel *= 0.85f;
            if (ch.currentLevel < 0.001f)
                ch.currentLevel = 0.0f;
            needsUpdate = true;
        }
    }

    if (needsUpdate)
        update();
    else {
        // Stop timer when nothing to animate
        killTimer(m_timerId);
        m_timerId = 0;
    }
}

} // namespace rt

