/*
 * CompositeServiceFrame.cpp - Frame compositing + prewarm (split from CompositeService.cpp).
 * Contains compositeFrame(), prewarmPlaybackResources(), and rendering helpers.
 */

#include "CompositeService.h"
#include "ClipRenderers.h"
#include "CompositeServiceBlend.h"
#include "CompositeServiceLayerBuild.h"

// Media / timeline
#include "media/FrameCache.h"
#include "media/MediaPool.h"
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


// ─────────────────────────────────────────────────────────────────────────────
// Background-thread prewarm dispatch
// ─────────────────────────────────────────────────────────────────────────────

void CompositeService::prewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH)
{
    if (m_shutdown.load(std::memory_order_acquire))
        return;

    std::lock_guard lock(m_prewarmMutex);
    m_prewarmRequest = {tick, outW, outH};
    m_prewarmPending = true;
    m_prewarmCv.notify_one();
}

void CompositeService::prewarmThreadLoop()
{
    while (true) {
        PrewarmRequest req;
        {
            std::unique_lock lock(m_prewarmMutex);
            m_prewarmCv.wait(lock, [this]() {
                return m_prewarmPending || m_destroying.load(std::memory_order_acquire);
            });
            if (m_destroying.load(std::memory_order_acquire))
                break;
            req = m_prewarmRequest;
            m_prewarmPending = false;
        }

        // Run the actual prewarm work on this background thread.
        // The internal try_to_lock on m_compositeMutex means this will
        // safely defer if compositeFrame() is mid-execution.
        doPrewarmPlaybackResources(req.tick, req.outW, req.outH);
    }
}

void CompositeService::doPrewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH)
{
    if (m_shutdown.load(std::memory_order_acquire))
        return;
    if (!m_timeline || !m_mediaPool || outW == 0 || outH == 0)
        return;

    // Prevent overlapping prewarm runs
    // (Thread_local guard is sufficient since this is called from the
    //  background prewarm thread, not the UI thread)
    static thread_local bool s_inPrewarm = false;
    if (s_inPrewarm)
        return;
    s_inPrewarm = true;

    std::unique_lock lock(m_compositeMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        s_inPrewarm = false;
        return;
    }

    auto& gpu = GpuContext::get();
    if (gpu.isInitialized()) {
        auto* comp = gpu.compositor(outW, outH);
        if (comp && comp->isInitialized()) {
            m_compositorReady.store(true);
            static std::atomic<int> s_prewarmLog{0};
            if (++s_prewarmLog <= 10 || s_prewarmLog % 30 == 0) {
                spdlog::info("[PERF] prewarmPlaybackResources: compositor ready {}x{} at tick={}",
                             outW, outH, tick);
            }
        }
    }

    // Program monitor preview tier
    const ResolutionTier previewTier = playbackTier();
    bool warmedEffectProcessor = false;

    struct PrerollTarget { uint64_t handle; int64_t frame; ResolutionTier tier; };
    std::vector<PrerollTarget> prerollTargets;
    prerollTargets.reserve(8);

    for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
        auto* track = m_timeline->track(ti - 1);
        if (!track || track->type() != TrackType::Video || track->isMuted())
            continue;

        auto active = track->clipsAtTime(tick);
        for (Clip* clip : active) {
            if (!clip)
                continue;

            if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                const auto& mediaPath = videoClip->mediaPath();
                if (mediaPath.empty())
                    continue;

                uint64_t handle = 0;
                auto it = m_openMediaHandles.find(mediaPath);
                if (it != m_openMediaHandles.end()) {
                    handle = it->second;
                } else if (m_mediaPool->isPathOpen(mediaPath)) {
                    handle = m_mediaPool->open(mediaPath);
                    if (handle != 0)
                        m_openMediaHandles[mediaPath] = handle;
                } else {
                    m_mediaPool->openAsync(mediaPath);
                    handle = 0;
                }

                if (handle == 0)
                    continue;

                double fps = videoClip->sourceFps();
                if (fps <= 0.0)
                    fps = 24.0;

                int64_t srcTick = tick - clip->timelineIn() + clip->sourceIn();
                if (srcTick < 0)
                    srcTick = 0;

                int64_t frameNum = static_cast<int64_t>(ticksToSeconds(srcTick) * fps);
                const auto* mediaInfo = m_mediaPool->getInfo(handle);
                if (!mediaInfo)
                    continue;

                if (videoClip->sourceFps() <= 0.0 && mediaInfo->fps > 0.0) {
                    fps = mediaInfo->fps;
                    frameNum = static_cast<int64_t>(ticksToSeconds(srcTick) * fps);
                }

                if (mediaInfo->frameCount <= 1) {
                    frameNum = 0;
                } else if (videoClip->isVideoCharacter()) {
                    frameNum = ((frameNum % mediaInfo->frameCount) + mediaInfo->frameCount)
                               % mediaInfo->frameCount;
                } else {
                    frameNum = std::clamp(frameNum, int64_t(0), mediaInfo->frameCount - 1);
                }

                const auto warmTier = videoClip->isVideoCharacter()
                    ? ResolutionTier::Half : previewTier;
                (void)m_mediaPool->tryGetFrame(handle, frameNum, warmTier);

                if (mediaInfo->frameCount > 1) {
                    m_mediaPool->schedulePrefetch(
                        handle, frameNum + 1, 60, /*urgent=*/true, warmTier);
                    prerollTargets.push_back({handle, frameNum + 1, warmTier});
                }

                if (videoClip->isVideoCharacter() &&
                    mediaInfo->frameCount > 1 &&
                    mediaInfo->frameCount <= MediaPool::LOOP_PREDECODE_MAX_FRAMES) {
                    m_mediaPool->startLoopPreDecode(handle, warmTier, clip->timelineIn());
                }

                if (!warmedEffectProcessor && gpu.isInitialized() &&
                    clip->effects().hasActiveEffects()) {
                    uint32_t fxW = mediaInfo->width;
                    uint32_t fxH = mediaInfo->height;
                    constexpr uint32_t kPreviewMaxDim = 960;
                    if (fxW > kPreviewMaxDim || fxH > kPreviewMaxDim) {
                        const float scale = std::min(
                            static_cast<float>(kPreviewMaxDim) / static_cast<float>(fxW),
                            static_cast<float>(kPreviewMaxDim) / static_cast<float>(fxH));
                        fxW = std::max<uint32_t>(2u, static_cast<uint32_t>(fxW * scale) & ~1u);
                        fxH = std::max<uint32_t>(2u, static_cast<uint32_t>(fxH * scale) & ~1u);
                    }
                    auto* fx = gpu.effectProcessor(fxW, fxH);
                    warmedEffectProcessor = fx && fx->isInitialized();
                }
            } else if (!warmedEffectProcessor && gpu.isInitialized() && clip->effects().hasActiveEffects()) {
                auto* fx = gpu.effectProcessor(outW, outH);
                warmedEffectProcessor = fx && fx->isInitialized();
            }
#ifdef ROUNDTABLE_HAS_SPINE
            if (auto* spineClip = dynamic_cast<SpineClip*>(clip);
                spineClip && m_animVideoCache) {
                const std::string baseAnim = spineClip->animationName();
                const bool animIsAlreadyTalk =
                    (baseAnim.size() >= 5 &&
                     baseAnim.compare(baseAnim.size() - 5, 5, "_talk") == 0);
                const std::string selectedAnim =
                    spineClip->isTalking() && !animIsAlreadyTalk
                        ? (baseAnim + "_talk") : baseAnim;
                const auto* entry = m_animVideoCache->getEntry(
                    spineClip->characterName(), spineClip->outfit(), selectedAnim);
                if (entry) {
                    const std::string mp = entry->videoPath.string();
                    uint64_t handle = 0;
                    auto mit = m_openMediaHandles.find(mp);
                    if (mit != m_openMediaHandles.end()) {
                        handle = mit->second;
                    } else {
                        handle = m_mediaPool->open(mp);
                        if (handle != 0) m_openMediaHandles[mp] = handle;
                    }
                    if (handle != 0) {
                        const auto* info = m_mediaPool->getInfo(handle);
                        if (info && info->frameCount > 1) {
                            const auto warmTier = ResolutionTier::Half;
                            int64_t animFrame = 0;
                            if (info->fps > 0.0 && info->frameCount > 0) {
                                const int64_t localTick =
                                    spineClip->useGlobalTime() ? tick
                                                               : (tick - clip->timelineIn());
                                const double animTime =
                                    ticksToSeconds(localTick) *
                                    static_cast<double>(spineClip->animationSpeed());
                                int64_t f = static_cast<int64_t>(animTime * info->fps);
                                if (spineClip->isLooping()) {
                                    f = ((f % info->frameCount) + info->frameCount) % info->frameCount;
                                } else {
                                    f = std::clamp(f, int64_t(0), info->frameCount - 1);
                                }
                                animFrame = f;
                            }
                            m_mediaPool->schedulePrefetch(
                                handle, animFrame, /*count=*/30, /*urgent=*/true, warmTier);
                            prerollTargets.push_back({handle, animFrame, warmTier});
                            if (info->frameCount <= MediaPool::LOOP_PREDECODE_MAX_FRAMES) {
                                m_mediaPool->startLoopPreDecode(handle, warmTier, clip->timelineIn());
                                spdlog::info("[PREWARM] Spine loop pre-decode: clip {} '{}' ({} frames), startFrame={}",
                                             clip->id(),
                                             std::filesystem::path(mp).filename().string(),
                                             info->frameCount, animFrame);
                            }
                        }
                    }
                }
            }
#endif
        }
    }

    // ── Seed m_lastActiveClipIds ────────────────────────────────────────
    {
        std::unordered_set<uint64_t> activeIds;
        for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
            auto* track = m_timeline->track(ti - 1);
            if (!track || track->type() != TrackType::Video || track->isMuted())
                continue;
            for (auto* clip : track->clipsAtTime(tick)) {
                if (clip && clip->isEnabled())
                    activeIds.insert(clip->id());
            }
        }
        m_lastActiveClipIds = std::move(activeIds);
    }

    // ── Pre-open ALL media handles on ALL video tracks (background) ─────
    {
        std::vector<std::string> pathsToOpen;
        for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
            auto* track = m_timeline->track(ti - 1);
            if (!track || track->type() != TrackType::Video || track->isMuted())
                continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                auto* clip = track->clip(ci);
                if (!clip) continue;
                auto* videoClip = dynamic_cast<VideoClip*>(clip);
                if (!videoClip) continue;
                const auto& mediaPath = videoClip->mediaPath();
                if (mediaPath.empty()) continue;
                if (m_openMediaHandles.count(mediaPath)) continue;
                pathsToOpen.push_back(mediaPath);
            }
        }

        if (!pathsToOpen.empty()) {
            std::sort(pathsToOpen.begin(), pathsToOpen.end());
            pathsToOpen.erase(std::unique(pathsToOpen.begin(), pathsToOpen.end()),
                              pathsToOpen.end());

            auto* pool = m_mediaPool;
            // Release mutex during background bulk-open so FrameProducer
            // is not starved
            lock.unlock();
            std::thread([pool, paths = std::move(pathsToOpen)]() {
                for (const auto& mediaPath : paths) {
                    namespace fs = std::filesystem;
                    fs::path p(mediaPath);
                    uint64_t handle = 0;
                    if (p.extension() == ".mov" || p.extension() == ".webm") {
                        fs::path mp4Path = fs::path(mediaPath).replace_extension(".mp4");
                        std::error_code ec;
                        if (fs::exists(mp4Path, ec))
                            handle = pool->open(mp4Path);
                        if (handle == 0) {
                            fs::path mp4InVideos = fs::path("assets") / "videos" / mp4Path.filename();
                            if (fs::exists(mp4InVideos, ec))
                                handle = pool->open(mp4InVideos);
                        }
                    }
                    if (handle == 0) {
                        uint64_t fallbackHandle = pool->open(mediaPath);
                        (void)fallbackHandle;
                    }
                }
                spdlog::info("[PERF] background bulk-open finished ({} paths)", paths.size());
            }).detach();
            lock.lock();
        }
    }

    // ── Bounded play-start preroll ──────────────────────────────────────
    if (!prerollTargets.empty()) {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + milliseconds(400);
        int pollCount = 0;
        size_t lastReady = 0;
        bool allWarm = false;
        do {
            // Release the mutex so the FrameProducer thread can composite
            // frames and submit GPU work.
            lock.unlock();
            std::this_thread::sleep_for(milliseconds(2));
            lock.lock();

            lastReady = 0;
            for (const auto& t : prerollTargets) {
                if (m_mediaPool->isFrameCached(t.handle, t.frame, t.tier))
                    ++lastReady;
            }
            if (lastReady == prerollTargets.size()) {
                allWarm = true;
                break;
            }
            ++pollCount;
        } while (steady_clock::now() < deadline);

        static std::atomic<int> s_prerollLog{0};
        if (++s_prerollLog <= 10 || s_prerollLog % 30 == 0) {
            if (allWarm) {
                spdlog::info("[PERF] play-start preroll: {}/{} frames warm after {} polls",
                             lastReady, prerollTargets.size(), pollCount);
            } else {
                spdlog::warn("[PERF] play-start preroll TIMEOUT: only {}/{} frames warm after {} polls",
                             lastReady, prerollTargets.size(), pollCount);
            }
        }
    }

    s_inPrewarm = false;
}

// ─────────────────────────────────────────────────────────────────────
// Timeline lookahead prewarm.
//
// Scans clips whose timelineIn() lies in (tick, tick + LOOKAHEAD_SECONDS]
// and proactively opens their media + schedules first-frame prefetch.
// This eliminates the 150-200ms "cold decoder open" stall at every shot
// boundary — the decoder is warm and frame 0 cached before the playhead
// reaches the clip.  VLC doesn't have this problem because it plays a
// single continuous stream; we stitch many files, so we must prewarm.
// Throttled to ~100ms (10 scans/sec) to avoid redundant work at 60fps.
// ─────────────────────────────────────────────────────────────────────
void CompositeService::prewarmUpcomingShots(int64_t tick)
{
    if (m_shutdown.load(std::memory_order_acquire))
        return;
    if (!m_timeline || !m_mediaPool) return;

    using namespace std::chrono;
    auto now = steady_clock::now();
    if (now - m_lastLookaheadScan < milliseconds(100))
        return;
    m_lastLookaheadScan = now;

    constexpr double LOOKAHEAD_SECONDS = 2.0;
    const int64_t lookaheadTicks = static_cast<int64_t>(LOOKAHEAD_SECONDS * kTicksPerSecond);
    const int64_t windowEnd = tick + lookaheadTicks;

    int prewarmedThisScan = 0;
    int openedThisScan = 0;

    for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
        auto* track = m_timeline->track(ti - 1);
        if (!track || track->type() != TrackType::Video || track->isMuted())
            continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* clip = track->clip(ci);
            if (!clip || !clip->isEnabled()) continue;

            const int64_t clipIn = clip->timelineIn();
            if (clipIn <= tick || clipIn > windowEnd) continue;
            if (m_lastActiveClipIds.count(clip->id())) continue;
            if (!m_prewarmedClipIds.insert(clip->id()).second) continue;

            // Resolve which media path this clip decodes from.  VideoClip
            // has a direct mediaPath; SpineClip routes through the
            // animation video cache for its pre-rendered .mp4/.webm.
            std::string mp;
            bool isCharacter = false;
            if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                mp = videoClip->mediaPath();
                isCharacter = videoClip->isVideoCharacter();
            }
#ifdef ROUNDTABLE_HAS_SPINE
            else if (auto* spineClip = dynamic_cast<SpineClip*>(clip)) {
                if (m_animVideoCache) {
                    const auto* entry = m_animVideoCache->getEntry(
                        spineClip->characterName(),
                        spineClip->outfit(),
                        spineClip->animationName());
                    if (entry) {
                        mp = entry->videoPath.string();
                        isCharacter = true;
                    }
                }
            }
#endif
            if (mp.empty()) {
                // Either a non-media clip or a Spine clip with no
                // pre-rendered video.  Drop the prewarm mark so it can be
                // re-considered later without duplicate log spam.
                m_prewarmedClipIds.erase(clip->id());
                continue;
            }

            if (!m_mediaPool->isPathOpen(mp)) {
                m_mediaPool->openAsync(mp);
                ++openedThisScan;
                // Re-queue: next scan (~100ms later) will schedule prefetch
                // once the async open completes.
                m_prewarmedClipIds.erase(clip->id());
                continue;
            }

            uint64_t handle = 0;
            auto it = m_openMediaHandles.find(mp);
            if (it != m_openMediaHandles.end()) {
                handle = it->second;
            } else {
                handle = m_mediaPool->open(mp); // fast: already open
                if (handle != 0) m_openMediaHandles[mp] = handle;
            }
            if (handle == 0) continue;

            const auto* info = m_mediaPool->getInfo(handle);
            if (!info) continue;

            double fps = (info->fps > 0.0) ? info->fps : 24.0;
            if (auto* vc = dynamic_cast<VideoClip*>(clip)) {
                if (vc->sourceFps() > 0.0) fps = vc->sourceFps();
            }

            const double srcSecs = static_cast<double>(clip->sourceIn()) / static_cast<double>(kTicksPerSecond);
            int64_t srcFrame = static_cast<int64_t>(srcSecs * fps);
            if (info->frameCount > 1) {
                if (isCharacter)
                    srcFrame = ((srcFrame % info->frameCount) + info->frameCount) % info->frameCount;
                else
                    srcFrame = std::clamp(srcFrame, int64_t(0), info->frameCount - 1);
            } else {
                srcFrame = 0;
            }

            // Upcoming-shot prewarm uses the current playback tier.
            // Characters stay Half (they composite tiny regardless).
            const auto warmTier = isCharacter ? ResolutionTier::Half
                                              : playbackTier();

            m_mediaPool->schedulePrefetch(handle, srcFrame,
                                          /*count=*/30, /*urgent=*/true, warmTier);
            ++prewarmedThisScan;

            // For short character loops (≤ LOOP_PREDECODE_MAX_FRAMES),
            // pre-decode the entire file into cache so playback is 100%
            // cache hits for the full duration of the shot.  Without this,
            // a 160-frame 60fps clip decoded reactively at ~12fps looks
            // frozen, updating only every ~80ms.  startLoopPreDecode is
            // idempotent and cheap once the loop is cached.
            if (isCharacter &&
                info->frameCount > 1 &&
                info->frameCount <= MediaPool::LOOP_PREDECODE_MAX_FRAMES) {
                m_mediaPool->startLoopPreDecode(handle, warmTier, clipIn);
            }

            const double leadMs = static_cast<double>(clipIn - tick)
                                  / static_cast<double>(kTicksPerSecond) * 1000.0;
            static std::atomic<int> s_lookaheadLog{0};
            if (++s_lookaheadLog <= 30 || s_lookaheadLog % 20 == 0) {
                spdlog::info("[LOOKAHEAD] prewarm clip {} '{}' frame {} tier={} frames={} (starts in {:.0f}ms)",
                             clip->id(),
                             std::filesystem::path(mp).filename().string(),
                             srcFrame, static_cast<int>(warmTier),
                             info->frameCount, leadMs);
            }
        }
    }

    // Drop prewarm marks for clips that have become active so that if the
    // same clip id reappears later on the timeline (looped section) it
    // gets prewarmed again.
    for (auto it = m_prewarmedClipIds.begin(); it != m_prewarmedClipIds.end();) {
        if (m_lastActiveClipIds.count(*it))
            it = m_prewarmedClipIds.erase(it);
        else
            ++it;
    }

    if (prewarmedThisScan > 0 || openedThisScan > 0) {
        spdlog::debug("[LOOKAHEAD] scan@tick={} window={:.1f}s: {} prewarmed, {} async-opens",
                      tick, LOOKAHEAD_SECONDS, prewarmedThisScan, openedThisScan);
    }
}

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

    // Thread-safety + re-entrancy guard Ã¢â‚¬â€ if compositeFrame is already
    // running, return immediately.  Two mechanisms:
    //   1. Same-thread re-entrancy: thread_local depth counter catches
    //      signal re-entrancy / paint-event recursion on the GUI thread.
    //      Without this, a recursive composite call chain can exhaust the
    //      call stack (1 MB) and cause STACK_OVERFLOW in ntdll.dll.
    //   2. Cross-thread contention: mutex try_lock catches the async
    //      render thread colliding with the UI thread.
    auto& depth = compositeDepth();
    if (depth > 0) {
        std::lock_guard lg(m_lastCompositeMtx);
        if (m_lastGoodComposite) {
            m_lastGoodComposite->gpuReady     = false;
            m_lastGoodComposite->gpuImageView = 0;
            m_lastGoodComposite->gpuSampler   = 0;
            m_lastGoodComposite->gpuSemaphore = 0;
        }
        return m_lastGoodComposite;
    }
    ++depth;
    struct DepthGuard {
        ~DepthGuard() { --compositeDepth(); }
    } depthGuard;

    // ── CPU Safe Mode (Phase 6/7) ────────────────────────────────────
    // When the GPU has failed persistently and backoff is exhausted, use
    // the minimal safe-mode compositor instead.  Safe mode produces one
    // frame every ~500ms using software decode + MemCpy — NOT for playback.
    // It's a recovery bridge to get back to GPU operation.
    // Phase 7.B: While in safe mode, periodically check GPU health and
    // auto-recover when the GPU becomes operational again.
    if (m_safeMode.load(std::memory_order_acquire)) {
        // Attempt periodic auto-recovery (throttled to every 5s internally)
        tryAutoRecoverFromSafeMode();

        // If recovery succeeded, m_safeMode is now false — fall through
        // to the normal GPU path below.
        if (m_safeMode.load(std::memory_order_acquire)) {
            // Still in safe mode — composite a safe-mode frame
            auto safeFrame = compositeSafeMode(tick, outW, outH);
            if (safeFrame) {
                // Update last-good-composite for continuity
                std::lock_guard lg(m_lastCompositeMtx);
                m_lastGoodComposite = safeFrame;
                m_lastGoodCompositeTick = tick;
                return safeFrame;
            }
            // compositeSafeMode returned nullptr (throttled) — return last
            // good frame if available
            std::lock_guard lg(m_lastCompositeMtx);
            if (m_lastGoodComposite) {
                m_lastGoodComposite->gpuReady     = false;
                m_lastGoodComposite->gpuImageView = 0;
                m_lastGoodComposite->gpuSampler   = 0;
                m_lastGoodComposite->gpuSemaphore = 0;
            }
            return m_lastGoodComposite;
        }
        // Recovery succeeded — fall through to normal GPU path
        spdlog::info("[SAFEMODE] Auto-recovery complete — resuming GPU compositing");
    }

    std::unique_lock lock(m_compositeMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        // If the composite cache was just invalidated, don't return the
        // stale m_lastGoodComposite -- return nullptr so the caller retries
        // next cycle with fresh data.  Otherwise keep the last good frame
        // on screen to prevent grey flashes during playback contention.
        if (m_cacheInvalidateRequested.load(std::memory_order_acquire))
            return nullptr;
        std::lock_guard lg(m_lastCompositeMtx);
        if (m_lastGoodComposite) {
            // Strip GPU-direct handles: the compositor's output texture may
            // have been resized or recreated since this frame was composited,
            // so its gpuImageView/gpuSampler are stale.  Returning a frame
            // with gpuReady=true would cause the VulkanViewport to sample
            // from destroyed resources, crashing the GPU driver.
            m_lastGoodComposite->gpuReady     = false;
            m_lastGoodComposite->gpuImageView = 0;
            m_lastGoodComposite->gpuSampler   = 0;
            m_lastGoodComposite->gpuSemaphore = 0;
        }
        return m_lastGoodComposite;
    }

    // Deferred cache invalidation: the UI thread may have requested
    // invalidation while we held the mutex (try_to_lock in
    // invalidateCompositeCache sets the atomic rather than racing).
    if (m_cacheInvalidateRequested.exchange(false, std::memory_order_acquire)) {
        if (m_engine)
            m_engine->clearLru();
        {
            std::lock_guard lg(m_lastCompositeMtx);
            m_lastGoodComposite.reset();
            m_lastGoodCompositeTick = -1;
        }
    }

    // All composites are now non-blocking — the scrub path was migrated from
    // blocking inline NVDEC decode (which held m_compositeMutex for 50-500ms,
    // freezing the UI) to the same non-blocking tryGetFrame that playback
    // uses.  Prefetch workers fill the cache asynchronously; the ProgramMonitor
    // settle mechanism retries until the frame arrives (~30-100ms).
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
            // Inspect the new clips' media:
            //   - If any path is not yet open: kick async opens.
            //   - If any first-frame is not yet cached: kick urgent prefetch.
            // In EITHER case we stay non-blocking and let the compositor
            // reuse the last-good composite for 1-2 frames.  This eliminates
            // the 200-1000ms "blocking decode" stalls that show up in the
            // perf log after every shot boundary, at the cost of a single
            // frame of "previous shot still showing" — which is what
            // Premiere/Resolve do under similar conditions.
            int unopenedCount = 0;
            int uncachedCount = 0;
            for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
                auto* track = m_timeline->track(ti - 1);
                if (!track || track->type() != TrackType::Video || track->isMuted())
                    continue;
                for (auto* clip : track->clipsAtTime(tick)) {
                    if (!clip || !clip->isEnabled()) continue;
                    if (m_lastActiveClipIds.find(clip->id()) != m_lastActiveClipIds.end())
                        continue; // not new
                    auto* videoClip = dynamic_cast<VideoClip*>(clip);
                    if (!videoClip) continue;
                    const auto& mp = videoClip->mediaPath();
                    if (mp.empty() || !m_mediaPool) continue;

                    if (!m_mediaPool->isPathOpen(mp)) {
                        m_mediaPool->openAsync(mp);
                        ++unopenedCount;
                        continue;
                    }
                    // Media is open — check whether the desired frame is
                    // already cached.  If not, kick urgent prefetch and
                    // stay non-blocking instead of paying ~150ms inline
                    // NVDEC seek+decode per uncached clip.
                    auto handle = m_mediaPool->open(mp);  // fast: already open
                    if (handle != 0) {
                        // Compute clip-relative source frame for current tick.
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
                            m_mediaPool->schedulePrefetch(handle, srcFrame - 1,
                                /*count=*/8, /*urgent=*/true, warmTier);
                            ++uncachedCount;
                        }
                    }
                }
            }

            // Always stay non-blocking at shot boundaries.  Worst case the
            // compositor renders the previous-shot composite for 1-2 frames
            // until prefetch lands the new frames.
            if (unopenedCount > 0 || uncachedCount > 0) {
                spdlog::info("[COMPOSITE] Shot boundary: {} new clips ({} unopened-async, {} uncached-prefetch) — staying non-blocking",
                             currentClipIds.size(), unopenedCount, uncachedCount);
                if (unopenedCount > 0) {
                    spdlog::warn("[LOOKAHEAD-MISS] shot-boundary caught {} clips with UNOPENED media — prewarm didn't reach far enough ahead or timeline was modified",
                                 unopenedCount);
                }
            } else {
                // All open and all frames already cached — fall through to
                // existing fast path; no special handling required.
            }
        }
        m_lastActiveClipIds = std::move(currentClipIds);
    #ifdef ROUNDTABLE_HAS_SPINE
        // Drop sticky pre-render frames for clips no longer active.
        for (auto it = m_lastPreRenderedSpineFrame.begin();
             it != m_lastPreRenderedSpineFrame.end();) {
            if (m_lastActiveClipIds.find(it->first) == m_lastActiveClipIds.end())
            it = m_lastPreRenderedSpineFrame.erase(it);
            else
            ++it;
        }
    #endif
    }

    // Update the frame scheduler's playhead so its lookahead window
    // aligns with the current composition position.  This ensures
    // schedulePrefetch() doesn't decode frames beyond the bounded window,
    // preventing queue buildup during long playback runs.
    if (m_mediaPool) {
        m_mediaPool->scheduler().setPlayhead(tick);
    }

    auto fetchMediaFrame = [&](MediaHandle handle, int64_t frameNumber,
                               ResolutionTier tier) -> std::shared_ptr<CachedFrame> {
        if (!m_mediaPool)
            return nullptr;
        // During playback AND scrub, always use non-blocking tryGetFrame.
        // Previously the scrub path called getFrame(scrubMode=true) which
        // did blocking inline NVDEC decode, holding m_compositeMutex for
        // 50-500ms and freezing the UI during playhead drag.  Now the
        // prefetch workers fill the cache asynchronously, and the
        // ProgramMonitor's settle mechanism (m_scrubSettleCounter × 16ms
        // = 240ms) retries until the frame lands.  The first retry after
        // the prefetch completes (typically 30-100ms) returns the cached
        // frame — imperceptibly later than blocking decode but without
        // freezing the entire UI.
        return m_mediaPool->tryGetFrame(handle, frameNumber, tier);
    };

    // Ã¢â€â‚¬Ã¢â€â‚¬ ONE-TIME STARTUP DIAGNOSTIC Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ PERF TIMING Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Always-on in Release builds for diagnostics.
    static int s_perfCounter = 0;
    const bool perfLog = (++s_perfCounter % 10 == 0);
    auto perfT0 = std::chrono::high_resolution_clock::now();
    auto perfTlayers = perfT0;
    auto perfTgpuUp  = perfT0;
    auto perfTcomp   = perfT0;

    // ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ Composite result LRU cache ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬
    // Check if we already composited this tick at this resolution.
    // Skip during scrub: the scrub decode path may be still catching up,
    // so a cached result from an earlier attempt at the same tick may have
    // missing layers.  Re-compositing is cheap (~2ms GPU).
    // Keep the LRU during export (forceFullRes) since frames are sequential
    // and there's no decode catch-up issue.
    if (m_engine && (!scrubMode || m_forceFullResolution.load())) {
        auto cached = m_engine->checkLru(tick, outW, outH);
        if (cached)
            return cached;
    }

    // If the output resolution changed, flush the LRU.
    if (m_engine) {
        m_engine->flushLruOnResize(outW, outH);
    }

    // Output resolution from caller (viewport actual pixel size).
    // Minimum sanity bounds.
    if (outW < 64) outW = 64;
    if (outH < 36) outH = 36;

    // Collect decoded layers from video tracks (bottom-up for compositing).
    // Premiere Pro order: V1 = lowest visual layer, V3 = topmost.
    // Tracks are stored with the topmost timeline track at index 0 (V3),
    // so we iterate in REVERSE to composite V1 first (bottom), V3 last (top).
    // LayerInfo is defined in CompositeServiceLayerBuild.h.
    std::vector<LayerInfo> layers;

    

    // Iterate video tracks in REVERSE index order so that the
    // highest-indexed video track (V1 = bottom of timeline UI)
    // is composited FIRST (behind), and the lowest-indexed video
    // track (V3 = top of timeline UI) is composited LAST (in front).
    //
    // Track storage:  index 0 = topmost UI row (V3), index N = bottom (V1).
    // Premiere convention: higher V number = visually on top.
    // GPU/CPU compositor draws layers[0] first (behind), layers[N] last.
    //
    // So we iterate from trackCount-1 down to 0, pushing V1 first, V3 last.
        int clipsAtTick = 0;   // count enabled clips that attempted rendering
    layers = buildLayersForFrame(tick, outW, outH, scrubMode, playbackNonBlocking,
                                 clipsAtTick, perfLog, lock, gpuSpineUsedThisFrame);


    if (layers.empty()) {
        static int s_emptyLog = 0;
        if (++s_emptyLog <= 20 || s_emptyLog % 60 == 0) {
            spdlog::info("[FLICKER-DIAG] compositeFrame tick={}: layers empty "
                         "(clipsAtTick={}, all skipped)",
                         tick, clipsAtTick);
        }

        // If clips existed at this tick but all decodes failed (transient
        // cache miss / decode hiccup), keep the previous good frame on
        // screen instead of flashing grey — but ONLY if the previous frame
        // is from a nearby tick (same shot).  If the tick is far away,
        // return nullptr to avoid showing the wrong character.
        if (clipsAtTick > 0) {
            std::lock_guard lg(m_lastCompositeMtx);
            if (m_lastGoodComposite && m_lastGoodCompositeTick >= 0) {
                constexpr int64_t kMaxStaleTicks = 48000 / 12; // ~2 frames at 24fps
                int64_t tickDelta = std::abs(tick - m_lastGoodCompositeTick);
                if (tickDelta <= kMaxStaleTicks)
                    return m_lastGoodComposite;
                // Stale frame from different shot — don't show it
                spdlog::info("[COMPOSITE] Suppressing stale lastGoodComposite "
                             "(tick={} vs last={}, delta={})",
                             tick, m_lastGoodCompositeTick, tickDelta);
            }
        }

        // Genuinely empty timeline at this tick (no clips).
        // Return a sentinel "empty" frame (width==0) so ProgramMonitor
        // can clear the display.
        auto emptyFrame = std::make_shared<CachedFrame>();
        {
            std::lock_guard lg(m_lastCompositeMtx);
            m_lastGoodComposite.reset();  // timeline truly empty
            m_lastGoodCompositeTick = -1;
        }
        return emptyFrame;
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

    // Single layer fast path ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â only when transforms are identity and no crop
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ PERF: layers collected Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    perfTlayers = std::chrono::high_resolution_clock::now();

    // ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ GPU compositing path ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚ÂÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬
        perfTlayers = std::chrono::high_resolution_clock::now();

    // GPU compositing path â€” extracted to tryCompositeOnGpu()
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
    // GPU failed â€” check for safe mode activation
    // When the GPU has failed persistently, enter CPU safe mode.
    int backoffAttempts = m_engine ? m_engine->backoffAttempts() : 0;
    if (backoffAttempts >= 3) {
        spdlog::warn("[SAFEMODE] GPU backoff exhausted ({} attempts) — entering CPU safe mode",
                     backoffAttempts);
        m_safeMode.store(true, std::memory_order_release);

        // Notify the UI layer (ProgramMonitor) that safe mode was entered
        if (m_safeModeCallback)
            m_safeModeCallback(true);

        auto safeFrame = compositeSafeMode(tick, outW, outH);
        if (safeFrame) {
            std::lock_guard lg(m_lastCompositeMtx);
            m_lastGoodComposite = safeFrame;
            m_lastGoodCompositeTick = tick;
            return safeFrame;
        }
        // Safe mode throttled or no content — return last good frame
        spdlog::info("[SAFEMODE] compositeSafeMode throttled or null, using last good frame");
        std::lock_guard lg(m_lastCompositeMtx);
        if (m_lastGoodComposite) {
            m_lastGoodComposite->gpuReady     = false;
            m_lastGoodComposite->gpuImageView = 0;
            m_lastGoodComposite->gpuSampler   = 0;
            m_lastGoodComposite->gpuSemaphore = 0;
        }
        return m_lastGoodComposite;
    }

    // Still in GPU cooldown — use legacy CPU fallback path.
    int gpuBackoffCount = m_engine ? m_engine->backoffAttempts() : 0;
    spdlog::warn("compositeFrame: GPU composite FAILED (backoff {}/3) — using legacy CPU path",
                 gpuBackoffCount);
// Multiple layers ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â CPU alpha composite with transforms (bottom to top)
    // Reuse composite buffer to avoid 8MB allocation + zero per frame.
    if (!m_compositeBuffer || m_compositeBuffer->width != outW ||
        m_compositeBuffer->height != outH) {
        m_compositeBuffer = std::make_shared<CachedFrame>();
        m_compositeBuffer->width  = outW;
        m_compositeBuffer->height = outH;
        m_compositeBuffer->stride = outW * 4;
        m_compositeBuffer->pixels.resize(static_cast<size_t>(outW) * outH * 4);
    }
    // Initialize to OPAQUE black (BGRA: 0,0,0,255).  This matches the GPU
    // compositor clear color and ensures video layers display correctly even
    // without a background layer.  Uses 32-bit writes (4x faster than
    // per-byte loop, and sets all 4 bytes in one store).
    {
        uint32_t* pixels32 = reinterpret_cast<uint32_t*>(m_compositeBuffer->pixels.data());
        const size_t nPixels = static_cast<size_t>(outW) * outH;
        // 0xFF000000 = A=255, R=0, G=0, B=0 in BGRA byte order
        std::fill_n(pixels32, nPixels, 0xFF000000u);
    }
    auto result = m_compositeBuffer;

    for (const auto& layer : layers) {
        // Zero-copy GPU layers have no CachedFrame ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â skip in CPU fallback
        if (!layer.frame) continue;
        auto& src = *layer.frame;
        if (!src.ensurePixels()) continue;

        const uint32_t srcStride = src.stride > 0 ? src.stride : src.width * 4;

        blitLayerWithTransform(
            result->pixels.data(), outW, outH,
            src.pixels.data(), src.width, src.height, srcStride,
            layer.opacity,
            layer.posX, layer.posY,
            layer.scX, layer.scY,
            layer.rot,
            layer.cropL, layer.cropR, layer.cropT, layer.cropB,
            layer.containFit);
    }

    // Insert into composite result LRU cache
    if (m_engine)
        m_engine->insertLru(tick, outW, outH, result);

    {
        std::lock_guard lg(m_lastCompositeMtx);
        m_lastGoodComposite = result;
        m_lastGoodCompositeTick = tick;
    }
    return result;
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
