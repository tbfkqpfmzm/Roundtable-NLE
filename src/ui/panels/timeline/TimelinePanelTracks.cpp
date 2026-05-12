/*
 * TimelinePanelTracks.cpp — Track management for TimelinePanel.
 * Split from TimelinePanel.cpp for maintainability.
 *
 * Contains: rebuildTracks(), ensureDefaultTracks(), refreshTrackContents(),
 *           clearGapSelection(), notifyZoomChanged(), setAnimVideoCache().
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"
#include "widgets/TimelineRuler.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "command/CommandStack.h"
#include "command/commands/TrackCommands.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QPointer>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  setTimeline — Set the timeline data model
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::setTimeline(Timeline* timeline)
{
    ++m_waveformLoadGeneration;
    m_pendingWaveformPaths.clear();
    m_failedWaveformPaths.clear();

    // Clear clip selection when switching to a new timeline so stale
    // clip IDs from the previous project don't persist in m_selection.
    m_selection.clear();
    m_timeline = timeline;
    if (timeline)
    {
        // Ensure at least 1 video track and 1 audio track exist (Premiere Pro default)
        ensureDefaultTracks();

        m_layoutEngine.setTotalDuration(timeline->duration());
        // Reset scroll so stale position from a previous timeline doesn't
        // persist if the deferred zoomToFit below bails out.
        m_layoutEngine.setScrollX(0);

        // Pass markers to ruler for rendering
        m_ruler->setMarkers(&timeline->markers());

        // Deferred initial zoom: set a sensible default view showing ~1 minute.
        // Uses zoomToFit capped at 60 seconds so very long timelines don't
        // start extremely zoomed out.  The user can still manually zoom or
        // use View > Zoom to Fit (which calls zoomToFit() uncapped).
        QTimer::singleShot(0, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timeline) {
                double w = m_ruler->width();
                if (w <= 0) w = width() - headerWidth();
                if (w <= 0) w = 800;
                m_layoutEngine.setViewportWidth(w);

                int64_t dur = m_timeline->duration();
                constexpr int64_t kMaxInitialView = 60; // seconds
                int64_t maxTicks = TimelineLayoutEngine::secondsToTicks(kMaxInitialView);
                if (dur <= 0 || dur > maxTicks) dur = maxTicks;
                m_layoutEngine.zoomToFit(0, dur, w);
                onScrollChanged();

                // Scroll to the saved playhead position so the timeline
                // viewport matches where the user left off (e.g. after
                // crash recovery / project re-open).  Without this, the
                // timeline always opens at scroll=0 regardless of the
                // playhead position stored in the project file.
                setPlayheadPosition(m_timeline->playheadPosition());
            }
        });
    }
    rebuildTracks();
}

// ═════════════════════════════════════════════════════════════════════════════
//  rebuildTracks — Full tear-down and re-create of all track widgets
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::rebuildTracks()
{
    // Disable repaints during the full rebuild to prevent paint → layout →
    // repaint recursion and use-after-free when old widgets are deleted
    // while paint events are still pending.
    setUpdatesEnabled(false);

    spdlog::info("[TimelinePanel] ENTER rebuildTracks: m_timeline={} trackCount={}", (void*)m_timeline, m_timeline ? m_timeline->trackCount() : 0);
    spdlog::info("rebuildTracks: disconnecting and deleting old widgets");
    for (size_t i = 0; i < m_trackHeaders.size(); ++i) {
        auto header = m_trackHeaders[i];
        TrackHeader* rawHeader = header;
        if (rawHeader) {
            spdlog::info("[LIFECYCLE] About to disable/disconnect TrackHeader {} at {} (type={})", (void*)rawHeader, i, rawHeader ? rawHeader->metaObject()->className() : "null");
            rawHeader->setEnabled(false);
            bool disconnected = rawHeader->disconnect();
            spdlog::info("[LIFECYCLE] TrackHeader {} disconnect() returned {}", (void*)rawHeader, disconnected);
        }
    }
    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        auto widget = m_trackWidgets[i];
        TimelineTrackWidget* rawWidget = widget;
        if (rawWidget) {
            spdlog::info("[LIFECYCLE] About to disable/disconnect TrackWidget {} at {} (type={})", (void*)rawWidget, i, rawWidget ? rawWidget->metaObject()->className() : "null");
            rawWidget->setEnabled(false);
            bool disconnected = rawWidget->disconnect();
            spdlog::info("[LIFECYCLE] TrackWidget {} disconnect() returned {}", (void*)rawWidget, disconnected);
        }
    }
    // NOTE: Do NOT call processEvents() here.  During loading, pending
    // paint/signal events could be delivered to disabled/disconnected
    // widgets and trigger re-entrant calls into the partially torn-down
    // timeline, causing use-after-free crashes.
    // Hide old widgets BEFORE scheduling deletion.  deleteLater() defers
    // destruction to the next event-loop iteration, but paint events can
    // still fire in the meantime.  If the Track object's storage was
    // reallocated (e.g. tracks added/removed from the timeline), the old
    // widget's m_track raw pointer is dangling — accessing it in a paint
    // event causes ACCESS_VIOLATION.  Hiding the widget prevents paint.
    for (size_t i = 0; i < m_trackHeaders.size(); ++i) {
        auto header = m_trackHeaders[i];
        TrackHeader* rawHeader = header;
        if (rawHeader) {
            rawHeader->hide();
            spdlog::info("[LIFECYCLE] Hidden TrackHeader {} at {}", (void*)rawHeader, i);
        }
    }
    for (size_t i = 0; i < m_trackHeaders.size(); ++i) {
        auto header = m_trackHeaders[i];
        TrackHeader* rawHeader = header;
        if (rawHeader) {
            rawHeader->deleteLater();
            spdlog::info("[LIFECYCLE] Scheduled deleteLater for TrackHeader {} at {}", (void*)rawHeader, i);
        }
    }
    m_trackHeaders.clear();
    spdlog::info("[STEP] Finished deleting all TrackHeaders");

    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        auto widget = m_trackWidgets[i];
        TimelineTrackWidget* rawWidget = widget;
        if (rawWidget) {
            rawWidget->hide();
            spdlog::info("[LIFECYCLE] Hidden TrackWidget {} at {}", (void*)rawWidget, i);
        }
    }
    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        auto widget = m_trackWidgets[i];
        TimelineTrackWidget* rawWidget = widget;
        if (rawWidget) {
            rawWidget->deleteLater();
            spdlog::info("[LIFECYCLE] Scheduled deleteLater for TrackWidget {} at {}", (void*)rawWidget, i);
        }
    }
    m_trackWidgets.clear();
    spdlog::info("[STEP] Finished deleting all TrackWidgets");

    spdlog::info("rebuildTracks: creating new widgets for {} tracks", m_timeline ? m_timeline->trackCount() : 0);

    if (!m_timeline) return;

    // ── Auto-rename tracks with Premiere Pro-style numbering ──────────────
    // Video tracks: V1 = lowest (bottom of video section), V2 above, etc.
    // Audio tracks: A1 = highest (top of audio section), A2 below, etc.
    {
        // Collect indices for each type
        std::vector<size_t> videoIndices, audioIndices;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            Track* t = m_timeline->track(i);
            if (t->isDivider()) continue; // Dividers have no name
            if (t->type() == TrackType::Video) videoIndices.push_back(i);
            else                                audioIndices.push_back(i);
        }

        // Video: topmost (lowest index) = highest number
        // e.g., 3 video tracks at indices 0,1,2 → V3, V2, V1
        // Always auto-rename so that old-style names like "Video 1" are
        // normalised to the short V#/A# scheme used everywhere else.
        for (size_t vi = 0; vi < videoIndices.size(); ++vi) {
            Track* t = m_timeline->track(videoIndices[vi]);
            int num = static_cast<int>(videoIndices.size() - vi);  // bottom-up
            t->setName("V" + std::to_string(num));
        }

        // Audio: topmost = A1, next = A2, etc.
        // Preserve custom track names (e.g. per-character names from AudioSync export).
        for (size_t ai = 0; ai < audioIndices.size(); ++ai) {
            Track* t = m_timeline->track(audioIndices[ai]);
            const std::string& cur = t->name();
            // Only auto-rename if name is empty or already an auto-name (A1, A2, ...)
            bool isAutoName = cur.empty()
                || (cur.size() >= 2 && cur[0] == 'A'
                    && std::all_of(cur.begin() + 1, cur.end(), ::isdigit));
            if (isAutoName) {
                int num = static_cast<int>(ai + 1);
                t->setName("A" + std::to_string(num));
            }
        }
    }

    auto* headerLayout = m_trackHeaderArea->layout();
    auto* trackLayout  = m_trackContentArea->layout();

    // Remove old items from layouts (headers, track widgets, stretches)
    while (headerLayout->count() > 0)
        headerLayout->takeAt(0);
    while (trackLayout->count() > 0)
        trackLayout->takeAt(0);
    spdlog::info("[STEP] Cleared layouts");

    // Vertically center tracks in both scroll areas (Premiere Pro style).
    // Both scroll areas are the same height (right column has a 24px spacer
    // matching the Add Track button) so stretches expand identically.
    static_cast<QVBoxLayout*>(headerLayout)->addStretch();
    static_cast<QVBoxLayout*>(trackLayout)->addStretch();
    spdlog::info("[STEP] Added stretches to layouts");

    // Determine the current "standard" track height so dividers can size
    // themselves as 1/4 of whatever the user's real tracks are right now.
    // Use the first non-divider track as reference; fall back to 80.
    float refTrackHeight = 80.0f;
    for (size_t ri = 0; ri < m_timeline->trackCount(); ++ri) {
        Track* t = m_timeline->track(ri);
        if (!t || t->isDivider()) continue;
        float rh = t->height();
        if (rh >= 1.0f) { refTrackHeight = rh; break; }
    }
    const float dividerHeight = std::max(8.0f, refTrackHeight * 0.25f);

    spdlog::info("[STEP] About to create headers and widgets for each track");
    for (size_t i = 0; i < m_timeline->trackCount(); ++i)
    {
        Track* track = m_timeline->track(i);
        float h = track->height();
        if (track->isDivider()) {
            h = dividerHeight;
            track->setHeight(h);
        } else if (h < 1.0f) {
            h = 80.0f;
            track->setHeight(h);
        }

        // Header
        spdlog::info("[LIFECYCLE] About to create TrackHeader for track {} (type={})", i, (track ? (int)track->type() : -1));
        auto* header = new TrackHeader(m_trackHeaderArea);
        spdlog::info("[LIFECYCLE] Created TrackHeader {} for track {} (type={})", (void*)header, i, (track ? (int)track->type() : -1));
        spdlog::info("[STEP] TrackHeader created and added to layout for track {}", i);
        header->setTrack(track, i);
        header->setHeight(h);
        header->setFixedWidth(m_headerScroll->width());
        header->setMouseTracking(true);  // Enable hover cursor changes
        headerLayout->addWidget(header);
        m_trackHeaders.push_back(header);

        // Track content
        spdlog::info("[LIFECYCLE] About to create TimelineTrackWidget for track {} (type={})", i, (track ? (int)track->type() : -1));
        auto* tw = new TimelineTrackWidget(m_trackContentArea);
        spdlog::info("[LIFECYCLE] Created TimelineTrackWidget {} for track {} (type={})", (void*)tw, i, (track ? (int)track->type() : -1));
        spdlog::info("[STEP] TimelineTrackWidget created and added to layout for track {}", i);
        tw->setLayoutEngine(&m_layoutEngine);
        tw->setTrack(track, i);
        tw->setFixedHeight(static_cast<int>(h));
        trackLayout->addWidget(tw);
        m_trackWidgets.push_back(tw);

        // Connect track height resize from header drag
        QObject::connect(header, &TrackHeader::heightChanged,
            this, [this](size_t trackIdx, float newHeight) {
            spdlog::debug("[SIGNAL] TrackHeader {} heightChanged for track {} newHeight={}", (void*)sender(), trackIdx, newHeight);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            Track* tk = m_timeline->track(trackIdx);
            tk->setHeight(newHeight);
            // Also resize the corresponding track widget
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(newHeight));
            // If a real (non-divider) track was resized, propagate the new
            // 1/4-height to every divider so they stay proportional.
            if (!tk->isDivider()) {
                float dh = std::max(8.0f, newHeight * 0.25f);
                for (size_t di = 0; di < m_timeline->trackCount(); ++di) {
                    Track* dt = m_timeline->track(di);
                    if (!dt || !dt->isDivider()) continue;
                    dt->setHeight(dh);
                    if (di < m_trackHeaders.size() && m_trackHeaders[di])
                        m_trackHeaders[di]->setHeight(dh);
                    if (di < m_trackWidgets.size() && m_trackWidgets[di])
                        m_trackWidgets[di]->setFixedHeight(static_cast<int>(dh));
                }
            }
            updateMinHeaderWidth();
        });

        // Connect track targeting toggle
        QObject::connect(header, &TrackHeader::targetToggled,
            this, [this](size_t trackIdx, bool targeted) {
            spdlog::debug("[SIGNAL] TrackHeader {} targetToggled for track {} targeted={}", (void*)sender(), trackIdx, targeted);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setTargeted(targeted);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
        });

        // Connect lock/mute/solo/syncLock toggles
        QObject::connect(header, &TrackHeader::lockToggled,
            this, [this](size_t trackIdx, bool locked) {
            spdlog::debug("[SIGNAL] TrackHeader {} lockToggled for track {} locked={}", (void*)sender(), trackIdx, locked);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setLocked(locked);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->update();
        });
        QObject::connect(header, &TrackHeader::muteToggled,
            this, [this](size_t trackIdx, bool muted) {
            spdlog::debug("[SIGNAL] TrackHeader {} muteToggled for track {} muted={}", (void*)sender(), trackIdx, muted);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setMuted(muted);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            emit contentChanged();
        });
        QObject::connect(header, &TrackHeader::soloToggled,
            this, [this](size_t trackIdx, bool soloed) {
            spdlog::debug("[SIGNAL] TrackHeader {} soloToggled for track {} soloed={}", (void*)sender(), trackIdx, soloed);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setSoloed(soloed);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            emit contentChanged();
        });
        QObject::connect(header, &TrackHeader::syncLockToggled,
            this, [this](size_t trackIdx, bool syncLocked) {
            spdlog::debug("[SIGNAL] TrackHeader {} syncLockToggled for track {} syncLocked={}", (void*)sender(), trackIdx, syncLocked);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setSyncLocked(syncLocked);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->update();
        });

        // Connect track collapse/expand toggle
        QObject::connect(header, &TrackHeader::collapseToggled,
            this, [this](size_t trackIdx, bool collapsed) {
            spdlog::debug("[SIGNAL] TrackHeader {} collapseToggled for track {} collapsed={}", (void*)sender(), trackIdx, collapsed);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            static constexpr float kCollapsedHeight = 20.0f;
            Track* t = m_timeline->track(trackIdx);
            t->setCollapsed(collapsed);
            float newH = collapsed ? kCollapsedHeight : 60.0f;
            t->setHeight(newH);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->setHeight(newH);
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(newH));
            updateMinHeaderWidth();
        });

        // Connect track management signals from header context menu
        QObject::connect(header, &TrackHeader::addTrackRequested,
            this, [this](bool video, bool above, size_t nearIndex) {
            spdlog::debug("[SIGNAL] TrackHeader {} addTrackRequested video={} above={} nearIndex={}", (void*)sender(), video, above, nearIndex);
            if (above)
                emit addTrackAbove(nearIndex, video);
            else
                emit addTrackBelow(nearIndex, video);
        });
        QObject::connect(header, &TrackHeader::deleteTrackRequested,
                this, [this](size_t trackIdx) {
            spdlog::debug("[SIGNAL] TrackHeader {} deleteTrackRequested for track {} (deferred)", (void*)sender(), trackIdx);
            QPointer<TimelinePanel> safeThis(this);
            QTimer::singleShot(0, this, [safeThis, trackIdx]() {
                if (!safeThis) {
                    spdlog::error("[DEFENSIVE] TimelinePanel was deleted before deferred deleteTrack could run!");
                    return;
                }
                spdlog::debug("[DEFENSIVE] Emitting deleteTrack for track {} after deferral", trackIdx);
                emit safeThis->deleteTrack(trackIdx);
            });
        });

        // Insert a divider track relative to this header's track.
        // Defer so we don't delete the TrackHeader that's still running
        // its contextMenu handler on the stack (use-after-free crash).
        QObject::connect(header, &TrackHeader::addDividerRequested,
                this, [this](bool above, size_t nearIdx) {
            spdlog::debug("[SIGNAL] TrackHeader {} addDividerRequested above={} nearIdx={}", (void*)sender(), above, nearIdx);
            if (!m_timeline) return;
            size_t insertAt = above ? nearIdx : nearIdx + 1;
            if (insertAt > m_timeline->trackCount())
                insertAt = m_timeline->trackCount();
            QPointer<TimelinePanel> safeThis(this);
            QTimer::singleShot(0, this, [safeThis, insertAt]() {
                if (!safeThis || !safeThis->m_timeline) {
                    spdlog::error("[DEFENSIVE] TimelinePanel or m_timeline was deleted before deferred addDivider could run!");
                    return;
                }
                spdlog::debug("[DEFENSIVE] Adding divider track at {} after deferral", insertAt);
                safeThis->m_timeline->addDividerTrack(insertAt);
                safeThis->rebuildTracks();
            });
        });

        // Drag-reorder signal wiring
        QObject::connect(header, &TrackHeader::reorderDragStarted,
            this, [this](size_t srcIdx) {
            spdlog::debug("[SIGNAL] TrackHeader {} reorderDragStarted srcIdx={}", (void*)sender(), srcIdx);
            m_reorderSrcIndex = srcIdx;
            if (m_ghostOverlay) {
                m_ghostOverlay->reorderMode = true;
                m_ghostOverlay->raise();
                m_ghostOverlay->show();
            }
        });
        QObject::connect(header, &TrackHeader::reorderDragMoved,
            this, [this](size_t /*srcIdx*/, const QPoint& gp) {
            spdlog::debug("[SIGNAL] TrackHeader {} reorderDragMoved", (void*)sender());
            updateReorderOverlay(gp);
        });
        QObject::connect(header, &TrackHeader::reorderDragFinished,
            this, [this](size_t srcIdx, const QPoint& gp, bool commit) {
            spdlog::debug("[SIGNAL] TrackHeader {} reorderDragFinished srcIdx={} commit={}", (void*)sender(), srcIdx, commit);
            size_t dst = computeReorderInsertionIndex(gp);
            if (m_ghostOverlay) {
                m_ghostOverlay->reorderMode = false;
                m_ghostOverlay->hide();
            }
            m_reorderSrcIndex = SIZE_MAX;
            if (!commit || !m_timeline) return;
            if (dst > srcIdx) --dst; // compensate for removal shift
            if (dst == srcIdx) return;
            if (srcIdx >= m_timeline->trackCount()) return;
            if (dst >= m_timeline->trackCount())
                dst = m_timeline->trackCount() - 1;
            // Defer past the mouseReleaseEvent so we don't delete the
            // TrackHeader that is currently on the stack.
            QTimer::singleShot(0, this, [this, srcIdx, dst]() {
                if (m_destroying.load(std::memory_order_acquire)) return;
                if (!m_timeline) return;
                m_timeline->moveTrack(srcIdx, dst);
                rebuildTracks();
            });
        });
        QObject::connect(header, &TrackHeader::trackSizePresetRequested,
            this, [this](size_t trackIdx, float height) {
            spdlog::debug("[SIGNAL] TrackHeader {} trackSizePresetRequested trackIdx={} height={}", (void*)sender(), trackIdx, height);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setHeight(height);
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(height));
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->setHeight(height);
            updateMinHeaderWidth();
        });

        // Connect track rename (undoable via command stack)
        QObject::connect(header, &TrackHeader::trackRenamed,
            this, [this](size_t trackIdx, const QString& newName) {
            spdlog::debug("[SIGNAL] TrackHeader {} trackRenamed trackIdx={} newName={}", (void*)sender(), trackIdx, newName.toStdString());
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            Track* track = m_timeline->track(trackIdx);
            auto cmd = std::make_unique<SetTrackPropertyCommand<std::string>>(
                track, newName.toStdString(),
                +[](const Track& t) -> std::string { return t.name(); },
                +[](Track& t, std::string v) { t.setName(v); },
                "Rename Track",
                static_cast<int>(CommandTypeId::SetTrackName));
            if (m_commandStack) {
                m_commandStack->execute(std::move(cmd));
            } else {
                cmd->execute();
            }
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            updateMinHeaderWidth();
        });

        // Connect clip selection
        QObject::connect(tw, &TimelineTrackWidget::clipClicked,
            this, [this](size_t trackIndex, size_t clipIndex, bool shiftHeld) {
            spdlog::debug("[SIGNAL] TimelineTrackWidget {} clipClicked trackIndex={} clipIndex={} shiftHeld={}", (void*)sender(), trackIndex, clipIndex, shiftHeld);
            // Update SelectionSet (this was previously just bounced to
            // clipSelected signal without updating m_selection)
            if (!m_timeline) return;
            Track* trk = m_timeline->track(trackIndex);
            if (!trk || clipIndex >= trk->clipCount()) return;
            const Clip* clip = trk->clip(clipIndex);
            if (!clip) return;

            ClipRef ref{trackIndex, clip->id()};
            if (!shiftHeld && !m_selection.isSelected(ref))
                m_selection.clear();
            m_selection.selectClip(ref, shiftHeld);

            // Linked A/V selection: if the clip has a groupId,
            // also select all clips with the same groupId across all tracks.
            uint64_t gid = clip->groupId();
            if (gid != 0) {
                for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                    Track* t = m_timeline->track(ti);
                    for (size_t ci = 0; ci < t->clipCount(); ++ci) {
                        const Clip* c = t->clip(ci);
                        if (c->groupId() == gid && c->id() != clip->id()) {
                            m_selection.selectClip({ti, c->id()}, true);
                            break; // one match per track for linked A/V
                        }
                    }
                }
            }

            emit selectionChanged();
            emit clipSelected(trackIndex, clipIndex);
            update();
        });

        // Install event filter so TimelinePanel gets mouse events for
        // drag/move/trim/marquee even though TimelineTrackWidget is the
        // deepest child that Qt delivers events to.
        tw->installEventFilter(this);
    }

    // Bottom stretch to balance vertical centering.
    static_cast<QVBoxLayout*>(headerLayout)->addStretch();
    static_cast<QVBoxLayout*>(trackLayout)->addStretch();

    spdlog::info("rebuildTracks: created {} headers, {} widgets for {} tracks",
                 m_trackHeaders.size(), m_trackWidgets.size(),
                 m_timeline->trackCount());

    // Diagnostic: log each widget's state
    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        auto tw = m_trackWidgets[i];
        auto* track = m_timeline->track(i);
        spdlog::info("  widget[{}]: track='{}' size={}x{} visible={} "
                     "fixedH={} clips={}",
                     i, track->name(),
                     tw->width(), tw->height(),
                     tw->isVisible(),
                     tw->minimumHeight(),
                     track->clipCount());
    }

    // Scroll so the video/audio boundary is vertically centered.
    // This ensures all video tracks and the top audio track are visible.
    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_verticalScroll || !m_timeline) return;
        // Find the pixel position of the first audio track (= bottom of video section)
        int videoBottom = 0;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            if (m_timeline->track(i)->type() == TrackType::Audio) break;
            videoBottom += static_cast<int>(m_timeline->track(i)->height());
        }
        // Account for the top stretch: in a centered layout the stretch
        // pushes content down. After layout, widget positions are final.
        // Scroll so that the video/audio boundary sits at ~40% from top.
        int viewH = m_verticalScroll->viewport()->height();
        int contentH = m_trackContentArea->sizeHint().height();
        if (contentH > viewH) {
            // Content overflows — scroll so video/audio boundary is centered
            int target = videoBottom - viewH * 2 / 5;
            if (target < 0) target = 0;
            m_verticalScroll->verticalScrollBar()->setValue(target);
        }
    });

    // Load waveform peaks for audio clips and pass to track widgets
    loadWaveforms();
    for (auto tw : m_trackWidgets)
        tw->setWaveformCache(&m_waveformPeaks);

    // Load video thumbnails and pass to track widgets
    loadThumbnails();
    for (auto tw : m_trackWidgets)
        tw->setThumbnailCache(&m_thumbnailCache);

    // Pass animation video cache to track widgets
    for (auto tw : m_trackWidgets)
        tw->setAnimVideoCache(m_animVideoCache);

    // Push in/out point overlays to track widgets
    if (m_timeline) {
        int64_t inPt  = m_timeline->inPoint();
        int64_t outPt = m_timeline->outPoint();
        for (auto tw : m_trackWidgets)
            tw->setInOutPoints(inPt, outPt);
    }

    // Re-apply visual selection to the newly created track widgets.
    // m_selection persists across rebuilds but the widget highlight state
    // is lost when old widgets are deleted.
    emit selectionChanged();

    // Update minimum header width based on current track names.
    // Run twice: once now (before first paint) and once deferred (after Qt's
    // layout pass settles widget widths). This ensures newly created track
    // headers reposition their label/buttons based on actual name length and
    // height — matching the behavior existing tracks get on resize.
    updateMinHeaderWidth();
    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        updateMinHeaderWidth();
        for (auto hdr : m_trackHeaders)
            if (hdr) hdr->update();
    });

    // Re-enable repaints — forces one consolidated layout + paint.
    setUpdatesEnabled(true);
    updateGeometry();
    repaint();
}

// ═════════════════════════════════════════════════════════════════════════════
//  ensureDefaultTracks
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::ensureDefaultTracks()
{
    if (!m_timeline) return;

    // Count existing video and audio tracks
    bool hasVideo = false;
    bool hasAudio = false;
    for (size_t i = 0; i < m_timeline->trackCount(); ++i)
    {
        const Track* t = m_timeline->track(i);
        if (t->type() == TrackType::Video) hasVideo = true;
        if (t->type() == TrackType::Audio) hasAudio = true;
    }

    // Like Premiere Pro: video tracks go FIRST, then audio tracks.
    // Add V1 before any audio tracks, add A1 at the end.
    if (!hasVideo)
    {
        // Insert V1 at position 0 (top) if no video tracks exist yet
        size_t insertIdx = 0;
        auto vTrack = std::make_unique<Track>(TrackType::Video, "V1");
        m_timeline->insertTrack(insertIdx, std::move(vTrack));
    }

    if (!hasAudio)
        m_timeline->addAudioTrack("A1");
}

// ═════════════════════════════════════════════════════════════════════════════
//  refreshTrackContents — Lightweight content refresh, NO widget rebuild
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::refreshTrackContents()
{
    if (!m_timeline) return;

    // Incrementally update waveform/thumbnail caches for any new clip IDs.
    // With the path-based secondary caches this is essentially free for
    // splits (same source file → instant copy from m_waveformByPath /
    // m_thumbnailByPath).
    loadWaveforms();
    loadThumbnails();

    // Ensure all track widgets point to the latest caches
    // (pointers are stable but set them on any new widgets).
    for (auto tw : m_trackWidgets) {
        tw->setWaveformCache(&m_waveformPeaks);
        tw->setThumbnailCache(&m_thumbnailCache);
        tw->setAnimVideoCache(m_animVideoCache);
    }

    // Repaint all track widgets so the new clip geometry is visible.
    onScrollChanged();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Simple accessors / setters
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::clearGapSelection()
{
    m_gapSelection.active = false;
    for (auto tw : m_trackWidgets)
        tw->setGapHighlight(-1, -1);
}

void TimelinePanel::notifyZoomChanged()
{
    onScrollChanged();
}

void TimelinePanel::setAnimVideoCache(const AnimationVideoCache* cache)
{
    m_animVideoCache = cache;
    for (auto tw : m_trackWidgets)
        tw->setAnimVideoCache(cache);
}

} // namespace rt
