/*
 * FrameCache — LRU cache for decoded video frames.
 *
 * Stores decoded frames in CPU memory with configurable capacity.
 * Supports multi-resolution (full + proxy) caching.
 * Thread-safe for concurrent producer/consumer access.
 *
 * Cache key: (mediaId, frameNumber, resolution).
 * Eviction: LRU with optional priority hints (keyframes kept longer).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rt {

/// Zero out the RGB channels of fully-transparent (A==0) pixels in-place.
/// FFmpeg sws_scale leaves non-zero RGB in transparent regions; GPU linear
/// texture filtering would bleed those colours into visible edges.
///
/// Uses branchless 32-bit masking: if alpha==0 the entire pixel becomes
/// 0x00000000; otherwise the pixel is kept unchanged.  This eliminates
/// the per-pixel branch that was causing ~4ms stalls on 1080p frames.
inline void clearTransparentPixelRGB(uint8_t* pixels, size_t pixelCount)
{
    auto* p = reinterpret_cast<uint32_t*>(pixels);
    for (size_t i = 0; i < pixelCount; ++i) {
        // BGRA layout: byte 3 is alpha.  Build a mask that is 0xFFFFFFFF
        // when alpha != 0 and 0x00000000 when alpha == 0.
        uint32_t alpha = (p[i] >> 24) & 0xFF;
        // branchless: -!!alpha produces 0xFFFFFFFF when alpha>0, 0 when 0
        uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(!!alpha));
        p[i] &= mask;
    }
}

/// Resolution tier for multi-resolution caching
enum class ResolutionTier : uint8_t
{
    Full,       // Original resolution
    Half,       // 50% resolution (proxy)
    Quarter,    // 25% resolution (thumbnail)
};

/// A cached frame — owns its pixel data
struct CachedFrame
{
    uint64_t    mediaId{0};     // Which media file this came from
    int64_t     frameNumber{0}; // Frame index in the source
    uint32_t    width{0};
    uint32_t    height{0};
    uint32_t    stride{0};      // Bytes per row
    ResolutionTier tier{ResolutionTier::Full};
    bool        isKeyframe{false};
    double      timestamp{0.0}; // PTS in seconds

    std::vector<uint8_t> pixels; // BGRA pixel data (pre-converted)

    /// True when this frame was decoded from a packed-alpha video and has
    /// already been unpacked (top-half RGB + bottom-half alpha → BGRA).
    /// Downstream code checks this to avoid double-unpacking.
    bool     unpackedAlpha{false};

    /// True when the BGRA pixels have been pre-multiplied by alpha.
    /// Native-alpha video (ProRes 4444, VP9+alpha) is stored as PMA so
    /// that GPU linear-filtering doesn't bleed opaque RGB into transparent
    /// edge pixels (the classic "white fringe" artefact).
    bool     premultipliedAlpha{false};

    /// True when this frame must NOT be evicted by the LRU policy.
    /// Used for static images (PNGs, single-frame media) where re-decoding
    /// triggers a sync prefetch round-trip and a visible "missing layer"
    /// frame in the compositor.  Pinned frames are skipped by both eviction
    /// passes; the only way to remove them is evictMedia() (handle close).
    bool     pinned{false};

    /// True when this frame belongs to a looping short clip (character
    /// animation, etc.).  Looping frames must NOT be evicted just because
    /// the media playhead has advanced past them — the loop will wrap
    /// right back around.  Pass-1 (behind-playhead) eviction skips these.
    /// Pass-2 (pure LRU) still applies when the cache is genuinely full.
    bool     isLoopFrame{false};

    // ── GPU-resident frame (zero-copy display) ──────────────────────────
    // When gpuReady is true, the compositor output can be displayed
    // directly via VulkanViewport without CPU readback.  Pixels may be empty.
    // These are Vulkan non-dispatchable handles stored as uint64_t to avoid
    // pulling vulkan.h into the core library.  Cast to VkImageView / VkSampler
    // in GPU code.
    uint64_t gpuImageView{0};   // VkImageView handle
    uint64_t gpuSampler{0};     // VkSampler handle
    /// VkSemaphore handle for inter-queue sync (compute→graphics).
    /// Atomic so the producer thread can clear it on a cached frame
    /// (m_lastGoodFrame re-publish) without racing with the presenter
    /// thread that reads it in ProgramMonitor::presentFrame.
    /// 0 = VK_NULL_HANDLE (no semaphore).
    mutable std::atomic<uint64_t> gpuSemaphore{0};
    bool     gpuReady{false};

    /// Opaque shared ownership of GPU resources (e.g., shared_ptr<Texture>).
    /// When all CachedFrame copies referencing this owner are destroyed,
    /// the GPU texture is released.  Keeps gpuImageView/gpuSampler valid.
    std::shared_ptr<void> gpuTextureOwner;

    // ── Lazy CPU readback ───────────────────────────────────────────────
    // When set, CPU pixels are deferred until actually needed.  The
    // callback reads back BGRA from the GPU texture into `pixels`.
    // Thread-safe: only the first caller runs the readback.
    using ReadbackFn = std::function<bool(std::vector<uint8_t>&)>;
    ReadbackFn lazyReadback;

    /// Materialise CPU pixels if they are empty and a lazy readback
    /// callback is available.  Returns true when `pixels` is non-empty
    /// after the call.  Safe to call from any thread.
    bool ensurePixels()
    {
        if (!pixels.empty()) return true;
        std::lock_guard<std::mutex> lock(m_readbackMutex);
        if (!pixels.empty()) return true;  // double-checked after lock
        if (lazyReadback) {
            lazyReadback(pixels);
            lazyReadback = nullptr;      // one-shot
        }
        return !pixels.empty();
    }

    /// Total memory used by this frame (CPU pixels + estimated GPU VRAM)
    [[nodiscard]] size_t memoryUsage() const noexcept
    {
        size_t mem = pixels.size() + sizeof(*this);
        // Account for GPU texture VRAM so the LRU cache can evict properly
        if (gpuTextureOwner)
            mem += static_cast<size_t>(width) * height * 4;
        return mem;
    }

private:
    mutable std::mutex m_readbackMutex;
};

/// Cache statistics
struct CacheStats
{
    size_t   hitCount{0};
    size_t   missCount{0};
    size_t   evictionCount{0};
    size_t   frameCount{0};
    size_t   memoryUsed{0};     // Bytes
    size_t   memoryCapacity{0}; // Bytes

    [[nodiscard]] double hitRate() const noexcept
    {
        size_t total = hitCount + missCount;
        return total > 0 ? static_cast<double>(hitCount) / total : 0.0;
    }
};

class FrameCache
{
public:
    /// Create a cache with the given memory capacity in bytes.
    /// Default: 32 GB — with ~21 character loops × 2 variants × ~5 MB/frame
    /// × 160-320 frames per loop, plus Wells ProRes 1.8 GB/variant, the
    /// working set for a full podcast timeline is ~35-50 GB of decoded
    /// frames.  16 GB forced LRU to constantly evict loop frames of
    /// currently-playing clips, producing random stutters.  32 GB keeps
    /// most content resident; LRU only kicks in when the timeline grows
    /// past ~2 minutes of unique active layers.
    // Default budget: 8 GB CPU RAM.  App::init() normally overrides this
    // with a size derived from installed physical RAM (~25%, capped to a
    // safe absolute) — see App.cpp.  The raw default only applies when
    // FrameCache is constructed without that dynamic sizing (e.g. tests).
    explicit FrameCache(size_t capacityBytes = 8ULL * 1024 * 1024 * 1024);
    ~FrameCache();

    // Non-copyable
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    // ── Insert / Lookup ─────────────────────────────────────────────────

    /// Insert a frame into the cache. Takes ownership of the pixel data.
    /// Evicts LRU frames if capacity is exceeded.
    void put(std::shared_ptr<CachedFrame> frame);

    /// Look up a cached frame. Returns nullptr if not cached.
    /// Moves the frame to the front of the LRU list (most recently used).
    [[nodiscard]] std::shared_ptr<CachedFrame> get(
        uint64_t mediaId, int64_t frameNumber,
        ResolutionTier tier = ResolutionTier::Full);

    /// Look up a cached frame WITHOUT promoting it in LRU order.
    /// Use for fallback/nearby-frame searches where touching the LRU
    /// would pollute the ordering and cause premature eviction of
    /// prefetched future frames.
    [[nodiscard]] std::shared_ptr<CachedFrame> getNoPromote(
        uint64_t mediaId, int64_t frameNumber,
        ResolutionTier tier = ResolutionTier::Full) const;

    /// Check if a frame is cached without promoting it in LRU order.
    [[nodiscard]] bool contains(uint64_t mediaId, int64_t frameNumber,
                                 ResolutionTier tier = ResolutionTier::Full) const;

    /// Return any cached frame for this media/tier, preferring the
    /// closest frame number to `preferFrame` that is <= preferFrame
    /// (then falling back to the closest any-direction match).  Does
    /// NOT promote the entry in LRU order.  Returns nullptr if no
    /// frames are cached for this media at this tier.
    ///
    /// Used by the compositor as a sticky last-good-frame fallback when
    /// the exact requested frame has not yet been decoded — preferable
    /// to skipping the layer entirely (which causes a visible pop-out).
    [[nodiscard]] std::shared_ptr<CachedFrame> getNearestBefore(
        uint64_t mediaId, int64_t preferFrame,
        ResolutionTier tier = ResolutionTier::Full) const;

    // ── Playhead tracking (for smart eviction) ──────────────────────────

    /// Tell the cache where the playhead is for a given media handle.
    /// The eviction policy uses this to keep frames ahead of the playhead
    /// and preferentially evict frames behind it.
    void setPlayhead(uint64_t mediaId, int64_t frameNumber);

    /// Phase B: declare a protected playback window for a media source.
    /// Frames inside [playheadFrame - behindCount, playheadFrame + aheadCount]
    /// are excluded from pass-1 eviction (behind-playhead sweep).  This is
    /// the data-side complement to UnifiedCache::setPlayheadWindow — call
    /// either; UnifiedCache forwards here.  Defaults (5 behind, 0 ahead)
    /// preserve historical behavior.
    void setPlayheadWindow(uint64_t mediaId,
                           int64_t playheadFrame,
                           int aheadCount,
                           int behindCount);

    // ── Bulk operations ─────────────────────────────────────────────────

    /// Evict all frames for a specific media file.
    void evictMedia(uint64_t mediaId);

    /// Evict frames that co-own GPU textures (CachedFrame::gpuTextureOwner set).
    /// This releases shared_ptr references so the GPU memory can be freed when
    /// GpuTextureCache's shared entries are also released.
    /// @param minBytes  Try to free at least this many bytes of CPU memory.
    /// @return Actual bytes freed.
    size_t evictGpuCoOwned(size_t minBytes);

    /// Remove a stale playhead entry (e.g. when a media handle is closed).
    void removePlayhead(uint64_t mediaId);

    /// Clear the entire cache.
    void clear();

    /// Set the capacity in bytes. May trigger evictions.
    void setCapacity(size_t capacityBytes);

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] CacheStats stats() const;
    [[nodiscard]] size_t frameCount() const;
    [[nodiscard]] size_t memoryUsed() const;

private:
    struct CacheKey
    {
        uint64_t       mediaId;
        int64_t        frameNumber;
        ResolutionTier tier;

        bool operator==(const CacheKey& o) const noexcept
        {
            return mediaId == o.mediaId && frameNumber == o.frameNumber && tier == o.tier;
        }
    };

    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& k) const noexcept
        {
            size_t h = std::hash<uint64_t>{}(k.mediaId);
            h ^= std::hash<int64_t>{}(k.frameNumber) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.tier)) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // LRU list: front = most recently used, back = least recently used
    using LruList = std::list<CacheKey>;
    using LruIterator = LruList::iterator;

    struct CacheEntry
    {
        std::shared_ptr<CachedFrame> frame;
        LruIterator                  lruIt;
    };

    void evictUntilFits(size_t neededBytes);

    mutable std::mutex                                         m_mutex;
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash>     m_map;
    LruList                                                    m_lru;
    size_t                                                     m_capacity;
    size_t                                                     m_used{0};

    // Playhead positions per media handle (for smart eviction)
    std::unordered_map<uint64_t, int64_t>                      m_playheads;

    // Phase B: protected window extent per media handle (frames behind /
    // ahead of the playhead that are excluded from pass-1 eviction).
    // Default = 5 behind / 0 ahead to preserve pre-Phase-B behavior.
    struct WindowExtent { int behind{5}; int ahead{0}; };
    std::unordered_map<uint64_t, WindowExtent>                 m_windows;

    // Stats
    mutable size_t m_hits{0};
    mutable size_t m_misses{0};
    size_t         m_evictions{0};
};

} // namespace rt
