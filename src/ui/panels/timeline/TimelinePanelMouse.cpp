/*
 * TimelinePanelMouse.cpp - Mouse event handlers for TimelinePanel.
 * Split from TimelinePanel.cpp for maintainability.
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"
#include "widgets/TimelineRuler.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/GraphicClip.h"
#include "timeline/EditOperations.h"
#include "timeline/Transition.h"
#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"

#include <QMouseEvent>
#include <QToolTip>
#include <QMenu>
#include <QRubberBand>

#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

namespace rt {
void TimelinePanel::mousePressEvent(QMouseEvent* event)
{
    if (!m_timeline || event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    QPointF pos = event->position();
    m_dragStart = pos;

    switch (m_activeTool)
    {
    case EditTool::Razor:
    {
        // Split at click position
        size_t ti = hitTestTrack(pos.y());
        if (ti < m_timeline->trackCount())
        {
            double px = pos.x() - headerWidth();
            int64_t tick = m_layoutEngine.pixelXToTime(px);
            Track* track = m_timeline->track(ti);
            for (size_t ci = 0; ci < track->clipCount(); ++ci)
            {
                const Clip* clip = track->clip(ci);
                if (tick > clip->timelineIn() && tick < clip->timelineOut())
                {
                    auto cmd = EditOperations::splitClip(*m_timeline, ti, clip->id(), tick);
                    executeCommand(std::move(cmd));
                    refreshTrackContents();
                    break;
                }
            }
        }
        event->accept();
        return;
    }
    case EditTool::Selection:
    {
        // â”€â”€ Check for transition-edge drag FIRST â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        {
            double px = pos.x() - headerWidth();
            size_t ti = hitTestTrack(pos.y());
            if (ti < m_timeline->trackCount()) {
                Track* track = m_timeline->track(ti);
                constexpr double kHandleThreshold = 8.0;  // pixels
                for (size_t trI = 0; trI < track->transitionCount(); ++trI) {
                    const Transition* trans = track->transition(trI);
                    if (!trans) continue;
                    int64_t tStart, tEnd;
                    trans->getRange(tStart, tEnd);
                    double pxStart = m_layoutEngine.timeToPixelX(tStart);
                    double pxEnd   = m_layoutEngine.timeToPixelX(tEnd);

                    // Determine which edges are draggable
                    bool canDragStart = (trans->rightClipId == 0)    // fade-out: start edge
                                     || (trans->leftClipId != 0 && trans->rightClipId != 0); // cross-dissolve
                    bool canDragEnd   = (trans->leftClipId == 0)     // fade-in: end edge
                                     || (trans->leftClipId != 0 && trans->rightClipId != 0); // cross-dissolve

                    if (canDragStart && std::abs(px - pxStart) < kHandleThreshold) {
                        m_dragMode = DragMode::TransitionTrim;
                        m_transTrimTrackIndex  = ti;
                        m_transTrimIndex       = trI;
                        m_transTrimIsStart     = true;
                        m_transTrimOrigDuration   = trans->duration;
                        m_transTrimOrigEditPoint  = trans->editPointTick;
                        event->accept();
                        return;
                    }
                    if (canDragEnd && std::abs(px - pxEnd) < kHandleThreshold) {
                        m_dragMode = DragMode::TransitionTrim;
                        m_transTrimTrackIndex  = ti;
                        m_transTrimIndex       = trI;
                        m_transTrimIsStart     = false;
                        m_transTrimOrigDuration   = trans->duration;
                        m_transTrimOrigEditPoint  = trans->editPointTick;
                        event->accept();
                        return;
                    }
                }
            }
        }

        // â”€â”€ Check for transition body click (select transition) â”€â”€â”€â”€â”€â”€â”€â”€
        {
            double px = pos.x() - headerWidth();
            size_t ti = hitTestTrack(pos.y());
            if (ti < m_timeline->trackCount()) {
                Track* track = m_timeline->track(ti);
                for (size_t trI = 0; trI < track->transitionCount(); ++trI) {
                    const Transition* trans = track->transition(trI);
                    if (!trans) continue;
                    int64_t tStart, tEnd;
                    trans->getRange(tStart, tEnd);
                    double pxStart = m_layoutEngine.timeToPixelX(tStart);
                    double pxEnd   = m_layoutEngine.timeToPixelX(tEnd);
                    if (px >= pxStart && px <= pxEnd) {
                        m_selection.clear();
                        m_selectedTransitionTrack = ti;
                        m_selectedTransitionIndex = trI;
                        // Update track widgets
                        for (size_t w = 0; w < m_trackWidgets.size(); ++w) {
                            m_trackWidgets[w]->setSelectedClips({});
                            m_trackWidgets[w]->setSelectedTransition(w == ti ? trI : SIZE_MAX);
                        }
                        emit selectionChanged();
                        emit transitionSelected(ti, trI);
                        event->accept();
                        return;
                    }
                }
            }
        }

        auto hitRef = hitTestClip(pos);
        if (hitRef)
        {
            // Clear any gap selection when clicking a clip
            if (m_gapSelection.active) {
                m_gapSelection.active = false;
                for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                    m_trackWidgets[w]->setGapHighlight(-1, -1);
            }

            // Check if we're near an edge for trim
            [[maybe_unused]] ClipEdge edge = hitTestClipEdge(pos, *hitRef);
            bool isShift = event->modifiers() & Qt::ShiftModifier;

            // If clicking on an already-selected clip WITHOUT shift,
            // defer the decision: if the user drags, move all selected clips;
            // if the user releases without moving, select just this one clip
            // (Premiere Pro behaviour).
            bool alreadySelected = m_selection.isSelected(*hitRef);
            if (isShift) {
                m_selection.toggleClip(*hitRef);
                emit selectionChanged();
                // Shift-click only toggles selection - don't initiate drag/trim
                m_dragMode = DragMode::None;
                event->accept();
                return;
            }

            if (alreadySelected) {
                // Defer: keep multi-selection for potential drag,
                // wait for release to decide click vs drag.
                m_dragClipRef = *hitRef;
                m_dragMode = DragMode::PendingClipClick;
                event->accept();
                return;
            }

            // Not already selected - immediately select just this clip
            m_selection.clear();
            m_selection.selectClip(*hitRef, false);
            emit selectionChanged();

            // Emit clipSelected so the Properties Panel updates
            {
                Track* trk = m_timeline->track(hitRef->trackIndex);
                size_t clipIdx = trk->findClipIndexById(hitRef->clipId);
                if (clipIdx < trk->clipCount())
                    emit clipSelected(hitRef->trackIndex, clipIdx);
            }

            // Determine drag mode
            m_dragClipRef = *hitRef;

            Track* track = m_timeline->track(hitRef->trackIndex);
            size_t idx = track->findClipIndexById(hitRef->clipId);
            if (idx < track->clipCount())
            {
                const Clip* clip = track->clip(idx);
                m_dragOriginalIn = clip->timelineIn();
                m_dragOriginalSourceIn = clip->sourceIn();
                m_dragOriginalDuration = clip->duration();
                m_dragOriginalTrack = hitRef->trackIndex;

                // Check edge proximity for trim
                double px = pos.x() - headerWidth();
                double clipLeft = m_layoutEngine.timeToPixelX(clip->timelineIn());
                double clipRight = m_layoutEngine.timeToPixelX(clip->timelineOut());

                constexpr double kEdgeThreshold = 6.0;
                if (std::abs(px - clipLeft) < kEdgeThreshold) {
                    m_dragMode = DragMode::ClipTrimHead;
                    m_lastClickedEdge = { *hitRef, ClipEdge::Head, true };
                }
                else if (std::abs(px - clipRight) < kEdgeThreshold) {
                    m_dragMode = DragMode::ClipTrimTail;
                    m_lastClickedEdge = { *hitRef, ClipEdge::Tail, true };
                }
                else {
                    m_dragMode = DragMode::ClipMove;
                    m_lastClickedEdge.valid = false;

                    // Record original positions of ALL selected clips
                    // so we can move them together as a group.
                    m_dragSelectedClips.clear();
                    m_dragTargetTrack = hitRef->trackIndex;
                    for (const auto& sel : m_selection.clips()) {
                        Track* selTrack = m_timeline->track(sel.trackIndex);
                        if (!selTrack) continue;
                        size_t si = selTrack->findClipIndexById(sel.clipId);
                        if (si < selTrack->clipCount()) {
                            DragClipState dcs;
                            dcs.ref = sel;
                            dcs.originalIn = selTrack->clip(si)->timelineIn();
                            dcs.originalTrack = sel.trackIndex;
                            m_dragSelectedClips.push_back(dcs);
                        }
                    }
                }

                // Initialize snap engine for this drag operation
                m_snapEngine.setPixelsPerSecond(m_layoutEngine.pixelsPerSecond());
                {
                    std::vector<uint64_t> excludeIds;
                    if (m_dragMode == DragMode::ClipMove) {
                        for (const auto& sel : m_selection.clips())
                            excludeIds.push_back(sel.clipId);
                    } else {
                        excludeIds.push_back(m_dragClipRef.clipId);
                    }
                    m_snapEngine.buildTargets(*m_timeline, m_playheadTick, 0.0, excludeIds);
                }
            }
        }
        else
        {
            // Clicked empty space — defer deselect until we know if the
            // user intends a click (deselect) or a drag (marquee select).
            // This is the Premiere Pro pattern: mouse-down starts a
            // potential marquee; deselect only fires on release without
            // meaningful movement.

            // Detect gap between clips on the clicked track
            size_t ti = hitTestTrack(pos.y());
            double px = pos.x() - headerWidth();
            int64_t clickTick = m_layoutEngine.pixelXToTime(px);
            bool gapFound = false;

            if (ti < m_timeline->trackCount() && clickTick >= 0) {
                Track* track = m_timeline->track(ti);

                // Collect and sort clips by position
                std::vector<const Clip*> sorted;
                sorted.reserve(track->clipCount());
                for (size_t ci = 0; ci < track->clipCount(); ++ci)
                    sorted.push_back(track->clip(ci));
                std::sort(sorted.begin(), sorted.end(),
                    [](const Clip* a, const Clip* b) {
                        return a->timelineIn() < b->timelineIn();
                    });

                // Check gap before first clip (from tick 0 to first clip start)
                if (!sorted.empty() && clickTick < sorted.front()->timelineIn()
                    && sorted.front()->timelineIn() > 0) {
                    m_gapSelection = { ti, 0, sorted.front()->timelineIn(), true };
                    gapFound = true;
                }

                // Check gaps between consecutive clips
                if (!gapFound) {
                    for (size_t i = 0; i + 1 < sorted.size(); ++i) {
                        int64_t gapStart = sorted[i]->timelineOut();
                        int64_t gapEnd   = sorted[i + 1]->timelineIn();
                        if (gapEnd > gapStart &&
                            clickTick >= gapStart && clickTick < gapEnd) {
                            m_gapSelection = { ti, gapStart, gapEnd, true };
                            gapFound = true;
                            break;
                        }
                    }
                }
            }

            if (gapFound) {
                // Show gap highlight on the appropriate track widget
                if (!(event->modifiers() & Qt::ShiftModifier)) {
                    m_selection.clear();
                    m_selectedTransitionTrack = SIZE_MAX;
                    m_selectedTransitionIndex = SIZE_MAX;
                    for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                        m_trackWidgets[w]->setSelectedTransition(SIZE_MAX);
                }
                for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                    m_trackWidgets[w]->setGapHighlight(-1, -1);
                if (m_gapSelection.trackIndex < m_trackWidgets.size())
                    m_trackWidgets[m_gapSelection.trackIndex]->setGapHighlight(
                        m_gapSelection.startTick, m_gapSelection.endTick);
                // Stay in PendingMarquee so a click-and-drag from inside
                // the gap can still produce a marquee selection box.
                // If the user releases without moving, the gap selection
                // is preserved (mouseRelease's PendingMarquee branch only
                // clears m_selection, which is already empty here).
                // If they drag, the PendingMarquee→MarqueeSelect transition
                // will clear the gap highlight via m_gapSelection.active.
                m_dragMode = DragMode::PendingMarquee;
                emit selectionChanged();
            } else {
                // Clear any existing gap selection
                m_gapSelection.active = false;
                for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                    m_trackWidgets[w]->setGapHighlight(-1, -1);
                m_lastClickedEdge.valid = false;
                // Don't clear selection yet — wait to see if user drags
                m_dragMode = DragMode::PendingMarquee;
            }
        }
        event->accept();
        return;
    }
    case EditTool::Slip:
    {
        auto hitRef = hitTestClip(pos);
        if (hitRef)
        {
            m_dragClipRef = *hitRef;
            m_dragMode = DragMode::SlipTool;
            Track* track = m_timeline->track(hitRef->trackIndex);
            size_t idx = track->findClipIndexById(hitRef->clipId);
            if (idx < track->clipCount())
                m_dragOriginalSourceIn = track->clip(idx)->sourceIn();
        }
        event->accept();
        return;
    }
    case EditTool::Slide:
    {
        auto hitRef = hitTestClip(pos);
        if (hitRef)
        {
            m_dragClipRef = *hitRef;
            m_dragMode = DragMode::SlideTool;
            Track* track = m_timeline->track(hitRef->trackIndex);
            size_t idx = track->findClipIndexById(hitRef->clipId);
            if (idx < track->clipCount())
                m_dragOriginalIn = track->clip(idx)->timelineIn();
        }
        event->accept();
        return;
    }
    case EditTool::Rolling:
    {
        // Find the nearest edit point between two adjacent clips
        size_t ti = hitTestTrack(pos.y());
        if (ti < m_timeline->trackCount()) {
            double px = pos.x() - headerWidth();
            int64_t clickTick = m_layoutEngine.pixelXToTime(px);
            Track* track = m_timeline->track(ti);

            // Look for the closest edit point (where one clip ends and another begins)
            int64_t bestDist = INT64_MAX;
            uint64_t bestLeft = 0, bestRight = 0;
            int64_t bestEditPt = 0;

            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                const Clip* clip = track->clip(ci);
                int64_t outTick = clip->timelineOut();

                // Find any clip that starts at or very near this clip's out
                for (size_t ci2 = 0; ci2 < track->clipCount(); ++ci2) {
                    if (ci2 == ci) continue;
                    const Clip* next = track->clip(ci2);
                    int64_t gap = std::abs(next->timelineIn() - outTick);
                    if (gap <= 1600) { // ~1 frame tolerance
                        int64_t editPt = outTick;
                        int64_t dist = std::abs(clickTick - editPt);
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestLeft = clip->id();
                            bestRight = next->id();
                            bestEditPt = editPt;
                        }
                    }
                }
            }

            // Accept if the click was reasonably close to an edit point
            double editPx = m_layoutEngine.timeToPixelX(bestEditPt);
            double clickPx = pos.x() - headerWidth();
            if (bestLeft != 0 && std::abs(editPx - clickPx) < 20.0) {
                m_dragMode = DragMode::RollingEdit;
                m_rollLeftClipId = bestLeft;
                m_rollRightClipId = bestRight;
                m_rollTrackIndex = ti;
                m_rollOriginalEditPoint = bestEditPt;
                // Capture original clip states for direct manipulation
                size_t li2 = track->findClipIndexById(bestLeft);
                size_t ri2 = track->findClipIndexById(bestRight);
                if (li2 < track->clipCount() && ri2 < track->clipCount()) {
                    const Clip* lc = track->clip(li2);
                    const Clip* rc = track->clip(ri2);
                    m_rollLeftOrigIn     = lc->timelineIn();
                    m_rollLeftOrigDur    = lc->duration();
                    m_rollLeftOrigSrcIn  = lc->sourceIn();
                    m_rollRightOrigIn    = rc->timelineIn();
                    m_rollRightOrigDur   = rc->duration();
                    m_rollRightOrigSrcIn = rc->sourceIn();
                }

                // Initialize snap engine for rolling edit
                m_snapEngine.setPixelsPerSecond(m_layoutEngine.pixelsPerSecond());
                m_snapEngine.buildTargets(*m_timeline, m_playheadTick, 0.0,
                                          {m_rollLeftClipId, m_rollRightClipId});
            }
        }
        event->accept();
        return;
    }
    case EditTool::Ripple:
    {
        // Ripple tool: trim head/tail with ripple (shift subsequent clips)
        auto hitRef = hitTestClip(pos);
        if (hitRef) {
            m_dragClipRef = *hitRef;
            Track* track = m_timeline->track(hitRef->trackIndex);
            size_t idx = track->findClipIndexById(hitRef->clipId);
            if (idx < track->clipCount()) {
                const Clip* clip = track->clip(idx);
                m_dragOriginalIn = clip->timelineIn();
                m_dragOriginalDuration = clip->duration();

                double px = pos.x() - headerWidth();
                double clipRight = m_layoutEngine.timeToPixelX(clip->timelineOut());

                constexpr double kEdgeThreshold = 6.0;
                if (std::abs(px - clipRight) < kEdgeThreshold)
                    m_dragMode = DragMode::ClipTrimTail;
                else
                    m_dragMode = DragMode::ClipTrimHead;
            }
        }
        event->accept();
        return;
    }
    case EditTool::Zoom:
    {
        // Zoom in at click position; Alt+click zooms out (like Premiere Pro).
        // After zooming, pan so the clicked point becomes the viewport center.
        double px = pos.x() - headerWidth();
        bool zoomOut = (event->modifiers() & Qt::AltModifier);
        double factor = zoomOut ? (1.0 / 2.0) : 2.0;
        m_layoutEngine.zoomAt(px, factor);
        double centerPx = std::max(m_ruler->width(), 100) / 2.0;
        double newScroll = m_layoutEngine.scrollX() + (px - centerPx);
        m_layoutEngine.setScrollX(std::max(newScroll, 0.0));
        onScrollChanged();
        event->accept();
        return;
    }
    case EditTool::Text:
    {
        // Click on empty area = create a new GraphicClip with a default TextLayer
        size_t ti = hitTestTrack(pos.y());
        if (ti < m_timeline->trackCount())
        {
            double px = pos.x() - headerWidth();
            int64_t tick = m_layoutEngine.pixelXToTime(px);
            if (tick < 0) tick = 0;

            // Only create if no clip exists at that position
            Track* track = m_timeline->track(ti);
            bool occupied = false;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                const Clip* c = track->clip(ci);
                if (tick >= c->timelineIn() && tick < c->timelineOut()) {
                    occupied = true;
                    break;
                }
            }

            if (!occupied) {
                // Create a 5-second GraphicClip with a default empty TextLayer
                int64_t duration = kTicksPerSecond * 5;
                auto gc = std::make_unique<GraphicClip>();
                gc->setTimelineIn(tick);
                gc->setDuration(duration);
                gc->setSourceIn(0);
                gc->setLabel("Text");
                gc->addTextLayer("Text Layer 1");

                if (m_commandStack) {
                    m_commandStack->execute(
                        std::make_unique<AddClipCommand>(track, std::move(gc)));
                } else {
                    track->addClip(std::move(gc));
                }

                refreshTrackContents();
                emit clipCreated();
            }
        }
        event->accept();
        return;
    }
    default:
        break;
    }

    QWidget::mousePressEvent(event);
}


} // namespace rt