/*
 * CompositeServiceCpuFallback.cpp — CPU Compositor Safe Mode (Phase 6).
 *
 * CONTEXT: The existing CPU compositor fallback in compositeFrame() is
 * explicitly tagged as "unusable" — it blends ALL layers via software,
 * which is extremely slow and produces unusable results.  Any GPU failure
 * is therefore fatal: the app cannot render frames.
 *
 * This file implements a MINIMAL safe-mode path that produces at most
 * one frame every 500ms for recovery purposes only.  It composites a
 * SINGLE track (the topmost visible video track) using MemCpy-based
 * blending, producing a BGRA bitmap suitable for QImage display on the
 * CPU QWidget viewport.
 *
 * IMPORTANT: This is NOT for real-time playback.  It's a recovery
 * feature — a bridge to get back to GPU operation.  The app should
 * never run in safe mode permanently.
 */

#include "CompositeService.h"
#include "Constants.h"
#include "GpuContext.h"
#include "media/FrameCache.h"
#include "media/MediaPool.h"
#include "media/MediaSourceService.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  CPU Safe Mode Compositing
// ═════════════════════════════════════════════════════════════════════════════
//
// Produces ONE frame at most every 500ms.  Composites only the topmost
// visible video track.  Uses blocking software decode + MemCpy blend.
// The result is a BGRA CachedFrame that can be displayed as a QImage in
// the CPU Viewport (index 0 of ProgramMonitor's QStackedLayout).
//
// Return value:  shared_ptr<CachedFrame> with CPU pixel data, or nullptr
//                if no video content is available.
//                width==0 means "empty frame" (clear display).
//
// ═════════════════════════════════════════════════════════════════════════════

std::shared_ptr<CachedFrame> CompositeService::compositeSafeMode(
    int64_t tick, uint32_t outW, uint32_t outH)
{
    // ── Throttle: at most one composite every 500ms ────────────────
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (m_lastSafeModeComposite != steady_clock::time_point{} &&
        now - m_lastSafeModeComposite < milliseconds(500))
    {
        // Too soon — return nullptr so the caller can show the last
        // safe-mode frame (if any) or a blank frame.
        return nullptr;
    }

    if (!m_timeline || !m_mediaPool || outW == 0 || outH == 0)
        return nullptr;

    // ── Guard: compositeSafeMode is NOT re-entrant ─────────────────
    static thread_local bool s_inSafeMode = false;
    if (s_inSafeMode)
        return nullptr;
    s_inSafeMode = true;
    // RAII guard ensures flag is cleared on exit
    struct Guard { ~Guard() { s_inSafeMode = false; } } guard;

    m_lastSafeModeComposite = now;

    // ── Step 1: Find the topmost visible video track ───────────────
    // Iterate tracks from lowest index (topmost in timeline UI = V3)
    // since we only want ONE track.  Take the first visible video track.
    Track* targetTrack = nullptr;
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        auto* track = m_timeline->track(ti);
        if (track && track->type() == TrackType::Video && !track->isMuted()) {
            targetTrack = track;
            break;
        }
    }

    if (!targetTrack) {
        spdlog::warn("[SAFEMODE] No visible video track at tick={}", tick);
        auto emptyFrame = std::make_shared<CachedFrame>();
        emptyFrame->width  = 0;
        emptyFrame->height = 0;
        return emptyFrame;
    }

    // ── Step 2: Find the clip at this tick on that track ───────────
    auto activeClips = targetTrack->clipsAtTime(tick);
    VideoClip* targetClip = nullptr;
    for (Clip* clip : activeClips) {
        if (clip && clip->isEnabled()) {
            targetClip = dynamic_cast<VideoClip*>(clip);
            if (targetClip)
                break;
        }
    }

    if (!targetClip) {
        spdlog::warn("[SAFEMODE] No enabled video clip at tick={}",
                     tick);
        auto emptyFrame = std::make_shared<CachedFrame>();
        emptyFrame->width  = 0;
        emptyFrame->height = 0;
        return emptyFrame;
    }

    // ── Step 3: Decode one frame (blocking, software decode) ──────
    const auto& mediaPath = targetClip->mediaPath();
    if (mediaPath.empty()) {
        spdlog::warn("[SAFEMODE] Clip has empty media path");
        return nullptr;
    }

    // Open the media (may block briefly — acceptable at 2 fps)
    uint64_t handle = 0;
    if (m_mediaPool->isPathOpen(mediaPath)) {
        handle = m_mediaPool->open(mediaPath);  // fast: already open
    } else {
        handle = m_mediaPool->open(mediaPath);  // may block
    }

    if (handle == 0) {
        spdlog::warn("[SAFEMODE] Failed to open media: {}", mediaPath);
        return nullptr;
    }

    const auto* mediaInfo = m_mediaPool->getInfo(handle);
    if (!mediaInfo) {
        spdlog::warn("[SAFEMODE] No media info for handle={}", handle);
        return nullptr;
    }

    // Compute the source frame number
    double fps = targetClip->sourceFps();
    if (fps <= 0.0 && mediaInfo->fps > 0.0)
        fps = mediaInfo->fps;
    if (fps <= 0.0) fps = 24.0;

    int64_t srcTick = tick - targetClip->timelineIn() + targetClip->sourceIn();
    if (srcTick < 0) srcTick = 0;

    int64_t frameNum = static_cast<int64_t>(
        ticksToSeconds(srcTick) * fps);

    if (mediaInfo->frameCount <= 1) {
        frameNum = 0;
    } else if (targetClip->isVideoCharacter()) {
        // Loop for character animations
        frameNum = ((frameNum % mediaInfo->frameCount) + mediaInfo->frameCount)
                   % mediaInfo->frameCount;
    } else {
        frameNum = std::clamp(frameNum, int64_t(0),
                              mediaInfo->frameCount - 1);
    }

    // Use a low resolution tier for safe mode to minimize decode cost
    constexpr ResolutionTier kSafeModeTier = ResolutionTier::Quarter;
    auto frame = m_mediaPool->getFrame(handle, frameNum, kSafeModeTier,
                                       /*scrubMode=*/true);

    if (!frame || frame->pixels.empty()) {
        spdlog::warn("[SAFEMODE] getFrame returned empty for handle={} frame={}",
                     handle, frameNum);
        return nullptr;
    }

    // ── Step 4: Create output CachedFrame with CPU pixel data ──────
    // If outW/outH match the decoded frame, just return it directly.
    // Otherwise scale to output resolution.
    if (frame->width == outW && frame->height == outH) {
        // Ensure no GPU handles are set — this is CPU-only
        frame->gpuReady     = false;
        frame->gpuImageView = 0;
        frame->gpuSampler   = 0;
        frame->gpuSemaphore = 0;
        frame->gpuTextureOwner.reset();
        return frame;
    }

    // ── Step 5: Create a properly-sized output frame ───────────────
    auto result = std::make_shared<CachedFrame>();
    result->width    = outW;
    result->height   = outH;
    result->stride   = outW * 4;
    result->mediaId  = handle;
    result->tier     = kSafeModeTier;
    result->pixels.resize(static_cast<size_t>(outW) * outH * 4);

    // Simple nearest-neighbour scale from decoded frame to output size
    // This is basic but acceptable for recovery display.
    const uint8_t* srcPixels = frame->pixels.data();
    uint8_t* dstPixels = result->pixels.data();
    const uint32_t srcW = frame->width;
    const uint32_t srcH = frame->height;
    const uint32_t srcStride = frame->stride > 0 ? frame->stride : srcW * 4;

    for (uint32_t dy = 0; dy < outH; ++dy) {
        uint32_t sy = std::min<uint32_t>(
            static_cast<uint32_t>(static_cast<float>(dy) * srcH / outH),
            srcH - 1);
        for (uint32_t dx = 0; dx < outW; ++dx) {
            uint32_t sx = std::min<uint32_t>(
                static_cast<uint32_t>(static_cast<float>(dx) * srcW / outW),
                srcW - 1);
            std::memcpy(dstPixels + (dy * outW + dx) * 4,
                        srcPixels + sy * srcStride + sx * 4,
                        4);
        }
    }

    spdlog::info("[SAFEMODE] Composited frame tick={} {}x{} -> {}x{} (handle={} frame={})",
                 tick, srcW, srcH, outW, outH, handle, frameNum);

    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Automatic GPU Recovery from Safe Mode (Phase 7.B)
// ═════════════════════════════════════════════════════════════════════════════
//
// Periodically checks GPU health while in safe mode.  If the GPU device
// is operational again, clears safe mode so the next compositeFrame()
// uses the normal GPU path — seamless and invisible to the user.
// Throttled to once every 5 seconds.
//
// ═════════════════════════════════════════════════════════════════════════════

bool CompositeService::tryAutoRecoverFromSafeMode()
{
    // Only attempt recovery when in safe mode
    if (!m_safeMode.load(std::memory_order_acquire))
        return true; // already recovered

    // ── Throttle: at most one check every 5 seconds ─────────────────
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (m_lastRecoveryCheck != steady_clock::time_point{} &&
        now - m_lastRecoveryCheck < seconds(5))
    {
        return false; // throttled
    }
    m_lastRecoveryCheck = now;

    // ── Check GPU health ────────────────────────────────────────────
    auto& gpu = GpuContext::get();
    const GpuState currentState = gpu.gpuState();

    spdlog::info("[SAFEMODE] Auto-recovery check: GPU state = {}",
                 static_cast<int>(currentState));

    if (currentState == GpuState::Healthy && gpu.isInitialized()) {
        // GPU is already healthy — something else must have recovered it.
        // Just clear our flag and notify.
        spdlog::info("[SAFEMODE] GPU is healthy — exiting safe mode automatically");
        m_safeMode.store(false, std::memory_order_release);
        m_lastSafeModeComposite = {};
        m_lastRecoveryCheck = {};
        if (m_safeModeCallback)
            m_safeModeCallback(false);
        return true;
    }

    if (currentState == GpuState::DeviceLost) {
        spdlog::info("[SAFEMODE] GPU device lost — attempting recovery");
        if (gpu.tryRecover()) {
            spdlog::info("[SAFEMODE] GPU recovery SUCCEEDED — exiting safe mode");
            m_safeMode.store(false, std::memory_order_release);
            m_lastSafeModeComposite = {};
            m_lastRecoveryCheck = {};
            if (m_safeModeCallback)
                m_safeModeCallback(false);
            return true;
        }
    }

    if (currentState == GpuState::Failed) {
        // GPU is in Failed state — try a full re-init via GpuContext
        spdlog::info("[SAFEMODE] GPU is in Failed state — attempting full re-init");
        gpu.shutdown();
        if (gpu.init()) {
            spdlog::info("[SAFEMODE] GPU re-init SUCCEEDED — exiting safe mode");
            m_safeMode.store(false, std::memory_order_release);
            m_lastSafeModeComposite = {};
            m_lastRecoveryCheck = {};
            if (m_safeModeCallback)
                m_safeModeCallback(false);
            return true;
        }
    }

    // Still recovering or recovery failed
    spdlog::info("[SAFEMODE] GPU still unavailable (state={}), staying in safe mode",
                 static_cast<int>(currentState));
    return true; // check was attempted
}

} // namespace rt
