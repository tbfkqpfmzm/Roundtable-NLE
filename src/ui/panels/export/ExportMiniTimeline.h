/*
 * ExportMiniTimeline — Scrub bar for the Export panel preview.
 *
 * Shows the full sequence duration with in/out point markers.
 * Regions outside the export range are dimmed (like Premiere Pro).
 * Click or drag to scrub the playhead and update the preview.
 */

#pragma once

#include <QWidget>
#include <cstdint>
#include <functional>

namespace rt {

class ExportMiniTimeline : public QWidget
{
    Q_OBJECT

public:
    explicit ExportMiniTimeline(QWidget* parent = nullptr);

    /// Set the total sequence duration in ticks.
    void setDuration(int64_t durationTicks);

    /// Set the in/out range (ticks). Pass -1 to clear.
    void setInOutRange(int64_t inTick, int64_t outTick);

    /// Set the current playhead position (ticks).
    void setPlayhead(int64_t tick);

    /// Get current playhead tick.
    [[nodiscard]] int64_t playhead() const noexcept { return m_playhead; }

signals:
    /// Emitted when the user scrubs (clicks or drags) to a new position.
    void scrubbed(int64_t tick);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override { return {400, 36}; }
    QSize minimumSizeHint() const override { return {200, 28}; }

private:
    /// Convert an x pixel position to a tick.
    int64_t xToTick(int x) const;
    /// Convert a tick to an x pixel position.
    int tickToX(int64_t tick) const;

    int64_t m_duration{0};     // Total sequence duration (ticks)
    int64_t m_inPoint{-1};     // In point (ticks), -1 = not set
    int64_t m_outPoint{-1};    // Out point (ticks), -1 = not set
    int64_t m_playhead{0};     // Current position (ticks)
    bool    m_dragging{false};
};

} // namespace rt
