/*
 * CompositeServiceLayerBuildVideo.cpp - VideoClip layer building helper.
 * Extracted from CompositeServiceLayerBuild.cpp (Step 3.1 of modularization plan).
 *
 * Contains:
 *   - resolveMediaFrame()   — media frame resolution with cache-aware fetch
 *   - buildVideoClipLayer() — per-clip VideoClip layer construction
 *
 * These helpers are called from buildLayersForFrame() to reduce the size
 * of the main timeline traversal function.
 */

#include "CompositeService.h"
#include "CompositeServiceLayerBuildInternal.h"

#include "media/FrameCache.h"
#include "media/MediaPool.h"
#include "media/UnifiedCache.h"
#include "Constants.h"
#include "timeline/VideoClip.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include "CompositeEngine.h"
#include "GpuContext.h"
#include "GpuTextureCache.h"

namespace rt {

// ─────────────────────────────────────────────────────────────────────────────
// resolveMediaFrame — cache-aware media frame fetch.
//
// Replaces the inline fetchMediaFrame lambda from buildLayersForFrame().
// During export (forceFullRes), tries tryGetFrame first then falls back to
// blocking getFrame.  During playback, tries non-blocking tryGetFrame only
// (the prefetch workers fill the cache asynchronously).  During scrub,
// returns nullptr on cache miss to avoid blocking the compositor mutex.
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<CachedFrame> CompositeService::resolveMediaFrame(
    MediaHandle handle, int64_t frameNumber, ResolutionTier tier,
    bool scrubMode) const
{
    if (!m_mediaPool)
        return nullptr;

    // Phase B: declare a protected playback window for this media on each
    // resolve.  The window stays alive in FrameCache (via UnifiedCache
    // forwarding) so background eviction won't drop frames the user is
    // about to scrub onto.  Values chosen to match the existing prefetch
    // scheduler defaults: 8 behind (small backwards-scrub buffer) and
    // 60 ahead (~2s at 30fps — covers typical seek-then-play patterns).
    if (m_unifiedCache) {
        m_unifiedCache->setPlayheadWindow(handle, frameNumber,
                                          /*aheadCount=*/60,
                                          /*behindCount=*/8,
                                          tier);
        m_unifiedCache->markAccess({handle, frameNumber, tier});
    }
    // During export (forceFullRes), first try the cache via tryGetFrame
    // — this sets the playhead + extends the interactive playback window
    // (unlike getFrame with scrubMode=true which skips both).  If the
    // exact frame IS cached, return it instantly.  Otherwise fall through
    // to blocking getFrame with scrubMode=true which does inline decode
    // via the scrub decoder (the !scrubMode path only returns stale
    // frames without decoding — useless for export).  The playhead is
    // already set by tryGetFrame, so the cache won't thrash-evict.
    // Also reject alternate-tier fallback frames (e.g. Half when Full
    // was requested) — export always wants the full-quality frame.
    if (m_forceFullResolution.load()) {
        auto frame = m_mediaPool->tryGetFrame(handle, frameNumber, tier);
        if (frame && frame->frameNumber == frameNumber && frame->tier == tier)
            return frame;
        return m_mediaPool->getFrame(handle, frameNumber, tier, true);
    }
    // Try non-blocking first (fast path for cached frames during playback).
    auto frame = m_mediaPool->tryGetFrame(handle, frameNumber, tier);
    if (frame) {
        // Tier mismatch (e.g. ResolutionTier::Half cached, Full requested).
        // The previous behavior was to REJECT the wrong-tier frame in
        // non-scrub mode (to avoid a briefly-blurry display) and return
        // nullptr, leaving the layer unrendered.  That had a fatal failure
        // mode during live file replacement: after invalidate, only a
        // single tier ends up in the cache before the paused composite
        // runs; if it's not Full, the layer is skipped, the output VkImage
        // retains the previous composite's pixels, and the user sees the
        // OLD media until they scrub (scrub mode bypassed this check).
        // Accept any-tier hit instead, and still schedule the correct-tier
        // prefetch so the next composite gets the sharper frame.  The
        // brief intermediate-tier moment is invisible for stills and
        // strictly better than displaying stale media.
        if (!scrubMode && frame->tier != tier) {
            m_mediaPool->schedulePrefetch(handle, frameNumber, 1, /*urgent=*/true, tier);
            // fall through and return the wrong-tier frame instead of nullptr
        }
        return frame;
    }
    // Cache miss path.
    //
    // Both scrub and playback fall through to MediaPool::getFrame, which
    // routes scrubMode requests through the dedicated NVDEC-backed scrub
    // decoder (lock-free relative to m_mutex). Cold scrub positions now
    // decode inline in ~5-15 ms (NVDEC) instead of waiting for the
    // prefetch worker to land — the previous "scrubMode → nullptr" guard
    // dated from when the scrub decoder was forced software (50-500 ms
    // per seek would freeze the compositor mutex; that risk is gone with
    // NVDEC). The compositor mutex is held during the call, but at NVDEC
    // speeds the hold is shorter than one 60fps tick, which is comparable
    // to a normal composite. Playback (scrubMode=false) still resolves
    // via the non-blocking path inside getFrame — it returns stale or
    // null without inline decoding, matching the "never stall the render
    // thread" design.
    return m_mediaPool->getFrame(handle, frameNumber, tier, scrubMode);
}

} // namespace rt
