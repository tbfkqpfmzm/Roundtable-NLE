/*
 * PrefetchTexturePool — recycled VkImage pool for the GPU-resident prefetch
 * decode path (UPGRADE_PLAN Phase 3).
 *
 * Why this exists: every frame produced by the new convertDecodedToCacheGpu
 * (Phase 4) needs its own destination VkImage so the compositor can sample
 * it later. vmaCreateImage costs ~0.3-1 ms each and fragments VRAM under
 * sustained scrubbing. This pool recycles destroyed-but-not-freed Textures
 * keyed by (w, h, format, usage), mirroring the pattern already in
 * GpuUploadManager::m_texPool.
 *
 * Thread safety: all public methods take an internal mutex. The pool is
 * accessed by prefetch worker threads (multiple) and by the compositor
 * thread (release via shared_ptr deleter, when GpuTextureCache evicts a
 * Phase-5 shared entry).
 *
 * Capacity: kMaxPerShape = 16 (vs GpuUploadManager's 8). Prefetch produces
 * frames sequentially at up to ~30 fps, and PREFETCH_AHEAD_COUNT is 60.
 * 16 per shape avoids churn during cold scroll; the actual VRAM cost is
 * dominated by GpuTextureCache entries, not pool reserve.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace rt {

class Texture;

class PrefetchTexturePool
{
public:
    PrefetchTexturePool() = default;
    ~PrefetchTexturePool();

    PrefetchTexturePool(const PrefetchTexturePool&) = delete;
    PrefetchTexturePool& operator=(const PrefetchTexturePool&) = delete;

    /// Acquire a recycled Texture of the requested shape, or nullptr if
    /// the pool for that shape is empty. The caller is responsible for
    /// creating a fresh Texture on nullptr return.
    [[nodiscard]] std::unique_ptr<Texture> acquire(
        uint32_t width, uint32_t height,
        VkFormat format, VkImageUsageFlags usage);

    /// Return a Texture to the pool for later reuse. If the per-shape
    /// FIFO is at capacity, `tex` is destroyed instead (its destructor
    /// releases VMA memory). Safe to pass nullptr.
    void release(std::unique_ptr<Texture> tex,
                 uint32_t width, uint32_t height,
                 VkFormat format, VkImageUsageFlags usage);

    /// Drop every pooled Texture. Caller must ensure no GPU work is in
    /// flight on these images (vkDeviceWaitIdle, or guaranteed by
    /// shutdown ordering).
    void clear();

    [[nodiscard]] size_t totalEntries() const;

private:
    struct PoolKey {
        uint32_t          width{0};
        uint32_t          height{0};
        VkFormat          format{VK_FORMAT_UNDEFINED};
        VkImageUsageFlags usage{0};
        bool operator==(const PoolKey& o) const noexcept {
            return width == o.width && height == o.height &&
                   format == o.format && usage == o.usage;
        }
    };

    struct PoolKeyHash {
        size_t operator()(const PoolKey& k) const noexcept {
            size_t h = std::hash<uint32_t>{}(k.width);
            h ^= std::hash<uint32_t>{}(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.format))
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(k.usage)
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    static constexpr size_t kMaxPerShape = 16;

    mutable std::mutex m_mtx;
    std::unordered_map<PoolKey,
        std::vector<std::unique_ptr<Texture>>, PoolKeyHash> m_buckets;
};

// ── Helper: create-or-acquire a pooled Texture wrapped in a shared_ptr ──
//
// On call: tries pool.acquire(); on miss, creates a fresh Texture via
// Texture::create(). On success, returns a shared_ptr<Texture> whose
// custom deleter returns the Texture to `pool` instead of destroying it.
// Returns nullptr on Vulkan allocation failure.
//
// `concurrentQueueFamilies` is forwarded into TextureConfig — pass
// {computeFamily, graphicsFamily} so the resulting image uses
// VK_SHARING_MODE_CONCURRENT and can be written by the prefetch worker's
// compute submission and sampled by the compositor's graphics submission
// without an explicit queue-ownership-transfer barrier.
//
// Lifetime invariant: `pool` must outlive every shared_ptr<Texture>
// returned. MediaPool owns the pool and the FrameCache that holds the
// shared_ptr (via CachedFrame::gpuTextureOwner), so member-destruction
// order guarantees this for the normal shutdown path.
class GpuContext;

std::shared_ptr<Texture> makePooledTexture(
    PrefetchTexturePool&       pool,
    GpuContext&                ctx,
    uint32_t                   width,
    uint32_t                   height,
    VkFormat                   format,
    VkImageUsageFlags          usage,
    std::vector<uint32_t>      concurrentQueueFamilies = {});

} // namespace rt
