/*
 * TimelinePanelMouseDrag.cpp - Mouse drag, move, and release handlers for TimelinePanel.
 * Split from TimelinePanelMouse.cpp for maintainability.
 */
#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "timeline/Transition.h"
#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"

#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QToolTip>
#include <QMenu>
#include <QRubberBand>

#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>
#include <limits>

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

void TimelinePanel::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_timeline)
    {
        QWidget::mouseMoveEvent(event);
        return;
    }

    // â”€â”€ Hover cursor feedback when not dragging â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (m_dragMode == DragMode::None)
    {
        if (m_activeTool == EditTool::Razor)
        {
            // Track razor position for the red line indicator
            double px = event->position().x() - headerWidth();
            int64_t tick = m_layoutEngine.pixelXToTime(px);
            size_t ti = hitTestTrack(event->position().y());

            // Check if cursor is over a clip in this track
            bool overClip = false;
            if (ti < m_timeline->trackCount()) {
                Track* trk = m_timeline->track(ti);
                for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                    const Clip* c = trk->clip(ci);
                    if (tick > c->timelineIn() && tick < c->timelineOut()) {
                        overClip = true;
                        break;
                    }
                }
            }

            // Set razor tick on the hovered track widget, clear others
            for (auto* tw : m_trackWidgets)
                tw->setRazorTick(overClip && tw->trackIndex() == ti ? tick : -1);

            QWidget::mouseMoveEvent(event);
            return;
        }
        if (m_activeTool == EditTool::Selection)
        {
            // Check for transition-edge hover first
            bool hoveringTransEdge = false;
            {
                double hpx = event->position().x() - headerWidth();
                size_t hti = hitTestTrack(event->position().y());
                if (hti < m_timeline->trackCount()) {
                    Track* hTrack = m_timeline->track(hti);
                    constexpr double kHandleThreshold = 8.0;
                    for (size_t trI = 0; trI < hTrack->transitionCount(); ++trI) {
                        const Transition* trans = hTrack->transition(trI);
                        if (!trans) continue;
                        int64_t tStart, tEnd;
                        trans->getRange(tStart, tEnd);
                        double pxStart = m_layoutEngine.timeToPixelX(tStart);
                        double pxEnd   = m_layoutEngine.timeToPixelX(tEnd);
                        bool canStart = (trans->rightClipId == 0)
                                     || (trans->leftClipId != 0 && trans->rightClipId != 0);
                        bool canEnd   = (trans->leftClipId == 0)
                                     || (trans->leftClipId != 0 && trans->rightClipId != 0);
                        if ((canStart && std::abs(hpx - pxStart) < kHandleThreshold) ||
                            (canEnd   && std::abs(hpx - pxEnd)   < kHandleThreshold)) {
                            setCursor(Qt::SizeHorCursor);
                            hoveringTransEdge = true;
                            break;
                        }
                    }
                }
            }
            if (!hoveringTransEdge) {
            auto hitRef = hitTestClip(event->position());
            if (hitRef)
            {
                // Check edge proximity for trim cursor
                Track* track = m_timeline->track(hitRef->trackIndex);
                size_t idx = track ? track->findClipIndexById(hitRef->clipId) : SIZE_MAX;
                if (idx < track->clipCount())
                {
                    const Clip* clip = track->clip(idx);
                    double px = event->position().x() - headerWidth();
                    double clipLeft  = m_layoutEngine.timeToPixelX(clip->timelineIn());
                    double clipRight = m_layoutEngine.timeToPixelX(clip->timelineOut());

                    constexpr double kEdgeThreshold = 6.0;
                    bool nearHead = std::abs(px - clipLeft)  < kEdgeThreshold;
                    bool nearTail = std::abs(px - clipRight) < kEdgeThreshold;
                    if (nearHead || nearTail)
                    {
                        setCursor(Qt::SizeHorCursor);
                        // Highlight the hovered edge on the track widget
                        int64_t edgeTick = nearHead ? clip->timelineIn() : clip->timelineOut();
                        for (auto* tw : m_trackWidgets)
                            tw->setHoverEdgeTick(hitRef->trackIndex == tw->trackIndex() ? edgeTick : -1);
                    }
                    else
                    {
                        setCursor(Qt::ArrowCursor);
                        for (auto* tw : m_trackWidgets)
                            tw->setHoverEdgeTick(-1);
                    }
                }
                else
                {
                    setCursor(Qt::ArrowCursor);
                    for (auto* tw : m_trackWidgets)
                        tw->setHoverEdgeTick(-1);
                }
            }
            else
            {
                setCursor(Qt::ArrowCursor);
                for (auto* tw : m_trackWidgets)
                    tw->setHoverEdgeTick(-1);
            }
            } // end if (!hoveringTransEdge)
        }
        QWidget::mouseMoveEvent(event);
        return;
    }

    QPointF pos = event->position();
    double deltaX = pos.x() - m_dragStart.x();
    double pps = m_layoutEngine.pixelsPerSecond();
    int64_t tickDelta = static_cast<int64_t>(deltaX / pps * 48000.0);

    switch (m_dragMode)
    {
    case DragMode::ClipMove:
    {
        int64_t newIn = m_dragOriginalIn + tickDelta;
        if (newIn < 0) newIn = 0;

        // Snap the primary dragged clip (both head and tail edges,
        // like Premiere Pro, so the right edge also snaps to existing
        // clip boundaries — prevents overlap "spill over").
        int64_t newOut = newIn + m_dragOriginalDuration;
        auto result = m_snapEngine.snapPair(newIn, newOut);
        if (result.didSnap) {
            newIn = result.snappedTick;
            // Show indicator at whichever edge actually snapped
            auto headCheck = m_snapEngine.snap(newIn);
            if (headCheck.didSnap && headCheck.delta == 0)
                setSnapIndicator(newIn);  // head edge snapped
            else
                setSnapIndicator(newIn + m_dragOriginalDuration);  // tail edge
        } else {
            setSnapIndicator(-1);
        }

        // Compute the actual delta applied to the primary clip
        int64_t actualDelta = newIn - m_dragOriginalIn;

        // Detect cross-track target
        size_t targetTrack = hitTestTrack(pos.y());
        if (targetTrack < m_timeline->trackCount()) {
            m_dragTargetTrack = targetTrack;
            m_ghostTrackVisible = false;
            if (m_ghostOverlay) m_ghostOverlay->hide();
        } else if (!m_trackWidgets.empty()) {
            // â”€â”€ Ghost track detection (Premiere Pro-style) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            // Cursor is OUTSIDE any existing track.  Determine if it's
            // above the topmost track or below the bottommost track.
            auto* firstTw = m_trackWidgets.front();
            auto* lastTw  = m_trackWidgets.back();
            QPoint firstTop = firstTw->mapTo(this, QPoint(0, 0));
            QPoint lastBot  = lastTw->mapTo(this, QPoint(0, lastTw->height()));

            // Determine dragged clip's track type
            TrackType dragType = TrackType::Video;
            if (!m_dragSelectedClips.empty()) {
                Track* srcTr = m_timeline->track(m_dragSelectedClips[0].ref.trackIndex);
                if (srcTr) dragType = srcTr->type();
            }

            // Find first/last track indices of each type
            size_t firstVideoIdx = SIZE_MAX, lastVideoIdx = SIZE_MAX;
            size_t firstAudioIdx = SIZE_MAX, lastAudioIdx = SIZE_MAX;
            for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                if (m_timeline->track(i)->type() == TrackType::Video) {
                    if (firstVideoIdx == SIZE_MAX) firstVideoIdx = i;
                    lastVideoIdx = i;
                } else {
                    if (firstAudioIdx == SIZE_MAX) firstAudioIdx = i;
                    lastAudioIdx = i;
                }
            }

            // Scroll area X position (tracks start after the header column)
            QPoint scrollOrig = m_verticalScroll->mapTo(this, QPoint(0, 0));
            int ghostX = scrollOrig.x();
            int ghostW = m_verticalScroll->width();

            if (dragType == TrackType::Video && pos.y() < firstTop.y() &&
                firstVideoIdx < m_trackWidgets.size()) {
                // Above topmost track â†’ ghost new video track above
                m_ghostTrackVisible = true;
                m_ghostTrackIsAbove = true;
                m_ghostTrackHeight = firstTw->height();  // set height BEFORE using it
                m_ghostTrackY = firstTop.y() - m_ghostTrackHeight;
                if (m_ghostOverlay) {
                    m_ghostOverlay->isAbove = true;
                    m_ghostOverlay->setGeometry(ghostX, m_ghostTrackY, ghostW, m_ghostTrackHeight);
                    m_ghostOverlay->raise();
                    m_ghostOverlay->show();
                    m_ghostOverlay->update();
                }
            } else if (dragType == TrackType::Audio && pos.y() > lastBot.y() &&
                       lastAudioIdx < m_trackWidgets.size()) {
                // Below bottommost track â†’ ghost new audio track below
                m_ghostTrackVisible = true;
                m_ghostTrackIsAbove = false;
                m_ghostTrackHeight = lastTw->height();
                m_ghostTrackY = lastBot.y();
                if (m_ghostOverlay) {
                    m_ghostOverlay->isAbove = false;
                    m_ghostOverlay->setGeometry(ghostX, m_ghostTrackY, ghostW, m_ghostTrackHeight);
                    m_ghostOverlay->raise();
                    m_ghostOverlay->show();
                    m_ghostOverlay->update();
                }
            } else {
                m_ghostTrackVisible = false;
                if (m_ghostOverlay) m_ghostOverlay->hide();
            }
            // Keep existing m_dragTargetTrack during ghost â€” don't update
        }

        // Compute per-clip cross-track delta from the primary clip.
        // Use m_dragOriginalTrack (the track where the user clicked) rather
        // than m_dragSelectedClips[0].originalTrack, because the first
        // selected clip may be on a different track than the one clicked,
        // which would create an immediate unwanted track offset.
        int trackDeltaLive = 0;
        if (m_dragTargetTrack != SIZE_MAX && !m_dragSelectedClips.empty()) {
            trackDeltaLive = static_cast<int>(m_dragTargetTrack)
                           - static_cast<int>(m_dragOriginalTrack);
        }

        // ── Y deadzone for cross-track shifts ───────────────────────────
        // Small accidental Y drift while dragging horizontally must NOT
        // cause clips to jump tracks. Require the cursor to have moved
        // at least kTrackShiftDeadzone pixels vertically before honoring
        // any track delta. Once that threshold is crossed, normal track
        // tracking resumes.
        constexpr double kTrackShiftDeadzone = 18.0;
        if (std::abs(pos.y() - m_dragStart.y()) < kTrackShiftDeadzone)
            trackDeltaLive = 0;

        // ── Group-safety clamp ──────────────────────────────────────────
        // When a multi-clip selection spans several tracks, clamp the
        // shared trackDelta so NO clip falls off either end of the
        // timeline. Without this, the per-clip clamp below would
        // collapse multiple selected clips onto track 0 or the last
        // track, overwriting (and destroying) each other.
        if (trackDeltaLive != 0) {
            size_t minOrig = SIZE_MAX, maxOrig = 0;
            for (const auto& dcs : m_dragSelectedClips) {
                minOrig = std::min(minOrig, dcs.originalTrack);
                maxOrig = std::max(maxOrig, dcs.originalTrack);
            }
            if (minOrig != SIZE_MAX) {
                const int lastTrack = static_cast<int>(m_timeline->trackCount()) - 1;
                const int minAllowed = -static_cast<int>(minOrig);
                const int maxAllowed = lastTrack - static_cast<int>(maxOrig);
                trackDeltaLive = std::clamp(trackDeltaLive, minAllowed, maxAllowed);
            }
        }

        // ── Move drag = overwrite ───────────────────────────────────────
        // Click-and-drag moves clips Premiere-style: passing through or
        // landing on top of a non-selected clip overwrites/trims it.
        // (Edge trim is the operation that stops at neighbors — see the
        // ClipTrimHead/ClipTrimTail cases above.)  No clamp here.

        // Move clips DIRECTLY in the data model (no commands) for live
        // visual feedback.  A single undoable command is created on
        // mouse-release so that Undo reverts the entire drag at once.
        for (auto& dcs : m_dragSelectedClips) {
            int64_t clipNewIn = std::max<int64_t>(0, dcs.originalIn + actualDelta);

            // Compute live destination track. The group-safety clamp
            // above guarantees this stays in [0, trackCount), so no
            // per-clip clamp is needed here — that earlier clamp was
            // what caused multi-track selections to collapse and
            // overwrite their own neighboring clips on small Y drift.
            int dst = static_cast<int>(dcs.originalTrack) + trackDeltaLive;
            size_t dstTrack = static_cast<size_t>(dst);

            // Ensure same track type; fall back to source track otherwise.
            // (This can still cause a same-type collision in mixed
            // selections, but cross-type fallback is safer than landing
            // on the wrong kind of track.)
            Track* srcTr = m_timeline->track(dcs.ref.trackIndex);
            if (!srcTr) continue;
            Track* dstTr = m_timeline->track(dstTrack);
            if (!dstTr || dstTr->type() != srcTr->type())
                dstTrack = dcs.ref.trackIndex;

            size_t curTrack = dcs.ref.trackIndex;

            if (dstTrack != curTrack) {
                // Transfer clip from current track to destination track
                Track* curTr = m_timeline->track(curTrack);
                size_t idx = curTr->findClipIndexById(dcs.ref.clipId);
                if (idx >= curTr->clipCount()) continue;
                auto clipPtr = curTr->removeClip(idx);
                clipPtr->setTimelineIn(clipNewIn);
                dstTr->addClip(std::move(clipPtr));
                dcs.ref.trackIndex = dstTrack;
            } else {
                // Same track â€” just move horizontally
                Track* tr = m_timeline->track(curTrack);
                size_t idx = tr->findClipIndexById(dcs.ref.clipId);
                if (idx >= tr->clipCount()) continue;
                tr->moveClip(idx, clipNewIn);
            }
        }
        // Do NOT call rebuildTracks() here â€” destroying widgets while
        // the mouse is grabbed causes Qt to lose the drag.  Just repaint
        // existing track widgets; they read clip data from the model.
        onScrollChanged();

        // Sync selection + drag state so dragging clips stay highlighted
        // and semi-transparent (Premiere Pro style visual feedback).
        {
            std::vector<std::vector<size_t>> perTrack(m_trackWidgets.size());
            for (const auto& dcs : m_dragSelectedClips) {
                size_t ti = dcs.ref.trackIndex;
                if (ti < m_trackWidgets.size()) {
                    Track* trk = m_timeline->track(ti);
                    if (trk) {
                        size_t idx = trk->findClipIndexById(dcs.ref.clipId);
                        if (idx < trk->clipCount())
                            perTrack[ti].push_back(idx);
                    }
                }
            }
            for (size_t ti = 0; ti < m_trackWidgets.size(); ++ti) {
                m_trackWidgets[ti]->setSelectedClips(perTrack[ti]);
                m_trackWidgets[ti]->setDraggedClips(perTrack[ti]);
            }
        }
        break;
    }
    case DragMode::ClipTrimHead:
    {
        int64_t newHead = m_dragOriginalIn + tickDelta;
        auto result = m_snapEngine.snap(newHead);
        if (result.didSnap) {
            newHead = result.snappedTick;
            setSnapIndicator(result.snappedTick);
        } else {
            setSnapIndicator(-1);
        }

        // Edge-trim must stop at any neighboring clip on the same track
        // (Premiere behavior). Find the closest non-self neighbor whose
        // tail is to the left of the dragged clip and clamp newHead so
        // it cannot extend past that neighbor's tail.
        if (Track* tr = m_timeline->track(m_dragClipRef.trackIndex)) {
            const int64_t origTail = m_dragOriginalIn + m_dragOriginalDuration;
            int64_t leftLimit = std::numeric_limits<int64_t>::min();
            for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
                const Clip* other = tr->clip(ci);
                if (!other || other->id() == m_dragClipRef.clipId) continue;
                if (other->timelineOut() <= m_dragOriginalIn &&
                    other->timelineOut() > leftLimit)
                    leftLimit = other->timelineOut();
            }
            if (newHead < leftLimit) newHead = leftLimit;
            // Don't let head pass tail (preserve >= 1 tick of duration).
            if (newHead >= origTail) newHead = origTail - 1;
        }

        auto cmd = EditOperations::trimClip(*m_timeline, m_dragClipRef.trackIndex,
                                            m_dragClipRef.clipId,
                                            ClipEdge::Head, newHead);
        executeCommand(std::move(cmd));
        // Move the hover-edge highlight with the dragged edge so the original
        // edge position doesn't stay drawn underneath as a stale blue line.
        for (auto* tw : m_trackWidgets)
            tw->setHoverEdgeTick(tw->trackIndex() == m_dragClipRef.trackIndex ? newHead : -1);
        onScrollChanged();
        break;
    }
    case DragMode::ClipTrimTail:
    {
        int64_t newTail = m_dragOriginalIn + m_dragOriginalDuration + tickDelta;
        auto result = m_snapEngine.snap(newTail);
        if (result.didSnap) {
            newTail = result.snappedTick;
            setSnapIndicator(result.snappedTick);
        } else {
            setSnapIndicator(-1);
        }

        // Edge-trim must stop at any neighboring clip on the same track.
        if (Track* tr = m_timeline->track(m_dragClipRef.trackIndex)) {
            const int64_t origTail = m_dragOriginalIn + m_dragOriginalDuration;
            int64_t rightLimit = std::numeric_limits<int64_t>::max();
            for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
                const Clip* other = tr->clip(ci);
                if (!other || other->id() == m_dragClipRef.clipId) continue;
                if (other->timelineIn() >= origTail &&
                    other->timelineIn() < rightLimit)
                    rightLimit = other->timelineIn();
            }
            if (newTail > rightLimit) newTail = rightLimit;
            if (newTail <= m_dragOriginalIn) newTail = m_dragOriginalIn + 1;
        }

        auto cmd = EditOperations::trimClip(*m_timeline, m_dragClipRef.trackIndex,
                                            m_dragClipRef.clipId,
                                            ClipEdge::Tail, newTail);
        executeCommand(std::move(cmd));
        // Move the hover-edge highlight with the dragged edge so the original
        // edge position doesn't stay drawn underneath as a stale blue line.
        for (auto* tw : m_trackWidgets)
            tw->setHoverEdgeTick(tw->trackIndex() == m_dragClipRef.trackIndex ? newTail : -1);
        onScrollChanged();
        break;
    }
    case DragMode::SlipTool:
    {
        auto cmd = EditOperations::slipClip(*m_timeline, m_dragClipRef.trackIndex,
                                            m_dragClipRef.clipId, tickDelta);
        executeCommand(std::move(cmd));
        onScrollChanged();
        break;
    }
    case DragMode::SlideTool:
    {
        auto cmd = EditOperations::slideClip(*m_timeline, m_dragClipRef.trackIndex,
                                            m_dragClipRef.clipId, tickDelta);
        executeCommand(std::move(cmd));
        onScrollChanged();
        break;
    }
    case DragMode::RollingEdit:
    {
        // Direct-manipulation: apply rolling edit to data model without commands.
        // A single undoable command is created on mouse-release.
        int64_t newEditPoint = m_rollOriginalEditPoint + tickDelta;
        auto result = m_snapEngine.snap(newEditPoint);
        if (result.didSnap) {
            newEditPoint = result.snappedTick;
            setSnapIndicator(result.snappedTick);
        } else {
            setSnapIndicator(-1);
        }

        Track* rollTrack = m_timeline->track(m_rollTrackIndex);
        if (rollTrack) {
            size_t li = rollTrack->findClipIndexById(m_rollLeftClipId);
            size_t ri = rollTrack->findClipIndexById(m_rollRightClipId);
            if (li < rollTrack->clipCount() && ri < rollTrack->clipCount()) {
                // Compute new durations from originals
                constexpr int64_t kMinDur = 2000;
                int64_t leftNewDur = newEditPoint - m_rollLeftOrigIn;
                if (leftNewDur < kMinDur) {
                    newEditPoint = m_rollLeftOrigIn + kMinDur;
                    leftNewDur = kMinDur;
                }
                int64_t rightEnd = m_rollRightOrigIn + m_rollRightOrigDur;
                int64_t rightNewDur = rightEnd - newEditPoint;
                if (rightNewDur < kMinDur) {
                    newEditPoint = rightEnd - kMinDur;
                    leftNewDur = newEditPoint - m_rollLeftOrigIn;
                    rightNewDur = kMinDur;
                }
                int64_t rightSrcDelta = newEditPoint - m_rollRightOrigIn;

                Clip* lc = rollTrack->clip(li);
                Clip* rc = rollTrack->clip(ri);
                lc->setDuration(leftNewDur);
                rc->setTimelineIn(newEditPoint);
                rc->setDuration(rightNewDur);
                rc->setSourceIn(m_rollRightOrigSrcIn + rightSrcDelta);
            }
        }
        onScrollChanged();
        break;
    }
    case DragMode::PendingMarquee:
    {
        // User started pressing in empty space; check if they've moved
        // far enough to convert to a real marquee drag.
        QPointF delta = pos - m_dragStart;
        if (delta.manhattanLength() < 5.0)
            break;  // not enough movement yet

        const bool shiftHeld =
            (QApplication::keyboardModifiers() & Qt::ShiftModifier);

        if (shiftHeld) {
            // Premiere-style additive marquee: keep prior selection as a
            // base set; the marquee adds to it on every move tick.
            m_marqueeBaseSelection = m_selection.clips();
        } else {
            // Plain marquee: clear prior selection.
            m_marqueeBaseSelection.clear();
            m_selection.clear();
            m_selectedTransitionTrack = SIZE_MAX;
            m_selectedTransitionIndex = SIZE_MAX;
            for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                m_trackWidgets[w]->setSelectedTransition(SIZE_MAX);
            emit selectionChanged();
        }
        // If the press landed inside a gap, the gap-highlight overlay was
        // shown.  Clear it now that a marquee is starting — the user is
        // selecting clips, not the gap.
        if (m_gapSelection.active) {
            m_gapSelection.active = false;
            for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                m_trackWidgets[w]->setGapHighlight(-1, -1);
        }
        m_dragMode = DragMode::MarqueeSelect;
        [[fallthrough]];
    }
    case DragMode::MarqueeSelect:
    {
        // Store endpoint for selection logic and edge-scroll polling.
        m_marqueeEnd = pos;
        m_marqueeLastMovePos = pos;

        // â”€â”€ Auto-scroll while cursor pegs the left/right edge â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Premiere Pro behaviour: while marquee dragging past the
        // viewport, the timeline pans so the user can keep selecting.
        if (!m_marqueeScrollTimer) {
            m_marqueeScrollTimer = new QTimer(this);
            m_marqueeScrollTimer->setInterval(16); // ~60 Hz
            connect(m_marqueeScrollTimer, &QTimer::timeout,
                    this, [this]() {
                if (m_dragMode != DragMode::MarqueeSelect) {
                    m_marqueeScrollTimer->stop();
                    return;
                }
                const int hdr = headerWidth();
                constexpr int kEdge   = 40;   // edge zone in pixels
                constexpr double kMaxV = 18.0; // px per tick
                double dx = 0.0;
                const double cx = m_marqueeLastMovePos.x();
                if (cx < hdr + kEdge)
                    dx = -kMaxV * std::clamp(
                        (hdr + kEdge - cx) / kEdge, 0.0, 1.0);
                else if (cx > width() - kEdge)
                    dx =  kMaxV * std::clamp(
                        (cx - (width() - kEdge)) / kEdge, 0.0, 1.0);
                if (dx == 0.0) return;
                double oldScroll = m_layoutEngine.scrollX();
                double newScroll = std::max(0.0, oldScroll + dx);
                if (newScroll == oldScroll) return;
                m_layoutEngine.setScrollX(newScroll);
                // Keep m_dragStart anchored in time-space: as scrollX
                // grows, the pixel that represents the original click
                // tick must shrink by the same amount.
                m_dragStart.setX(m_dragStart.x() - (newScroll - oldScroll));
                onScrollChanged();
                // Re-issue marquee selection at new scroll position.
                refreshMarqueeSelection();
            });
        }
        const int hdr = headerWidth();
        constexpr int kEdge = 40;
        const bool atEdge = (pos.x() < hdr + kEdge) ||
                            (pos.x() > width() - kEdge);
        if (atEdge && !m_marqueeScrollTimer->isActive())
            m_marqueeScrollTimer->start();
        else if (!atEdge && m_marqueeScrollTimer->isActive())
            m_marqueeScrollTimer->stop();

        // Show QRubberBand overlay â€” it draws ON TOP of child widgets
        if (!m_rubberBand)
            m_rubberBand = new QRubberBand(QRubberBand::Rectangle, this);

        // Clamp the visible rubber band to the track area so it doesn't
        // bleed into the left-side track-header column.
        const int rightLimit = width();
        double rbX1 = std::clamp(m_dragStart.x(),
                                 static_cast<double>(hdr),
                                 static_cast<double>(rightLimit));
        double rbX2 = std::clamp(pos.x(),
                                 static_cast<double>(hdr),
                                 static_cast<double>(rightLimit));
        QRect marqueeRect = QRect(
            QPoint(static_cast<int>(std::min(rbX1, rbX2)),
                   static_cast<int>(std::min(m_dragStart.y(), pos.y()))),
            QPoint(static_cast<int>(std::max(rbX1, rbX2)),
                   static_cast<int>(std::max(m_dragStart.y(), pos.y()))));
        m_rubberBand->setGeometry(marqueeRect);
        m_rubberBand->show();

        refreshMarqueeSelection();
        update();
        break;
    }
    case DragMode::TransitionTrim:
    {
        // Adjust transition duration by dragging the free edge.
        Track* track = m_timeline->track(m_transTrimTrackIndex);
        if (track && m_transTrimIndex < track->transitionCount()) {
            Transition t = *track->transition(m_transTrimIndex);
            double px = pos.x() - headerWidth();
            int64_t dragTick = m_layoutEngine.pixelXToTime(px);

            // Compute new duration based on which edge is being dragged
            constexpr int64_t kMinTransDur = 1600;  // ~1 frame at 30 fps
            int64_t newDur = t.duration;

            if (t.leftClipId == 0) {
                // Fade-in: range is [editPt, editPt + dur], dragging end edge
                newDur = std::max(kMinTransDur, dragTick - t.editPointTick);
            } else if (t.rightClipId == 0) {
                // Fade-out: range is [editPt - dur, editPt], dragging start edge
                newDur = std::max(kMinTransDur, t.editPointTick - dragTick);
            } else {
                // Cross-dissolve: symmetric â€” distance from edit point doubled
                int64_t dist = std::abs(dragTick - t.editPointTick);
                newDur = std::max(kMinTransDur, dist * 2);
            }

            t.duration = newDur;
            track->setTransition(m_transTrimIndex, t);
        }
        onScrollChanged();
        break;
    }
    default:
        break;
    }

    event->accept();
}

void TimelinePanel::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragMode == DragMode::ClipMove && m_timeline && !m_dragSelectedClips.empty()) {
        // â”€â”€ Ghost track: create a new track if the user released in the ghost zone â”€â”€
        size_t newTrackIndex = SIZE_MAX;
        if (m_ghostTrackVisible) {
            if (m_ghostTrackIsAbove) {
                // Create new video track.  addVideoTrack inserts before the
                // first audio track, but we want it at position 0 (above
                // all existing video tracks).  Insert at index 0 manually.
                auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                auto* ptr = newTrack.get();
                (void)ptr;
                m_timeline->insertTrack(0, std::move(newTrack));
                newTrackIndex = 0;
                // All existing track indices shift +1
                for (auto& dcs : m_dragSelectedClips) {
                    dcs.originalTrack += 1;
                    dcs.ref.trackIndex += 1;
                }
                m_dragOriginalTrack += 1;
            } else {
                // Create new audio track at the end
                Track* at = m_timeline->addAudioTrack("");
                (void)at;
                newTrackIndex = m_timeline->trackCount() - 1;
            }
            m_ghostTrackVisible = false;
            if (m_ghostOverlay) m_ghostOverlay->hide();
        }

        // â”€â”€ Commit the drag as a SINGLE undoable operation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // During mouseMoveEvent we moved clips directly in the data model
        // (no commands).  Now we:
        //   1. Record final positions
        //   2. Move clips back to their original positions
        //   3. Create+execute ONE command for the full movement
        //   4. Resolve overlapping clips (overwrite)

        // Determine cross-track delta (use clicked track, not first selected)
        int trackDelta = 0;
        if (newTrackIndex != SIZE_MAX) {
            // Ghost-track: redirect to the newly created track
            trackDelta = static_cast<int>(newTrackIndex)
                       - static_cast<int>(m_dragOriginalTrack);
        } else if (m_dragTargetTrack != SIZE_MAX && !m_dragSelectedClips.empty()) {
            trackDelta = static_cast<int>(m_dragTargetTrack)
                       - static_cast<int>(m_dragOriginalTrack);
        }

        // Group-safety clamp + Y deadzone — same logic as the live-drag
        // path so the committed move matches what was previewed.
        // Skip the deadzone check when ghost-creating a new track.
        if (newTrackIndex == SIZE_MAX) {
            constexpr double kTrackShiftDeadzone = 18.0;
            if (std::abs(event->position().y() - m_dragStart.y()) < kTrackShiftDeadzone)
                trackDelta = 0;
        }
        if (trackDelta != 0) {
            size_t minOrig = SIZE_MAX, maxOrig = 0;
            for (const auto& dcs : m_dragSelectedClips) {
                minOrig = std::min(minOrig, dcs.originalTrack);
                maxOrig = std::max(maxOrig, dcs.originalTrack);
            }
            if (minOrig != SIZE_MAX) {
                const int lastTrack = static_cast<int>(m_timeline->trackCount()) - 1;
                const int minAllowed = -static_cast<int>(minOrig);
                const int maxAllowed = lastTrack - static_cast<int>(maxOrig);
                trackDelta = std::clamp(trackDelta, minAllowed, maxAllowed);
            }
        }

        // 1. Capture final positions (clip may be on a different track due to live drag)
        struct FinalPos {
            size_t srcTrack; uint64_t clipId;
            int64_t finalIn; int64_t originalIn;
            size_t dstTrack;   // target track after cross-track move
        };
        std::vector<FinalPos> finals;
        for (const auto& dcs : m_dragSelectedClips) {
            // Find the clip wherever it currently is (may have been moved cross-track)
            int64_t curPos = dcs.originalIn;
            size_t currentTrackIdx = dcs.ref.trackIndex;
            {
                Track* tr = m_timeline->track(currentTrackIdx);
                if (tr) {
                    size_t idx = tr->findClipIndexById(dcs.ref.clipId);
                    if (idx < tr->clipCount())
                        curPos = tr->clip(idx)->timelineIn();
                }
            }

            // Compute destination track (from original track + delta)
            int dst = static_cast<int>(dcs.originalTrack) + trackDelta;
            if (dst < 0) dst = 0;
            if (dst >= static_cast<int>(m_timeline->trackCount())) dst = static_cast<int>(m_timeline->trackCount()) - 1;
            size_t dstTrack = static_cast<size_t>(dst);

            // Ensure destination track has the same type as original
            Track* origTr = m_timeline->track(dcs.originalTrack);
            if (!origTr) continue;
            if (m_timeline->track(dstTrack)->type() != origTr->type())
                dstTrack = dcs.originalTrack; // fall back to same track

            if (curPos != dcs.originalIn || dstTrack != dcs.originalTrack)
                finals.push_back({dcs.originalTrack, dcs.ref.clipId, curPos, dcs.originalIn, dstTrack});
        }

        spdlog::info("[OVERLAP-DIAG] mouseRelease: {} finals from {} dragSelectedClips, trackDelta={}",
                     finals.size(), m_dragSelectedClips.size(), trackDelta);

        // 2. Restore clips to their original tracks and positions
        for (const auto& fp : finals) {
            // The clip may currently be on dstTrack (from live drag) â€” move it back
            // to the source track first.
            Track* curTr = nullptr;
            size_t curIdx = SIZE_MAX;

            // Try to find the clip on its current (possibly moved) track first
            for (size_t st = 0; st < m_timeline->trackCount(); ++st) {
                Track* t = m_timeline->track(st);
                size_t idx = t->findClipIndexById(fp.clipId);
                if (idx < t->clipCount()) {
                    curTr = t;
                    curIdx = idx;
                    break;
                }
            }
            if (!curTr || curIdx >= curTr->clipCount()) continue;

            Track* srcTr = m_timeline->track(fp.srcTrack);
            if (!srcTr) continue;

            if (curTr != srcTr) {
                // Transfer back to original track
                auto clipPtr = curTr->removeClip(curIdx);
                clipPtr->setTimelineIn(fp.originalIn);
                srcTr->addClip(std::move(clipPtr));
            } else {
                curTr->moveClip(curIdx, fp.originalIn);
            }
        }

        // 3 + 4. Create a SINGLE compound command for move + overwrite + ghost track.
        // We manually execute sub-commands, then pushWithoutExecute so
        // that one Ctrl+Z undoes the move, any overlap resolution, AND
        // the ghost track creation (if applicable).
        bool hasGhostTrack = (newTrackIndex != SIZE_MAX);
        bool didMove = false;

        // Alt+drag = COPY clips (Premiere Pro style) instead of moving
        bool altCopy = (event->modifiers() & Qt::AltModifier) && !finals.empty();

        if (altCopy) {
            auto masterCompound = std::make_unique<CompoundCommand>("Copy clips");

            if (hasGhostTrack) {
                masterCompound->addCommand(
                    std::make_unique<InsertTrackAtCommand>(m_timeline, newTrackIndex));
            }

            // Clone each clip and add at the final position
            for (const auto& fp : finals) {
                Track* srcTr = m_timeline->track(fp.srcTrack);
                if (!srcTr) continue;
                size_t idx = srcTr->findClipIndexById(fp.clipId);
                if (idx >= srcTr->clipCount()) continue;

                auto cloned = srcTr->clip(idx)->clone();
                cloned->setTimelineIn(fp.finalIn);

                Track* dstTr = m_timeline->track(fp.dstTrack);
                if (!dstTr) continue;

                auto cmd = std::make_unique<AddClipCommand>(dstTr, std::move(cloned));
                cmd->execute();
                masterCompound->addCommand(std::move(cmd));
            }

            // Resolve overlaps on the destination tracks
            for (const auto& fp : finals) {
                Track* dstTr = m_timeline->track(fp.dstTrack);
                if (!dstTr) continue;
                // Find the newly added clip by finalIn position
                for (size_t ci = 0; ci < dstTr->clipCount(); ++ci) {
                    if (dstTr->clip(ci)->timelineIn() == fp.finalIn &&
                        dstTr->clip(ci)->id() != fp.clipId) {
                        auto overwrite = EditOperations::resolveOverlaps(
                            *m_timeline, fp.dstTrack, dstTr->clip(ci)->id());
                        if (overwrite) {
                            overwrite->execute();
                            masterCompound->addCommand(std::move(overwrite));
                        }
                        break;
                    }
                }
            }

            if (masterCompound->size() > 0) {
                m_commandStack->pushWithoutExecute(std::move(masterCompound));
                onScrollChanged();
                emit contentChanged();
                didMove = true;
            }
        }
        else if (!finals.empty() || hasGhostTrack) {
            auto masterCompound = std::make_unique<CompoundCommand>("Move clips");

            // 3a-pre. If a ghost track was created, add its command FIRST.
            // Compound undo runs in reverse, so this will be undone LAST
            // (after clips are moved back to their original tracks).
            if (hasGhostTrack) {
                masterCompound->addCommand(
                    std::make_unique<InsertTrackAtCommand>(m_timeline, newTrackIndex));
            }

            // 3a. Build and execute move commands
            for (const auto& fp : finals) {
                std::unique_ptr<Command> cmd;
                if (fp.srcTrack != fp.dstTrack) {
                    cmd = EditOperations::moveClipToTrack(
                        *m_timeline, fp.srcTrack, fp.dstTrack, fp.clipId, fp.finalIn);
                } else {
                    cmd = EditOperations::moveClip(
                        *m_timeline, fp.srcTrack, fp.clipId, fp.finalIn);
                }
                if (cmd) {
                    cmd->execute();
                    masterCompound->addCommand(std::move(cmd));
                }
            }

            // 3b. Resolve overlapping clips (Premiere-style overwrite).
            // Clips are now at final positions so overlap detection works.
            // After cross-track moves the clip ID changes (clone), so we
            // find the moved clip by its finalIn position on the dest track.
            for (const auto& fp : finals) {
                uint64_t resolveId = fp.clipId;
                if (fp.srcTrack != fp.dstTrack) {
                    Track* dstTr = m_timeline->track(fp.dstTrack);
                    if (dstTr) {
                        for (size_t ci = 0; ci < dstTr->clipCount(); ++ci) {
                            if (dstTr->clip(ci)->timelineIn() == fp.finalIn) {
                                resolveId = dstTr->clip(ci)->id();
                                break;
                            }
                        }
                    }
                }
                auto overwrite = EditOperations::resolveOverlaps(
                    *m_timeline, fp.dstTrack, resolveId);
                if (overwrite) {
                    overwrite->execute();
                    masterCompound->addCommand(std::move(overwrite));
                }
            }

            if (masterCompound->size() > 0) {
                m_commandStack->pushWithoutExecute(std::move(masterCompound));
                onScrollChanged();
                emit contentChanged();
                didMove = true;
            }
        }

        m_dragSelectedClips.clear();
        m_dragTargetTrack = SIZE_MAX;
        m_ghostTrackVisible = false;
        if (m_ghostOverlay) m_ghostOverlay->hide();

        // Only rebuild tracks when clips actually moved (avoids expensive
        // widget teardown + reselection churn on simple click-and-release).
        if (didMove) {
            rebuildTracks();
            // Re-sync selection highlights to new widgets
            emit selectionChanged();
        }
    }
    // â”€â”€ Rolling edit: commit as a single undoable command â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (m_dragMode == DragMode::RollingEdit && m_timeline) {
        Track* rollTrack = m_timeline->track(m_rollTrackIndex);
        if (rollTrack) {
            size_t li = rollTrack->findClipIndexById(m_rollLeftClipId);
            size_t ri = rollTrack->findClipIndexById(m_rollRightClipId);
            if (li < rollTrack->clipCount() && ri < rollTrack->clipCount()) {
                // Capture the current (final) edit point
                int64_t finalEditPoint = rollTrack->clip(li)->timelineOut();

                // Restore clips to their original state
                Clip* lc = rollTrack->clip(li);
                Clip* rc = rollTrack->clip(ri);
                lc->setTimelineIn(m_rollLeftOrigIn);
                lc->setDuration(m_rollLeftOrigDur);
                lc->setSourceIn(m_rollLeftOrigSrcIn);
                rc->setTimelineIn(m_rollRightOrigIn);
                rc->setDuration(m_rollRightOrigDur);
                rc->setSourceIn(m_rollRightOrigSrcIn);

                // Now create and execute ONE command for the entire drag
                if (finalEditPoint != m_rollOriginalEditPoint) {
                    auto cmd = EditOperations::rollingEdit(
                        *m_timeline, m_rollTrackIndex,
                        m_rollLeftClipId, m_rollRightClipId, finalEditPoint);
                    if (cmd) executeCommand(std::move(cmd));
                }
            }
        }
    }
    // â”€â”€ Transition trim: commit duration change as a single undo step â”€â”€â”€
    if (m_dragMode == DragMode::TransitionTrim && m_timeline) {
        Track* track = m_timeline->track(m_transTrimTrackIndex);
        if (track && m_transTrimIndex < track->transitionCount()) {
            int64_t finalDur = track->transition(m_transTrimIndex)->duration;

            // Restore original duration, then execute command so undo works
            Transition restored = *track->transition(m_transTrimIndex);
            restored.duration = m_transTrimOrigDuration;
            track->setTransition(m_transTrimIndex, restored);

            if (finalDur != m_transTrimOrigDuration) {
                Transition newValues = *track->transition(m_transTrimIndex);
                newValues.duration = finalDur;
                auto cmd = std::make_unique<SetTransitionPropertyCommand>(
                    track, m_transTrimIndex, newValues);
                executeCommand(std::move(cmd));
            }
        }
    }
    if (m_dragMode == DragMode::PendingMarquee) {
        // Mouse released without enough movement — this was a click in
        // empty space, so deselect now.
        if (!(event->modifiers() & Qt::ShiftModifier)) {
            m_selection.clear();
            m_selectedTransitionTrack = SIZE_MAX;
            m_selectedTransitionIndex = SIZE_MAX;
            for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                m_trackWidgets[w]->setSelectedTransition(SIZE_MAX);
        }
        emit selectionChanged();
        update();
    }
    if (m_dragMode == DragMode::MarqueeSelect) {
        // Hide the rubber-band overlay
        if (m_rubberBand)
            m_rubberBand->hide();
        if (m_marqueeScrollTimer && m_marqueeScrollTimer->isActive())
            m_marqueeScrollTimer->stop();
        m_marqueeBaseSelection.clear();
        update();  // Clear any leftover artifacts
    }
    // Clear snap indicator line when drag ends
    setSnapIndicator(-1);
    m_ghostTrackVisible = false;
    if (m_ghostOverlay) m_ghostOverlay->hide();
    m_dragMode = DragMode::None;
    QWidget::mouseReleaseEvent(event);
}

void TimelinePanel::refreshMarqueeSelection()
{
    if (!m_timeline) return;

    // Update marquee rectangle and select clips
    double x1 = m_dragStart.x() - headerWidth();
    double x2 = m_marqueeLastMovePos.x() - headerWidth();
    int64_t t1 = m_layoutEngine.pixelXToTime(x1);
    int64_t t2 = m_layoutEngine.pixelXToTime(x2);
    size_t track1 = hitTestTrack(m_dragStart.y());
    size_t track2 = hitTestTrack(m_marqueeLastMovePos.y());

    // Clamp to valid track range so marquee works even when the
    // drag starts/ends in the empty space above or below tracks.
    size_t trackCount = m_timeline->trackCount();
    if (trackCount > 0) {
        const int topY = m_trackWidgets.empty() ? 0
                       : m_trackWidgets.front()->mapTo(this, QPoint(0, 0)).y();
        if (track1 == SIZE_MAX)
            track1 = (m_dragStart.y() < topY) ? 0 : trackCount - 1;
        if (track2 == SIZE_MAX)
            track2 = (m_marqueeLastMovePos.y() < topY) ? 0 : trackCount - 1;
    }

    TimelineRect rect{t1, t2, track1, track2};
    m_selection.selectRect(*m_timeline, rect);

    // Shift+marquee additive: re-add the base selection captured at
    // marquee start so the user can extend the existing selection
    // instead of replacing it.
    if (!m_marqueeBaseSelection.empty()) {
        for (const auto& ref : m_marqueeBaseSelection) {
            if (!m_selection.isSelected(ref))
                m_selection.selectClip(ref, /*additive=*/true);
        }
    }
    emit selectionChanged();
}

} // namespace rt
