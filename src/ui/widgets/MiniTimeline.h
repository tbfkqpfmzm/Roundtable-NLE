/*
 * MiniTimeline — Compact scrub bar widget for source monitor.
 *
 * Step 15: Source & Program Monitors
 *
 * A thin horizontal bar that shows:
 *   - Clip duration as a filled region
 *   - Playhead marker (draggable)
 *   - In-point / out-point brackets
 *   - Frame number or timecode on hover
 *
 * Click or drag anywhere to scrub. Used below the Source Monitor viewport.
 * Can also be used in the Program Monitor for a simplified timeline scrubber.
 *
 * Pure geometry logic is testable without a display:
 *   - positionToTick()  / tickToPosition()
 *   - clampTick()
 *   - in/out point setting
 */

#pragma once

#include <QWidget>

#include <cstdint>
#include <functional>
#include <vector>

namespace rt {

struct MarkerCue
{
    int64_t  tick{0};
    uint32_t color{0xFF4444FF}; // RGBA
};

/// Compact scrub bar for source/program monitors.
class MiniTimeline : public QWidget
{
    Q_OBJECT

public:
    explicit MiniTimeline(QWidget* parent = nullptr);
    ~MiniTimeline() override;

    // ── Range ───────────────────────────────────────────────────────────

    /// Set the total duration (in ticks) that this bar represents.
    void setDuration(int64_t ticks);
    [[nodiscard]] int64_t duration() const noexcept { return m_duration; }

    // ── Playhead ────────────────────────────────────────────────────────

    /// Set the current playhead position (in ticks).
    void setPlayhead(int64_t tick);
    [[nodiscard]] int64_t playhead() const noexcept { return m_playhead; }

    // ── In / Out points ─────────────────────────────────────────────────

    void setInPoint(int64_t tick);
    void setOutPoint(int64_t tick);
    void clearInOutPoints();

    [[nodiscard]] int64_t inPoint()  const noexcept { return m_inPoint; }
    [[nodiscard]] int64_t outPoint() const noexcept { return m_outPoint; }
    [[nodiscard]] bool hasInPoint()  const noexcept { return m_inPoint >= 0; }
    [[nodiscard]] bool hasOutPoint() const noexcept { return m_outPoint >= 0; }

    /// Duration of the selected region (in-to-out), or full duration if not set.
    [[nodiscard]] int64_t selectedDuration() const noexcept;

    // ── Frame rate (for timecode display) ───────────────────────────────

    void setFrameRate(double fps) noexcept;
    [[nodiscard]] double frameRate() const noexcept { return m_fps; }

    // ── Marker cue points ───────────────────────────────────────────────

    /// Set marker positions to display as colored ticks.
    void setMarkers(const std::vector<MarkerCue>& markers);

    // ── Coordinate mapping (public for testing) ─────────────────────────

    /// Convert an X pixel position (within the bar) to a tick.
    [[nodiscard]] int64_t positionToTick(double xPixel) const noexcept;

    /// Convert a tick to an X pixel position.
    [[nodiscard]] double tickToPosition(int64_t tick) const noexcept;

    /// Clamp a tick to [0, duration].
    [[nodiscard]] int64_t clampTick(int64_t tick) const noexcept;

    // ── Appearance ──────────────────────────────────────────────────────

    /// Bar height.
    static constexpr int kBarHeight = 56;

    /// Margin from left/right edges.
    static constexpr int kMarginH = 8;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// Emitted when the user scrubs to a new position.
    void scrubbed(int64_t tick);

    /// Emitted when in-point changes.
    void inPointChanged(int64_t tick);

    /// Emitted when out-point changes.
    void outPointChanged(int64_t tick);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    /// The usable width for the scrub bar (excluding margins).
    [[nodiscard]] double barWidth() const noexcept;

    int64_t m_duration{0};
    int64_t m_playhead{0};
    int64_t m_inPoint{-1};    ///< -1 = not set
    int64_t m_outPoint{-1};   ///< -1 = not set
    double  m_fps{24.0};
    bool    m_dragging{false};
    std::vector<MarkerCue> m_markers;
};

} // namespace rt
