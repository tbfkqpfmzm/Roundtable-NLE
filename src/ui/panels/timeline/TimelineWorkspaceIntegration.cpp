/*
 * TimelineWorkspaceIntegration.cpp — Timeline integration functions
 * extracted from TimelineWorkspace.cpp.
 *
 * Contains: setTimeline(), invalidateCompositeCache(), refreshAfterUndoRedo().
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Settings.h"

#include "panels/audio/AudioMixer.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/EffectsPanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/timeline/TimelinePanel.h"

#include "command/CommandStack.h"
#include "media/AudioPlaybackService.h"
#include "media/MediaPool.h"
#include "media/MediaSourceService.h"
#include "media/PlaybackController.h"
#include "spine/ModelManager.h"
#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"

#include <QTimer>

#include <spdlog/spdlog.h>

#include <unordered_set>

namespace rt {

void TimelineWorkspace::setTimeline(Timeline* timeline) {
    // Reset audio service state for the new timeline
    if (m_audioPlayback) {
        m_audioPlayback->cancelWarm();
        m_audioPlayback->waitForWarm();
        m_audioPlayback->reset();
        m_audioPlayback->setTimeline(timeline);
    }

    m_timeline = timeline;
    if (m_compositeService) {
        m_compositeService->setTimeline(timeline);
        m_compositeService->setMediaPool(m_mediaPool);
        m_compositeService->setModelManager(m_modelManager);
        m_compositeService->clearMediaHandles();
#ifdef ROUNDTABLE_HAS_SPINE
        m_compositeService->setSpineLoadScheduler(
            [this](const std::string& c, const std::string& o, int s, const std::string& a) {
                scheduleSpineSharedLoad(c, o, s, a);
            });
#endif

        // ── Phase 7.B: Wire safe mode callback ──────────────────────
        // When safe mode is entered or exited, update the ProgramMonitor's
        // safe mode banner and viewport state automatically.  This is the
        // seamless, invisible recovery mechanism — the user never needs to
        // click a "Refresh" button.
        m_compositeService->setSafeModeCallback(
            [this](bool safeModeActive) {
                if (m_programMonitor) {
                    m_programMonitor->setSafeModeBannerVisible(safeModeActive);
                }
                if (safeModeActive) {
                    spdlog::warn("[SAFEMODE] Safe mode ENTERED — using CPU fallback");
                } else {
                    spdlog::info("[SAFEMODE] Safe mode EXITED — GPU compositing restored");
                }
            });
    }

    // Forward to TimelinePanel so its track widgets and ensureDefaultTracks
    // operate on the correct timeline (e.g. after project open).
    // Also forward nullptr to clear the dangling reference when a project
    // is deleted while open, preventing use-after-free crashes.
    if (m_timelinePanel)
        m_timelinePanel->setTimeline(timeline);

    // Forward to PropertiesPanel so shot switching can find group clips.
    if (m_propertiesPanel)
        m_propertiesPanel->setTimeline(timeline);

    // Forward to EffectControlsPanel
    if (m_effectControlsPanel)
        m_effectControlsPanel->setTimeline(timeline);

    // Forward to AudioMixer so channel strips rebuild for the new project.
    if (m_audioMixer)
        m_audioMixer->setTimeline(timeline);

    // Forward to ProgramMonitor so its mini-timeline gets the correct
    // duration, in/out points, and playhead range.
    if (m_programMonitor) {
        m_programMonitor->setTimeline(timeline);

        if (timeline) {
            // Re-wire the composite callback — setCurrentProject() in
            // MainWindow calls setCompositeCallback(nullptr) during cleanup,
            // so we must re-establish it when a new timeline is set.
            m_programMonitor->setCompositeCallback(
                [this](int64_t tick, uint32_t w, uint32_t h, bool scrubMode)
                    -> std::shared_ptr<CachedFrame> {
                    return compositeFrame(tick, w, h, scrubMode);
                });

            // Re-start polling so the Program Monitor updates on every tick.
            m_programMonitor->startPolling();
        }
    }

#ifdef ROUNDTABLE_HAS_SPINE
    // Pre-warm the spine cache so first compositeFrame doesn't block on
    // disk I/O (skel parse + PNG decode).  ~100-200ms moved from first
    // render to project-open time where it's imperceptible.
    if (timeline)
        preloadSpineAssets();
#endif

    // Force an initial composite so the Program Monitor shows the frame
    // at the current playhead as soon as the project opens, rather than
    // waiting for the user to scrub or press play.
    if (m_programMonitor && timeline) {
        QTimer::singleShot(100, this, [this]() {
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
    }

    // Pre-warm the audio decode cache in a background thread so the first
    // play starts instantly instead of blocking on file I/O + resampling.
    if (timeline)
        warmAudioCacheAsync();

    // Pre-open video/image media handles so the first compositeFrame
    // doesn't block on decoder initialization + file probing.
    if (timeline)
        preOpenVideoMedia();
}

void TimelineWorkspace::invalidateCompositeCache()
{
    if (m_compositeService) m_compositeService->requestCacheInvalidation();
}

void TimelineWorkspace::refreshAfterUndoRedo()
{
    invalidateCompositeCache();

    // After undo/redo, clips may have been added or removed. Clear any
    // selected-clip / ShotPanel pointer that might now be dangling.
    if (m_selectedClip && m_timeline) {
        bool found = false;
        for (size_t ti = 0; ti < m_timeline->trackCount() && !found; ++ti) {
            auto* track = m_timeline->track(ti);
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                if (track->clip(ci) == m_selectedClip) { found = true; break; }
            }
        }
        if (!found) {
            m_selectedClip = nullptr;
            m_selectedGraphicLayerIdx = -1;
            if (m_propertiesPanel) m_propertiesPanel->clearClip();
            if (m_effectControlsPanel) m_effectControlsPanel->clearClip();
        }
    }

    // Rebuild the timeline track widgets so split/merged clips are visible.
    if (m_timelinePanel) m_timelinePanel->rebuildTracks();

#ifdef ROUNDTABLE_HAS_SPINE
    // Purge spine cache entries for clip IDs that no longer exist on the
    // timeline (prevents use-after-free when compositing references a
    // deleted clip's spine state).
    if (m_compositeService && m_timeline) {
        std::unordered_set<uint64_t> liveIds;
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            auto* track = m_timeline->track(ti);
            if (!track) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci)
                liveIds.insert(track->clip(ci)->id());
        }
        m_compositeService->purgeDeadSpineStates(liveIds);
    }
#endif

    // If selected clip still exists, refresh property panels to reflect undo/redo
    if (m_selectedClip) {
        if (m_propertiesPanel) m_propertiesPanel->refreshEffects();
        if (m_effectControlsPanel) m_effectControlsPanel->refresh();
    } else if (m_effectControlsPanel && m_effectControlsPanel->clip()) {
        // The panel still has a clip bound (e.g. selection was transiently
        // cleared by rebuildTracks) — refresh it so keyframe display is current.
        m_effectControlsPanel->refresh();
    }

    updateTransformOverlay();
    if (m_programMonitor) {
        m_programMonitor->requestRefresh();
        // Also reset view state as a defense-in-depth measure so that
        // undo/redo (which may have changed dock layout indirectly) does
        // not leave the Program Monitor showing stale/corrupted content.
        m_programMonitor->resetViewState();
    }
    schedulePostEditWork();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
