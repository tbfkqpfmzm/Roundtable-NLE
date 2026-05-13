/*
 * CacheCoordinator.cpp — System-adaptive cache budget + VRAM pressure mgmt.
 */

#include "CacheCoordinator.h"
#include "FrameCache.h"
#include "DiskFrameCache.h"

#include <spdlog/spdlog.h>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

CacheCoordinator::CacheCoordinator()
{
    m_totalRam = queryTotalPhysicalRam();
    m_lastPressureCheck = std::chrono::steady_clock::now();

    spdlog::info("CacheCoordinator: system RAM = {:.1f} GB",
                 m_totalRam / (1024.0 * 1024.0 * 1024.0));
}

CacheCoordinator::~CacheCoordinator() = default;

// ═════════════════════════════════════════════════════════════════════════════
// Cache registration
// ═════════════════════════════════════════════════════════════════════════════

void CacheCoordinator::setFrameCache(FrameCache* cache)
{
    m_frameCache = cache;
    if (!m_frameCache) return;

    const size_t budget = recommendedFrameCacheBudget();
    m_frameCache->setCapacity(budget);

    spdlog::info("CacheCoordinator: FrameCache budget = {:.1f} GB ({:.0f}% of RAM)",
                 budget / (1024.0 * 1024.0 * 1024.0),
                 m_totalRam > 0 ? (budget * 100.0 / m_totalRam) : 50.0);
}

void CacheCoordinator::setDiskCache(DiskFrameCache* cache)
{
    m_diskCache = cache;
    if (!m_diskCache) return;

    const size_t budget = recommendedDiskCacheBudget();
    m_diskCache->setBudget(budget);

    spdlog::info("CacheCoordinator: DiskFrameCache budget = {:.1f} GB",
                 budget / (1024.0 * 1024.0 * 1024.0));
}

// ═════════════════════════════════════════════════════════════════════════════
// Budget queries
// ═════════════════════════════════════════════════════════════════════════════

size_t CacheCoordinator::recommendedFrameCacheBudget() const noexcept
{
    constexpr size_t kGiB = 1024ull * 1024ull * 1024ull;
    constexpr size_t kMin = 4 * kGiB;
    constexpr size_t kMax = 64 * kGiB;

    if (m_totalRam == 0) return 16 * kGiB;

    // 50% of installed RAM for decoded frame data.
    //   64 GB system → 32 GB  → ~4000 1080p BGRA frames
    //   32 GB system → 16 GB  → ~2000 frames
    //   16 GB system →  8 GB  → ~1000 frames
    size_t budget = m_totalRam / 2;

    // Clamp to reasonable range
    return std::clamp(budget, kMin, kMax);
}

size_t CacheCoordinator::recommendedGpuTexCacheBudget(
    size_t deviceVramBytes) const noexcept
{
    constexpr size_t kGiB = 1024ull * 1024ull * 1024ull;
    constexpr size_t kMin = 512ull * 1024 * 1024;
    constexpr size_t kMax = 20 * kGiB;

    if (deviceVramBytes == 0) return 4 * kGiB;

    // 60% of device VRAM for cached textures.
    // Leaves ~40% for: framebuffers, effect ping-pong images,
    // compositor staging surfaces, swapchain, VMA overhead.
    //   24 GB RTX 4080  → ~14.4 GB → ~3000 packed-alpha frames
    //   12 GB RTX 4070  → ~7.2 GB  → ~1500 frames
    //    8 GB RTX 4060  → ~4.8 GB  → ~1000 frames
    size_t budget = deviceVramBytes * 6 / 10;  // 60%

    return std::clamp(budget, kMin, kMax);
}

size_t CacheCoordinator::recommendedDiskCacheBudget() const noexcept
{
    constexpr size_t kGiB = 1024ull * 1024ull * 1024ull;
    constexpr size_t kMin = 4 * kGiB;
    constexpr size_t kMax = 32 * kGiB;

    // 5% of free space on the current drive
    std::error_code ec;
    const auto space = std::filesystem::space(".", ec);
    if (ec || space.available == 0)
        return 8 * kGiB;

    size_t budget = static_cast<size_t>(space.available) / 20;  // 5%
    return std::clamp(budget, kMin, kMax);
}

// ═════════════════════════════════════════════════════════════════════════════
// Per-frame hook
// ═════════════════════════════════════════════════════════════════════════════

void CacheCoordinator::onFrameCompleted()
{
    // Throttle: check pressure at most every kPressureInterval
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastPressureCheck < kPressureInterval)
        return;
    m_lastPressureCheck = now;

    checkVramPressure();
}

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void CacheCoordinator::onGpuAvailable(size_t deviceVramBytes)
{
    m_totalVram = deviceVramBytes;
    m_vramBudget = recommendedGpuTexCacheBudget(deviceVramBytes);

    spdlog::info("CacheCoordinator: GPU VRAM = {:.1f} GB, tex cache budget = {:.1f} GB",
                 deviceVramBytes / (1024.0 * 1024.0 * 1024.0),
                 m_vramBudget / (1024.0 * 1024.0 * 1024.0));

}

void CacheCoordinator::onGpuLost()
{
    m_totalVram = 0;
    m_vramBudget = 0;
}

void CacheCoordinator::logBudgets() const
{
    spdlog::info("── Cache budgets ──────────────────────────");

    if (m_frameCache) {
        const auto s = m_frameCache->stats();
        spdlog::info("FrameCache:    {:.1f} GB / {:.1f} GB  ({} frames, {:.1f}% hit)",
                     s.memoryUsed / (1024.0 * 1024.0 * 1024.0),
                     s.memoryCapacity / (1024.0 * 1024.0 * 1024.0),
                     s.frameCount, s.hitRate() * 100.0);
    }

    spdlog::info("GpuTexCache:   budget = {:.1f} GB  (applied by CompositeEngine)",
                 m_vramBudget / (1024.0 * 1024.0 * 1024.0));

    spdlog::info("───────────────────────────────────────────");
}

// ═════════════════════════════════════════════════════════════════════════════
// Private helpers
// ═════════════════════════════════════════════════════════════════════════════

size_t CacheCoordinator::queryTotalPhysicalRam()
{
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        return static_cast<size_t>(ms.ullTotalPhys);
#endif
    return 0;
}

void CacheCoordinator::checkVramPressure()
{
    if (!m_frameCache)
        return;

    // Invoke the VRAM pressure callback (registered by CompositeEngine).
    // If the GPU texture cache is under pressure, evict GPU-co-owned
    // frames from the CPU FrameCache to free VRAM indirectly.
    size_t vramBudget = 0;
    if (!m_vramPressureFn || !m_vramPressureFn(&vramBudget))
        return;

    // VRAM is under pressure.  Evict GPU-co-owned frames from FrameCache.
    const size_t targetBytes = vramBudget / 10;

    spdlog::info("CacheCoordinator: VRAM pressure detected — "
                 "evicting GPU-co-owned frames from FrameCache");

    const size_t freed = m_frameCache->evictGpuCoOwned(targetBytes);

    if (freed > 0) {
        spdlog::info("CacheCoordinator: freed {:.1f} MB from FrameCache (GPU-co-owned)",
                     freed / (1024.0 * 1024.0));
    }
}

} // namespace rt
