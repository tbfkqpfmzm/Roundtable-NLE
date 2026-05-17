/*
 * TimelineRuler — Timecode ruler widget at the top of the timeline.
 *
 * Step 12: Draws major/minor tick marks with adaptive labels,
 * playhead triangle, and in/out range markers.
 */

#pragma once

#include <QWidget>
#include <QPen>
#include <QFont>
#include <vector>

namespace rt {

class TimelineLayoutEngine;
struct Marker;

/// Render bar segment status (Premiere Pro style colored bar above timeline).
enum class RenderBarStatus : uint8_t
{
    None,       ///< No bar (no clips in this region)
    NeedsRender,///< Red — complex effects not yet pre-rendered
    Mixed,      ///< Yellow — partially cached
    Cached,     ///< Green — all frames in GPU texture cache
    RealTime    ///< Green — simple enough for real-time (no effects)
};

/// A single render bar segment.
struct RenderBarSegment
{
    int64_t         startTick{0};
    int64_t         endTick{0};
    RenderBarStatus status{RenderBarStatus::None};
};

/// Horizontal timecode ruler at the top of the timeline panel.
class TimelineRuler : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineRuler(QWidget* parent = nullptr);
    ~TimelineRuler() override = default;

    /// Set the layout engine (non-owning pointer).
    void setLayoutEngine(const TimelineLayoutEngine* engine);

    /// Set the current playhead position.
    void setPlayheadTick(int64_t tick);

    /// Set in/out range for visual brackets.
    void setInOutRange(int64_t inTick, int64_t outTick);
    void clearInOutRange();

    /// Set the marker list (non-owning pointer) for rendering.
    void setMarkers(const std::vector<Marker>* markers);

    /// Update the render bar segments (Premiere Pro style colored bar).
    void setRenderBar(std::vector<RenderBarSegment> segments);

    /// Clear the render bar.
    void clearRenderBar();

    /// Ruler height constant.
    static constexpr int kRulerHeight = 32;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// Emitted when user clicks on ruler to move playhead.
    void playheadScrubbed(int64_t tick);

    /// Emitted when user double-clicks on ruler to request marker creation.
    void markerRequested(int64_t tick);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    const TimelineLayoutEngine* m_engine{nullptr};
    int64_t m_playheadTick{0};
    double  m_lastPaintedPlayheadPx{-1000};
    int64_t m_inPoint{-1};
    int64_t m_outPoint{-1};
    bool    m_scrubbing{false};
    const std::vector<Marker>* m_markers{nullptr};
    std::vector<RenderBarSegment> m_renderBar;

    QFont m_font;
    QPen  m_majorPen;
    QPen  m_minorPen;

    void scrubToPosition(int x);
};

} // namespace rt
