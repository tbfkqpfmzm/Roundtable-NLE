/*
 * MediaPoolOpen.cpp — File open/resolution and release for MediaPool.
 * Extracted from MediaPool.cpp for maintainability.
 */

#include "MediaPool.h"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

#include "GpuContext.h"
#include "Nv12Converter.h"
#include "cuda/CudaVulkanInterop.h"

#include <volk.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace rt {

// ─── ProRes codec detection ────────────────────────────────────────────────
static bool isProResCodec(const std::string& codecName)
{
    return codecName.find("prores") != std::string::npos;
}

// ─── Open ───────────────────────────────────────────────────────────────────

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
        // ── Search common asset directories for the file ────────────────
        namespace fs = std::filesystem;
        fs::path filename = filePath.filename();

        // Build a list of candidate paths to try
        std::vector<fs::path> candidates;

        // Alternate video extensions — prefer .mp4 (NVDEC hw decode) over .mov (ProRes sw decode)
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

        // Common image extensions — for presets that reference background
        // images as bare filenames without extension (e.g. "TABLE_LARGE_FINAL")
        const std::vector<fs::path> imgExts = {".png", ".jpg", ".jpeg"};
        for (const auto& imgExt : imgExts) {
            altExts.push_back(fs::path(filePath).replace_extension(imgExt));
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
                spdlog::info("MediaPool: resolved '{}' → '{}'",
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

    // ── ProRes native decode (no transcoding) ──────────────────────────
    // ProRes 4444 is our preferred format for alpha video: intra-frame codec with
    // native YUVA alpha.  Software decode is fast enough for 1080p on modern CPUs.
    // No auto-transcode — keep the ProRes quality and instant random access.
    std::filesystem::path actualPath = actualFilePath;
    if (isProResCodec(decoder->info().codecName))
    {
        spdlog::info("MediaPool: using ProRes native decode for '{}' (sw, native alpha)",
                     filePath.filename().string());
        // Fall through — use the software decoder as-is.
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

// ─── Release / Close ───────────────────────────────────────────────────────

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

void MediaPool::invalidatePath(const std::filesystem::path& filePath)
{
    // Canonicalize identically to open() so we hit the same map key.
    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string key = ec ? filePath.string() : canonical.string();

    // Flush queued prefetch work for the (about to be closed) handle.
    MediaHandle handle = InvalidMedia;
    {
        std::lock_guard lock(m_mutex);
        m_failedPaths.erase(key);  // allow a fresh open attempt
        auto pathIt = m_pathToHandle.find(key);
        if (pathIt == m_pathToHandle.end())
            return;                 // not open — next open() reads fresh
        handle = pathIt->second;
    }

    {
        std::lock_guard pfLock(m_prefetchMutex);
        m_prefetchQueue.erase(
            std::remove_if(m_prefetchQueue.begin(), m_prefetchQueue.end(),
                [handle](const PrefetchTask& t) { return t.handle == handle; }),
            m_prefetchQueue.end());
    }
    m_scheduler.cancel(handle);

    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(handle);
    if (it == m_entries.end())
        return;

    spdlog::info("MediaPool: invalidating handle {} '{}' (file changed)",
                 handle, it->second.path.filename().string());

    m_pathToHandle.erase(key);
    m_cache->evictMedia(handle);

#ifdef ROUNDTABLE_HAS_FFMPEG
    if (it->second.swsCtx)
        sws_freeContext(static_cast<SwsContext*>(it->second.swsCtx));
#endif
    m_entries.erase(it);

    auto scrubIt = m_scrubDecoders.find(handle);
    if (scrubIt != m_scrubDecoders.end()) {
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (scrubIt->second.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(scrubIt->second.swsCtx));
#endif
        m_scrubDecoders.erase(scrubIt);
    }
}

// ─── Async open worker ─────────────────────────────────────────────────────

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

} // namespace rt
