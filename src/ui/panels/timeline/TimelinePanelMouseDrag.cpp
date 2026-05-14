/*
 * TimelinePanelMouseDrag.cpp — Mouse drag handlers coordinator.
 *
 * Thin coordinator after extracting mouseMoveEvent →
 * TimelinePanelMouseDragMove.cpp and mouseReleaseEvent →
 * TimelinePanelMouseDragRelease.cpp.
 *
 * Contains: mouseDoubleClickEvent().
 */
#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include <QMouseEvent>
#include <QApplication>

#include <algorithm>
#include <set>

namespace rt {

void TimelinePanel::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!m_timeline || event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    auto ref = hitTestClip(event->position());
    if (ref) {
        auto* track = m_timeline->track(ref->trackIndex);
        if (track) {
            for (size_t i = 0; i < track->clipCount(); ++i) {
                if (track->clip(i)->id() == ref->clipId) {
                    emit clipDoubleClicked(ref->trackIndex, i);
                    event->accept();
                    return;
                }
            }
        }
    }

    QWidget::mouseDoubleClickEvent(event);
}

// mouseMoveEvent    → TimelinePanelMouseDragMove.cpp
// mouseReleaseEvent → TimelinePanelMouseDragRelease.cpp

// ═════════════════════════════════════════════════════════════════════════════
// Marquee selection — shared between move and release
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::refreshMarqueeSelection()
{
    if (!m_timeline || !m_rubberBand || !m_rubberBand->isVisible())
        return;

    QRect rb = m_rubberBand->geometry();
    if (rb.isEmpty())
        return;

    // ── Convert pixel X → timeline ticks ──────────────────────────────
    const int hdr = headerWidth();
    double startPx = static_cast<double>(rb.left()) - hdr;
    double endPx   = static_cast<double>(rb.right()) - hdr;

    int64_t startTick = m_layoutEngine.pixelXToTime(std::max(0.0, startPx));
    int64_t endTick   = m_layoutEngine.pixelXToTime(std::max(0.0, endPx));
    if (startTick > endTick) std::swap(startTick, endTick);

    // ── Convert pixel Y → track range ────────────────────────────────
    size_t topTrack    = SIZE_MAX;
    size_t bottomTrack = SIZE_MAX;

    for (size_t i = 0; i < m_trackWidgets.size(); ++i)
    {
        TimelineTrackWidget* tw = m_trackWidgets[i].data();
        if (!tw) continue;
        QPoint topLeft = tw->mapTo(this, QPoint(0, 0));
        double trackTop    = topLeft.y();
        double trackBottom = trackTop + tw->height();

        if (rb.top() < trackBottom && rb.bottom() > trackTop)
        {
            if (topTrack == SIZE_MAX)
                topTrack = i;
            bottomTrack = i;
        }
    }

    if (topTrack == SIZE_MAX)
        return; // marquee is outside all tracks

    // ── Build the TimelineRect ────────────────────────────────────────
    TimelineRect trect;
    trect.startTick  = startTick;
    trect.endTick    = endTick;
    trect.topTrack   = topTrack;
    trect.bottomTrack = bottomTrack;

    // ── Compute selection ─────────────────────────────────────────────
    const bool shiftHeld =
        (QApplication::keyboardModifiers() & Qt::ShiftModifier);

    if (shiftHeld)
    {
        // Additive marquee: union of base selection + rectangle
        SelectionSet rectSel;
        rectSel.selectRect(*m_timeline, trect);

        std::set<std::pair<size_t, uint64_t>> combined;
        for (const auto& ref : m_marqueeBaseSelection)
            combined.insert({ref.trackIndex, ref.clipId});
        for (const auto& ref : rectSel.clips())
            combined.insert({ref.trackIndex, ref.clipId});

        m_selection.clear();
        for (const auto& [ti, cid] : combined)
            m_selection.selectClip(ClipRef{ti, cid}, true);
    }
    else
    {
        m_selection.selectRect(*m_timeline, trect);
    }

    emit selectionChanged();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
