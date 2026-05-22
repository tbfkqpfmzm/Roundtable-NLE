/*
 * MediaPoolPerf.cpp — Performance metrics logging for MediaPool.
 * Extracted from MediaPool.cpp for maintainability.
 */

#include "MediaPool.h"
#include "GpuUploadManager.h"

#include <spdlog/spdlog.h>

namespace rt {

// ─── Performance report ─────────────────────────────────────────────────────

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

    // UPGRADE_PLAN Phase 0 D.1: cold-frame composite-upload cost.  Should
    // drop sharply once the GPU-resident decode path (Phase 4-5) lands.
    const uint64_t firstUploadUs    = GpuUploadManager::firstUploadTotalUs();
    const uint64_t firstUploadCount = GpuUploadManager::firstUploadCount();
    if (firstUploadCount > 0) {
        spdlog::info("  GPU first uploads:   {} ({:.2f} ms total, {:.2f} ms avg)",
                     firstUploadCount,
                     firstUploadUs / 1000.0,
                     (firstUploadUs / 1000.0) / firstUploadCount);
    } else {
        spdlog::info("  GPU first uploads:   0");
    }
    GpuUploadManager::resetFirstUploadStats();

    // UPGRADE_PLAN Phase 9: dispatch ratio so an operator can see at a
    // glance whether GPU-resident is actually firing.  Bumped to warn so
    // it survives the warn+ logger filter — these counters are the main
    // signal that the pipeline change is doing what it claims.
    const uint64_t gpuResident = m_perf.gpuResidentDecoded.exchange(0, std::memory_order_relaxed);
    const uint64_t cpuConvert  = m_perf.cpuConvertDecoded.exchange(0, std::memory_order_relaxed);
    const uint64_t convertTotal = gpuResident + cpuConvert;
    if (convertTotal > 0) {
        const double gpuPct = 100.0 * gpuResident / convertTotal;
        spdlog::warn("[UPGRADE_PLAN PERF] decode dispatch: GPU={} CPU={} "
                     "({:.1f}% GPU-resident) | first-uploads={} ({:.2f}ms avg)",
                     gpuResident, cpuConvert, gpuPct,
                     firstUploadCount,
                     firstUploadCount > 0
                         ? (firstUploadUs / 1000.0) / firstUploadCount
                         : 0.0);
    }

    spdlog::info("====================================================");
}

} // namespace rt
