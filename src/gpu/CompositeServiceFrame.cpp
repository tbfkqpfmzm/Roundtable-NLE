/*
 * CompositeServiceFrame.cpp - Frame compositing orchestrator.
 * Extracted from CompositeService.cpp; prewarm methods moved to
 * CompositeServiceFramePrewarm.cpp (Step 3.1 of modularization plan).
 *
 * Contains:
 *   - compositeFrame()   — main compositing entry point
 *   - renderTitleClip()  — TitleClip rendering wrapper
 *   - renderGraphicClip() — GraphicClip rendering wrapper
 */

#include "CompositeService.h"
#include "ClipRenderers.h"
#include "CompositeServiceBlend.h"
#include "CompositeServiceLayerBuild.h"

// Media / timeline
#include "media/FrameCache.h"
#include "media/MediaPool.h"
#include "media/UnifiedCache.h"
#include "Constants.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "timeline/OpacityMask.h"

#include "project/Project.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/AnimationVideoCache.h"
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "stb_image.h"
#endif

#include <thread>
#include <unordered_set>

// GPU compositing
#include "CompositeEngine.h"
#include "GpuContext.h"
#include "Compositor.h"
#include "EffectProcessor.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

std::shared_ptr<CachedFrame> CompositeService::compositeFrame(int64_t tick, uint32_t outW, uint32_t outH,
                                                                bool scrubMode)
try
{
    if (m_shutdown.load(std::memory_order_acquire))
        return nullptr;
    if (!m_timeline) return nullptr;

    // Suppress GPU compositing when a modal dialog is active (QDialog::exec
    // nested event loop).  Paint events still fire for widgets behind the
    // dialog, and invoking the Vulkan compositor from a paint event cascade
    // triggers deep driver call stacks that overflow the NVIDIA driver stack
    // (STACK_OVERFLOW in nvoglv64.dll).  Return the last good cached frame
    // instead, keeping the display valid without touching the GPU.
    if (s_modalDialogActive.load(std::memory_order_acquire)) {
        std::lock_guard lg(m_lastCompositeMtx);
        if (m_lastGoodComposite) {
            m_lastGoodComposite->gpuReady     = false;
            m_lastGoodComposite->gpuImageView = 0;
            m_lastGoodComposite->gpuSampler   = 0;
            m_lastGoodComposite->gpuSemaphore = 0;
        }
        return m_lastGoodComposite;
    }

    // Re-entrancy guard — safety net against unexpected recursive calls.
    // FrameProducer is the sole compositor thread, so this should never
    // trigger in normal operation.  Kept as defense-in-depth against
    // signal re-entrancy or callback chains.
    //
    // A2: Allow exactly ONE level of legitimate recursion (nested
    // SequenceClip composition).  The buildLayersForFrame path swaps
    // m_timeline to the inner sequence and recursively calls
    // compositeFrame to render the inner timeline.  Without this
    // allowance, the previous depth>0 → return-lastGood logic caused
    // nested sequences to silently show stale frames.  Two levels of
    // nesting is enough for almost any real project; deeper recursion
    // is treated as runaway re-entry and short-circuits.
    auto& depth = compositeDepth();
    constexpr int kMaxCompositeDepth = 2;
    if (depth >= kMaxCompositeDepth) {
        std::lock_guard lg(m_lastCompositeMtx);
        return m_lastGoodComposite;
    }
    ++depth;
    struct DepthGuard {
        ~DepthGuard() { --compositeDepth(); }
    } depthGuard;

    // P2: CPU safe-mode fallback was deleted here.  When GPU compositing
    // fails persistently, GpuContext::tryRecover() transitions to Failed
    // and fires the fatal-failure callback (modal restart dialog).  No
    // per-frame "safe-mode" compositor — every major NLE treats device-
    // lost as fatal; we do the same.

    // Blocking lock — FrameProducer is now the sole compositor thread.
    // The old try_to_lock pattern (which returned stale m_lastGoodComposite
    // on contention) is removed.  compositeFrame() now ALWAYS produces the
    // correct frame for the requested tick.  Never returns stale frames.
    // Uses unique_lock (not lock_guard) because buildLayersForFrame()
    // temporarily unlocks to allow async media opens.
    std::unique_lock lock(m_compositeMutex);

    // Phase B: advance the UnifiedCache generation counter and run its
    // throttled eviction + adaptive-budget passes.  These are very cheap
    // — eviction is gated by a 3s throttle, rebalance is gated by a
    // 30-frame throttle — but having the call site here ensures every
    // composite tick contributes to its LRU view.
    if (m_unifiedCache) {
        m_unifiedCache->onFrameStart();
        m_unifiedCache->runEvictionPass();
        m_unifiedCache->rebalanceBudgets();
    }

    // Check deferred cache invalidation request (set by requestCacheInvalidation
    // from the UI thread via atomic flag, avoiding deadlock).
    if (m_cacheInvalidateRequested.exchange(false, std::memory_order_acquire)) {
        if (m_engine)
            m_engine->clearLru();
        {
            std::lock_guard lg(m_lastCompositeMtx);
            m_lastGoodComposite.reset();
            m_lastGoodCompositeTick = -1;
        }
    }

    // All composites use non-blocking tryGetFrame — the old blocking inline
    // NVDEC decode (which held m_compositeMutex for 50-500ms, freezing the
    // UI) was migrated to async prefetch workers.  The ProgramMonitor settle
    // mechanism retries until the frame arrives (~30-100ms).
    // The blocking lock on m_compositeMutex (instead of old try_to_lock)
    // ensures compositeFrame() ALWAYS produces the correct frame.
    bool playbackNonBlocking = true;
    // Only one GPU Spine render per compositeFrame — the shared FBO gets
    // cleared by beginFrame(), destroying previous renders.
    bool gpuSpineUsedThisFrame = false;

    // ── Timeline lookahead prewarm ─────────────────────────────────────
    // Proactively open media + schedule first-frame prefetch for clips
    // coming up in the next ~2s.  Kills the reactive cold-decoder stall
    // at every shot boundary.  Also run during export (forceFullRes)
    // so the decode cache stays hot and we don't block on every frame.
    if (playbackNonBlocking || m_forceFullResolution.load()) {
        prewarmUpcomingShots(tick);
    }

    // Shot-boundary detection: when new clips appear (different from last
    // composite), force blocking decode so the correct character shows
    // immediately instead of flashing the previous shot's frame.
    // Also run during export so cache isn't polluted by stale clip IDs.
    if (playbackNonBlocking || m_forceFullResolution.load()) {
        std::unordered_set<uint64_t> currentClipIds;
        for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
            auto* track = m_timeline->track(ti - 1);
            if (!track || track->type() != TrackType::Video || track->isMuted())
                continue;
            for (auto* clip : track->clipsAtTime(tick)) {
                if (clip && clip->isEnabled())
                    currentClipIds.insert(clip->id());
            }
        }
        bool hasNewClips = false;
        for (auto id : currentClipIds) {
            if (m_lastActiveClipIds.find(id) == m_lastActiveClipIds.end()) {
                hasNewClips = true;
                break;
            }
        }
        if (hasNewClips) {
            int unopenedCount = 0;
            int uncachedCount = 0;
            for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
                auto* track = m_timeline->track(ti - 1);
                if (!track || track->type() != TrackType::Video || track->isMuted())
                    continue;
                for (auto* clip : track->clipsAtTime(tick)) {
                    if (!clip || !clip->isEnabled()) continue;
                    if (m_lastActiveClipIds.find(clip->id()) != m_lastActiveClipIds.end())
                        continue;
                    auto* videoClip = dynamic_cast<VideoClip*>(clip);
                    if (!videoClip) continue;
                    const auto& mp = videoClip->mediaPath();
                    if (mp.empty() || !m_mediaPool) continue;

                    if (!m_mediaPool->isPathOpen(mp)) {
                        m_mediaPool->openAsync(mp);
                        ++unopenedCount;
                        continue;
                    }
                    auto handle = m_mediaPool->open(mp);
                    if (handle != 0) {
                        const int64_t clipTickOffset = (tick - clip->timelineIn()) + clip->sourceIn();
                        const auto* info = m_mediaPool->getInfo(handle);
                        const double srcFps = (info && info->fps > 0.0)
                                              ? info->fps
                                              : (videoClip->sourceFps() > 0.0
                                                 ? videoClip->sourceFps() : 30.0);
                        const double secs = static_cast<double>(clipTickOffset)
                                            / static_cast<double>(rt::kTicksPerSecond);
                        const int64_t srcFrame = static_cast<int64_t>(secs * srcFps);
                        const auto warmTier = videoClip->isVideoCharacter()
                                              ? ResolutionTier::Half
                                              : playbackTier();
                        if (!m_mediaPool->isFrameCached(handle, srcFrame, warmTier)) {
                            // Two-stage prefetch on shot boundary: a small
                            // *urgent* batch (the current + previous frame)
                            // followed by a non-urgent fan-out for the next
                            // half-second.  The previous version requested
                            // count=8 urgent per new clip, which at a typical
                            // multi-clip boundary (2-3 new clips + Spine
                            // atlas upload happening in parallel) flooded
                            // NVDEC + the graphics queue with ~16-24
                            // simultaneous urgent decodes and tripped TDR.
                            // The urgent pair guarantees the frame we are
                            // about to composite is hot; the deferred batch
                            // covers the upcoming playhead motion without
                            // sharing a single contention window.
                            m_mediaPool->schedulePrefetch(handle, srcFrame - 1,
                                /*count=*/2, /*urgent=*/true, warmTier);
                            m_mediaPool->schedulePrefetch(handle, srcFrame + 1,
                                /*count=*/6, /*urgent=*/false, warmTier);
                            ++uncachedCount;
                        }
                    }
                }
            }

            if (unopenedCount > 0 || uncachedCount > 0) {
                spdlog::info("[COMPOSITE] Shot boundary: {} new clips ({} unopened-async, {} uncached-prefetch) — staying non-blocking",
                             currentClipIds.size(), unopenedCount, uncachedCount);
                if (unopenedCount > 0) {
                    spdlog::warn("[LOOKAHEAD-MISS] shot-boundary caught {} clips with UNOPENED media — prewarm didn't reach far enough ahead or timeline was modified",
                                 unopenedCount);
                }
            }
        }
        m_lastActiveClipIds = std::move(currentClipIds);
    #ifdef ROUNDTABLE_HAS_SPINE
        for (auto it = m_lastPreRenderedSpineFrame.begin();
             it != m_lastPreRenderedSpineFrame.end();) {
            if (m_lastActiveClipIds.find(it->first) == m_lastActiveClipIds.end())
            it = m_lastPreRenderedSpineFrame.erase(it);
            else
            ++it;
        }
    #endif
    }

    if (m_mediaPool) {
        m_mediaPool->scheduler().setPlayhead(tick);
    }

    auto fetchMediaFrame = [&](MediaHandle handle, int64_t frameNumber,
                               ResolutionTier tier) -> std::shared_ptr<CachedFrame> {
        if (!m_mediaPool)
            return nullptr;
        return m_mediaPool->tryGetFrame(handle, frameNumber, tier);
    };

    // ── ONE-TIME STARTUP DIAGNOSTIC ──────────────────────────────────
    {
        static bool s_startupLogged = false;
        if (!s_startupLogged) {
            s_startupLogged = true;
            auto& gpu = GpuContext::get();
            spdlog::info("========== COMPOSITE PIPELINE CONFIG ==========");
            spdlog::info("  gpuDisplayMode = {}", m_gpuDisplayMode);
            spdlog::info("  GpuContext initialized = {}", gpu.isInitialized());
            spdlog::info("  CudaVulkanInterop = {}", gpu.cudaVulkanInterop() ? "YES" : "NO");
            spdlog::info("  outW={} outH={}", outW, outH);
            if (m_mediaPool)
                spdlog::info("  open media count = {}", m_mediaPool->openCount());
            if (m_engine) {
                auto* texCache = m_engine->textureCache();
                if (texCache)
                    spdlog::info("  GpuTexCache budget = {:.0f} MB, entries = {}",
                                 texCache->budget() / 1048576.0,
                                 texCache->entryCount());
                else
                    spdlog::info("  GpuTexCache = NOT yet created");
            }
            spdlog::info("===============================================");
        }
    }

    // ── PERF TIMING ─────────────────────────────────────────────────
    static int s_perfCounter = 0;
    const bool perfLog = (++s_perfCounter % 10 == 0);
    auto perfT0 = std::chrono::high_resolution_clock::now();
    auto perfTlayers = perfT0;
    auto perfTgpuUp  = perfT0;
    auto perfTcomp   = perfT0;

    // Composite result LRU cache
    if (m_engine && (!scrubMode || m_forceFullResolution.load())) {
        auto cached = m_engine->checkLru(tick, outW, outH);
        if (cached)
            return cached;
    }

    if (m_engine) {
        m_engine->flushLruOnResize(outW, outH);
    }

    if (outW < 64) outW = 64;
    if (outH < 36) outH = 36;

    std::vector<LayerInfo> layers;

    int clipsAtTick = 0;
    layers = buildLayersForFrame(tick, outW, outH, scrubMode, playbackNonBlocking,
                                 clipsAtTick, perfLog, lock, gpuSpineUsedThisFrame);

    // ── A1: Settle-window fallback (Premiere-style hold) ─────────────────
    // If we resolved FEWER layers than the active clip count, the composite
    // would render with missing layers — the visible "layers build up over
    // several frames" symptom.  Hold the prior complete composite for up to
    // kSettleWindowMs ms instead.  After that timeout, accept the partial
    // composite so we don't freeze indefinitely if a clip is permanently
    // unresolvable (e.g. missing media file).
    //
    // Skip during scrub (the user wants immediate feedback on every drag
    // step, partial frames are fine) and during export (forceFullResolution,
    // which always blocks until full).
    if (!scrubMode && !m_forceFullResolution.load() &&
        clipsAtTick > 0 && static_cast<int>(layers.size()) < clipsAtTick)
    {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        std::shared_ptr<CachedFrame> hold;
        bool withinSettle = false;
        {
            std::lock_guard lg(m_lastCompositeMtx);
            if (m_lastGoodComposite && m_lastFullCompositeAt.time_since_epoch().count() != 0)
            {
                const auto elapsed = duration_cast<milliseconds>(
                    now - m_lastFullCompositeAt).count();
                if (elapsed < kSettleWindowMs) {
                    hold = m_lastGoodComposite;
                    withinSettle = true;
                }
            }
        }
        if (withinSettle && hold) {
            static int s_settleLog = 0;
            if (++s_settleLog % 30 == 0) {
                spdlog::info("[SETTLE] tick={} resolved={}/{} — holding prior composite",
                             tick, layers.size(), clipsAtTick);
            }
            return hold;
        }
    }

    if (layers.empty()) {
        static int s_emptyLog = 0;
        if (++s_emptyLog <= 20 || s_emptyLog % 60 == 0) {
            spdlog::info("[FLICKER-DIAG] compositeFrame tick={}: layers empty "
                         "(clipsAtTick={}, all skipped)",
                         tick, clipsAtTick);
        }

        if (clipsAtTick > 0) {
            std::lock_guard lg(m_lastCompositeMtx);
            if (m_lastGoodComposite && m_lastGoodCompositeTick >= 0) {
                // Allow at most 2 stale frames so the viewport doesn't
                // freeze on the previous shot for multiple frame durations
                // while the new shot's first frame decodes.  At 24fps each
                // frame is 2000 ticks (48000/24); at 60fps it's 800 ticks.
                // Use 2000 ticks which is 1 frame at 24fps or ~2.5 frames
                // at 60fps — brief enough to avoid visible freeze.
                constexpr int64_t kMaxStaleTicks = 48000 / 24;
                int64_t tickDelta = std::abs(tick - m_lastGoodCompositeTick);
                if (tickDelta <= kMaxStaleTicks)
                    return m_lastGoodComposite;
                spdlog::info("[COMPOSITE] Suppressing stale lastGoodComposite "
                             "(tick={} vs last={}, delta={})",
                             tick, m_lastGoodCompositeTick, tickDelta);
            }
        }

        // ── Shot boundary: return empty sentinel ────────────────────────
        // The stale frame was shown for at most 1-2 frames (kMaxStaleTicks
        // above), then the new clip's first frame still isn't decoded.
        // Return an empty frame (width=0) which propagates through the
        // FrameProducer → FramePresenter → ProgramMonitor chain as a
        // "clear viewport" signal.  Meanwhile the async prefetch workers
        // (started by the shot-boundary detection above) continue decoding
        // the new clip's frames; when they arrive, the next composite tick
        // will produce the correct frame.
        spdlog::info("[COMPOSITE] Shot boundary tick={}: returning empty sentinel "
                     "({} clips present, no decoded frames available)",
                     tick, clipsAtTick);
        {
            auto emptyFrame = std::make_shared<CachedFrame>();
            std::lock_guard lg(m_lastCompositeMtx);
            m_lastGoodComposite.reset();
            m_lastGoodCompositeTick = -1;
            return emptyFrame;
        }
    }

    // Log layer details for packed-alpha diagnostics
    {
        static int s_layerDiag = 0;
        if (++s_layerDiag % 60 == 0) {
            for (size_t li = 0; li < layers.size(); ++li) {
                const auto& L = layers[li];
                spdlog::info("[FLICKER-DIAG] tick={} layer[{}]: {}x{} isPacked={} containFit={} "
                             "gpuTex={} opacity={:.2f} clipId={} unpackedAlpha={}",
                             tick, li, L.frameWidth, L.frameHeight,
                             L.isPacked, L.containFit, L.gpuTextureReady,
                             L.opacity, L.clipId,
                             (L.frame ? L.frame->unpackedAlpha : false));
            }
        }
    }

    // Single layer fast path
    if (!m_gpuDisplayMode && layers.size() == 1) {
        const auto& L = layers[0];
        bool isIdentity = L.opacity >= 0.999f &&
                          std::abs(L.posX) < 0.5f && std::abs(L.posY) < 0.5f &&
                          std::abs(L.scX - 1.0f) < 0.001f &&
                          std::abs(L.scY - 1.0f) < 0.001f &&
                          std::abs(L.rot) < 0.01f &&
                          L.cropL < 0.01f && L.cropR < 0.01f &&
                          L.cropT < 0.01f && L.cropB < 0.01f &&
                          L.effects.empty() &&
                          !L.gpuTextureReady;
        if (isIdentity && L.frame && L.frame->width == outW && L.frame->height == outH)
            return L.frame;
    }

    // ── PERF: layers collected ────────────────────────────────────────
    perfTlayers = std::chrono::high_resolution_clock::now();

    // GPU compositing path
    {
        int effectLayerCount = 0, effectPassCount = 0, transitionCount = 0;
        auto gpuResult = tryCompositeOnGpu(layers, outW, outH, tick, scrubMode,
                                            perfLog, perfT0, perfTlayers,
                                            effectLayerCount, effectPassCount,
                                            transitionCount);
        if (gpuResult) {
            return gpuResult;
        }
    }
    // P2 (corrected): GPU composite returned nullptr.  This is NOT
    // necessarily device-lost — it can be a transient miss (first
    // frame, no decoded inputs, allocation hiccup).  The actual
    // device-lost signal is fired inside CompositeEngine::composite-
    // ViaRenderGraph on submit failure (see GpuContext.signalDeviceLost
    // call there).  Here we just return the last good frame and log at
    // debug level so the path stays quiet during normal warm-up.
    static thread_local int s_quietLogCounter = 0;
    if (++s_quietLogCounter % 30 == 1) {
        spdlog::debug("[COMPOSITE] tryCompositeOnGpu returned nullptr — "
                      "returning last-good (counter={})", s_quietLogCounter);
    }
    {
        std::lock_guard lg(m_lastCompositeMtx);
        if (m_lastGoodComposite) {
            m_lastGoodComposite->gpuReady     = false;
            m_lastGoodComposite->gpuImageView = 0;
            m_lastGoodComposite->gpuSampler   = 0;
            m_lastGoodComposite->gpuSemaphore = 0;
        }
        return m_lastGoodComposite;
    }
}
catch (const std::exception& ex)
{
    spdlog::error("compositeFrame: exception: {}", ex.what());
    std::lock_guard lg(m_lastCompositeMtx);
    return m_lastGoodComposite;
}
catch (...)
{
    spdlog::error("compositeFrame: unknown exception");
    std::lock_guard lg(m_lastCompositeMtx);
    return m_lastGoodComposite;
}


std::shared_ptr<CachedFrame> CompositeService::renderTitleClip(
    TitleClip* clip, int64_t tick, uint32_t outW, uint32_t outH)
{
    return rt::renderTitleClip(clip, tick, outW, outH);
}

std::shared_ptr<CachedFrame> CompositeService::renderGraphicClip(
    GraphicClip* clip, int64_t tick, uint32_t outW, uint32_t outH)
{
    uint32_t refW = 0, refH = 0;
    if (m_project) {
        refW = m_project->settings().resolution().width;
        refH = m_project->settings().resolution().height;
    }
    return rt::renderGraphicClip(clip, tick, outW, outH, refW, refH);
}


} // namespace rt
