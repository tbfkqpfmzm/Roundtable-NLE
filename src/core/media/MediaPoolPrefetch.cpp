// MediaPoolPrefetch.cpp - Background prefetch subsystem (extracted from MediaPool.cpp).

#include "MediaPool.h"
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

namespace rt {

namespace {

constexpr double kSlowHardwarePreviewFallbackMs = 180.0;
constexpr uint64_t kSlowHardwarePreviewMinPixels = 1920ull * 1080ull;

int64_t steadyClockMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool isInteractivePlaybackActive(const std::atomic<int64_t>& untilMs)
{
    return untilMs.load(std::memory_order_acquire) > steadyClockMillis();
}

bool shouldFallbackToSoftwarePreview(const VideoDecoder& decoder,
                                     ResolutionTier tier,
                                     uint32_t sourceWidth,
                                     uint32_t sourceHeight,
                                     double totalMs,
                                     bool packedAlpha)
{
    if (!decoder.isHardwareAccelerated() || tier == ResolutionTier::Full) {
        return false;
    }

    // Packed-alpha: SW decode is ~200ms/frame, strictly worse than NVDEC hiccups.
    if (packedAlpha) {
        return false;
    }

    if (totalMs < kSlowHardwarePreviewFallbackMs) {
        return false;
    }

    const uint64_t sourcePixels = static_cast<uint64_t>(sourceWidth) *
                                  static_cast<uint64_t>(sourceHeight);
    return sourcePixels >= kSlowHardwarePreviewMinPixels;
}

#ifdef ROUNDTABLE_HAS_FFMPEG
void resetPrefetchConversionState(PrefetchDecoderState& state)
{
    if (state.swsCtx) {
        sws_freeContext(static_cast<SwsContext*>(state.swsCtx));
        state.swsCtx = nullptr;
    }
    state.swsSrcW = 0;
    state.swsSrcH = 0;
    state.swsSrcFmt = -1;
    state.swsDstW = 0;
    state.swsDstH = 0;
}
#endif

// Consolidated: reopen decoder with specified mode (software or hardware).
bool reopenPrefetchDecoder(PrefetchDecoderState& state,
                            const PrefetchTask& task,
                            bool forceSoftware)
{
    auto newDecoder = std::make_unique<VideoDecoder>();
    const int maxThreads = forceSoftware ? 2 : 0;
    if (!newDecoder->open(task.filePath, /*forceSoftware=*/forceSoftware, /*maxThreads=*/maxThreads)) {
        const char* mode = forceSoftware ? "software" : "hardware";
        spdlog::warn("MediaPool prefetch: failed to reopen '{}' in {} mode after slow path",
                     task.filePath.filename().string(), mode);
        return false;
    }

    state.decoder = std::move(newDecoder);
    state.lastDecodedFrame = -1;
#ifdef ROUNDTABLE_HAS_FFMPEG
    resetPrefetchConversionState(state);
#endif
    return true;
}

bool reopenPrefetchDecoderAsSoftware(PrefetchDecoderState& state,
                                     const PrefetchTask& task)
{
    return reopenPrefetchDecoder(state, task, /*forceSoftware=*/true);
}

void logDecodePerf(const PrefetchTask& task, bool needSeek, int fwdFrames,
                    double decodeMs, double convertMs, double totalMs,
                    uint32_t width, uint32_t height)
{
    static thread_local int s_decLog = 0;
    if (++s_decLog % 10 != 1) return;

    if (needSeek) {
        spdlog::info("[PERF] prefetch decode: handle={} frame={} tier={} "
                     "needSeek=true fwdFrames={} decode={:.1f}ms convert={:.1f}ms total={:.1f}ms "
                     "{}x{}",
                     task.handle, task.frameNumber,
                     static_cast<int>(task.tier),
                     fwdFrames, decodeMs, convertMs, totalMs,
                     width, height);
    } else {
        spdlog::info("[PERF] prefetch decode: handle={} frame={} tier={} "
                     "needSeek=false decode={:.1f}ms convert={:.1f}ms total={:.1f}ms "
                     "{}x{}",
                     task.handle, task.frameNumber,
                     static_cast<int>(task.tier),
                     decodeMs, convertMs, totalMs,
                     width, height);
    }
}

} // namespace

/// is half the original height with correct BGRA alpha, halving its
/// memory footprint and eliminating per-frame work in the compositor.
#if 0 // Currently unused
static void unpackPackedAlphaInPlace(std::shared_ptr<CachedFrame>& frame)
{
    if (!frame || frame->height == 0 || (frame->height % 2 != 0))
        return;

    if (frame->pixels.empty()) {
        spdlog::warn("unpackPackedAlphaInPlace: no CPU pixels — skipping");
        return;
    }

    const uint32_t halfH = frame->height / 2;
    const uint32_t w     = frame->width;
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    uint8_t* pixels = frame->pixels.data();

    for (uint32_t y = 0; y < halfH; ++y) {
        uint8_t*       rgbRow   = pixels + y * rowBytes;
        const uint8_t* alphaRow = pixels + (halfH + y) * rowBytes;
        for (uint32_t x = 0; x < w; ++x) {
            rgbRow[x * 4 + 3] = alphaRow[x * 4 + 0];
        }
    }

    frame->height = halfH;
    frame->pixels.resize(static_cast<size_t>(w) * halfH * 4);
    frame->pixels.shrink_to_fit();
    frame->unpackedAlpha = true;
}
#endif // unused unpackPackedAlphaInPlace

void MediaPool::startPrefetchThread()
{
    if (m_prefetchRunning) return;
    m_prefetchRunning = true;
    m_prefetchThreads.reserve(PREFETCH_THREAD_COUNT);
    for (int i = 0; i < PREFETCH_THREAD_COUNT; ++i) {
        m_prefetchThreads.emplace_back(&MediaPool::prefetchWorker, this, i);
    }
    spdlog::info("MediaPool: {} prefetch worker threads started", PREFETCH_THREAD_COUNT);
    startLoopPreDecodeWorkers();
}

void MediaPool::stopPrefetchThread()
{
    {
        std::lock_guard lock(m_prefetchMutex);
        m_prefetchRunning = false;
        m_prefetchQueue.clear();
        m_prefetchPackedOwner.clear();
    }
    m_prefetchCv.notify_all();
    for (auto& t : m_prefetchThreads) {
        if (t.joinable())
            t.join();
    }
    m_prefetchThreads.clear();

    stopLoopPreDecodeWorkers();
    {
        std::lock_guard lock(m_loopPreDecodeMutex);
        m_loopPreDecodeActive.clear();
        m_loopPreDecodeDone.clear();
        std::priority_queue<LoopPreDecodeTask> empty;
        std::swap(m_loopPreDecodeQueue, empty);
    }

    spdlog::info("MediaPool: prefetch worker threads stopped");
}

void MediaPool::startLoopPreDecodeWorkers()
{
    if (m_loopPreDecodeRunning.exchange(true)) return;
    m_loopPreDecodeThreads.reserve(LOOP_PREDECODE_MAX_CONCURRENT);
    for (int i = 0; i < LOOP_PREDECODE_MAX_CONCURRENT; ++i) {
        m_loopPreDecodeThreads.emplace_back(&MediaPool::loopPreDecodeDispatcher, this);
    }
    spdlog::info("MediaPool: {} loop pre-decode worker threads started",
                 LOOP_PREDECODE_MAX_CONCURRENT);
}

void MediaPool::stopLoopPreDecodeWorkers()
{
    if (!m_loopPreDecodeRunning.exchange(false)) return;
    m_loopPreDecodeCv.notify_all();
    for (auto& t : m_loopPreDecodeThreads) {
        if (t.joinable())
            t.join();
    }
    m_loopPreDecodeThreads.clear();
}

void MediaPool::loopPreDecodeDispatcher()
{
#ifdef _WIN32
    // Background work — never preempt playback/composite/prefetch threads.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        LoopPreDecodeTask task;
        {
            std::unique_lock lock(m_loopPreDecodeMutex);
            m_loopPreDecodeCv.wait(lock, [this] {
                return !m_loopPreDecodeRunning.load() || !m_loopPreDecodeQueue.empty();
            });
            if (!m_loopPreDecodeRunning.load() && m_loopPreDecodeQueue.empty())
                return;
            task = m_loopPreDecodeQueue.top();
            m_loopPreDecodeQueue.pop();
        }

        spdlog::info("[PERF] Loop pre-decode: handle={} '{}' starting decode (priority={})",
                     task.handle, task.path.filename().string(), task.priority);
        loopPreDecodeWorker(task.handle, std::move(task.path), task.info,
                            task.packedAlpha, task.tier);
    }
}

void MediaPool::startLoopPreDecode(MediaHandle handle, ResolutionTier tier, int64_t priority)
{
    std::filesystem::path path;
    VideoStreamInfo info;
    bool packed = false;
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (!entry) return;
        path   = entry->path;
        info   = entry->info;
        packed = entry->packedAlpha;
    }

    // Skip clips too long to be a meaningful loop.
    if (info.frameCount > LOOP_PREDECODE_MAX_FRAMES) {
        spdlog::info("[PERF] Loop pre-decode: SKIP handle={} '{}' ({} frames > cap {})",
                     handle, path.filename().string(),
                     info.frameCount, LOOP_PREDECODE_MAX_FRAMES);
        return;
    }

    // Skip codecs that decode catastrophically slowly in SW (ProRes, DNxHD).
    {
        const std::string& cn = info.codecName;
        if (cn == "prores" || cn == "prores_ks" || cn == "prores_aw" ||
            cn == "dnxhd"  || cn == "dnxhr") {
            spdlog::info("[PERF] Loop pre-decode: SKIP handle={} '{}' (codec={} too slow for SW pre-decode)",
                         handle, path.filename().string(), cn);
            return;
        }
    }

    {
        std::lock_guard lock(m_loopPreDecodeMutex);
        if (m_loopPreDecodeActive.count(handle) ||
            m_loopPreDecodeDone.count(handle))
            return;

        m_loopPreDecodeActive.insert(handle);
        LoopPreDecodeTask task;
        task.priority    = priority;
        task.seq         = ++m_loopPreDecodeSeq;
        task.handle      = handle;
        task.path        = path;
        task.info        = info;
        task.packedAlpha = packed;
        task.tier        = tier;
        m_loopPreDecodeQueue.push(std::move(task));
    }
    m_loopPreDecodeCv.notify_one();

    spdlog::info("[PERF] Loop pre-decode: enqueued handle={} '{}' ({} frames, priority={})",
                 handle, path.filename().string(), info.frameCount, priority);
}

void MediaPool::loopPreDecodeWorker(
    MediaHandle handle, std::filesystem::path path,
    VideoStreamInfo info, bool packedAlpha, ResolutionTier tier)
{
    auto t0 = std::chrono::high_resolution_clock::now();

    // Use hardware decode (NVDEC) when available.  The RTX 4090 supports
    // 5+ concurrent NVDEC sessions; the 4 prefetch workers + loop workers
    // stay well within that budget.  FFmpeg auto-falls back to software
    // if the session limit is hit, so there is zero crash risk.
    VideoDecoder decoder;
    if (!decoder.open(path, /*forceSoftware=*/false, /*maxThreads=*/2)) {
        spdlog::warn("Loop pre-decode: failed to open decoder for handle={} '{}'",
                     handle, path.filename().string());
        std::lock_guard lock(m_loopPreDecodeMutex);
        m_loopPreDecodeActive.erase(handle);
        return;
    }

    const int64_t frameCount = info.frameCount;
    // Stop 2 frames before EOF to avoid inaccurate container metadata.
    const int64_t safeFrameCount = std::max<int64_t>(1, frameCount - 2);
    int64_t decoded_count = 0;
    int64_t skipped_count = 0;
    int consecutive_failures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 3;

    // Start at playhead % frameCount so playback doesn't wait for frame 0.
    int64_t startFrame = 0;
    {
        const int64_t ph = m_playheadFrame.load(std::memory_order_relaxed);
        if (ph > 0 && frameCount > 1) {
            startFrame = ((ph % frameCount) + frameCount) % frameCount;
        }
    }
    if (startFrame > 0 && info.fps > 0.0) {
        decoder.seek(static_cast<double>(startFrame) / info.fps, SeekMode::Precise);
    } else {
        decoder.seek(0.0, SeekMode::Precise);
    }

    SwsContext* sws = nullptr;
    int swsSrcW = 0, swsSrcH = 0, swsSrcFmt = -1;
    int swsDstW = 0, swsDstH = 0;

    for (int64_t fi = 0; fi < safeFrameCount; ++fi) {
        const int64_t f = (startFrame + fi) % std::max<int64_t>(1, frameCount);
        if (!m_prefetchRunning) break;

        // Yield to playback during interactive periods.
        while (isInteractivePlaybackActive(m_interactivePlaybackUntilMs)) {
            if (!m_prefetchRunning) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (!m_prefetchRunning) break;

        // Re-seek on wraparound transition.
        if (fi > 0 && f == 0 && startFrame > 0) {
            decoder.seek(0.0, SeekMode::Precise);
        }

        if (m_cache->contains(handle, f, tier)) {
            ++skipped_count;
            DecodedFrame discard;
            decoder.decodeNext(discard);
            continue;
        }

        DecodedFrame raw;
        if (!decoder.decodeNext(raw)) {
            spdlog::debug("Loop pre-decode: decode failed at frame {} for handle={}",
                          f, handle);
            ++skipped_count;
            if (++consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                spdlog::debug("Loop pre-decode: {} consecutive failures at EOF, stopping for handle={}",
                              consecutive_failures, handle);
                break;
            }
            continue;
        }
        consecutive_failures = 0;

        if (raw.isHardware) {
            DecodedFrame cpuFrame;
            if (!decoder.transferHardwareFrame(raw, cpuFrame)) continue;
            raw = std::move(cpuFrame);
        }

        if (!raw.data[0] || raw.width == 0 || raw.height == 0) continue;

        auto cached = std::make_shared<CachedFrame>();
        cached->mediaId     = handle;
        cached->frameNumber = f;
        cached->width       = raw.width;
        cached->height      = raw.height;
        cached->tier        = tier;
        cached->isKeyframe  = raw.isKeyframe;
        cached->timestamp   = raw.timestamp;
        cached->pinned      = (info.frameCount <= 1);
        cached->isLoopFrame = true;

#ifdef ROUNDTABLE_HAS_FFMPEG
        AVPixelFormat srcFmt = AV_PIX_FMT_YUV420P;
        if (raw.rawFormat >= 0) {
            srcFmt = static_cast<AVPixelFormat>(raw.rawFormat);
        } else {
            switch (raw.format) {
                case PixelFormat::YUV420P: srcFmt = AV_PIX_FMT_YUV420P; break;
                case PixelFormat::NV12:    srcFmt = AV_PIX_FMT_NV12;    break;
                case PixelFormat::BGRA:    srcFmt = AV_PIX_FMT_BGRA;    break;
                case PixelFormat::RGBA:    srcFmt = AV_PIX_FMT_RGBA;    break;
                default:                   srcFmt = AV_PIX_FMT_YUV420P; break;
            }
        }

        const int w = static_cast<int>(raw.width);
        const int h = static_cast<int>(raw.height);

        int maxDim = (tier == ResolutionTier::Quarter) ? 480
                   : (tier == ResolutionTier::Half)    ? 960
                   :                                     1920;
        const int contentH = (packedAlpha && h > 1) ? h / 2 : h;
        int dW = w, dH = h;
        if (w > maxDim || contentH > maxDim) {
            const float scale = std::min(
                static_cast<float>(maxDim) / w,
                static_cast<float>(maxDim) / contentH);
            dW = std::max(2, static_cast<int>(w * scale) & ~1);
            dH = std::max(2, static_cast<int>(h * scale) & ~1);
        }

        const bool needsResize = (dW != w || dH != h);

        if (srcFmt == AV_PIX_FMT_BGRA && !needsResize) {
            cached->stride = w * 4;
            cached->pixels.resize(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                std::memcpy(cached->pixels.data() + y * cached->stride,
                            raw.data[0] + y * raw.linesize[0],
                            static_cast<size_t>(w) * 4);
            }
        } else {
            if (!sws || swsSrcW != w || swsSrcH != h ||
                swsSrcFmt != static_cast<int>(srcFmt) ||
                swsDstW != dW || swsDstH != dH) {
                if (sws) sws_freeContext(sws);
                sws = sws_getContext(w, h, srcFmt,
                                     dW, dH, AV_PIX_FMT_BGRA,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                swsSrcW = w;  swsSrcH = h;
                swsSrcFmt = static_cast<int>(srcFmt);
                swsDstW = dW; swsDstH = dH;
            }
            if (!sws) continue;

            cached->width  = static_cast<uint32_t>(dW);
            cached->height = static_cast<uint32_t>(dH);
            cached->stride = static_cast<uint32_t>(dW) * 4;
            cached->pixels.resize(static_cast<size_t>(dW) * dH * 4);

            uint8_t* dstData[1] = { cached->pixels.data() };
            int dstLinesize[1] = { static_cast<int>(cached->stride) };

            sws_scale(sws, raw.data, raw.linesize, 0, h,
                      dstData, dstLinesize);
        }

        if (info.hasAlpha && !packedAlpha && !cached->pixels.empty()) {
            clearTransparentPixelRGB(cached->pixels.data(),
                                     cached->pixels.size() / 4);
        }
#endif

        m_cache->put(cached);
        if (m_diskCache) m_diskCache->putAsync(cached);
        m_perf.prefetchDeliveries.fetch_add(1, std::memory_order_relaxed);
        ++decoded_count;

        if (decoded_count % 50 == 0) {
            spdlog::info("[PERF] Loop pre-decode: handle={} progress {}/{} frames",
                         handle, f + 1, frameCount);
        }
    }

    if (sws) sws_freeContext(sws);

    auto t1 = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    spdlog::info("[PERF] Loop pre-decode DONE: handle={} '{}' decoded={} skipped={} "
                 "total={:.0f}ms ({:.1f}ms/frame)",
                 handle, path.filename().string(),
                 decoded_count, skipped_count, totalMs,
                 decoded_count > 0 ? totalMs / decoded_count : 0.0);

    m_cache->removePlayhead(handle);

    {
        std::lock_guard lock(m_loopPreDecodeMutex);
        m_loopPreDecodeActive.erase(handle);
        m_loopPreDecodeDone.insert(handle);
    }
}

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
            // lookahead window (unless urgent).  This prevents the prefetch
            // workers from decoding frames that are too far ahead, which
            // wastes decode work and causes queue buildup during seek/scrub.
            if (!m_scheduler.withinLookahead(fn, afterFrame) && !(urgent && i == 0)) {
                if (i > 0) break; // Once we hit the lookahead bound, stop entirely
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

                    // Mismatched ownership cleanup.
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
                        const bool swHoldsNvdecCapable =
                            (workerId >= PREFETCH_NVDEC_WORKERS) && !anyPackedAlpha;
                        const bool nvdecHoldsAlpha =
                            (workerId < PREFETCH_NVDEC_WORKERS) && anyPackedAlpha;
                        if (swHoldsNvdecCapable || nvdecHoldsAlpha) {
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
            auto acceptable = [&](const PrefetchTask& t) -> bool {
                if (workerId >= PREFETCH_NVDEC_WORKERS && !t.packedAlpha)
                    return false;
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
            const int64_t maxFollow = (task.info.frameCount > 1)
                ? task.info.frameCount - 1 : INT64_MAX;

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

std::shared_ptr<CachedFrame> MediaPool::convertDecodedToCache(
    PrefetchDecoderState& state, const PrefetchTask& task,
    DecodedFrame& decoded, int64_t frameNumber)
{
    if (decoded.isHardware) {
        DecodedFrame cpuFrame;
        if (!state.decoder->transferHardwareFrame(decoded, cpuFrame))
            return nullptr;
        decoded = std::move(cpuFrame);
    }

    auto pool = m_pixelPool;
    auto cached = std::shared_ptr<CachedFrame>(new CachedFrame, [pool](CachedFrame* f) {
        pool->recycle(std::move(f->pixels));
        delete f;
    });
    cached->mediaId     = task.handle;
    cached->frameNumber = frameNumber;
    cached->width       = decoded.width;
    cached->height      = decoded.height;
    cached->tier        = task.tier;
    cached->isKeyframe  = decoded.isKeyframe;
    cached->timestamp   = decoded.timestamp;
    cached->pinned      = (task.info.frameCount <= 1);
    cached->isLoopFrame = task.isLoop;

    if (decoded.data[0] && decoded.width > 0 && decoded.height > 0) {
#ifdef ROUNDTABLE_HAS_FFMPEG
        AVPixelFormat srcFmt = AV_PIX_FMT_YUV420P;
        if (decoded.rawFormat >= 0) {
            srcFmt = static_cast<AVPixelFormat>(decoded.rawFormat);
        } else {
            switch (decoded.format) {
                case PixelFormat::YUV420P: srcFmt = AV_PIX_FMT_YUV420P; break;
                case PixelFormat::NV12:    srcFmt = AV_PIX_FMT_NV12;    break;
                case PixelFormat::BGRA:    srcFmt = AV_PIX_FMT_BGRA;    break;
                case PixelFormat::RGBA:    srcFmt = AV_PIX_FMT_RGBA;    break;
                default:                   srcFmt = AV_PIX_FMT_YUV420P; break;
            }
        }

        const int w = static_cast<int>(decoded.width);
        const int h = static_cast<int>(decoded.height);

        int maxDim = 1920;
        switch (task.tier) {
            case ResolutionTier::Half:    maxDim =  960; break;
            case ResolutionTier::Quarter: maxDim =  480; break;
            default:                      maxDim = 1920; break;
        }
        const int contentH = (task.packedAlpha && h > 1) ? h / 2 : h;
        int dstW = w, dstH = h;
        if (w > maxDim || contentH > maxDim) {
            const float scale = std::min(
                static_cast<float>(maxDim) / w,
                static_cast<float>(maxDim) / contentH);
            dstW = std::max(2, static_cast<int>(w * scale) & ~1);
            dstH = std::max(2, static_cast<int>(h * scale) & ~1);
        }

        const bool needsResize = (dstW != w || dstH != h);

        if (srcFmt == AV_PIX_FMT_BGRA && !needsResize) {
            const uint32_t stride = static_cast<uint32_t>(decoded.linesize[0]);
            cached->stride = w * 4;
            cached->pixels = pool->acquire(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                std::memcpy(cached->pixels.data() + y * cached->stride,
                            decoded.data[0] + y * stride,
                            static_cast<size_t>(w) * 4);
            }
        } else {
            SwsContext* sws = nullptr;
            if (state.swsCtx && state.swsSrcW == w && state.swsSrcH == h &&
                state.swsSrcFmt == static_cast<int>(srcFmt) &&
                state.swsDstW == dstW && state.swsDstH == dstH) {
                sws = static_cast<SwsContext*>(state.swsCtx);
            } else {
                if (state.swsCtx) {
                    sws_freeContext(static_cast<SwsContext*>(state.swsCtx));
                    state.swsCtx = nullptr;
                }
                sws = sws_getContext(
                    w, h, srcFmt,
                    dstW, dstH, AV_PIX_FMT_BGRA,
                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (sws) {
                    state.swsCtx    = sws;
                    state.swsSrcW   = w;
                    state.swsSrcH   = h;
                    state.swsSrcFmt = static_cast<int>(srcFmt);
                    state.swsDstW   = dstW;
                    state.swsDstH   = dstH;
                }
            }

            if (sws) {
                cached->width  = static_cast<uint32_t>(dstW);
                cached->height = static_cast<uint32_t>(dstH);
                cached->stride = static_cast<uint32_t>(dstW) * 4;
                cached->pixels = pool->acquire(static_cast<size_t>(dstW) * dstH * 4);

                uint8_t* dstData[1] = { cached->pixels.data() };
                int dstLinesize[1] = { static_cast<int>(cached->stride) };

                sws_scale(sws,
                          decoded.data, decoded.linesize,
                          0, h,
                          dstData, dstLinesize);
            } else {
                return nullptr;
            }
        }

        if (task.info.hasAlpha && !task.packedAlpha && !cached->pixels.empty()) {
            clearTransparentPixelRGB(cached->pixels.data(),
                                     cached->pixels.size() / 4);
        }
#else
        return nullptr;
#endif
    }

    return cached;
}

std::shared_ptr<CachedFrame> MediaPool::decodePrefetchFrame(
    PrefetchDecoderState& state, const PrefetchTask& task)
{
    auto perfDecodeT0 = std::chrono::high_resolution_clock::now();
    auto& decoder = *state.decoder;
    double targetTime = task.frameNumber / task.fps;

    // Sequential fast path: delta==1 or delta<=150 → advance decoder instead of seeking.
    bool needSeek = true;
    if (state.lastDecodedFrame >= 0) {
        int64_t delta = task.frameNumber - state.lastDecodedFrame;
        if (delta == 1) {
            needSeek = false;
        } else if (delta > 1 && delta <= 150) {
            needSeek = false;
            DecodedFrame skip;
            for (int64_t i = 0; i < delta - 1; ++i) {
                if (!decoder.decodeNext(skip)) {
                    needSeek = true;
                    break;
                }
            }
        }
    }

    if (needSeek) {
        // Keyframe seek + forward decode (avoids Precise seek B-frame bug).
        if (!decoder.seek(targetTime, SeekMode::Keyframe)) {
            spdlog::debug("MediaPool prefetch: seek failed handle={} frame={}",
                          task.handle, task.frameNumber);
            state.lastDecodedFrame = -1;
            return nullptr;
        }
        const double halfFrame = 0.5 / task.fps;
        DecodedFrame fwd;
        int fwdCount = 0;
        constexpr int kMaxForwardDecode = 600;
        while (fwdCount < kMaxForwardDecode) {
            if (!decoder.decodeNext(fwd)) {
                state.lastDecodedFrame = -1;
                return nullptr;
            }
            ++fwdCount;
            if (fwd.timestamp >= targetTime - halfFrame)
                break;
        }
        state.lastDecodedFrame = task.frameNumber;

        auto perfDecodeT1a = std::chrono::high_resolution_clock::now();
        auto cached = convertDecodedToCache(state, task, fwd, task.frameNumber);
        if (!cached) return nullptr;

        auto perfDecodeT1 = std::chrono::high_resolution_clock::now();
        const double totalMs = std::chrono::duration<double, std::milli>(perfDecodeT1 - perfDecodeT0).count();
        const double decodeOnlyMs = std::chrono::duration<double, std::milli>(perfDecodeT1a - perfDecodeT0).count();
        const double convertMs = totalMs - decodeOnlyMs;

        logDecodePerf(task, true, fwdCount, decodeOnlyMs, convertMs, totalMs,
                      fwd.width, fwd.height);
        return cached;
    }

    DecodedFrame decoded;
    if (!decoder.decodeNext(decoded)) {
        state.lastDecodedFrame = -1;
        return nullptr;
    }
    state.lastDecodedFrame = task.frameNumber;

    auto perfDecodeT1a = std::chrono::high_resolution_clock::now();
    auto cached = convertDecodedToCache(state, task, decoded, task.frameNumber);
    if (!cached)
        return nullptr;

    auto perfDecodeT1 = std::chrono::high_resolution_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(perfDecodeT1 - perfDecodeT0).count();
    const double decodeOnlyMs = std::chrono::duration<double, std::milli>(perfDecodeT1a - perfDecodeT0).count();
    const double convertMs = totalMs - decodeOnlyMs;

    logDecodePerf(task, false, 0, decodeOnlyMs, convertMs, totalMs,
                  decoded.width, decoded.height);

    // NVDEC→SW fallback only on sequential decodes (seek frames are inherently slow).
    if (!needSeek &&
        shouldFallbackToSoftwarePreview(*state.decoder,
                                        task.tier,
                                        decoded.width,
                                        decoded.height,
                                        totalMs,
                                        task.packedAlpha)) {
        ++state.consecutiveSlowHwFrames;
        constexpr int kFallbackSlowFrameThreshold = 3;
        if (state.consecutiveSlowHwFrames >= kFallbackSlowFrameThreshold) {
            spdlog::warn("[PERF] MediaPool prefetch: handle={} '{}' switching to software after {} consecutive slow hw frames (last={:.1f}ms total, src={}x{}, dst={}x{}, tier={})",
                         task.handle,
                         task.filePath.filename().string(),
                         state.consecutiveSlowHwFrames,
                         totalMs,
                         decoded.width,
                         decoded.height,
                         cached->width,
                         cached->height,
                         static_cast<int>(task.tier));
            reopenPrefetchDecoderAsSoftware(state, task);
            state.consecutiveSlowHwFrames = 0;
        }
    } else {
        state.consecutiveSlowHwFrames = 0;
    }

    // Rolling average decode time (EMA: 0.875*old + 0.125*sample)
    {
        uint64_t us = static_cast<uint64_t>(totalMs * 1000.0);
        uint64_t prev = m_perf.avgDecodeUs.load(std::memory_order_relaxed);
        uint64_t avg = (prev * 7 + us) / 8;
        m_perf.avgDecodeUs.store(avg, std::memory_order_relaxed);
    }

    return cached;
}

} // namespace rt
