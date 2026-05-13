/*
 * TimelinePanelMouseDragRelease.cpp — mouseReleaseEvent extracted from
 * TimelinePanelMouseDrag.cpp.
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/TransitionCmds.h"

#include <spdlog/spdlog.h>

namespace rt {

void TimelinePanel::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragMode == DragMode::ClipMove && m_timeline && !m_dragSelectedClips.empty()) {
        // ── Ghost track: create a new track if the user released in the ghost zone ──
        size_t newTrackIndex = SIZE_MAX;
        if (m_ghostTrackVisible) {
            if (m_ghostTrackIsAbove) {
                auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                auto* ptr = newTrack.get();
                (void)ptr;
                m_timeline->insertTrack(0, std::move(newTrack));
                newTrackIndex = 0;
                for (auto& dcs : m_dragSelectedClips) {
                    dcs.originalTrack += 1;
                    dcs.ref.trackIndex += 1;
                }
                m_dragOriginalTrack += 1;
            } else {
                Track* at = m_timeline->addAudioTrack("");
                (void)at;
                newTrackIndex = m_timeline->trackCount() - 1;
            }
            m_ghostTrackVisible = false;
            if (m_ghostOverlay) m_ghostOverlay->hide();
        }

        // ── Commit the drag as a SINGLE undoable operation ──────────────
        int trackDelta = 0;
        if (newTrackIndex != SIZE_MAX) {
            trackDelta = static_cast<int>(newTrackIndex)
                       - static_cast<int>(m_dragOriginalTrack);
        } else if (m_dragTargetTrack != SIZE_MAX && !m_dragSelectedClips.empty()) {
            trackDelta = static_cast<int>(m_dragTargetTrack)
                       - static_cast<int>(m_dragOriginalTrack);
        }

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

        // Capture final positions
        struct FinalPos {
            size_t srcTrack; uint64_t clipId;
            int64_t finalIn; int64_t originalIn;
            size_t dstTrack;
        };
        std::vector<FinalPos> finals;
        for (const auto& dcs : m_dragSelectedClips) {
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

            int dst = static_cast<int>(dcs.originalTrack) + trackDelta;
            if (dst < 0) dst = 0;
            if (dst >= static_cast<int>(m_timeline->trackCount())) dst = static_cast<int>(m_timeline->trackCount()) - 1;
            size_t dstTrack = static_cast<size_t>(dst);

            Track* origTr = m_timeline->track(dcs.originalTrack);
            if (!origTr) continue;
            if (m_timeline->track(dstTrack)->type() != origTr->type())
                dstTrack = dcs.originalTrack;

            if (curPos != dcs.originalIn || dstTrack != dcs.originalTrack)
                finals.push_back({dcs.originalTrack, dcs.ref.clipId, curPos, dcs.originalIn, dstTrack});
        }

        spdlog::info("[OVERLAP-DIAG] mouseRelease: {} finals from {} dragSelectedClips, trackDelta={}",
                     finals.size(), m_dragSelectedClips.size(), trackDelta);

        // Restore clips to their original tracks and positions
        for (const auto& fp : finals) {
            Track* curTr = nullptr;
            size_t curIdx = SIZE_MAX;

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
                auto clipPtr = curTr->removeClip(curIdx);
                clipPtr->setTimelineIn(fp.originalIn);
                srcTr->addClip(std::move(clipPtr));
            } else {
                curTr->moveClip(curIdx, fp.originalIn);
            }
        }

        bool hasGhostTrack = (newTrackIndex != SIZE_MAX);
        bool didMove = false;

        // Alt+drag = COPY clips
        bool altCopy = (event->modifiers() & Qt::AltModifier) && !finals.empty();

        if (altCopy) {
            auto masterCompound = std::make_unique<CompoundCommand>("Copy clips");

            if (hasGhostTrack) {
                masterCompound->addCommand(
                    std::make_unique<InsertTrackAtCommand>(m_timeline, newTrackIndex));
            }

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

            for (const auto& fp : finals) {
                Track* dstTr = m_timeline->track(fp.dstTrack);
                if (!dstTr) continue;
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

            if (hasGhostTrack) {
                masterCompound->addCommand(
                    std::make_unique<InsertTrackAtCommand>(m_timeline, newTrackIndex));
            }

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

        if (didMove) {
            rebuildTracks();
            emit selectionChanged();
        }
    }

    // ── Rolling edit: commit as a single undoable command ────────────────
    if (m_dragMode == DragMode::RollingEdit && m_timeline) {
        Track* rollTrack = m_timeline->track(m_rollTrackIndex);
        if (rollTrack) {
            size_t li = rollTrack->findClipIndexById(m_rollLeftClipId);
            size_t ri = rollTrack->findClipIndexById(m_rollRightClipId);
            if (li < rollTrack->clipCount() && ri < rollTrack->clipCount()) {
                int64_t finalEditPoint = rollTrack->clip(li)->timelineOut();

                Clip* lc = rollTrack->clip(li);
                Clip* rc = rollTrack->clip(ri);
                lc->setTimelineIn(m_rollLeftOrigIn);
                lc->setDuration(m_rollLeftOrigDur);
                lc->setSourceIn(m_rollLeftOrigSrcIn);
                rc->setTimelineIn(m_rollRightOrigIn);
                rc->setDuration(m_rollRightOrigDur);
                rc->setSourceIn(m_rollRightOrigSrcIn);

                if (finalEditPoint != m_rollOriginalEditPoint) {
                    auto cmd = EditOperations::rollingEdit(
                        *m_timeline, m_rollTrackIndex,
                        m_rollLeftClipId, m_rollRightClipId, finalEditPoint);
                    if (cmd) executeCommand(std::move(cmd));
                }
            }
        }
    }

    // ── Transition trim: commit duration change as a single undo step ───
    if (m_dragMode == DragMode::TransitionTrim && m_timeline) {
        Track* track = m_timeline->track(m_transTrimTrackIndex);
        if (track && m_transTrimIndex < track->transitionCount()) {
            Transition t = *track->transition(m_transTrimIndex);
            auto cmd = std::make_unique<SetTransitionPropertyCommand>(
                track, m_transTrimIndex, t);
            if (cmd) executeCommand(std::move(cmd));
        }
    }

    // ── Reset all drag-related state ─────────────────────────────────────
    if (m_dragMode != DragMode::None) {
        m_dragMode = DragMode::None;
        m_dragClipRef = {};
        m_dragSelectedClips.clear();
        m_dragTargetTrack = SIZE_MAX;
        m_ghostTrackVisible = false;
        if (m_ghostOverlay) m_ghostOverlay->hide();

        if (m_marqueeScrollTimer) m_marqueeScrollTimer->stop();
        m_marqueeLastMovePos = QPointF();

        if (m_rubberBand) m_rubberBand->hide();
        setCursor(Qt::ArrowCursor);
        for (auto tw : m_trackWidgets)
            tw->setHoverEdgeTick(-1);

        // Clear gap selection on release
        if (m_gapSelection.active) {
            m_gapSelection.active = false;
            for (auto tw : m_trackWidgets)
                tw->setGapHighlight(-1, -1);
        }
    }

    QWidget::mouseReleaseEvent(event);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
