/*
 * AnimationCache — Pre-baked Spine animation frame cache.
 *
 * Step 27: Spine Animation Cache
 *
 * Pre-renders entire Spine animation cycles to CPU pixel buffers (RGBA).
 * During playback and export, cached frames replace expensive skeleton
 * evaluation + mesh extraction, yielding 10-50x speedup.
 *
 * Architecture:
 *   - CPU-only at the core layer (no GPU dependency)
 *   - Stores RGBA pixel data per frame
 *   - Disk persistence to assets/cache/animations/
 *   - Cache key: character|outfit|stance|animation|talking|WxH@fps
 *   - LRU eviction when memory budget exceeded
 *   - Thread-safe for concurrent producer/consumer access
 *
 * Usage:
 *   1. AnimationCache cache;
 *   2. cache.setCacheDirectory("assets/cache/animations/");
 *   3. cache.setMemoryBudget(512 * 1024 * 1024);  // 512 MB
 *   4. During export/prebake:
 *        CachedAnimationEntry entry;
 *        entry.key = AnimationCacheKey::fromClip(clip, 1920, 1080, 30.0f);
 *        entry.frames.push_back({pixels, w, h});
 *        cache.put(entry);
 *   5. During playback:
 *        auto* entry = cache.get(key);
 *        if (entry) renderFromCache(entry->frame(frameIndex));
 *        else       renderLive(spineEngine);
 *   6. cache.saveToDisk(key);  // persist for next session
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

// Forward declaration
class SpineClip;

// ═════════════════════════════════════════════════════════════════════════════
// Cache key
// ═════════════════════════════════════════════════════════════════════════════

/// Uniquely identifies a cached animation.
struct AnimationCacheKey
{
    std::string character;      ///< Character name (e.g. "Modernia")
    std::string outfit;         ///< Outfit name (e.g. "default", "outfit_01")
    std::string stance;         ///< Stance ("default", "aim", "cover")
    std::string animation;      ///< Animation name (e.g. "idle", "talk_01")
    bool        talking{false}; ///< Talking track active?
    uint32_t    width{0};       ///< Render width in pixels
    uint32_t    height{0};      ///< Render height in pixels
    float       fps{30.0f};     ///< Target frame rate

    /// Build a cache key from a SpineClip + resolution + fps.
    static AnimationCacheKey fromClip(const SpineClip& clip,
                                       uint32_t width, uint32_t height,
                                       float fps = 30.0f);

    /// Convert to a unique string (for hash map keys and directory names).
    [[nodiscard]] std::string toString() const;

    /// Generate a short hex hash of toString() (for disk directory name).
    [[nodiscard]] std::string toHash() const;

    bool operator==(const AnimationCacheKey& other) const noexcept;
};

/// Hash functor for AnimationCacheKey.
struct AnimationCacheKeyHash
{
    size_t operator()(const AnimationCacheKey& k) const noexcept;
};

// ═════════════════════════════════════════════════════════════════════════════
// Cached frame
// ═════════════════════════════════════════════════════════════════════════════

/// One frame of a cached animation (RGBA pixel data).
struct CachedAnimFrame
{
    std::vector<uint8_t> pixels; ///< RGBA pixel data (width * height * 4 bytes)
    uint32_t width{0};
    uint32_t height{0};

    /// Memory usage in bytes.
    [[nodiscard]] size_t memoryUsage() const noexcept
    {
        return pixels.size() + sizeof(*this);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// Cached animation entry
// ═════════════════════════════════════════════════════════════════════════════

/// A complete pre-baked animation cycle.
struct CachedAnimationEntry
{
    AnimationCacheKey                   key;
    std::vector<CachedAnimFrame>        frames;
    float                               duration{0.0f};  ///< Total animation duration (seconds)
    float                               fps{30.0f};      ///< Frame rate

    /// Number of frames.
    [[nodiscard]] size_t frameCount() const noexcept { return frames.size(); }

    /// Get frame at index (clamped).
    [[nodiscard]] const CachedAnimFrame* frame(size_t index) const noexcept;

    /// Get frame for a given time position (loops if necessary).
    [[nodiscard]] const CachedAnimFrame* frameAtTime(float timeSeconds) const noexcept;

    /// Total memory usage across all frames.
    [[nodiscard]] size_t memoryUsage() const noexcept;
};

// ═════════════════════════════════════════════════════════════════════════════
// Cache statistics
// ═════════════════════════════════════════════════════════════════════════════

struct AnimationCacheStats
{
    size_t hitCount{0};
    size_t missCount{0};
    size_t entryCount{0};
    size_t totalFrames{0};
    size_t memoryUsed{0};       ///< Bytes
    size_t memoryBudget{0};     ///< Bytes
    size_t diskEntries{0};      ///< Animations cached on disk
    size_t evictionCount{0};

    [[nodiscard]] double hitRate() const noexcept
    {
        size_t total = hitCount + missCount;
        return total > 0 ? static_cast<double>(hitCount) / total : 0.0;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// Animation Cache
// ═════════════════════════════════════════════════════════════════════════════

/// Pre-baked spine animation frame cache with LRU eviction and disk persistence.
class AnimationCache
{
public:
    AnimationCache();
    ~AnimationCache();

    // Non-copyable
    AnimationCache(const AnimationCache&) = delete;
    AnimationCache& operator=(const AnimationCache&) = delete;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the disk cache directory (e.g. "assets/cache/animations/").
    void setCacheDirectory(const std::filesystem::path& dir);
    [[nodiscard]] const std::filesystem::path& cacheDirectory() const noexcept;

    /// Set the memory budget in bytes. Triggers LRU eviction if exceeded.
    /// Default: 512 MB.
    void setMemoryBudget(size_t bytes);
    [[nodiscard]] size_t memoryBudget() const noexcept;

    // ── Insert / Lookup ─────────────────────────────────────────────────

    /// Insert a pre-baked animation into the cache. Takes ownership of frame data.
    /// Evicts LRU entries if memory budget is exceeded.
    void put(std::shared_ptr<CachedAnimationEntry> entry);

    /// Look up a cached animation. Returns nullptr if not cached.
    /// Promotes the entry in LRU order (most recently used).
    [[nodiscard]] std::shared_ptr<CachedAnimationEntry> get(
        const AnimationCacheKey& key);

    /// Check if an animation is cached (in memory or on disk).
    [[nodiscard]] bool contains(const AnimationCacheKey& key) const;

    /// Check if an animation is cached in memory only.
    [[nodiscard]] bool containsInMemory(const AnimationCacheKey& key) const;

    /// Check if an animation is cached on disk.
    [[nodiscard]] bool containsOnDisk(const AnimationCacheKey& key) const;

    // ── Eviction ────────────────────────────────────────────────────────

    /// Evict all in-memory entries for a specific character.
    void evictCharacter(const std::string& character);

    /// Evict a specific animation by key.
    void evict(const AnimationCacheKey& key);

    /// Clear all in-memory entries.
    void clearMemory();

    /// Clear both memory and disk caches.
    void clearAll();

    // ── Disk persistence ────────────────────────────────────────────────

    /// Save a cached animation to disk. Returns true on success.
    bool saveToDisk(const AnimationCacheKey& key);

    /// Load a cached animation from disk into memory. Returns true on success.
    bool loadFromDisk(const AnimationCacheKey& key);

    /// Scan the cache directory and populate the disk index.
    void scanDiskCache();

    /// Invalidate disk cache for a character (e.g. when model files change).
    void invalidateCharacter(const std::string& character);

    // ── Invalidation ────────────────────────────────────────────────────

    /// Set a callback that checks if a cache entry is still valid.
    /// Called with the key whenever a cache hit occurs.
    /// If the callback returns false, the entry is evicted.
    using ValidatorCallback = std::function<bool(const AnimationCacheKey&)>;
    void setValidator(ValidatorCallback cb);

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] AnimationCacheStats stats() const;
    [[nodiscard]] size_t entryCount() const;
    [[nodiscard]] size_t memoryUsed() const;

    // ── Iteration (for UI display) ──────────────────────────────────────

    /// Get all cache keys currently in memory.
    [[nodiscard]] std::vector<AnimationCacheKey> keys() const;

    /// Get all cache keys available on disk.
    [[nodiscard]] std::vector<AnimationCacheKey> diskKeys() const;

private:
    void evictUntilFits(size_t neededBytes);

    // ── LRU tracking ────────────────────────────────────────────────────
    using LruList = std::list<AnimationCacheKey>;
    using LruIterator = LruList::iterator;

    struct CacheRecord
    {
        std::shared_ptr<CachedAnimationEntry> entry;
        LruIterator                           lruIt;
    };

    mutable std::mutex  m_mutex;
    std::unordered_map<AnimationCacheKey, CacheRecord, AnimationCacheKeyHash> m_map;
    LruList             m_lru;
    size_t              m_budget{512ULL * 1024 * 1024};  ///< 512 MB default
    size_t              m_used{0};

    // Disk index: keys known to be on disk (without loading pixel data)
    std::unordered_map<AnimationCacheKey, std::filesystem::path, AnimationCacheKeyHash> m_diskIndex;

    // Cache directory
    std::filesystem::path m_cacheDir;

    // Validation callback
    ValidatorCallback     m_validator;

    // Stats
    mutable size_t m_hits{0};
    mutable size_t m_misses{0};
    size_t         m_evictions{0};
};

} // namespace rt
