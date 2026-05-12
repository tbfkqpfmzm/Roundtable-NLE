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
} // namespace rt