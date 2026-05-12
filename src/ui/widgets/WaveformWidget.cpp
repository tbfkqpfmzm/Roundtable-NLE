/*
 * WaveformWidget.cpp — Qt waveform display widget.
 * Step 19: GPU Waveform Renderer
 */

#include "widgets/WaveformWidget.h"
#include "Theme.h"
#include "WaveformRenderer.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

WaveformWidget::WaveformWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(40);
}

WaveformWidget::~WaveformWidget() = default;

QSize WaveformWidget::sizeHint() const
{
    return {400, 120};
}

QSize WaveformWidget::minimumSizeHint() const
{
    return {100, 40};
}

// ═════════════════════════════════════════════════════════════════════════════
// Data source
// ═════════════════════════════════════════════════════════════════════════════

void WaveformWidget::setWaveformData(const WaveformData* wfData)
{
    m_waveformData = wfData;

    if (wfData && wfData->totalFrames > 0) {
        if (m_visibleEnd <= m_visibleStart) {
            m_visibleStart = 0;
            m_visibleEnd = wfData->totalFrames;
        }
        updateSamplesPerPixel();
    }

    update();
}

// ═════════════════════════════════════════════════════════════════════════════
// View control
// ═════════════════════════════════════════════════════════════════════════════

void WaveformWidget::setVisibleRange(int64_t startFrame, int64_t endFrame)
{
    if (startFrame >= endFrame) return;

    m_visibleStart = startFrame;
    m_visibleEnd   = endFrame;
    updateSamplesPerPixel();
    update();
    emit visibleRangeChanged(m_visibleStart, m_visibleEnd);
}

void WaveformWidget::zoomIn()
{
    int64_t visible = m_visibleEnd - m_visibleStart;
    int64_t newVisible = std::max(visible / 2, static_cast<int64_t>(100));
    int64_t center = (m_visibleStart + m_visibleEnd) / 2;
    setVisibleRange(center - newVisible / 2, center + newVisible / 2);
}

void WaveformWidget::zoomOut()
{
    int64_t visible = m_visibleEnd - m_visibleStart;
    int64_t newVisible = visible * 2;
    int64_t totalFrames = m_waveformData ? m_waveformData->totalFrames : newVisible;
    newVisible = std::min(newVisible, totalFrames);
    int64_t center = (m_visibleStart + m_visibleEnd) / 2;
    int64_t start = std::max(center - newVisible / 2, static_cast<int64_t>(0));
    int64_t end = start + newVisible;
    if (end > totalFrames) {
        end = totalFrames;
        start = std::max(end - newVisible, static_cast<int64_t>(0));
    }
    setVisibleRange(start, end);
}

void WaveformWidget::zoomToFit()
{
    if (m_waveformData && m_waveformData->totalFrames > 0) {
        setVisibleRange(0, m_waveformData->totalFrames);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Playhead
// ═════════════════════════════════════════════════════════════════════════════

void WaveformWidget::setPlayheadFrame(int64_t frame)
{
    m_playheadFrame = frame;
    update();
}

void WaveformWidget::setPlayheadVisible(bool visible)
{
    m_playheadVisible = visible;
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
// Segments
// ═════════════════════════════════════════════════════════════════════════════

void WaveformWidget::setSegments(const std::vector<WaveformSegment>& segments)
{
    m_segments = segments;
    update();
}

void WaveformWidget::setActiveSegment(int index)
{
    m_activeSegment = index;
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
// Appearance
// ═════════════════════════════════════════════════════════════════════════════

void WaveformWidget::setBackgroundColor(const QColor& color)
{
    m_bgColor = color;
    update();
}

void WaveformWidget::setWaveformColors(const QColor& low, const QColor& mid, const QColor& high)
{
    m_colorLow  = low;
    m_colorMid  = mid;
    m_colorHigh = high;
    update();
}

void WaveformWidget::setPlayheadColor(const QColor& color)
{
    m_playheadColor = color;
    update();
}

void WaveformWidget::setTimeGridVisible(bool visible)
{
    m_timeGridVisible = visible;
    update();
}

void WaveformWidget::setChannelCount(int channels)
{
    m_channelCount = channels;
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
// Coordinate conversion
// ═════════════════════════════════════════════════════════════════════════════

double WaveformWidget::frameToPixelX(int64_t frame) const
{
    if (m_visibleEnd <= m_visibleStart) return 0.0;
    double frac = static_cast<double>(frame - m_visibleStart) /
                  static_cast<double>(m_visibleEnd - m_visibleStart);
    return frac * width();
}

int64_t WaveformWidget::pixelXToFrame(double x) const
{
    if (width() <= 0) return m_visibleStart;
    double frac = x / width();
    return m_visibleStart +
           static_cast<int64_t>(frac * (m_visibleEnd - m_visibleStart));
}

// ═════════════════════════════════════════════════════════════════════════════
// Paint
// ═════════════════════════════════════════════════════════════════════════════

void WaveformWidget::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // Background
    painter.fillRect(rect(), m_bgColor);

    if (m_waveformData && m_waveformData->totalFrames > 0) {
        paintWaveform(painter);
        paintSegments(painter);
    }

    if (m_timeGridVisible)
        paintTimeGrid(painter);

    if (m_playheadVisible)
        paintPlayhead(painter);

    --s_paintDepth;
}

void WaveformWidget::paintWaveform(QPainter& painter)
{
    if (!m_waveformData) return;

    int w = width();
    int h = height();
    if (w <= 0 || h <= 0) return;

    uint32_t spp = static_cast<uint32_t>(std::max(m_samplesPerPixel, 1.0));
    int64_t visibleFrames = m_visibleEnd - m_visibleStart;

    int channels = m_channelCount > 0
        ? m_channelCount
        : static_cast<int>(m_waveformData->channels);
    channels = std::max(channels, 1);

    float channelHeight = static_cast<float>(h) / channels;

    for (int ch = 0; ch < channels; ++ch) {
        // Get best mip level
        const auto* mip = m_waveformData->bestLevel(spp);
        if (!mip) continue;

        // Calculate peak range
        int64_t startPeak = m_visibleStart / mip->samplesPerPeak;
        int64_t endPeak = (m_visibleEnd + mip->samplesPerPeak - 1) / mip->samplesPerPeak;
        int64_t totalPeaksPerCh = static_cast<int64_t>(mip->peaksPerChannel());

        startPeak = std::clamp(startPeak, static_cast<int64_t>(0), totalPeaksPerCh);
        endPeak   = std::clamp(endPeak, startPeak, totalPeaksPerCh);

        size_t peakCount = static_cast<size_t>(endPeak - startPeak);
        if (peakCount == 0) continue;

        // Get peak data for this channel
        uint16_t channelIdx = static_cast<uint16_t>(ch < mip->channels ? ch : 0);
        const WaveformPeak* peakData = mip->peaks.data() +
            static_cast<size_t>(startPeak) * mip->channels + channelIdx;

        float centerY = channelHeight * (ch + 0.5f);
        float halfH   = channelHeight * 0.45f;

        // Convert visible frame offset to pixel offset
        float startX = static_cast<float>(
            frameToPixelX(startPeak * mip->samplesPerPeak));
        float pxPerPeak = static_cast<float>(w) *
            static_cast<float>(mip->samplesPerPeak) /
            static_cast<float>(visibleFrames);

        // Use WaveformRenderer::layoutPeaks for coordinate conversion
        auto columns = WaveformRenderer::layoutPeaks(
            peakData, peakCount,
            startX, pxPerPeak,
            centerY, halfH);

        // Draw as vertical lines with gradient color
        for (const auto& col : columns) {
            if (col.x < -1.0f || col.x > w + 1.0f) continue;

            painter.setPen(gradientColor(col.amplitude));
            int x  = static_cast<int>(col.x);
            int y1 = static_cast<int>(col.yMin);
            int y2 = static_cast<int>(col.yMax);
            // Ensure at least 1 pixel tall
            if (y1 == y2) y1 = y2 + 1;
            painter.drawLine(x, y1, x, y2);
        }

        // Center line
        painter.setPen(Theme::colors().border);
        painter.drawLine(0, static_cast<int>(centerY), w, static_cast<int>(centerY));
    }
}

void WaveformWidget::paintPlayhead(QPainter& painter)
{
    double px = frameToPixelX(m_playheadFrame);
    if (px < -2 || px > width() + 2) return;

    int x = static_cast<int>(px);

    // Glow effect (3-pass with decreasing opacity)
    painter.setPen(QPen(QColor(m_playheadColor.red(), m_playheadColor.green(),
                               m_playheadColor.blue(), 40), 5));
    painter.drawLine(x, 0, x, height());

    painter.setPen(QPen(QColor(m_playheadColor.red(), m_playheadColor.green(),
                               m_playheadColor.blue(), 80), 3));
    painter.drawLine(x, 0, x, height());

    painter.setPen(QPen(m_playheadColor, 1));
    painter.drawLine(x, 0, x, height());
}

void WaveformWidget::paintSegments(QPainter& painter)
{
    for (int i = 0; i < static_cast<int>(m_segments.size()); ++i) {
        const auto& seg = m_segments[static_cast<size_t>(i)];
        double x1 = frameToPixelX(seg.startFrame);
        double x2 = frameToPixelX(seg.endFrame);

        if (x2 < 0 || x1 > width()) continue;

        bool isActive = (i == m_activeSegment) || seg.active;
        QColor activeCol = Theme::colors().warning; activeCol.setAlpha(60);
        QColor inactiveCol = Theme::colors().border; inactiveCol.setAlpha(30);
        QColor color = isActive ? activeCol : inactiveCol;

        painter.fillRect(QRectF(x1, 0, x2 - x1, height()), color);

        // Segment boundary lines
        QColor boundaryCol = Theme::colors().warning;
        boundaryCol.setAlpha(isActive ? 200 : 80);
        painter.setPen(boundaryCol);
        painter.drawLine(static_cast<int>(x1), 0, static_cast<int>(x1), height());
    }
}

void WaveformWidget::paintTimeGrid(QPainter& painter)
{
    if (!m_waveformData || m_waveformData->sampleRate == 0) return;

    double sampleRate = m_waveformData->sampleRate;
    double visibleSeconds = (m_visibleEnd - m_visibleStart) / sampleRate;

    // Choose grid interval based on zoom
    double gridInterval;
    if (visibleSeconds > 300)      gridInterval = 60.0;
    else if (visibleSeconds > 60)  gridInterval = 10.0;
    else if (visibleSeconds > 20)  gridInterval = 5.0;
    else if (visibleSeconds > 5)   gridInterval = 1.0;
    else if (visibleSeconds > 1)   gridInterval = 0.25;
    else                           gridInterval = 0.05;

    double startTime = m_visibleStart / sampleRate;
    double firstGrid = std::ceil(startTime / gridInterval) * gridInterval;

    painter.setPen(Theme::colors().border);
    QFont font = painter.font();
    font.setPixelSize(10);
    painter.setFont(font);

    for (double t = firstGrid; t * sampleRate < m_visibleEnd; t += gridInterval) {
        auto frame = static_cast<int64_t>(t * sampleRate);
        double px = frameToPixelX(frame);

        painter.setPen(Theme::colors().border);
        painter.drawLine(static_cast<int>(px), 0, static_cast<int>(px), height());

        // Time label
        int minutes = static_cast<int>(t) / 60;
        double seconds = t - minutes * 60.0;
        QString label;
        if (gridInterval >= 1.0)
            label = QString("%1:%2").arg(minutes).arg(static_cast<int>(seconds), 2, 10, QChar('0'));
        else
            label = QString("%1:%2").arg(minutes).arg(seconds, 5, 'f', 2, QChar('0'));

        painter.setPen(Theme::colors().textDisabled);
        painter.drawText(static_cast<int>(px) + 3, height() - 3, label);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Mouse events
// ═════════════════════════════════════════════════════════════════════════════

void WaveformWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_scrubbing = true;
        m_lastMousePos = event->pos();

        int64_t frame = pixelXToFrame(event->pos().x());
        frame = std::clamp(frame, static_cast<int64_t>(0),
                           m_waveformData ? m_waveformData->totalFrames : frame);
        m_playheadFrame = frame;
        emit positionChanged(frame);

        // Check segment clicks
        for (int i = 0; i < static_cast<int>(m_segments.size()); ++i) {
            const auto& seg = m_segments[static_cast<size_t>(i)];
            if (frame >= seg.startFrame && frame < seg.endFrame) {
                emit segmentClicked(i);
                break;
            }
        }

        update();
    }

    QWidget::mousePressEvent(event);
}

void WaveformWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_scrubbing) {
        int64_t frame = pixelXToFrame(event->pos().x());
        frame = std::clamp(frame, static_cast<int64_t>(0),
                           m_waveformData ? m_waveformData->totalFrames : frame);
        m_playheadFrame = frame;
        emit positionChanged(frame);
        update();
    }

    QWidget::mouseMoveEvent(event);
}

void WaveformWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_scrubbing = false;
    }
    QWidget::mouseReleaseEvent(event);
}

void WaveformWidget::wheelEvent(QWheelEvent* event)
{
    if (event->angleDelta().y() > 0)
        zoomIn();
    else if (event->angleDelta().y() < 0)
        zoomOut();

    event->accept();
}

void WaveformWidget::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;
    updateSamplesPerPixel();
    QWidget::resizeEvent(event);
    s_inResize = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Helpers
// ═════════════════════════════════════════════════════════════════════════════

void WaveformWidget::updateSamplesPerPixel()
{
    if (width() > 0 && m_visibleEnd > m_visibleStart) {
        m_samplesPerPixel = static_cast<double>(m_visibleEnd - m_visibleStart) / width();
    }
}

QColor WaveformWidget::gradientColor(float amplitude) const
{
    amplitude = std::clamp(amplitude, 0.0f, 1.0f);

    if (amplitude < 0.5f) {
        float t = amplitude * 2.0f;
        int r = static_cast<int>(m_colorLow.red()   + t * (m_colorMid.red()   - m_colorLow.red()));
        int g = static_cast<int>(m_colorLow.green() + t * (m_colorMid.green() - m_colorLow.green()));
        int b = static_cast<int>(m_colorLow.blue()  + t * (m_colorMid.blue()  - m_colorLow.blue()));
        return QColor(r, g, b);
    } else {
        float t = (amplitude - 0.5f) * 2.0f;
        int r = static_cast<int>(m_colorMid.red()   + t * (m_colorHigh.red()   - m_colorMid.red()));
        int g = static_cast<int>(m_colorMid.green() + t * (m_colorHigh.green() - m_colorMid.green()));
        int b = static_cast<int>(m_colorMid.blue()  + t * (m_colorHigh.blue()  - m_colorMid.blue()));
        return QColor(r, g, b);
    }
}

} // namespace rt

