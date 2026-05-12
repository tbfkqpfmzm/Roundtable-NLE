/*
 * MediaPoolPerf.cpp — Performance metrics logging for MediaPool.
 * Extracted from MediaPool.cpp for maintainability.
 */

#include "MediaPool.h"

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
    spdlog::info("====================================================");
}

} // namespace rt
