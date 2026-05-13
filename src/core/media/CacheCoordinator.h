/*
 * CacheCoordinator — Unified cache management for Roundtable NLE.
 *
 * Coordinates up to three caches (CPU FrameCache, DiskFrameCache) with
 * system-adaptive budgets and a VRAM pressure hook.
 *
 * Budget philosophy (all computed at runtime — no static waste):
 *   FrameCache (CPU RAM):   50% of installed physical RAM
 *   GpuTextureCache (VRAM): 60% of device-local VRAM (calculated here,
 *                            applied by CompositeEngine)
 *   DiskFrameCache (disk):   5% of free space on cache drive, 4-32 GB
 *
 * VRAM pressure monitoring:
 *   The GPU layer (CompositeEngine) registers a callback via
 *   setVramPressureFn().  After each composited frame, onFrameCompleted()
 *   calls this callback.  If it returns true (under pressure), the
 *   coordinator tells FrameCache to evict GPU-co-owned frames, freeing
 *   VRAM indirectly via shared_ptr release.
 *
 * Thread safety:
 *   All setters are called once during init (single-threaded).
 *   onFrameCompleted() is called from the composite thread.
 *   Budget queries are read-only after init.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>

namespace rt {

class FrameCache;
class DiskFrameCache;

class CacheCoordinator
{
public:
    /// Signature for VRAM pressure check callback.
    /// Returns true when the GPU texture cache is under pressure (>90%).
    /// Returns budget bytes via the out-parameter.
    using VramPressureFn = std::function<bool(size_t* vramBudgetOut)>;

    CacheCoordinator();
    ~CacheCoordinator();

    // Non-copyable
    CacheCoordinator(const CacheCoordinator&) = delete;
    CacheCoordinator& operator=(const CacheCoordinator&) = delete;

    // ── Register caches (called once during init) ──────────────────────────

    /// Register the CPU-side FrameCache.  Sets its capacity to the
    /// recommended budget immediately.
    void setFrameCache(FrameCache* cache);

    /// Register the disk-backed DiskFrameCache.
    void setDiskCache(DiskFrameCache* cache);

    /// Register a callback to check GPU VRAM pressure.
    /// Called from onFrameCompleted().  The callback lives in the GPU layer
    /// (CompositeEngine) which has access to GpuTextureCache.
    void setVramPressureFn(VramPressureFn fn) { m_vramPressureFn = std::move(fn); }

    // ── Budget queries (used by factories before caches exist) ─────────────

    /// Recommended CPU RAM budget for FrameCache (bytes).
    /// 50% of installed physical RAM.
    [[nodiscard]] size_t recommendedFrameCacheBudget() const noexcept;

    /// Recommended VRAM budget for GpuTextureCache (bytes).
    /// 60% of device-local VRAM.
    /// @param deviceVramBytes  Device-local VRAM reported by VMA.
    [[nodiscard]] size_t recommendedGpuTexCacheBudget(
        size_t deviceVramBytes) const noexcept;

    /// Recommended disk budget for DiskFrameCache (bytes).
    /// 5% of free space on the cache drive, clamped 4-32 GB.
    [[nodiscard]] size_t recommendedDiskCacheBudget() const noexcept;

    // ── Per-frame hook ─────────────────────────────────────────────────────

    /// Called after each composited frame (from the composite thread).
    /// Periodically invokes the VRAM pressure callback and triggers
    /// cross-cache eviction if the GPU texture cache is over 90% full.
    void onFrameCompleted();

    // ── Lifecycle events ───────────────────────────────────────────────────

    /// Called when GPU becomes available (after successful init).
    /// Stores VRAM size for budget queries.
    void onGpuAvailable(size_t deviceVramBytes);

    /// Called when GPU is lost — resets VRAM tracking.
    void onGpuLost();

    /// Log all current budgets and usage to spdlog.
    void logBudgets() const;

private:
    /// Query installed physical RAM (bytes) via OS API.
    static size_t queryTotalPhysicalRam();

    /// Check VRAM pressure and evict co-owned GPU textures from FrameCache.
    void checkVramPressure();

    FrameCache*      m_frameCache{nullptr};
    DiskFrameCache*  m_diskCache{nullptr};

    VramPressureFn   m_vramPressureFn;

    size_t m_totalRam{0};
    size_t m_totalVram{0};
    size_t m_vramBudget{0};

    // Throttle pressure checks to once every N seconds
    std::chrono::steady_clock::time_point m_lastPressureCheck;
    static constexpr auto kPressureInterval = std::chrono::seconds(3);
};

} // namespace rt
