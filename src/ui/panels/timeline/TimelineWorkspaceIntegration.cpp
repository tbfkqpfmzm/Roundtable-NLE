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
#include "timeline/VideoClip.h"
#include "timeline/ImageClip.h"
#include "timeline/AudioClip.h"

#include <QFileSystemWatcher>
#include <QTimer>

#include <filesystem>

#include <spdlog/spdlog.h>

#include "panels/monitors/SourceMonitor.h"

#include <unordered_set>

namespace rt {

namespace {
// (size, mtime-ticks) content signature for live-reload change detection.
// Returns {0,0} if the file can't be stat'd (treated as "changed" so a
// genuine first appearance still reloads).
std::pair<std::uintmax_t, std::int64_t> mediaFileSig(const std::string& p)
{
    std::error_code ec;
    std::uintmax_t sz = std::filesystem::file_size(p, ec);
    if (ec) sz = 0;
    std::error_code ec2;
    auto lwt = std::filesystem::last_write_time(p, ec2);
    std::int64_t mt = ec2 ? 0
        : static_cast<std::int64_t>(lwt.time_since_epoch().count());
    return {sz, mt};
}
} // namespace

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

        // Safe-mode callback wiring removed in P2 of CLAUDE_IMPROVEMENT_PLAN.
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

    // Arm the live file-swap watcher for the new project's media.
    if (timeline)
        rescanMediaWatch();
}

void TimelineWorkspace::invalidateCompositeCache()
{
    if (m_compositeService) m_compositeService->requestCacheInvalidation();
}

void TimelineWorkspace::invalidateCompositeCacheRange(int64_t fromTick, int64_t toTick)
{
    if (m_compositeService)
        m_compositeService->requestCacheInvalidationRange(fromTick, toTick);
}

void TimelineWorkspace::refreshChangedMedia(const std::filesystem::path& path)
{
    if (path.empty()) return;

    spdlog::warn("[LIVE-RELOAD] refreshChangedMedia '{}' — invalidate + "
                 "forget + recomposite", path.string());

    // 1. Force MediaPool to re-read the file: evicts CPU+disk cache and
    //    the stale-frame fallback, reopens the decoder in place. The handle
    //    is PRESERVED (returned here) so every consumer keeps working.
    MediaHandle changed = InvalidMedia;
    if (m_mediaPool) changed = m_mediaPool->invalidatePath(path);

    // 1b. Prime the cache synchronously so EVERY composite (the current
    //     playhead AND any tick the user scrubs to before the async decode
    //     finishes) immediately hits with new pixels.  Without this, a
    //     scrub to another part of the same clip in the ~50-150 ms after
    //     invalidate finds an empty cache, the layer build returns
    //     nullptr, the layer is skipped, and the output VkImage briefly
    //     retains whatever was last drawn (the "scrubs another part shows
    //     old briefly" symptom).  Blocking the GUI thread for one PNG
    //     decode is acceptable in exchange for instant-everywhere refresh.
    //     Also schedules an urgent Full-tier prefetch as a fallback if
    //     the blocking decode fails (e.g. file still mid-write).
    if (m_mediaPool && changed != InvalidMedia) {
        m_mediaPool->schedulePrefetch(changed, /*afterFrame=*/0,
                                      /*count=*/1, /*urgent=*/true,
                                      ResolutionTier::Full);
        (void)m_mediaPool->getFrame(changed, /*frame=*/0,
                                    ResolutionTier::Full,
                                    /*scrubMode=*/false);
    }

    // 2. Evict that media's GPU textures. GpuTextureCache is keyed by
    //    (mediaId, frame, tier); since the handle is preserved that key is
    //    unchanged and the compositor would otherwise keep drawing the
    //    stale uploaded texture even though MediaPool now decodes the new
    //    file (this was the "updates in Source Monitor but not the
    //    timeline when scrubbed" symptom).
    if (m_compositeService) {
        if (changed != InvalidMedia)
            m_compositeService->invalidateMediaTextures(changed);
        // Also drop the compositor's cached path→handle mapping (harmless
        // belt-and-suspenders now that the handle is preserved).
        m_compositeService->forgetMediaPath(path.string());
    }

    // 3. Flush composited output and refresh the visible monitor so every
    //    timeline instance shows the new content immediately.
    invalidateCompositeCache();
    if (m_timelinePanel) m_timelinePanel->rebuildTracks();
    if (m_programMonitor) m_programMonitor->requestRefresh();

    // 4. Deferred multi-shot refresh — the new file decode is async (the
    //    still is re-opened+decoded on the next getFrame), and the first
    //    refresh often races a still-in-progress write (perf_log shows
    //    'self-test FAILED GetLastError=32 — FILE_SHARE_DELETE not active'
    //    on the immediate reopen, then a clean reopen ~250ms later).  A
    //    single synchronous requestRefresh() composites BEFORE the new
    //    pixels exist, so the user sees blank until they scrub.  Schedule
    //    a few follow-up refreshes so the freshly-decoded frame is
    //    presented WITHOUT any user interaction.  Each shot also reloads
    //    the Source Monitor in place if it happens to be showing the
    //    changed media (otherwise the user has to re-double-click).
    auto kickRefresh = [this, changed](int shot) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        spdlog::warn("[LIVE-RELOAD] kick#{} firing — invalidating composite + "
                     "requestRefresh (handle={})", shot, changed);
        invalidateCompositeCache();
        if (m_programMonitor) m_programMonitor->requestRefresh();
        if (m_sourceMonitor && m_mediaPool && changed != InvalidMedia &&
            m_sourceMonitor->mediaHandle() == changed) {
            m_sourceMonitor->loadClip(changed, m_mediaPool);
        }
    };
    QTimer::singleShot(150,  this, [kickRefresh]() { kickRefresh(1); });
    QTimer::singleShot(450,  this, [kickRefresh]() { kickRefresh(2); });
    QTimer::singleShot(1000, this, [kickRefresh]() { kickRefresh(3); });

    // QFileSystemWatcher stops watching a path once the file is replaced
    // (Explorer's "overwrite" is a delete+create / rename). Re-arm so a
    // second swap of the same file is still detected.
    rescanMediaWatch();
}

void TimelineWorkspace::rescanMediaWatch()
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    if (!m_timeline) return;

    // Lazily create the watcher + its debounce timer on first use.
    if (!m_mediaWatcher) {
        m_mediaWatcher = new QFileSystemWatcher(this);
        m_mediaWatchDebounce = new QTimer(this);
        m_mediaWatchDebounce->setSingleShot(true);
        m_mediaWatchDebounce->setInterval(250);  // coalesce write bursts

        connect(m_mediaWatcher, &QFileSystemWatcher::fileChanged, this,
                [this](const QString& path) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            spdlog::warn("[LIVE-RELOAD] QFileSystemWatcher::fileChanged fired "
                         "for '{}'", path.toStdString());
            m_mediaWatchPending.insert(path.toStdString());
            m_mediaWatchDebounce->start();
        });

        connect(m_mediaWatchDebounce, &QTimer::timeout, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            auto pending = std::move(m_mediaWatchPending);
            m_mediaWatchPending.clear();
            for (const auto& p : pending) {
                std::error_code ec;
                // A still-missing file is mid-rewrite — requeue and wait.
                if (!std::filesystem::exists(p, ec)) {
                    spdlog::warn("[LIVE-RELOAD] debounce: '{}' missing "
                                 "(mid-rewrite) — requeue", p);
                    m_mediaWatchPending.insert(p);
                    continue;
                }
                // Content-change guard: QFileSystemWatcher fires on
                // attribute touches and Windows replays buffered events on
                // window restore. Only reload if (size, mtime) actually
                // changed — otherwise we'd needlessly invalidate the
                // composite cache and leave the Program Monitor blank.
                auto sig = mediaFileSig(p);
                auto prev = m_mediaWatchSig.find(p);
                if (prev != m_mediaWatchSig.end() && prev->second == sig) {
                    spdlog::warn("[LIVE-RELOAD] debounce: '{}' unchanged "
                                 "(size+mtime same) — skip (spurious event)",
                                 p);
                    continue;
                }
                m_mediaWatchSig[p] = sig;
                spdlog::warn("[LIVE-RELOAD] debounce: '{}' content changed — "
                             "refreshing", p);
                refreshChangedMedia(std::filesystem::path(p));
            }
            if (!m_mediaWatchPending.empty())
                m_mediaWatchDebounce->start();
        });
    }

    // Collect every distinct media file the timeline currently references.
    std::set<QString> want;
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        auto* track = m_timeline->track(ti);
        if (!track) continue;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* clip = track->clip(ci);
            if (!clip) continue;
            std::string mp;
            if      (auto* v = dynamic_cast<VideoClip*>(clip)) mp = v->mediaPath();
            else if (auto* i = dynamic_cast<ImageClip*>(clip)) mp = i->mediaPath();
            else if (auto* a = dynamic_cast<AudioClip*>(clip)) mp = a->mediaPath();
            if (mp.empty()) continue;
            std::error_code ec;
            if (std::filesystem::exists(mp, ec))
                want.insert(QString::fromStdString(mp));
        }
    }

    // Also watch every file the MediaPool actually has open. Clip-type
    // enumeration above misses non-Video/Image/Audio clips, nested
    // sequences, and clips opened by the shot-boundary prewarm before the
    // timeline-edit hooks run (this is exactly why STUCK.png was never
    // watched). The pool is the ground truth for "files the app touches".
    if (m_mediaPool) {
        for (const auto& p : m_mediaPool->openMediaPaths()) {
            std::error_code ec;
            if (!p.empty() && std::filesystem::exists(p, ec))
                want.insert(QString::fromStdString(p.string()));
        }
    }

    // Diff against the currently-watched set: drop stale, add new. (Re-adding
    // an already-watched path is a no-op in QFileSystemWatcher.)
    const QStringList watched = m_mediaWatcher->files();
    QStringList toRemove;
    for (const QString& w : watched)
        if (want.find(w) == want.end())
            toRemove << w;
    if (!toRemove.isEmpty())
        m_mediaWatcher->removePaths(toRemove);
    for (const QString& w : want)
        m_mediaWatcher->addPath(w);  // false == already watched (not an error)

    const QStringList nowWatched = m_mediaWatcher->files();

    // Seed the content signature for every watched path (only if absent —
    // never clobber a sig the debounce handler already advanced). Without a
    // baseline, the first (possibly spurious, post-restore) fileChanged
    // would be treated as a real change and blank the Program Monitor.
    std::set<std::string> watchedSet;
    for (const QString& w : nowWatched) {
        std::string s = w.toStdString();
        watchedSet.insert(s);
        m_mediaWatchSig.emplace(s, mediaFileSig(s));
    }
    // Prune signatures for paths no longer watched.
    for (auto it = m_mediaWatchSig.begin(); it != m_mediaWatchSig.end(); )
        it = (watchedSet.count(it->first) == 0)
                 ? m_mediaWatchSig.erase(it) : std::next(it);

    spdlog::warn("[LIVE-RELOAD] rescanMediaWatch: {} clip media paths, "
                 "{} now watched by QFileSystemWatcher",
                 want.size(), nowWatched.size());
    for (const QString& w : nowWatched)
        spdlog::warn("[LIVE-RELOAD]   watching: '{}'", w.toStdString());
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

    // Clips may have been added/removed/relinked — keep the live file-swap
    // watcher in sync with the timeline's current media set.
    rescanMediaWatch();

    schedulePostEditWork();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
