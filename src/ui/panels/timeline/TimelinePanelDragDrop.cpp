/*
 * TimelinePanelDragDrop.cpp - Drag-and-drop from Project Bin to Timeline.
 * Split from TimelinePanel.cpp for maintainability.
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "timeline/Transition.h"
#include "command/CommandStack.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"
#include "effects/Effect.h"
#include "media/MediaPool.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QTreeWidget>

#include <spdlog/spdlog.h>

namespace rt {
namespace {
constexpr size_t kGhostDropTrackVideoAbove = SIZE_MAX - 1;
constexpr size_t kGhostDropTrackAudioBelow = SIZE_MAX - 2;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Drag-and-drop from Project Bin → Timeline
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::dragEnterEvent(QDragEnterEvent* event)
{
    // Accept effect drags, transition drags, media drags, sequence drags, or external file drops
    if (event->mimeData()->hasFormat("application/x-roundtable-effect") ||
        event->mimeData()->hasFormat(kTransitionMimeType) ||
        event->mimeData()->hasFormat("application/x-roundtable-media") ||
        event->mimeData()->hasFormat("application/x-roundtable-sequence") ||
        qobject_cast<QTreeWidget*>(event->source()) ||
        event->mimeData()->hasUrls())
    {
        event->acceptProposedAction();

        // Build snap targets for the incoming drag operation
        if (m_timeline) {
            m_snapEngine.setPixelsPerSecond(m_layoutEngine.pixelsPerSecond());
            m_snapEngine.buildTargets(*m_timeline, m_playheadTick, 0.0, {});
        }
    }
    else
        event->ignore();
}

void TimelinePanel::dragMoveEvent(QDragMoveEvent* event)
{
    // ── Transition drag (custom MIME) ───────────────────────────────────
    if (event->mimeData()->hasFormat(kTransitionMimeType)) {
        QPointF pos = event->position();
        size_t trackIdx = hitTestTrack(pos.y());

        // Adjust X for track header column (same as hitTestClip)
        double cursorPx = pos.x() - headerWidth();

        TransitionDropTarget bestTarget;

        if (m_timeline && trackIdx < m_timeline->trackCount()) {
            const Track* track = m_timeline->track(trackIdx);

            // Find the closest clip edge to the cursor
            double bestDist = kEdgeSnapPixels + 1;

            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                const Clip* clip = track->clip(ci);
                if (!clip) continue;

                // Check tail edge (end of this clip)
                double tailX = m_layoutEngine.timeToPixelX(clip->timelineOut());
                double tailDist = std::abs(cursorPx - tailX);
                if (tailDist < bestDist) {
                    bestDist = tailDist;
                    bestTarget.trackIndex = trackIdx;
                    bestTarget.editPointTick = clip->timelineOut();
                    bestTarget.leftClipId = clip->id();
                    bestTarget.rightClipId = 0;

                    // Check if there's an adjacent clip starting at or near this edge
                    for (size_t cj = 0; cj < track->clipCount(); ++cj) {
                        if (cj == ci) continue;
                        const Clip* adjClip = track->clip(cj);
                        if (!adjClip) continue;
                        int64_t gap = std::abs(adjClip->timelineIn() - clip->timelineOut());
                        if (gap <= 1600) { // ~33ms at 48kHz
                            bestTarget.rightClipId = adjClip->id();
                            break;
                        }
                    }
                }

                // Check head edge (start of this clip)
                double headX = m_layoutEngine.timeToPixelX(clip->timelineIn());
                double headDist = std::abs(cursorPx - headX);
                if (headDist < bestDist) {
                    bestDist = headDist;
                    bestTarget.trackIndex = trackIdx;
                    bestTarget.editPointTick = clip->timelineIn();
                    bestTarget.rightClipId = clip->id();
                    bestTarget.leftClipId = 0;

                    // Check if there's an adjacent clip ending at or near this edge
                    for (size_t cj = 0; cj < track->clipCount(); ++cj) {
                        if (cj == ci) continue;
                        const Clip* adjClip = track->clip(cj);
                        if (!adjClip) continue;
                        int64_t gap = std::abs(adjClip->timelineOut() - clip->timelineIn());
                        if (gap <= 1600) {
                            bestTarget.leftClipId = adjClip->id();
                            break;
                        }
                    }
                }
            }

            // If no edge within snap distance, snap to nearest edge of
            // the clip under cursor (Premiere Pro: drop anywhere on clip)
            if (bestTarget.trackIndex == SIZE_MAX) {
                int64_t cursorTick = m_layoutEngine.pixelXToTime(cursorPx);
                for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                    const Clip* clip = track->clip(ci);
                    if (!clip) continue;
                    if (cursorTick >= clip->timelineIn() && cursorTick < clip->timelineOut()) {
                        // Cursor is over this clip — use nearest edge
                        int64_t headDist = std::abs(cursorTick - clip->timelineIn());
                        int64_t tailDist = std::abs(cursorTick - clip->timelineOut());
                        if (headDist <= tailDist) {
                            bestTarget.trackIndex = trackIdx;
                            bestTarget.editPointTick = clip->timelineIn();
                            bestTarget.rightClipId = clip->id();
                            bestTarget.leftClipId = 0;
                            for (size_t cj = 0; cj < track->clipCount(); ++cj) {
                                if (cj == ci) continue;
                                const Clip* adj = track->clip(cj);
                                if (adj && std::abs(adj->timelineOut() - clip->timelineIn()) <= 1600) {
                                    bestTarget.leftClipId = adj->id();
                                    break;
                                }
                            }
                        } else {
                            bestTarget.trackIndex = trackIdx;
                            bestTarget.editPointTick = clip->timelineOut();
                            bestTarget.leftClipId = clip->id();
                            bestTarget.rightClipId = 0;
                            for (size_t cj = 0; cj < track->clipCount(); ++cj) {
                                if (cj == ci) continue;
                                const Clip* adj = track->clip(cj);
                                if (adj && std::abs(adj->timelineIn() - clip->timelineOut()) <= 1600) {
                                    bestTarget.rightClipId = adj->id();
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }

        // Update highlight on track widgets
        bool hasTarget = bestTarget.trackIndex != SIZE_MAX;
        if (hasTarget) {
            m_transitionDropTarget = bestTarget;
        } else {
            m_transitionDropTarget.reset();
        }
        for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
            if (hasTarget && i == bestTarget.trackIndex)
                m_trackWidgets[i]->setTransitionDropEdgeTick(bestTarget.editPointTick);
            else
                m_trackWidgets[i]->clearTransitionDropEdge();
        }

        event->acceptProposedAction();
        return;
    }

    // ── Effect drag (custom MIME) ───────────────────────────────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-effect")) {
        QPointF pos = event->position();
        auto hitRef = hitTestClip(pos);
        if (hitRef != m_effectDropTarget) {
            m_effectDropTarget = hitRef;
            // Forward highlight to the correct TimelineTrackWidget
            for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
                if (hitRef && i == hitRef->trackIndex)
                    m_trackWidgets[i]->setEffectHighlightClipId(hitRef->clipId);
                else
                    m_trackWidgets[i]->clearEffectHighlight();
            }
        }
        event->acceptProposedAction();
        return;
    }

    // ── Sequence drag (from project bin) ────────────────────────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-sequence")) {
        m_effectDropTarget.reset();
        for (auto* tw : m_trackWidgets) tw->clearEffectHighlight();

        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        // Read actual duration from MIME, fall back to 5 seconds
        int64_t previewDur = static_cast<int64_t>(5.0 * 48000.0);

        // Source in/out from Source Monitor drag take priority
        if (event->mimeData()->hasFormat("application/x-roundtable-source-in")
            && event->mimeData()->hasFormat("application/x-roundtable-source-out")) {
            int64_t srcIn  = event->mimeData()->data("application/x-roundtable-source-in").toLongLong();
            int64_t srcOut = event->mimeData()->data("application/x-roundtable-source-out").toLongLong();
            if (srcOut > srcIn) previewDur = srcOut - srcIn;
        } else if (event->mimeData()->hasFormat("application/x-roundtable-sequence-duration")) {
            bool durOk = false;
            int64_t d = event->mimeData()->data("application/x-roundtable-sequence-duration")
                            .toLongLong(&durOk);
            if (durOk && d > 0) previewDur = d;
        }

        auto snapRes = m_snapEngine.snapPair(tick, tick + previewDur);
        if (snapRes.didSnap) tick = snapRes.snappedTick;

        // Show preview on video tracks only
        bool trackCompatible = false;
        if (m_timeline && trackIdx < m_timeline->trackCount()) {
            trackCompatible = (m_timeline->track(trackIdx)->type() == TrackType::Video);
        }
        for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
            if (i == trackIdx && trackCompatible)
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, false);
            else
                m_trackWidgets[i]->clearMediaDragPreview();
        }
        event->acceptProposedAction();
        return;
    }

    // ── Media drag (custom MIME, QTreeWidget fallback, or external file URLs) ──
    if (event->mimeData()->hasFormat("application/x-roundtable-media")
        || qobject_cast<QTreeWidget*>(event->source())
        || event->mimeData()->hasUrls()) {
        m_effectDropTarget.reset();
        for (auto* tw : m_trackWidgets) tw->clearEffectHighlight();
        // Compute drag preview position and duration
        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        // Determine clip duration for the preview
        int64_t previewDur = 0;
        bool isAudio = false;

        // Check source in/out (from Source Monitor drag)
        if (event->mimeData()->hasFormat("application/x-roundtable-source-in")
            && event->mimeData()->hasFormat("application/x-roundtable-source-out")) {
            int64_t srcIn  = event->mimeData()->data("application/x-roundtable-source-in").toLongLong();
            int64_t srcOut = event->mimeData()->data("application/x-roundtable-source-out").toLongLong();
            if (srcOut > srcIn) previewDur = srcOut - srcIn;
        }

        // Try to get duration from MediaPool via handle
        if (previewDur <= 0 && m_mediaPool
            && event->mimeData()->hasFormat("application/x-roundtable-media")) {
            bool ok = false;
            uint64_t handle = event->mimeData()->data("application/x-roundtable-media")
                                  .toULongLong(&ok);
            if (ok && handle != 0) {
                const auto* info = m_mediaPool->getInfo(handle);
                if (info && info->duration > 0.0)
                    previewDur = static_cast<int64_t>(info->duration * 48000.0);
                // Determine if audio by checking video stream
                if (info)
                    isAudio = (info->videoStreamIndex < 0);
            }
        }

        // Resolve duration by opening the file (handles character
        // animations whose media handle isn't populated until first use).
        if (previewDur <= 0 && m_mediaPool
            && event->mimeData()->hasUrls()
            && !event->mimeData()->urls().isEmpty()) {
            QString path = event->mimeData()->urls().first().toLocalFile();
            if (!path.isEmpty()) {
                auto h = m_mediaPool->open(path.toStdString());
                if (h != 0) {
                    const auto* info = m_mediaPool->getInfo(h);
                    if (info && info->duration > 0.0)
                        previewDur = static_cast<int64_t>(info->duration * 48000.0);
                    if (info)
                        isAudio = (info->videoStreamIndex < 0);
                }
            }
        }

        // Fallback: 5-second default if all else fails
        if (previewDur <= 0) {
            previewDur = static_cast<int64_t>(5.0 * 48000.0);
        }
        if (!isAudio && event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty()) {
            QString ext = QFileInfo(event->mimeData()->urls().first().toLocalFile())
                              .suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            if (audioExts.contains(ext)) isAudio = true;
        }

        // Snap both head and tail (Premiere Pro-style edge snapping)
        if (previewDur > 0) {
            auto snapRes = m_snapEngine.snapPair(tick, tick + previewDur);
            if (snapRes.didSnap) tick = snapRes.snappedTick;
        } else {
            auto snapRes = m_snapEngine.snap(tick);
            if (snapRes.didSnap) tick = snapRes.snappedTick;
        }

        // Show snap indicator on track widgets
        for (auto* tw : m_trackWidgets) tw->setSnapIndicatorTick(-1);

        // Update only the target track's preview; clear all others.
        // Skip preview if media type doesn't match the track type
        // (e.g. audio file over a video track, or video over audio track).
        bool trackCompatible = false;
        bool mediaHasAudio = false;
        if (m_timeline && trackIdx < m_timeline->trackCount()) {
            TrackType tt = m_timeline->track(trackIdx)->type();
            trackCompatible = isAudio ? (tt == TrackType::Audio)
                                      : (tt == TrackType::Video);
            // Check if this video file also has audio (for dual ghost)
            if (!isAudio && m_mediaPool
                && event->mimeData()->hasFormat("application/x-roundtable-media")) {
                bool ok = false;
                uint64_t h = event->mimeData()->data("application/x-roundtable-media")
                                 .toULongLong(&ok);
                if (ok && h != 0) {
                    const auto* info = m_mediaPool->getInfo(h);
                    if (info && info->hasAudio)
                        mediaHasAudio = true;
                }
            }
        }
        // Find target audio track index for companion preview.
        // If the cursor hovers an audio track, the preview follows it there;
        // otherwise fall back to the first audio track.
        size_t audioTrackIdx = SIZE_MAX;
        if (mediaHasAudio && m_timeline) {
            if (trackIdx < m_timeline->trackCount() &&
                m_timeline->track(trackIdx)->type() == TrackType::Audio) {
                audioTrackIdx = trackIdx;
            } else {
                for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                    if (m_timeline->track(i)->type() == TrackType::Audio) {
                        audioTrackIdx = i; break;
                    }
                }
            }
        }
        // Find target video track index for companion preview when
        // cursor is over an audio track (Premiere Pro style: video
        // always previews on the bottom video track even when hovering
        // audio).
        size_t videoTrackIdx = SIZE_MAX;
        if (!isAudio && m_timeline && trackIdx < m_timeline->trackCount() &&
            m_timeline->track(trackIdx)->type() == TrackType::Audio) {
            // Cursor over audio — preview video on bottom video track
            for (size_t i = m_timeline->trackCount(); i > 0; --i) {
                if (m_timeline->track(i - 1)->type() == TrackType::Video) {
                    videoTrackIdx = i - 1; break;
                }
            }
        }
        for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
            if (i == trackIdx && trackCompatible)
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, isAudio);
            else if (mediaHasAudio && i == audioTrackIdx)
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, true);
            else if (!isAudio && i == videoTrackIdx)
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, false);
            else
                m_trackWidgets[i]->clearMediaDragPreview();
        }

        // ── Ghost track overlay for bin/external drags ──
        if (!m_trackWidgets.empty()) {
            auto* firstTw = m_trackWidgets.front();
            auto* lastTw  = m_trackWidgets.back();
            QPoint firstTop = firstTw->mapTo(this, QPoint(0, 0));
            QPoint lastBot  = lastTw->mapTo(this, QPoint(0, lastTw->height()));

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

            if (!isAudio && pos.y() < firstTop.y() && firstVideoIdx < m_trackWidgets.size()) {
                // Above topmost track → ghost new video track above
                m_ghostTrackVisible = true;
                m_ghostTrackIsAbove = true;
                m_ghostTrackHeight = firstTw->height();
                m_ghostTrackY = firstTop.y() - m_ghostTrackHeight;
            } else if (isAudio && pos.y() > lastBot.y() && lastAudioIdx < m_trackWidgets.size()) {
                // Below bottommost track → ghost new audio track below
                m_ghostTrackVisible = true;
                m_ghostTrackIsAbove = false;
                m_ghostTrackHeight = lastTw->height();
                m_ghostTrackY = lastBot.y();
            } else {
                m_ghostTrackVisible = false;
                if (m_ghostOverlay) m_ghostOverlay->hide();
            }
            if (m_ghostTrackVisible && m_ghostOverlay) {
                m_ghostOverlay->isAbove = m_ghostTrackIsAbove;
                m_ghostOverlay->setGeometry(ghostX, m_ghostTrackY, ghostW, m_ghostTrackHeight);
                m_ghostOverlay->raise();
                m_ghostOverlay->show();
                m_ghostOverlay->update();
            }
        }

        event->acceptProposedAction();
    } else {
        // Hide ghost overlay if not dragging media
        m_ghostTrackVisible = false;
        if (m_ghostOverlay) m_ghostOverlay->hide();
        event->ignore();
    }
}

void TimelinePanel::dragLeaveEvent(QDragLeaveEvent* event)
{
    m_effectDropTarget.reset();
    m_transitionDropTarget.reset();
    m_ghostTrackVisible = false;
    if (m_ghostOverlay) m_ghostOverlay->hide();
    for (auto* tw : m_trackWidgets) {
        tw->clearEffectHighlight();
        tw->clearTransitionDropEdge();
        tw->clearMediaDragPreview();
    }
    QWidget::dragLeaveEvent(event);
}

void TimelinePanel::dropEvent(QDropEvent* event)
{
    const bool ghostWasVisible = m_ghostTrackVisible;
    const bool ghostWasAbove = m_ghostTrackIsAbove;

    m_ghostTrackVisible = false;
    if (m_ghostOverlay) m_ghostOverlay->hide();

    // Clear all drag previews
    for (auto* tw : m_trackWidgets) tw->clearMediaDragPreview();

    // Compute drop-time ghost zones directly from cursor Y and current track geometry,
    // so routing does not depend on whether a prior dragMove state was preserved.
    auto computeGhostDropZones = [this](const QPointF& pos, bool& aboveTopVideo, bool& belowBottomAudio) {
        aboveTopVideo = false;
        belowBottomAudio = false;
        if (!m_timeline || m_trackWidgets.empty()) return;

        size_t firstVideoIdx = SIZE_MAX;
        size_t lastAudioIdx = SIZE_MAX;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            auto* tr = m_timeline->track(i);
            if (!tr) continue;
            if (tr->type() == TrackType::Video) {
                if (firstVideoIdx == SIZE_MAX) firstVideoIdx = i;
            } else {
                lastAudioIdx = i;
            }
        }

        if (firstVideoIdx < m_trackWidgets.size()) {
            auto* w = m_trackWidgets[firstVideoIdx];
            QPoint top = w->mapTo(this, QPoint(0, 0));
            aboveTopVideo = (pos.y() < top.y());
        }
        if (lastAudioIdx < m_trackWidgets.size()) {
            auto* w = m_trackWidgets[lastAudioIdx];
            QPoint bot = w->mapTo(this, QPoint(0, w->height()));
            belowBottomAudio = (pos.y() > bot.y());
        }
    };

    // ── Transition drop (custom MIME type) ──────────────────────────────
    if (event->mimeData()->hasFormat(kTransitionMimeType)) {
        // Clear highlights
        for (auto* tw : m_trackWidgets) tw->clearTransitionDropEdge();

        if (!m_transitionDropTarget || !m_timeline) {
            m_transitionDropTarget.reset();
            event->ignore();
            return;
        }

        QByteArray transData = event->mimeData()->data(kTransitionMimeType);
        bool ok = false;
        int transType = transData.toInt(&ok);
        if (!ok) { m_transitionDropTarget.reset(); event->ignore(); return; }

        auto target = *m_transitionDropTarget;
        m_transitionDropTarget.reset();

        // Must have at least one clip
        if (target.leftClipId == 0 && target.rightClipId == 0) {
            event->ignore();
            return;
        }

        emit transitionDroppedAtEdge(target.trackIndex, target.leftClipId,
                                     target.rightClipId, target.editPointTick,
                                     transType);
        event->acceptProposedAction();
        return;
    }

    // ── Effect drop (custom MIME type) ──────────────────────────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-effect")) {
        m_effectDropTarget.reset();
        for (auto* tw : m_trackWidgets) tw->clearEffectHighlight();

        QByteArray effectData = event->mimeData()->data("application/x-roundtable-effect");
        bool ok = false;
        int effectType = effectData.toInt(&ok);
        if (!ok) { event->ignore(); return; }

        // Hit-test to find which clip was dropped on
        QPointF pos = event->position();
        auto hitRef = hitTestClip(pos);
        if (!hitRef || !m_timeline) { event->ignore(); return; }

        auto* track = m_timeline->track(hitRef->trackIndex);
        if (!track) { event->ignore(); return; }

        size_t clipIdx = track->findClipIndexById(hitRef->clipId);
        if (clipIdx == SIZE_MAX) { event->ignore(); return; }

        emit effectDroppedOnClip(hitRef->trackIndex, hitRef->clipId, effectType);
        event->acceptProposedAction();
        return;
    }

    // ── Sequence drop (from project bin or Source Monitor) ─────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-sequence")) {
        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());
        bool aboveTopVideo = false;
        bool belowBottomAudio = false;
        computeGhostDropZones(pos, aboveTopVideo, belowBottomAudio);
        if ((ghostWasVisible && ghostWasAbove) || aboveTopVideo)
            trackIdx = kGhostDropTrackVideoAbove;

        bool ok = false;
        size_t seqIndex = event->mimeData()->data("application/x-roundtable-sequence")
                              .toULongLong(&ok);

        if (ok) {
            // Read source in/out if present (from Source Monitor drag-out)
            int64_t sourceIn  = -1;
            int64_t sourceOut = -1;
            if (event->mimeData()->hasFormat("application/x-roundtable-source-in"))
                sourceIn = event->mimeData()->data("application/x-roundtable-source-in").toLongLong();
            if (event->mimeData()->hasFormat("application/x-roundtable-source-out"))
                sourceOut = event->mimeData()->data("application/x-roundtable-source-out").toLongLong();

            // Snap
            int64_t dropDur = 0;
            if (sourceIn >= 0 && sourceOut > sourceIn) {
                dropDur = sourceOut - sourceIn;
            } else if (event->mimeData()->hasFormat("application/x-roundtable-sequence-duration")) {
                bool durOk = false;
                dropDur = event->mimeData()->data("application/x-roundtable-sequence-duration")
                              .toLongLong(&durOk);
                if (!durOk) dropDur = 0;
            }
            if (dropDur > 0) {
                auto snapRes = m_snapEngine.snapPair(tick, tick + dropDur);
                if (snapRes.didSnap) tick = snapRes.snappedTick;
            } else {
                auto snapRes = m_snapEngine.snap(tick);
                if (snapRes.didSnap) tick = snapRes.snappedTick;
            }

            emit sequenceDropped(seqIndex, tick, trackIdx, sourceIn, sourceOut);
        }
        event->acceptProposedAction();
        return;
    }

    // ── Media drop (custom MIME from MediaDragTreeWidget / ThumbnailGrid) ──
    if (event->mimeData()->hasFormat("application/x-roundtable-media")) {
        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        bool ok = false;
        uint64_t handle = event->mimeData()->data("application/x-roundtable-media")
                              .toULongLong(&ok);
        // Get file path from URLs
        QString filePath;
        if (event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty())
            filePath = event->mimeData()->urls().first().toLocalFile();

        bool isAudioDrop = false;
        if (ok && handle != 0 && m_mediaPool) {
            if (const auto* info = m_mediaPool->getInfo(handle))
                isAudioDrop = (info->videoStreamIndex < 0);
        }
        if (!isAudioDrop && !filePath.isEmpty()) {
            QString ext = QFileInfo(filePath).suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            isAudioDrop = audioExts.contains(ext);
        }
        bool aboveTopVideo = false;
        bool belowBottomAudio = false;
        computeGhostDropZones(pos, aboveTopVideo, belowBottomAudio);
        if (!isAudioDrop && ((ghostWasVisible && ghostWasAbove) || aboveTopVideo))
            trackIdx = kGhostDropTrackVideoAbove;
        else if (isAudioDrop && ((ghostWasVisible && !ghostWasAbove) || belowBottomAudio))
            trackIdx = kGhostDropTrackAudioBelow;

        // Check for source in/out points (from Source Monitor drag)
        int64_t sourceIn = -1;
        int64_t sourceOut = -1;
        if (event->mimeData()->hasFormat("application/x-roundtable-source-in"))
            sourceIn = event->mimeData()->data("application/x-roundtable-source-in").toLongLong();
        if (event->mimeData()->hasFormat("application/x-roundtable-source-out"))
            sourceOut = event->mimeData()->data("application/x-roundtable-source-out").toLongLong();

        // Snap the drop tick to existing clip edges (match preview)
        int64_t dropDur = 0;
        if (sourceIn >= 0 && sourceOut > sourceIn) {
            dropDur = sourceOut - sourceIn;
        } else if (ok && handle != 0 && m_mediaPool) {
            const auto* info = m_mediaPool->getInfo(handle);
            if (info && info->duration > 0.0)
                dropDur = static_cast<int64_t>(info->duration * 48000.0);
        }
        // Resolve via file path if handle had no info (same as preview)
        if (dropDur <= 0 && m_mediaPool && !filePath.isEmpty()) {
            auto h = m_mediaPool->open(filePath.toStdString());
            if (h != 0) {
                const auto* info = m_mediaPool->getInfo(h);
                if (info && info->duration > 0.0)
                    dropDur = static_cast<int64_t>(info->duration * 48000.0);
                if (!ok || handle == 0) handle = h;
            }
        }
        if (dropDur > 0) {
            auto snapRes = m_snapEngine.snapPair(tick, tick + dropDur);
            if (snapRes.didSnap) tick = snapRes.snappedTick;
        } else {
            auto snapRes = m_snapEngine.snap(tick);
            if (snapRes.didSnap) tick = snapRes.snappedTick;
        }

        if (ok && !filePath.isEmpty()) {
            if (sourceIn >= 0 && sourceOut > sourceIn)
                emit mediaDroppedWithRegion(filePath, handle, tick, trackIdx,
                                            sourceIn, sourceOut);
            else
                emit mediaDropped(filePath, handle, tick, trackIdx);
        }

        event->acceptProposedAction();
        return;
    }

    // ── External file drop (from Windows Explorer) ───────────────────────
    if (event->mimeData()->hasUrls() &&
        !event->mimeData()->hasFormat("application/x-roundtable-media") &&
        !qobject_cast<QTreeWidget*>(event->source())) {
        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        bool isAudioDrop = false;
        if (!event->mimeData()->urls().isEmpty()) {
            QString ext = QFileInfo(event->mimeData()->urls().first().toLocalFile())
                              .suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            isAudioDrop = audioExts.contains(ext);
        }
        bool aboveTopVideo = false;
        bool belowBottomAudio = false;
        computeGhostDropZones(pos, aboveTopVideo, belowBottomAudio);
        if (!isAudioDrop && ((ghostWasVisible && ghostWasAbove) || aboveTopVideo))
            trackIdx = kGhostDropTrackVideoAbove;
        else if (isAudioDrop && ((ghostWasVisible && !ghostWasAbove) || belowBottomAudio))
            trackIdx = kGhostDropTrackAudioBelow;

        auto snapRes = m_snapEngine.snap(tick);
        if (snapRes.didSnap) tick = snapRes.snappedTick;

        for (const QUrl& url : event->mimeData()->urls()) {
            QString localPath = url.toLocalFile();
            if (!localPath.isEmpty())
                emit externalFileDropped(localPath, tick, trackIdx);
        }

        event->acceptProposedAction();
        return;
    }

    // ── Media drop fallback (QTreeWidget default drag) ──────────────────
    auto* srcTree = qobject_cast<QTreeWidget*>(event->source());
    if (!srcTree) { event->ignore(); return; }

    auto selected = srcTree->selectedItems();
    if (selected.isEmpty()) { event->ignore(); return; }

    // Map drop position to timeline tick and track
    QPointF pos = event->position();
    double px = pos.x() - headerWidth();
    int64_t tick = m_layoutEngine.pixelXToTime(px);
    if (tick < 0) tick = 0;
    size_t trackIdx = hitTestTrack(pos.y());
    bool aboveTopVideo = false;
    bool belowBottomAudio = false;
    computeGhostDropZones(pos, aboveTopVideo, belowBottomAudio);

    // Snap the drop tick to clip edges
    {
        auto snapRes = m_snapEngine.snap(tick);
        if (snapRes.didSnap) tick = snapRes.snappedTick;
    }

    for (auto* item : selected) {
        // Skip bin (folder) items
        if (item->data(0, Qt::UserRole + 2).toBool()) continue;

        // Handle sequence items
        if (item->data(0, Qt::UserRole + 3).toBool()) {
            size_t seqIndex = item->data(0, Qt::UserRole + 4).toULongLong();
            size_t seqTrack = trackIdx;
            if ((ghostWasVisible && ghostWasAbove) || aboveTopVideo)
                seqTrack = kGhostDropTrackVideoAbove;
            emit sequenceDropped(seqIndex, tick, seqTrack);
            continue;
        }

        QString filePath = item->data(0, Qt::UserRole).toString();
        uint64_t handle  = item->data(0, Qt::UserRole + 1).toULongLong();
        if ((ghostWasVisible || aboveTopVideo || belowBottomAudio) && !filePath.isEmpty()) {
            QString ext = QFileInfo(filePath).suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            const bool isAudioDrop = audioExts.contains(ext);
            if (!isAudioDrop && ((ghostWasVisible && ghostWasAbove) || aboveTopVideo))
                trackIdx = kGhostDropTrackVideoAbove;
            else if (isAudioDrop && ((ghostWasVisible && !ghostWasAbove) || belowBottomAudio))
                trackIdx = kGhostDropTrackAudioBelow;
        }
        if (!filePath.isEmpty())
            emit mediaDropped(filePath, handle, tick, trackIdx);
    }

    event->acceptProposedAction();
}


} // namespace rt