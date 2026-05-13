/*
 * CompositeEngine — GPU compositing engine extracted from CompositeService.
 *
 * Owns all GPU compositing resources (command submission, staging ring,
 * upload manager, texture cache, layer texture pool) and encapsulates
 * the single-submit GPU pipeline: upload -> effects -> transitions ->
 * composite -> readback.
 *
 * CompositeService holds an instance and delegates to it, keeping only
 * the higher-level orchestration (layer building, prewarm, spine, safe mode).
 *
 * NOTE: This class is in the global namespace (not rt::) to avoid a
 * vexing C2888 "symbol cannot be defined within namespace 'rt'" compiler
 * error that occurs when the header is processed after volk.h redefines
 * Vulkan handle types within the precompiled header.  All rt:: types are
 * qualified explicitly.
 */

#pragma once

#include "media/FrameCache.h"            // rt::CachedFrame
#include "CompositeServiceLayerBuild.h"  // rt::LayerInfo
#include "Compositor.h"                  // rt::Compositor, rt::BlendMode, rt::ABPair
#include "TransitionRenderer.h"          // rt::GpuTransitionType, rt::TransitionSourceInfo
#include "EffectProcessor.h"             // rt::EffectProcessor, rt::EffectType
#include "GpuTextureCache.h"             // rt::GpuTextureCache
#include "GpuWorkSubmission.h"           // rt::GpuWorkSubmission
#include "GpuUploadManager.h"            // rt::GpuUploadManager
#include "StagingRing.h"                 // rt::StagingRing
#include "vulkan/Texture.h"              // rt::Texture

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt { class CacheCoordinator; }

// ── Composite result LRU entry ──────────────────────────────────────────────

struct CompositeCacheEntry
{
    int64_t  tick{-1};
    uint32_t w{0}, h{0};
    std::shared_ptr<rt::CachedFrame> frame;
};

// ═════════════════════════════════════════════════════════════════════════════

class CompositeEngine
{
public:
    CompositeEngine();
    ~CompositeEngine();

    CompositeEngine(const CompositeEngine&) = delete;
    CompositeEngine& operator=(const CompositeEngine&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────
    void init(VkDevice device);
    void shutdown();

    // ── LRU cache ───────────────────────────────────────────────────────
    [[nodiscard]] std::shared_ptr<rt::CachedFrame> checkLru(
        int64_t tick, uint32_t w, uint32_t h) const;
    void insertLru(int64_t tick, uint32_t w, uint32_t h,
                   std::shared_ptr<rt::CachedFrame> frame);
    void flushLruOnResize(uint32_t w, uint32_t h);
    void clearLru();

    // ── Main GPU compositing entry point ────────────────────────────────
    [[nodiscard]] std::shared_ptr<rt::CachedFrame> composite(
        const std::vector<rt::LayerInfo>& layers,
        uint32_t outW, uint32_t outH,
        int64_t tick, bool scrubMode,
        bool gpuDisplayMode,
        rt::Compositor* compositor,
        rt::EffectProcessor* effectProcessor,
        rt::TransitionRenderer* transitionRenderer,
        bool perfLog,
        std::chrono::high_resolution_clock::time_point perfT0,
        std::chrono::high_resolution_clock::time_point& perfTlayers,
        std::chrono::high_resolution_clock::time_point& perfTgpuUp,
        std::chrono::high_resolution_clock::time_point& perfTcomp,
        int& effectLayerCount, int& effectPassCount,
        int& transitionCount);

    // ── GPU state ───────────────────────────────────────────────────────
    [[nodiscard]] bool isGpuAvailable() const noexcept;
    void notifyDeviceLost() noexcept;
    [[nodiscard]] int backoffAttempts() const noexcept { return m_gpuBackoffAttempts; }
    void resetBackoff() noexcept;
    [[nodiscard]] bool isGpuCompositeEnabled() const noexcept
        { return m_gpuCompositeState > 0; }

    // ── Cache coordinator ───────────────────────────────────────────────
    /// Set the CacheCoordinator for system-adaptive budgets and VRAM
    /// pressure monitoring.  Must be called before the first composite frame.
    void setCacheCoordinator(rt::CacheCoordinator* coordinator) noexcept
        { m_cacheCoordinator = coordinator; }

    // ── Texture cache ───────────────────────────────────────────────────
    [[nodiscard]] rt::GpuTextureCache* textureCache() const noexcept
        { return m_gpuTexCache.get(); }
    void clearTextureCache();
    [[nodiscard]] int vramUsagePercent() const noexcept;

    // ── Semaphore (inter-queue compute->graphics sync) ──────────────────
    [[nodiscard]] VkSemaphore compositeSemaphore() const noexcept
        { return m_compositeSemaphore; }

    // ── Resource cleanup helpers ────────────────────────────────────────
    void destroyCompositeSlot();
    void clearGpuTexCache();

private:
    // ── Owned GPU resources ───────────────────────────────────────────
    std::unique_ptr<rt::GpuWorkSubmission> m_gpuSubmission;
    std::unique_ptr<rt::StagingRing> m_stagingRing;
    std::unique_ptr<rt::GpuUploadManager> m_uploadManager;
    std::unique_ptr<rt::GpuTextureCache> m_gpuTexCache;

    // Layer texture pool
    struct PoolTexKey { uint64_t mediaId{0}; int64_t frameNumber{-1}; };
    std::vector<std::unique_ptr<rt::Texture>> m_gpuLayerTextures;
    std::vector<PoolTexKey> m_gpuLayerTexKeys;
    std::vector<std::unique_ptr<rt::Texture>> m_gpuMaskTextures;

    // Cache coordinator (optional — for dynamic budgets + VRAM pressure)
    rt::CacheCoordinator* m_cacheCoordinator{nullptr};

    // GPU state machine: 0 = untested, 1 = enabled, -1 = permanently failed
    int m_gpuCompositeState{0};

    // Inter-queue semaphore (compute->graphics)
    VkSemaphore m_compositeSemaphore{nullptr};
    VkDevice    m_device{nullptr};

    // Exponential backoff state
    static constexpr int kGpuBackoffInitialMs = 100;
    static constexpr int kGpuBackoffMaxMs     = 10000;
    std::chrono::steady_clock::time_point m_gpuBackoffUntil{};
    int  m_gpuBackoffAttempts{0};

    // Composite result LRU
    static constexpr size_t kCacheSize = 8;
    std::vector<CompositeCacheEntry> m_compositeLru;
    size_t m_compositeLruIdx{0};
};
