/*
 * MediaPoolPrefetchSchedule.cpp — Prefetch scheduling and worker logic
 * extracted from MediaPoolPrefetch.cpp.
 *
 * Contains: schedulePrefetch(), prefetchWorker().
 */

#include "MediaPool.h"
#include "MediaPoolPrefetchInternal.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

#include <algorithm>
#include <unordered_map>

namespace rt {

void MediaPool::schedulePrefetch(MediaHandle handle, int64_t afterFrame, int count, bool urgent, ResolutionTier tier)
{
    m_playheadFrame.store(afterFrame, std::memory_order_relaxed);
    const bool interactivePlayback = urgent && isInteractivePlaybackActive(m_interactivePlaybackUntilMs);

    // Cooldown: skip queue rebuild if we recently scheduled for this handle.
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_prefetchMutex);
        auto it = m_lastScheduleTime.find(handle);
        if (it != m_lastScheduleTime.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
            const int cooldownMs = interactivePlayback ? 120 : 40;
            if (elapsed.count() < cooldownMs) {
                m_prefetchCv.notify_all();
                return;
            }
        }
        m_lastScheduleTime[handle] = now;
    }

    std::filesystem::path path;
    VideoStreamInfo info;
    bool packed = false;
    bool isLooping = false;
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (!entry) return;
        info = entry->info;
        path = entry->path;
        packed = entry->packedAlpha;
        isLooping = entry->loopPreDecodeStarted;
    }
    const int64_t maxFrame = info.frameCount > 0 ? info.frameCount - 1 : INT64_MAX;
    const int64_t fc = info.frameCount;

    if (info.frameCount <= 1) {
        if (m_cache->contains(handle, 0, tier))
            return;
        count = 1;
        // CONSUMERS (CompositeServiceLayerBuild, FrameRenderer, …) always
        // look up still images at frame 0.  If we left afterFrame as-is
        // (e.g. 20 because the timeline playhead is on tick 20), the task
        // below sets task.frameNumber = afterFrame, the decoded result is
        // stored under (handle, 20, tier), and every subsequent consumer
        // lookup at frame 0 misses the cache and triggers another full
        // PNG software-decode (~150 ms for a 4K still).  Force-align the
        // prefetch index to 0 so the producer and consumer agree.
        afterFrame = 0;
    }

    // Skip if loop pre-decode already finished for this handle.
    {
        std::lock_guard lock(m_loopPreDecodeMutex);
        if (m_loopPreDecodeDone.count(handle)) {
            return;
        }
    }

    {
        std::lock_guard lock(m_prefetchMutex);

        // Remove tasks for this handle + prune stale frames behind playhead.
        {
            const int64_t playhead = m_playheadFrame.load(std::memory_order_relaxed);
            m_prefetchQueue.erase(
                std::remove_if(m_prefetchQueue.begin(), m_prefetchQueue.end(),
                    [handle, playhead](const PrefetchTask& t) {
                        if (t.handle == handle) return true;
                        if (t.frameNumber < playhead - 4) return true;
                        return false;
                    }),
                m_prefetchQueue.end());
        }

        const size_t kMaxQueueSize = interactivePlayback ? 32u : 64u;
        while (m_prefetchQueue.size() > kMaxQueueSize) {
            m_prefetchQueue.pop_back();
        }

        const double projFps = m_projectFps.load(std::memory_order_relaxed);
        const int stride = (info.fps > projFps * 1.4)
            ? std::max(1, static_cast<int>(std::round(info.fps / projFps)))
            : 1;
        const int effectiveCount = interactivePlayback ? std::min(count, 16) : count;
        // Update scheduler playhead for lookahead bounding.
        m_scheduler.setPlayhead(afterFrame);
        for (int i = 0; i < effectiveCount; ++i) {
            int64_t fn = afterFrame + i * stride;

            if (fn > maxFrame) {
                if (isLooping && fc > 1) {
                    fn = ((fn % fc) + fc) % fc;
                } else {
                    break;
                }
            }

            if (m_cache->contains(handle, fn, tier))
                continue;

            // Scheduler lookahead gate: skip frames outside the bounded
            // lookahead window (unless urgent).
            if (!m_scheduler.withinLookahead(fn, afterFrame) && !(urgent && i == 0)) {
                if (i > 0) break;
                continue;
            }

            PrefetchTask task;
            task.handle      = handle;
            task.filePath    = path;
            task.frameNumber = fn;
            task.tier        = tier;
            task.fps         = info.fps > 0 ? info.fps : 30.0;
            task.info        = info;
            task.packedAlpha = packed;
            task.urgent      = (urgent && i == 0);
            task.isLoop      = isLooping;
            if (task.urgent) {
                m_prefetchQueue.push_front(task);
            } else {
                const int64_t playhead = m_playheadFrame.load(std::memory_order_relaxed);
                const int64_t dist = std::abs(fn - playhead);
                auto it = m_prefetchQueue.begin();
                while (it != m_prefetchQueue.end() && it->urgent) ++it;
                while (it != m_prefetchQueue.end()) {
                    const int64_t existDist = std::abs(it->frameNumber - playhead);
                    if (existDist > dist) break;
                    ++it;
                }
                m_prefetchQueue.insert(it, task);
            }
            m_perf.prefetchScheduled.fetch_add(1, std::memory_order_relaxed);
        }
    }
    m_prefetchCv.notify_all();
}

void MediaPool::prefetchWorker(int workerId)
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    spdlog::info("MediaPool: prefetch worker {} started (ABOVE_NORMAL priority)", workerId);

    std::unordered_map<MediaHandle, PrefetchDecoderState> decoders;
    size_t queueSize = 0;
    int skippedCached = 0;
    int decodedCount = 0;

    while (m_prefetchRunning) {
        PrefetchTask task;
        {
            std::unique_lock lock(m_prefetchMutex);
            m_prefetchCv.wait(lock, [this] {
                return !m_prefetchQueue.empty() || !m_prefetchRunning;
            });
            if (!m_prefetchRunning) break;
            if (m_prefetchQueue.empty()) continue;

            queueSize = m_prefetchQueue.size();

            // Release stale ownerships.
            for (auto it = m_prefetchPackedOwner.begin(); it != m_prefetchPackedOwner.end();) {
                if (it->second == workerId) {
                    bool hasTask = std::any_of(
                        m_prefetchQueue.begin(), m_prefetchQueue.end(),
                        [h = it->first](const PrefetchTask& qt) { return qt.handle == h; });
                    const bool decoderWarm = decoders.count(it->first) > 0;
                    if (!hasTask && !decoderWarm) { it = m_prefetchPackedOwner.erase(it); continue; }

                    bool anyPackedAlpha = false;
                    bool sawTask = false;
                    for (const auto& qt : m_prefetchQueue) {
                        if (qt.handle == it->first) {
                            anyPackedAlpha = qt.packedAlpha;
                            sawTask = true;
                            break;
                        }
                    }
                    if (sawTask) {
                        // Only NVDEC-vs-packed-alpha is a hard mismatch:
                        // NVDEC cannot decode packed-alpha sources, so an
                        // NVDEC worker that took ownership of one must hand
                        // it back to a SW worker. Plain (non-packed) video
                        // is fine on either pool, so we no longer evict a
                        // SW worker that owns a non-packed handle — that
                        // change is what lets SW workers help on plain
                        // H.264, instead of yielding back to the two NVDEC
                        // workers and starving the cache during cold-start.
                        const bool nvdecHoldsAlpha =
                            (workerId < PREFETCH_NVDEC_WORKERS) && anyPackedAlpha;
                        if (nvdecHoldsAlpha) {
                            auto dit = decoders.find(it->first);
                            if (dit != decoders.end()) {
#ifdef ROUNDTABLE_HAS_FFMPEG
                                if (dit->second.swsCtx)
                                    sws_freeContext(static_cast<SwsContext*>(dit->second.swsCtx));
#endif
                                decoders.erase(dit);
                            }
                            it = m_prefetchPackedOwner.erase(it);
                            m_prefetchCv.notify_all();
                            continue;
                        }
                    }
                }
                ++it;
            }

            // Per-handle ownership: one worker per handle for sequential decode.
            // Affinity:
            //   - NVDEC workers (id < PREFETCH_NVDEC_WORKERS) refuse packed-alpha
            //     tasks because NVDEC cannot decode them.
            //   - SW workers (id >= PREFETCH_NVDEC_WORKERS) accept BOTH packed
            //     and plain video. Previously SW workers refused non-packed
            //     tasks, which meant only the 2 NVDEC workers could prefetch
            //     plain H.264 — and per-handle ownership reduced that to 1
            //     effective worker per file. The 8 SW workers sat idle during
            //     cold-start, starving the FrameCache. With this relaxed rule
            //     SW workers help fill the cache for plain video, particularly
            //     when several H.264 clips are first encountered together.
            auto acceptable = [&](const PrefetchTask& t) -> bool {
                if (workerId < PREFETCH_NVDEC_WORKERS && t.packedAlpha)
                    return false;
                auto ownerIt = m_prefetchPackedOwner.find(t.handle);
                if (ownerIt == m_prefetchPackedOwner.end()) return true;
                return ownerIt->second == workerId;
            };

            auto pick = std::find_if(m_prefetchQueue.begin(),
                                     m_prefetchQueue.end(), acceptable);
            if (pick == m_prefetchQueue.end()) {
                m_prefetchCv.wait_for(lock, std::chrono::milliseconds(2));
                continue;
            }
            m_prefetchPackedOwner.emplace(pick->handle, workerId);
            task = *pick;
            m_prefetchQueue.erase(pick);

            // Batch-claim consecutive frames to avoid seek contention.
            constexpr int kBatchClaim = 30;
            {
                int64_t claimFn = task.frameNumber + 1;
                const int64_t claimMax = task.frameNumber + kBatchClaim;
                auto it = m_prefetchQueue.begin();
                while (it != m_prefetchQueue.end() && claimFn <= claimMax) {
                    if (it->handle == task.handle && it->frameNumber == claimFn) {
                        it = m_prefetchQueue.erase(it);
                        ++claimFn;
                    } else {
                        ++it;
                    }
                }
            }
        }

        if (m_cache->contains(task.handle, task.frameNumber, task.tier)) {
            ++skippedCached;
            continue;
        }

        const size_t maxDecoders = (workerId < PREFETCH_NVDEC_WORKERS) ? 8 : 4;

        bool isNewHandle = (decoders.find(task.handle) == decoders.end());
        if (isNewHandle && decoders.size() >= maxDecoders) {
            MediaHandle evictHandle = 0;
            auto oldestTime = std::chrono::steady_clock::time_point::max();
            for (auto& [h, s] : decoders) {
                if (s.decoder && s.lastUsed < oldestTime) {
                    oldestTime = s.lastUsed;
                    evictHandle = h;
                }
            }
            if (evictHandle != 0) {
                auto eit = decoders.find(evictHandle);
                if (eit != decoders.end()) {
#ifdef ROUNDTABLE_HAS_FFMPEG
                    if (eit->second.swsCtx)
                        sws_freeContext(static_cast<SwsContext*>(eit->second.swsCtx));
#endif
                    decoders.erase(eit);
                    {
                        std::lock_guard lk(m_prefetchMutex);
                        auto oit = m_prefetchPackedOwner.find(evictHandle);
                        if (oit != m_prefetchPackedOwner.end() && oit->second == workerId)
                            m_prefetchPackedOwner.erase(oit);
                    }
                    spdlog::debug("MediaPool prefetch[{}]: evicted decoder for handle {} (limit={})",
                                  workerId, evictHandle, maxDecoders);
                }
            }
        }

        auto& state = decoders[task.handle];
        state.lastUsed = std::chrono::steady_clock::now();
        if (!state.decoder) {
            auto openT0 = std::chrono::steady_clock::now();
            state.decoder = std::make_unique<VideoDecoder>();
            const bool useHwDecode = (workerId < PREFETCH_NVDEC_WORKERS)
                                   && !task.info.hasAlpha;
            const int swThreads = useHwDecode ? 0 : 2;
            if (!state.decoder->open(task.filePath, /*forceSoftware=*/!useHwDecode, /*maxThreads=*/swThreads)) {
                spdlog::warn("MediaPool prefetch[{}]: failed to open decoder for handle {} '{}'",
                             workerId, task.handle, task.filePath.filename().string());
                state.decoder.reset();
                continue;
            }
            auto openMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - openT0).count();
            spdlog::info("MediaPool prefetch[{}]: opened {} decoder for handle {} '{}' in {:.0f}ms",
                         workerId, useHwDecode ? "NVDEC" : "SW",
                         task.handle, task.filePath.filename().string(), openMs);
        }

        auto decT0 = std::chrono::steady_clock::now();
        auto frame = decodePrefetchFrame(state, task);
        auto decMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - decT0).count();
        if (decMs > 80.0) {
            spdlog::info("[PERF SLOW DECODE] prefetch[{}] handle={} frame={} took {:.0f}ms "
                         "(packedAlpha={} hasAlpha={})",
                         workerId, task.handle, task.frameNumber, decMs,
                         task.packedAlpha, task.info.hasAlpha);
        }
        if (frame) {
            m_cache->put(frame);
            if (m_diskCache) m_diskCache->putAsync(frame);
            m_perf.prefetchDeliveries.fetch_add(1, std::memory_order_relaxed);
            ++decodedCount;
            if (decodedCount % 30 == 1) {
                spdlog::info("[PERF] prefetch[{}]: decoded handle={} frame={} {}x{} "
                             "queueDepth={} skippedCached={} totalDecoded={}",
                             workerId, task.handle, task.frameNumber,
                             frame->width, frame->height,
                             queueSize, skippedCached, decodedCount);
                skippedCached = 0;
            }

            // Sequential follow-up: decode next consecutive frames while decoder is positioned.
            constexpr int kMaxFollowUp = 30;
            // For stills (frameCount 0 or 1) there is no "next consecutive
            // frame" — the previous version defaulted to INT64_MAX when
            // frameCount <= 1, which made the follow-up loop decode 30
            // phantom frames against an image2 demuxer (each producing the
            // same PNG over and over, hundreds of ms of wasted software
            // decode per still per scheduling cycle).
            const int64_t maxFollow = (task.info.frameCount > 1)
                ? task.info.frameCount - 1
                : (task.info.frameCount == 1 ? 0 : -1);

            ResolutionTier followTier = task.tier;
            {
                std::lock_guard fLock(m_prefetchMutex);
                for (const auto& qt : m_prefetchQueue) {
                    if (qt.handle == task.handle) {
                        followTier = qt.tier;
                        break;
                    }
                }
            }

            for (int f = 1; f <= kMaxFollowUp; ++f) {
                const int64_t nextFn = task.frameNumber + f;
                if (nextFn > maxFollow) break;
                if (m_cache->contains(task.handle, nextFn, followTier)) continue;
                if (!m_prefetchRunning) break;

                // Bail on urgent task for a DIFFERENT handle (same handle is fine).
                if (f > 3) {
                    std::lock_guard uLock(m_prefetchMutex);
                    for (const auto& qt : m_prefetchQueue) {
                        if (qt.urgent && qt.handle != task.handle) {
                            goto endFollowUp;
                        }
                    }
                }

                PrefetchTask followTask = task;
                followTask.frameNumber = nextFn;
                followTask.tier        = followTier;
                followTask.urgent      = false;

                auto followFrame = decodePrefetchFrame(state, followTask);
                if (followFrame) {
                    m_cache->put(followFrame);
                    if (m_diskCache) m_diskCache->putAsync(followFrame);
                    m_perf.prefetchDeliveries.fetch_add(1, std::memory_order_relaxed);
                    ++decodedCount;
                } else {
                    break;
                }
            }
            endFollowUp:;
        }
    }

    // Cleanup thread-local decoders and sws contexts
#ifdef ROUNDTABLE_HAS_FFMPEG
    for (auto& [h, state] : decoders) {
        if (state.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(state.swsCtx));
    }
#endif
    spdlog::debug("MediaPool: prefetch worker {} exiting", workerId);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
