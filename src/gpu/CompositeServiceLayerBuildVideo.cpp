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
    // During export (forceFullRes), first try the cache via tryGetFrame
    // — this sets the playhead + extends the interactive playback window
    // (unlike getFrame with scrubMode=true which skips both).  If the
    // exact frame IS cached, return it instantly.  Otherwise fall through
    // to blocking getFrame with scrubMode=true which does inline decode
    // via the scrub decoder (the !scrubMode path only returns stale
    // frames without decoding — useless for export).  The playhead is
    // already set by tryGetFrame, so the cache won't thrash-evict.
    if (m_forceFullResolution.load()) {
        auto frame = m_mediaPool->tryGetFrame(handle, frameNumber, tier);
        if (frame && frame->frameNumber == frameNumber)
            return frame;
        return m_mediaPool->getFrame(handle, frameNumber, tier, true);
    }
    // Try non-blocking first (fast path for cached frames during playback).
    auto frame = m_mediaPool->tryGetFrame(handle, frameNumber, tier);
    if (frame)
        return frame;
    // During timeline scrub, avoid blocking decode (would hold the
    // compositor mutex for 50-500ms, causing the program monitor to
    // freeze on a stale m_lastGoodComposite).  The scrub settle loop
    // will re-try next cycle when the prefetch has landed the frame.
    if (scrubMode)
        return nullptr;
    // Fall back to blocking read (correct path for cold cache).
    return m_mediaPool->getFrame(handle, frameNumber, tier, scrubMode);
}

} // namespace rt
