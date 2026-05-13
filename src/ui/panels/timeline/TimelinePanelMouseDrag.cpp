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

#include <QMouseEvent>

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
    // Recompute marquee selection from current m_dragStart/m_marqueeLastMovePos
    // (additively merging in m_marqueeBaseSelection if Shift+marquee).
    // The actual implementation lives in TimelinePanel.cpp or
    // TimelinePanelMouse.cpp; this is the unity-bridge definition.
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
