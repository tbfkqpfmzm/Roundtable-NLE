/*
 * GpuTextureCache — LRU cache of Vulkan textures in VRAM.
 *
 * Stores decoded video frames as GPU textures so that repeated lookups
 * (scrubbing, looping, static layers) skip the CPU→GPU PCIe upload.
 *
 * Keyed by (mediaId, frameNumber, tier).  The cache owns the GPU memory
 * and evicts the least-recently-used textures when VRAM budget is exceeded.
 *
 * Thread safety: NOT thread-safe — must be called from the composite thread.
 */

#pragma once

#include "vulkan/Texture.h"
#include "vulkan/Allocator.h"

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

namespace rt {

class GpuTextureCache
{
public:
    /// @param vramBudgetBytes  Maximum VRAM for cached textures.
    ///        Default 4 GB. Half-tier packed-alpha character textures
    ///        are ~4-5 MB each; 4 GB allows ~800-1000 frames before
    ///        LRU eviction begins.
    explicit GpuTextureCache(size_t vramBudgetBytes = 4ULL * 1024 * 1024 * 1024);
    ~GpuTextureCache();

    // Non-copyable
    GpuTextureCache(const GpuTextureCache&) = delete;
    GpuTextureCache& operator=(const GpuTextureCache&) = delete;

    // ── Lookup / Insert ─────────────────────────────────────────────────

    /// Look up a texture by (mediaId, frameNumber).
    /// Returns the descriptor info if cached, or a zeroed struct if not.
    /// Promotes the entry in the LRU on hit.
    struct LookupResult {
        VkDescriptorImageInfo descriptor{};
        bool                  found{false};
        uint32_t              width{0};
        uint32_t              height{0};
        bool                  isPacked{false}; ///< True if texture is packed-alpha (2× height)
        bool                  isPMA{false};    ///< True if texture is premultiplied-alpha
    };

    [[nodiscard]] LookupResult get(uint64_t mediaId, int64_t frameNumber);

    /// Insert a texture — takes ownership of `tex`.
    /// Evicts old entries if VRAM budget is exceeded.
    void put(uint64_t mediaId, int64_t frameNumber,
             std::unique_ptr<Texture> tex, size_t textureBytes,
             bool isPacked = false, bool isPMA = false,
             bool isLoopFrame = false);

    /// Insert a texture with shared ownership — used for CUDA zero-copy
    /// frames where the CachedFrame (FrameCache) and GpuTexCache co-own
    /// the GPU texture.  This allows dirty-tracking cache hits to skip
    /// the entire decode + FrameCache lookup path.
    void putShared(uint64_t mediaId, int64_t frameNumber,
                   std::shared_ptr<void> sharedOwner,
                   VkDescriptorImageInfo descriptor,
                   uint32_t width, uint32_t height,
                   size_t textureBytes,
                   bool isPacked = false, bool isPMA = false,
                   bool isLoopFrame = false);

    // ── Maintenance ─────────────────────────────────────────────────────

    /// Evict all entries.
    void clear();

    /// Evict all entries for a specific media.
    void evictMedia(uint64_t mediaId);

    /// Set the VRAM budget.  May trigger evictions.
    void setBudget(size_t bytes);

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] size_t entryCount() const noexcept { return m_map.size(); }
    [[nodiscard]] size_t memoryUsed() const noexcept { return m_used; }
    [[nodiscard]] size_t budget()     const noexcept { return m_budget; }
    [[nodiscard]] size_t hits()       const noexcept { return m_hits; }
    [[nodiscard]] size_t misses()     const noexcept { return m_misses; }

    /// Returns true if VRAM usage exceeds 90% of budget (pressure state).
    [[nodiscard]] bool isUnderPressure() const noexcept {
        return m_used > (m_budget * 9 / 10);
    }

    /// Returns VRAM usage as a percentage 0-100.
    [[nodiscard]] int usagePercent() const noexcept {
        return m_budget > 0 ? static_cast<int>(m_used * 100 / m_budget) : 0;
    }

private:
    struct CacheKey {
        uint64_t mediaId;
        int64_t  frameNumber;

        bool operator==(const CacheKey& o) const noexcept {
            return mediaId == o.mediaId && frameNumber == o.frameNumber;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const noexcept {
            size_t h = std::hash<uint64_t>{}(k.mediaId);
            h ^= std::hash<int64_t>{}(k.frameNumber) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    using LruList = std::list<CacheKey>;
    using LruIter = LruList::iterator;

    struct Entry {
        std::unique_ptr<Texture> texture;       // Exclusive ownership (CPU-uploaded)
        std::shared_ptr<void>    sharedOwner;   // Shared ownership (CUDA zero-copy)
        VkDescriptorImageInfo    sharedDescriptor{}; // Descriptor for shared textures
        size_t                   bytes{0};
        uint32_t                 width{0};
        uint32_t                 height{0};
        bool                     isPacked{false}; // Packed-alpha (2× height, UV split)
        bool                     isPMA{false};    // Premultiplied-alpha (native alpha video)
        bool                     isLoopFrame{false}; // Belongs to a short looping clip
        LruIter                  lruIt;
    };

    void evictUntilFits(size_t needed);

    std::unordered_map<CacheKey, Entry, CacheKeyHash> m_map;
    LruList m_lru;

    size_t m_budget;
    size_t m_used{0};
    size_t m_hits{0};
    size_t m_misses{0};
};

} // namespace rt
