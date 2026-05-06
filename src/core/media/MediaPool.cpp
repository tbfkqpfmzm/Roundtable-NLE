/*
 * MediaPool.cpp â€” manages decoders and shared frame cache
 */

#include "MediaPool.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <spdlog/spdlog.h>
#include <chrono>

#include "GpuContext.h"
#include "Nv12Converter.h"
#include "cuda/CudaVulkanInterop.h"

// volk provides Vulkan function pointers (VK_NO_PROTOTYPES is defined globally)
#include <volk.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cstdlib>
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace rt {

namespace {

constexpr int64_t kInteractivePlaybackDeferMs = 2500;

int64_t steadyClockMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void extendInteractivePlaybackWindow(std::atomic<int64_t>& untilMs)
{
    const int64_t candidate = steadyClockMillis() + kInteractivePlaybackDeferMs;
    int64_t current = untilMs.load(std::memory_order_relaxed);
    while (current < candidate &&
           !untilMs.compare_exchange_weak(current, candidate,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
    }
}

} // namespace

// â”€â”€â”€ Packed-alpha unpack helper â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

/// Unpack a packed-alpha frame in-place.  Packed layout: top half = RGB
/// (A=255), bottom half = alpha as greyscale.  After unpack, the frame
/// Check if a codec name corresponds to ProRes.
static bool isProResCodec(const std::string& codecName)
{
    return codecName.find("prores") != std::string::npos;
}

// â”€â”€â”€ Construction â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

MediaPool::MediaPool(std::shared_ptr<FrameCache> cache)
    : m_cache(cache ? std::move(cache) : std::make_shared<FrameCache>())
    , m_pixelPool(std::make_shared<PixelBufferPool>())
{
    // Configure the scheduler lookahead to match the existing prefetch window.
    m_scheduler.setMaxLookahead(PREFETCH_AHEAD_COUNT);
    m_scheduler.setMaxWorkers(PREFETCH_THREAD_COUNT);

    startPrefetchThread();
    startOpenWorker();
}

MediaPool::~MediaPool()
{
    stopOpenWorker();
    stopPrefetchThread();
    closeAll();
}

// â”€â”€â”€ Open / Close â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

MediaHandle MediaPool::open(const std::filesystem::path& filePath)
{
    auto openT0 = std::chrono::steady_clock::now();

    // Canonicalize the path for dedup (filesystem I/O — outside lock)
    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string key = ec ? filePath.string() : canonical.string();

    // ── Fast path: already open or known-failed? ─────────────────────
    {
        std::lock_guard lock(m_mutex);

        // Return immediately for paths that already failed to open.
        // Prevents per-frame filesystem I/O for missing media files.
        if (m_failedPaths.count(key)) {
            return InvalidMedia;
        }

        auto pathIt = m_pathToHandle.find(key);
        if (pathIt != m_pathToHandle.end()) {
            auto entryIt = m_entries.find(pathIt->second);
            if (entryIt != m_entries.end()) {
                entryIt->second.refCount++;
                spdlog::debug("MediaPool: reusing handle {} for '{}' (refCount={})",
                              pathIt->second, filePath.filename().string(),
                              entryIt->second.refCount);
                return pathIt->second;
            }
        }
    }

    // ── Slow path: open decoder WITHOUT holding the mutex ────────────
    // The ffmpeg avformat_open_input() probe can take 50-200+ ms.
    // Holding m_mutex during the probe would block prefetch workers,
    // thumbnail generation, and other concurrent MediaPool callers.
    auto decoder = std::make_unique<VideoDecoder>();
    std::filesystem::path actualFilePath = filePath;
    bool directOpen = false;
    {
        namespace fs = std::filesystem;
        std::error_code existsEc;
        if (fs::exists(filePath, existsEc))
            directOpen = decoder->open(filePath.string(), /*forceSoftware=*/false, /*maxThreads=*/4);
    }
    if (!directOpen) {
        // â”€â”€ Search common asset directories for the file â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        namespace fs = std::filesystem;
        fs::path filename = filePath.filename();

        // Build a list of candidate paths to try
        std::vector<fs::path> candidates;

        // Alternate extensions — prefer .mp4 (NVDEC hw decode) over .mov (ProRes sw decode)
        std::vector<fs::path> altExts;
        if (filePath.extension() == ".webm") {
            altExts.push_back(fs::path(filePath).replace_extension(".mp4"));
            altExts.push_back(fs::path(filePath).replace_extension(".mov"));
        } else if (filePath.extension() == ".mov") {
            altExts.push_back(fs::path(filePath).replace_extension(".mp4"));
            altExts.push_back(fs::path(filePath).replace_extension(".webm"));
        } else if (filePath.extension() == ".mp4") {
            altExts.push_back(fs::path(filePath).replace_extension(".webm"));
            altExts.push_back(fs::path(filePath).replace_extension(".mov"));
        }

        // Asset search directories
        const fs::path searchDirs[] = {
            fs::path("assets") / "backgrounds",
            fs::path("assets") / "videos",
            fs::path("assets"),
        };

        // Original filename in each search dir
        for (const auto& dir : searchDirs)
            candidates.push_back(dir / filename);

        // Alternate extensions at original path and in search dirs
        for (const auto& altExt : altExts) {
            candidates.push_back(altExt);
            fs::path altName = altExt.filename();
            for (const auto& dir : searchDirs)
                candidates.push_back(dir / altName);
        }

        bool found = false;
        for (const auto& candidate : candidates) {
            std::error_code ec2;
            if (!fs::exists(candidate, ec2)) continue;
            decoder = std::make_unique<VideoDecoder>();
            if (decoder->open(candidate.string(), /*forceSoftware=*/false, /*maxThreads=*/4)) {
                spdlog::info("MediaPool: resolved '{}' â†’ '{}'",
                             filePath.string(), candidate.string());
                actualFilePath = candidate;
                found = true;
                break;
            }
        }

        if (!found) {
            spdlog::error("MediaPool: failed to open '{}' (caching as failed)", filePath.string());
            {
                std::lock_guard lock(m_mutex);
                m_failedPaths.insert(key);
            }
            return InvalidMedia;
        }
    }

    // â”€â”€ ProRes native decode (no transcoding) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // ProRes 4444 is our preferred format for alpha video: intra-frame codec with
    // native YUVA alpha.  Software decode is fast enough for 1080p on modern CPUs.
    // No auto-transcode â€” keep the ProRes quality and instant random access.
    std::filesystem::path actualPath = actualFilePath;
    if (isProResCodec(decoder->info().codecName))
    {
        spdlog::info("MediaPool: using ProRes native decode for '{}' (sw, native alpha)",
                     filePath.filename().string());
        // Fall through â€” use the software decoder as-is.
        // ProRes 4444 intra-frame: every frame is a keyframe, instant random access,
        // native YUVA444P10LE alpha.  No transcoding needed.
    }

    // Re-acquire lock to store the new entry.
    // Double-check dedup: another thread may have opened the same file
    // while we were probing without the lock.
    std::lock_guard lock(m_mutex);

    {
        auto pathIt2 = m_pathToHandle.find(key);
        if (pathIt2 != m_pathToHandle.end()) {
            auto entryIt2 = m_entries.find(pathIt2->second);
            if (entryIt2 != m_entries.end()) {
                entryIt2->second.refCount++;
                spdlog::debug("MediaPool: race dedup — reusing handle {} for '{}'",
                              pathIt2->second, filePath.filename().string());
                return pathIt2->second;
            }
        }
    }

    MediaHandle handle = m_nextHandle++;
    MediaEntry entry;
    entry.handle      = handle;
    entry.path        = actualPath;
    entry.info        = decoder->info();
    entry.packedAlpha = entry.info.packedAlpha;  // propagate packed-alpha detection
    entry.decoder     = std::move(decoder);
    entry.refCount    = 1;

    spdlog::info("MediaPool: opened '{}' handle={} ({}x{}, {:.2f}fps, {:.1f}s, {} frames, hw={})",
                 actualPath.filename().string(), handle,
                 entry.info.width, entry.info.height,
                 entry.info.fps, entry.info.duration, entry.info.frameCount,
                 entry.decoder->isHardwareAccelerated() ? "yes" : "no");

    m_pathToHandle[key] = handle;

    // If we transcoded, also map the original path so future opens of the
    // .mov file find the transcoded entry directly.
    if (actualPath != filePath)
    {
        std::error_code ecOrig;
        auto origCanonical = std::filesystem::canonical(filePath, ecOrig);
        std::string origKey = ecOrig ? filePath.string() : origCanonical.string();
        m_pathToHandle[origKey] = handle;
    }

    m_entries.emplace(handle, std::move(entry));

    // Register with disk cache for persistent cross-session caching
    if (m_diskCache)
        m_diskCache->registerMedia(handle, actualPath);

    {
        double openMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - openT0).count();
        if (openMs > 20.0)
            spdlog::info("[PERF] MediaPool::open '{}': {:.0f}ms",
                         filePath.filename().string(), openMs);
    }

    return handle;
}

void MediaPool::release(MediaHandle handle)
{
    std::lock_guard lock(m_mutex);

    auto it = m_entries.find(handle);
    if (it == m_entries.end()) return;

    it->second.refCount--;
    if (it->second.refCount <= 0) {
        spdlog::info("MediaPool: closing handle {} '{}'",
                     handle, it->second.path.filename().string());

        // Remove from path map
        std::error_code ec;
        auto canonical = std::filesystem::canonical(it->second.path, ec);
        std::string key = ec ? it->second.path.string() : canonical.string();
        m_pathToHandle.erase(key);

        // Evict cached frames for this media
        m_cache->evictMedia(handle);

        // Free cached sws context and close decoder
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (it->second.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(it->second.swsCtx));
#endif
        m_entries.erase(it);

        // Also clean up the prefetch decoder for this handle
        {
            std::lock_guard pfLock(m_prefetchMutex);
            // Remove queued tasks for this handle
            m_prefetchQueue.erase(
                std::remove_if(m_prefetchQueue.begin(), m_prefetchQueue.end(),
                    [handle](const PrefetchTask& t) { return t.handle == handle; }),
                m_prefetchQueue.end());
        }
        // Cancel any pending scheduler work for this handle.
        m_scheduler.cancel(handle);
        // Note: prefetch decoder cleanup is thread-local in each worker.
        // Workers will detect that the handle is invalid on next use.

        // Clean up dedicated scrub decoder for this handle
        auto scrubIt = m_scrubDecoders.find(handle);
        if (scrubIt != m_scrubDecoders.end()) {
#ifdef ROUNDTABLE_HAS_FFMPEG
            if (scrubIt->second.swsCtx)
                sws_freeContext(static_cast<SwsContext*>(scrubIt->second.swsCtx));
#endif
            m_scrubDecoders.erase(scrubIt);
        }
    }
}

void MediaPool::closeAll()
{
    // Flush the prefetch queue first
    {
        std::lock_guard pfLock(m_prefetchMutex);
        m_prefetchQueue.clear();
    }
    // Cancel all pending scheduler work (e.g. on seek / project close).
    m_scheduler.cancelAll();

    std::lock_guard lock(m_mutex);
#ifdef ROUNDTABLE_HAS_FFMPEG
    for (auto& [h, entry] : m_entries) {
        if (entry.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(entry.swsCtx));
    }
#endif
    m_entries.clear();
    m_pathToHandle.clear();
    m_cache->clear();
    // Clean up scrub decoders
#ifdef ROUNDTABLE_HAS_FFMPEG
    for (auto& [h, s] : m_scrubDecoders) {
        if (s.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(s.swsCtx));
        s.swsCtx = nullptr;
    }
#endif
    m_scrubDecoders.clear();
}
// --- Scrub decoder management ------------------------------------------------

PrefetchDecoderState& MediaPool::getScrubDecoder(
    MediaHandle handle, const std::filesystem::path& path, const VideoStreamInfo& /*info*/)
{
    auto& state = m_scrubDecoders[handle];
    if (!state.decoder) {
        state.decoder = std::make_unique<VideoDecoder>();
        // forceSoftware=true (NVDEC cold-init is 650ms), maxThreads=2,
        // sliceOnlyThreading=true (scrub decoders seek constantly —
        // FF_THREAD_FRAME causes H.264 reference frame corruption after
        // avcodec_flush_buffers, producing distorted multi-copy artifacts)
        if (!state.decoder->open(path, /*forceSoftware=*/true, /*maxThreads=*/2, /*sliceOnlyThreading=*/true)) {
            spdlog::warn("MediaPool: scrub decoder open failed for handle={}", handle);
            state.decoder.reset();
        }
        state.lastDecodedFrame = -1;
    }
    return state;
}

// --- Frame access -------------------------------------------------------------
void MediaPool::logPerfReport()
{
    auto hits     = m_perf.cacheHits.exchange(0, std::memory_order_relaxed);
    auto nearby   = m_perf.nearbyHits.exchange(0, std::memory_order_relaxed);
    auto stale    = m_perf.staleReturns.exchange(0, std::memory_order_relaxed);
    auto inlined  = m_perf.inlineDecodes.exchange(0, std::memory_order_relaxed);
    auto prefetch = m_perf.prefetchDeliveries.exchange(0, std::memory_order_relaxed);
    auto misses   = m_perf.totalMisses.exchange(0, std::memory_order_relaxed);
    auto total    = m_perf.totalRequests.exchange(0, std::memory_order_relaxed);
    auto sched    = m_perf.prefetchScheduled.exchange(0, std::memory_order_relaxed);
    auto avgUs    = m_perf.avgDecodeUs.load(std::memory_order_relaxed);

    if (total == 0) return; // nothing to report

    double hitPct   = total > 0 ? (hits * 100.0 / total) : 0.0;
    double dropPct  = total > 0 ? ((nearby + stale + misses) * 100.0 / total) : 0.0;

    auto cacheStats = m_cache->stats();

    spdlog::info("====== [PERF] MediaPool Frame Delivery Report ======");
    spdlog::info("  Total requests:      {}", total);
    spdlog::info("  Cache hits (exact):  {} ({:.1f}%)", hits, hitPct);
    spdlog::info("  Nearby-frame drops:  {}", nearby);
    spdlog::info("  Stale-frame returns: {}", stale);
    spdlog::info("  Inline decodes:      {}", inlined);
    spdlog::info("  Total misses (null): {}", misses);
    spdlog::info("  Frame drop rate:     {:.1f}%", dropPct);
    spdlog::info("  Prefetch scheduled:  {} tasks", sched);
    spdlog::info("  Prefetch delivered:  {} frames", prefetch);
    spdlog::info("  Avg decode time:     {} us", avgUs);
    spdlog::info("  FrameCache: {} entries, {:.0f}/{:.0f} MB, hit={:.1f}%",
                 cacheStats.frameCount,
                 cacheStats.memoryUsed / 1048576.0,
                 cacheStats.memoryCapacity / 1048576.0,
                 cacheStats.hitRate() * 100.0);
    spdlog::info("====================================================");
}

std::shared_ptr<CachedFrame> MediaPool::getFrame(
    MediaHandle handle, int64_t frameNumber, ResolutionTier tier, bool scrubMode)
{
    auto perfGetT0 = std::chrono::high_resolution_clock::now();
    m_perf.totalRequests.fetch_add(1, std::memory_order_relaxed);

    // Periodic perf report (every ~2 seconds at 30fps = every 60 frames)
    {
        static std::atomic<int> s_reportCounter{0};
        if (++s_reportCounter % 60 == 0) {
            logPerfReport();
        }
    }

    if (!scrubMode)
        extendInteractivePlaybackWindow(m_interactivePlaybackUntilMs);

    // Defense-in-depth: clamp frame number to valid range.
    // Callers should do this, but a stale or miscalculated frame number
    // beyond the video's frame count causes a guaranteed decode failure
    // (grey/static output) and wastes 50-100ms on a futile seek.
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (entry && entry->info.frameCount > 1 && frameNumber >= entry->info.frameCount) {
            frameNumber = entry->info.frameCount - 1;
        }
    }
    if (frameNumber < 0) frameNumber = 0;

    // ── Backward-seek detection ──────────────────────────────────────────
    // When the requested frame is behind the last-good frame, clear the
    // stale state so the anti-rewind guard in tryGetFrame doesn't lock
    // the display to a future position after a backward seek/scrub.
    {
        std::lock_guard lg(m_lastGoodMtx);
        auto it = m_lastGoodFrame.find(handle);
        if (it != m_lastGoodFrame.end() && it->second) {
            int64_t delta = frameNumber - it->second->frameNumber;
            if (delta < -1) {
                m_lastGoodFrame.erase(it);
            }
        }
    }

    // Tell the cache where playback is so eviction protects future frames
    if (!scrubMode)
        m_cache->setPlayhead(handle, frameNumber);

    // First check cache (no decoder lock needed)
    auto cached = m_cache->get(handle, frameNumber, tier);
    if (cached) {
        m_perf.cacheHits.fetch_add(1, std::memory_order_relaxed);
        static int s_hitLog = 0;
        if (++s_hitLog % 120 == 0) {
            auto stats = m_cache->stats();
            spdlog::info("[PERF] FrameCache stats: {} entries, {:.0f}MB used / {:.0f}MB cap, hit={:.1f}% "
                         "(handle={} frame={} tier={})",
                         stats.frameCount,
                         stats.memoryUsed / 1048576.0,
                         stats.memoryCapacity / 1048576.0,
                         stats.hitRate() * 100.0,
                         handle, frameNumber, static_cast<int>(tier));
        }
        // Update last-good-frame for playback stale-frame fallback
        if (!scrubMode) {
            std::lock_guard lg(m_lastGoodMtx);
            m_lastGoodFrame[handle] = cached;
        }
        return cached;
    }

    // ── Disk cache check (second-level persistent cache) ─────────────
    // ~2-3 ms on NVMe SSD vs 10-100 ms re-decode.
    if (m_diskCache) {
        cached = m_diskCache->get(handle, frameNumber, tier);
        if (cached) {
            m_cache->put(cached); // Promote back to RAM
            m_perf.cacheHits.fetch_add(1, std::memory_order_relaxed);
            if (!scrubMode) {
                std::lock_guard lg(m_lastGoodMtx);
                m_lastGoodFrame[handle] = cached;
            }
            return cached;
        }
    }


    // ── Frame-drop fallback (scrub AND playback) ────────────────────────
    // If the exact frame isn't cached, accept the nearest cached frame
    // within a small window.  During playback this is "frame dropping" â€”
    // the display shows a slightly stale frame while the prefetch thread
    // catches up.  This keeps the UI thread unblocked and is exactly what
    // Premiere Pro does for smooth playback.
    //
    // Search radius: Â±4 for scrub, Â±5 for playback (wider to absorb
    // prefetch lag and keep the compositor moving without blocking).
    // Also try the other resolution tier since the
    // prefetch thread may have filled a different tier.
    {
        const int64_t searchRadius = scrubMode ? 15 : 5;
        for (int64_t delta = 1; delta <= searchRadius; ++delta) {
            cached = m_cache->getNoPromote(handle, frameNumber - delta, tier);
            if (cached) {
                m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
                // Piggyback prefetch so the target frame is decoded soon
                schedulePrefetch(handle, frameNumber, PREFETCH_AHEAD_COUNT, /*urgent=*/true, tier);
                if (!scrubMode) {
                    std::lock_guard lg(m_lastGoodMtx);
                    m_lastGoodFrame[handle] = cached;
                }
                return cached;
            }
            cached = m_cache->getNoPromote(handle, frameNumber + delta, tier);
            if (cached) {
                m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
                if (!scrubMode) {
                    std::lock_guard lg(m_lastGoodMtx);
                    m_lastGoodFrame[handle] = cached;
                }
                return cached;
            }
        }
        // Also try the opposite resolution tier as last resort
        auto altTier = (tier == ResolutionTier::Half)
                           ? ResolutionTier::Full : ResolutionTier::Half;
        cached = m_cache->getNoPromote(handle, frameNumber, altTier);
        if (cached) {
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            if (!scrubMode) {
                std::lock_guard lg(m_lastGoodMtx);
                m_lastGoodFrame[handle] = cached;
            }
            return cached;
        }
        for (int64_t delta = 1; delta <= 2; ++delta) {
            cached = m_cache->getNoPromote(handle, frameNumber - delta, altTier);
            if (cached) {
                m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
                if (!scrubMode) {
                    std::lock_guard lg(m_lastGoodMtx);
                    m_lastGoodFrame[handle] = cached;
                }
                return cached;
            }
        }
    }

    // ── Playback cache-miss path ─────────────────────────────────────────
    // Schedule urgent prefetch so background workers decode this frame and
    // upcoming ones.  During playback, we NEVER block the render thread
    // with inline decode — matching Premiere Pro's architecture where the
    // composition thread never calls into the decoder.
    //
    // Changed: Previously fell through to inline decode for first-access
    // (no stale frame).  Now returns nullptr unconditionally during playback
    // and lets the compositor show the last good composite or a black layer
    // for 1-2 frames while prefetch workers fill the cache.  This eliminates
    // the 30-300ms stalls that caused visible judder on long-GOP codecs.
    if (!scrubMode) {
        // Schedule aggressive prefetch: current frame + ahead
        schedulePrefetch(handle, frameNumber, PREFETCH_AHEAD_COUNT, /*urgent=*/true, tier);

        // Try to return last-good-frame (stale but visible)
        std::shared_ptr<CachedFrame> stale;
        {
            std::lock_guard lg(m_lastGoodMtx);
            auto it = m_lastGoodFrame.find(handle);
            if (it != m_lastGoodFrame.end())
                stale = it->second;
        }
        if (stale) {
            m_perf.staleReturns.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> s_staleLog{0};
            if (++s_staleLog % 30 == 0) {
                spdlog::info("[PERF] getFrame STALE: handle={} wanted={} returning last-good "
                             "(prefetch scheduled)", handle, frameNumber);
            }
            return stale;
        }

        // No stale frame (first-ever access for this handle during playback).
        // Return nullptr — the compositor will show the last good composite
        // or a transparent layer.  Prefetch workers will fill this frame
        // within 1-3 callbacks (~30-100ms).  This is strictly better than
        // inline decode which blocks for 30-300ms and causes visible judder.
        m_perf.totalMisses.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> s_firstAccessLog{0};
        if (++s_firstAccessLog <= 20 || s_firstAccessLog % 60 == 0) {
            spdlog::info("[PERF] getFrame: first access for handle={} frame={} "
                         "-> non-blocking miss (prefetch will fill)", handle, frameNumber);
        }
        return nullptr;
    }

    // ── Scrub cache-miss: schedule urgent prefetch + lock-free decode ────
    // Uses a dedicated scrub decoder (separate from the main entry decoder)
    // so we NEVER hold m_mutex during the decode.  This eliminates the
    // 60-150ms UI-thread stall that caused scrub jitter.
    schedulePrefetch(handle, frameNumber, PREFETCH_AHEAD_COUNT, /*urgent=*/true, tier);

    m_perf.inlineDecodes.fetch_add(1, std::memory_order_relaxed);

    // Brief lock: copy metadata needed to open/reuse the scrub decoder.
    std::filesystem::path filePath;
    VideoStreamInfo info{};
    bool packedAlpha = false;
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (!entry || !entry->decoder) return nullptr;
        filePath = entry->path;
        info = entry->info;
        packedAlpha = entry->packedAlpha;
    }
    // m_mutex released — decode is now lock-free.

    auto& scrubState = getScrubDecoder(handle, filePath, info);

    // Build a PrefetchTask so we can reuse decodePrefetchFrame/convertDecodedToCache
    PrefetchTask scrubTask;
    scrubTask.handle = handle;
    scrubTask.filePath = filePath;
    scrubTask.frameNumber = frameNumber;
    scrubTask.tier = tier;
    scrubTask.fps = info.fps > 0 ? info.fps : 30.0;
    scrubTask.info = info;
    scrubTask.packedAlpha = packedAlpha;

    auto result = decodePrefetchFrame(scrubState, scrubTask);
    if (result) {
        m_cache->put(result);
        if (m_diskCache) m_diskCache->putAsync(result);
    }

    // Always-on decode timing (Release-visible)
    {
        auto perfGetT1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(perfGetT1 - perfGetT0).count();
        static int s_missLog = 0;
        if (++s_missLog % 30 == 0 || ms > 20.0) {
            spdlog::info("[PERF] getFrame CACHE MISS: handle={} frame={} tier={} -> {:.1f}ms (gpuReady={}, {}x{})",
                         handle, frameNumber, static_cast<int>(tier), ms,
                         result ? result->gpuReady : false,
                         result ? result->width : 0, result ? result->height : 0);
        }
    }

    // Update last-good-frame so future playback misses can return stale
    if (result && !scrubMode) {
        std::lock_guard lg(m_lastGoodMtx);
        m_lastGoodFrame[handle] = result;
    }

    return result;
}

// ─── tryGetFrame (non-blocking playback path) ───────────────────────────────

std::shared_ptr<CachedFrame> MediaPool::tryGetFrame(
    MediaHandle handle, int64_t frameNumber, ResolutionTier tier)
{
    m_perf.totalRequests.fetch_add(1, std::memory_order_relaxed);

    // Periodic perf report during playback (every ~2s at 30fps = every 60 frames)
    {
        static std::atomic<int> s_tryReportCounter{0};
        if (++s_tryReportCounter % 60 == 0) {
            logPerfReport();
        }
    }

    extendInteractivePlaybackWindow(m_interactivePlaybackUntilMs);

    bool isPackedAlpha = false;

    // Clamp frame number
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (entry) {
            isPackedAlpha = entry->packedAlpha;
            if (entry->info.frameCount > 1 && frameNumber >= entry->info.frameCount)
                frameNumber = entry->info.frameCount - 1;
        }
    }
    if (frameNumber < 0) frameNumber = 0;

    // ── Backward-seek detection ──────────────────────────────────────────
    // Clear stale anti-rewind state when playback position moves backward
    // (e.g. after scrubbing forward then pressing play at an earlier point).
    // Without this, the anti-rewind guard locks the display to the highest
    // frame ever seen, causing permanent stuck frames.
    {
        std::lock_guard lg(m_lastGoodMtx);
        auto it = m_lastGoodFrame.find(handle);
        if (it != m_lastGoodFrame.end() && it->second) {
            int64_t delta = frameNumber - it->second->frameNumber;
            if (delta < -1) {
                m_lastGoodFrame.erase(it);
            }
        }
    }

    // Tell the cache where playback is so eviction protects future frames.
    // Previously missing from tryGetFrame — eviction was blind during
    // playback and could evict freshly prefetched ahead-of-playhead frames.
    m_cache->setPlayhead(handle, frameNumber);

    // Helper: update last-good-frame and return.  Ensures tryGetFrame
    // NEVER returns a frame older than the most recent one shown for
    // this handle — preventing the visual "rewind" flicker.
    auto returnFrame = [&](std::shared_ptr<CachedFrame> f) -> std::shared_ptr<CachedFrame> {
        std::lock_guard lg(m_lastGoodMtx);
        auto& lastGood = m_lastGoodFrame[handle];
        if (lastGood && f->frameNumber < lastGood->frameNumber) {
            // Would go backwards — return the last good frame instead.
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            return lastGood;
        }
        lastGood = f;
        return f;
    };

    // Exact cache hit
    auto cached = m_cache->get(handle, frameNumber, tier);
    if (cached) {
        m_perf.cacheHits.fetch_add(1, std::memory_order_relaxed);
        return returnFrame(std::move(cached));
    }

    // Nearby-frame reuse in non-blocking playback.
    // For packed-alpha character media, allow a wider search radius so we
    // return a nearby cached frame instead of nullptr during warm-up.
    // This avoids long runs of invisible characters when exact frame prefetch
    // lags behind. For non-packed content keep the tight ±1 radius.
    const int nearbyRadius = isPackedAlpha ? 6 : 1;
    for (int d = 1; d <= nearbyRadius; ++d) {
        cached = m_cache->get(handle, frameNumber + d, tier);
        if (cached) {
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            schedulePrefetch(handle, frameNumber, PREFETCH_AHEAD_COUNT, /*urgent=*/true, tier);
            return returnFrame(std::move(cached));
        }
        cached = m_cache->get(handle, frameNumber - d, tier);
        if (cached) {
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            schedulePrefetch(handle, frameNumber, PREFETCH_AHEAD_COUNT, /*urgent=*/true, tier);
            return returnFrame(std::move(cached));
        }
    }

    // Try alternate tier as last resort (exact match only — no forward
    // search to avoid the same temporal-jump issue at a different tier).
    auto altTier = (tier == ResolutionTier::Half)
                       ? ResolutionTier::Full : ResolutionTier::Half;
    cached = m_cache->get(handle, frameNumber, altTier);
    if (cached) {
        m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
        schedulePrefetch(handle, frameNumber + 1, PREFETCH_AHEAD_COUNT, /*urgent=*/false, tier);
        return returnFrame(std::move(cached));
    }

    // Total miss — return last-good frame if available (holds the most
    // recent frame shown for this handle).  This prevents flicker:
    // FrameProducer re-publishes the same composed image, so the display
    // holds steady instead of oscillating between old and new frames.
    {
        std::lock_guard lg(m_lastGoodMtx);
        auto it = m_lastGoodFrame.find(handle);
        if (it != m_lastGoodFrame.end() && it->second) {
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            schedulePrefetch(handle, frameNumber, PREFETCH_AHEAD_COUNT, /*urgent=*/true, tier);
            return it->second;
        }
    }

    m_perf.totalMisses.fetch_add(1, std::memory_order_relaxed);
    schedulePrefetch(handle, frameNumber, PREFETCH_AHEAD_COUNT, /*urgent=*/true, tier);

    static std::atomic<int> s_tryMissLog{0};
    if (++s_tryMissLog % 30 == 0) {
        spdlog::info("[PERF] tryGetFrame MISS: handle={} frame={} tier={} (prefetch scheduled)",
                     handle, frameNumber, static_cast<int>(tier));
    }
    return nullptr;
}

// ─── Queries ─────────────────────────────────────────────────────────────────

const VideoStreamInfo* MediaPool::getInfo(MediaHandle handle) const
{
    std::lock_guard lock(m_mutex);
    auto* entry = findEntry(handle);
    return entry ? &entry->info : nullptr;
}

std::filesystem::path MediaPool::getPath(MediaHandle handle) const
{
    std::lock_guard lock(m_mutex);
    auto* entry = findEntry(handle);
    return entry ? entry->path : std::filesystem::path{};
}

bool MediaPool::isValid(MediaHandle handle) const
{
    std::lock_guard lock(m_mutex);
    return m_entries.find(handle) != m_entries.end();
}

size_t MediaPool::openCount() const
{
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

// ─── Async open worker ──────────────────────────────────────────────────────
// Lets the compositor request media opens at shot boundaries without
// stalling the playback thread on the ~500ms NVDEC initialization cost.

bool MediaPool::isPathOpen(const std::filesystem::path& filePath) const
{
    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string key = ec ? filePath.string() : canonical.string();
    std::lock_guard lock(m_mutex);
    return m_pathToHandle.find(key) != m_pathToHandle.end();
}

void MediaPool::openAsync(const std::filesystem::path& filePath)
{
    if (filePath.empty()) return;
    if (!m_openWorkerRunning.load(std::memory_order_acquire)) return;

    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string key = ec ? filePath.string() : canonical.string();

    // Skip if already open or in flight or known-failed.
    {
        std::lock_guard mlk(m_mutex);
        if (m_pathToHandle.find(key) != m_pathToHandle.end()) return;
        if (m_failedPaths.count(key))                         return;
    }
    {
        std::lock_guard wlk(m_openWorkerMutex);
        if (!m_openWorkerInFlight.insert(key).second) return; // already queued
        m_openWorkerQueue.push_back(filePath);
    }
    m_openWorkerCv.notify_one();
}

void MediaPool::startOpenWorker()
{
    m_openWorkerRunning.store(true, std::memory_order_release);
    m_openWorker = std::thread([this]{ openWorkerLoop(); });
}

void MediaPool::stopOpenWorker()
{
    m_openWorkerRunning.store(false, std::memory_order_release);
    m_openWorkerCv.notify_all();
    if (m_openWorker.joinable())
        m_openWorker.join();
    std::lock_guard wlk(m_openWorkerMutex);
    m_openWorkerQueue.clear();
    m_openWorkerInFlight.clear();
}

void MediaPool::openWorkerLoop()
{
    while (m_openWorkerRunning.load(std::memory_order_acquire)) {
        std::filesystem::path path;
        {
            std::unique_lock wlk(m_openWorkerMutex);
            m_openWorkerCv.wait(wlk, [this]{
                return !m_openWorkerQueue.empty()
                    || !m_openWorkerRunning.load(std::memory_order_acquire);
            });
            if (!m_openWorkerRunning.load(std::memory_order_acquire))
                return;
            path = std::move(m_openWorkerQueue.front());
            m_openWorkerQueue.pop_front();
        }

        // Perform the actual open (heavy: NVDEC init etc).
        auto t0 = std::chrono::steady_clock::now();
        MediaHandle h = open(path);
        auto ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (h != InvalidMedia) {
            spdlog::info("[PERF] MediaPool::openAsync '{}': {:.0f}ms (handle={})",
                         path.filename().string(), ms, h);
        } else {
            spdlog::warn("[PERF] MediaPool::openAsync '{}': FAILED after {:.0f}ms",
                         path.filename().string(), ms);
        }

        // Clear in-flight marker.
        std::error_code ec;
        auto canonical = std::filesystem::canonical(path, ec);
        std::string key = ec ? path.string() : canonical.string();
        std::lock_guard wlk(m_openWorkerMutex);
        m_openWorkerInFlight.erase(key);
    }
}

void MediaPool::setPackedAlpha(MediaHandle handle, bool packed)
{
    std::lock_guard lock(m_mutex);
    auto* entry = findEntry(handle);
    if (entry)
        entry->packedAlpha = packed;
}

bool MediaPool::isFrameCached(MediaHandle handle, int64_t frameNumber,
                              ResolutionTier tier) const
{
    return m_cache && m_cache->contains(handle, frameNumber, tier);
}

// â”€â”€â”€ Internal â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

MediaEntry* MediaPool::findEntry(MediaHandle handle)
{
    auto it = m_entries.find(handle);
    return it != m_entries.end() ? &it->second : nullptr;
}

const MediaEntry* MediaPool::findEntry(MediaHandle handle) const
{
    auto it = m_entries.find(handle);
    return it != m_entries.end() ? &it->second : nullptr;
}

} // namespace rt


