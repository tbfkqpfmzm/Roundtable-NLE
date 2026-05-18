/*
 * GpuTextureCache.cpp — LRU cache of Vulkan textures in VRAM.
 */

#include "GpuTextureCache.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace rt {

GpuTextureCache::GpuTextureCache(size_t vramBudgetBytes)
    : m_budget(vramBudgetBytes)
{
}

GpuTextureCache::~GpuTextureCache()
{
    clear();
}

// ── Lookup ──────────────────────────────────────────────────────────────────

GpuTextureCache::LookupResult GpuTextureCache::get(
    uint64_t mediaId, int64_t frameNumber, uint8_t tier)
{
    CacheKey key{mediaId, frameNumber, tier};
    auto it = m_map.find(key);
    if (it == m_map.end()) {
        ++m_misses;
        return {};
    }

    // For shared-ownership entries, verify the texture is still alive.
    // The CachedFrame (FrameCache) may have been evicted, destroying the
    // GPU texture.  In that case, remove the stale entry.
    if (!it->second.texture && it->second.sharedOwner) {
        // sharedOwner is alive — texture is valid
    } else if (!it->second.texture && !it->second.sharedOwner) {
        // Stale entry — remove it
        m_used -= it->second.bytes;
        m_lru.erase(it->second.lruIt);
        m_map.erase(it);
        ++m_misses;
        return {};
    }

    // Promote to front of LRU
    m_lru.splice(m_lru.begin(), m_lru, it->second.lruIt);

    ++m_hits;
    LookupResult r;
    r.found      = true;
    // Use appropriate descriptor based on ownership type
    if (it->second.texture) {
        r.descriptor = it->second.texture->descriptorInfo();
    } else {
        r.descriptor = it->second.sharedDescriptor;
    }
    r.width      = it->second.width;
    r.height     = it->second.height;
    r.isPacked   = it->second.isPacked;
    r.isPMA      = it->second.isPMA;
    return r;
}

// ── Insert ──────────────────────────────────────────────────────────────────

void GpuTextureCache::put(uint64_t mediaId, int64_t frameNumber, uint8_t tier,
                          std::unique_ptr<Texture> tex, size_t textureBytes,
                          bool isPacked, bool isPMA, bool isLoopFrame)
{
    if (!tex) return;

    CacheKey key{mediaId, frameNumber, tier};

    // If already cached, keep the existing entry (first decode wins).
    // Replacing here causes visible flicker when NVDEC and the SW decoder
    // produce subtly different RGB for the same H.264 frame (BT.709 limited
    // vs sws_scale's default colorspace), because consecutive composites
    // would alternate between the two outputs.  The source pixels don't
    // change, so the first cached version is the source of truth.
    auto existing = m_map.find(key);
    if (existing != m_map.end()) {
        // Promote to front of LRU so we don't evict it just because
        // a re-upload was attempted.
        m_lru.splice(m_lru.begin(), m_lru, existing->second.lruIt);
        return;
    }

    // Evict to make room
    evictUntilFits(textureBytes);

    // Insert
    m_lru.push_front(key);
    Entry entry;
    entry.width       = tex->width();
    entry.height      = tex->height();
    entry.bytes       = textureBytes;
    entry.isPacked    = isPacked;
    entry.isPMA       = isPMA;
    entry.isLoopFrame = isLoopFrame;
    entry.texture     = std::move(tex);
    entry.lruIt       = m_lru.begin();

    m_used += textureBytes;
    m_map.emplace(key, std::move(entry));
}

void GpuTextureCache::putShared(uint64_t mediaId, int64_t frameNumber, uint8_t tier,
                                std::shared_ptr<void> sharedOwner,
                                VkDescriptorImageInfo descriptor,
                                uint32_t width, uint32_t height,
                                size_t textureBytes,
                                bool isPacked, bool isPMA, bool isLoopFrame)
{
    if (!sharedOwner) return;

    CacheKey key{mediaId, frameNumber, tier};

    // If already cached, skip (don't replace existing entries)
    auto existing = m_map.find(key);
    if (existing != m_map.end()) return;

    // Don't evict for shared entries — they don't own VRAM exclusively.
    // The VRAM is owned by the FrameCache's CachedFrame.  We just track
    // the descriptor for dirty-tracking lookups.

    m_lru.push_front(key);
    Entry entry;
    entry.width            = width;
    entry.height           = height;
    entry.bytes            = textureBytes;
    entry.isPacked         = isPacked;
    entry.isPMA            = isPMA;
    entry.isLoopFrame      = isLoopFrame;
    entry.sharedOwner      = std::move(sharedOwner);
    entry.sharedDescriptor = descriptor;
    entry.lruIt            = m_lru.begin();

    m_used += textureBytes;
    m_map.emplace(key, std::move(entry));
}

// ── Maintenance ─────────────────────────────────────────────────────────────

void GpuTextureCache::clear()
{
    m_map.clear();
    m_lru.clear();
    m_used = 0;
}

void GpuTextureCache::evictMedia(uint64_t mediaId)
{
    for (auto it = m_map.begin(); it != m_map.end(); ) {
        if (it->first.mediaId == mediaId) {
            m_used -= it->second.bytes;
            m_lru.erase(it->second.lruIt);
            it = m_map.erase(it);
        } else {
            ++it;
        }
    }
}

void GpuTextureCache::setBudget(size_t bytes)
{
    m_budget = bytes;
    evictUntilFits(0);
}

// ── Pinning ─────────────────────────────────────────────────────────────────

void GpuTextureCache::pin(uint64_t mediaId, int64_t frameNumber, uint8_t tier)
{
    CacheKey key{mediaId, frameNumber, tier};
    auto it = m_map.find(key);
    if (it != m_map.end()) {
        ++it->second.pinCount;
    }
}

void GpuTextureCache::unpin(uint64_t mediaId, int64_t frameNumber, uint8_t tier)
{
    CacheKey key{mediaId, frameNumber, tier};
    auto it = m_map.find(key);
    if (it != m_map.end() && it->second.pinCount > 0) {
        --it->second.pinCount;
    }
}

void GpuTextureCache::unpinAll()
{
    for (auto& [key, entry] : m_map)
        entry.pinCount = 0;
}

// ── Eviction ────────────────────────────────────────────────────────────────

void GpuTextureCache::evictUntilFits(size_t needed)
{
    // Pass 1: evict only non-loop, non-pinned frames from the LRU back.
    // Loop frames belong to short looping clips that are guaranteed to
    // be needed again every 1-3 seconds; evicting them just forces a
    // CPU→GPU re-upload on the next loop iteration, producing a visible
    // pipeline bubble (typically 30-60ms per frame for a 1920×3840
    // packed-alpha character).  Walk the LRU back-to-front skipping
    // loop entries and pinned entries (those referenced by in-flight
    // GPU command buffers).
    auto walkAndEvict = [&](bool includeLoopFrames) {
        if (m_lru.empty()) return;
        auto it = m_lru.end();
        while (m_used + needed > m_budget && it != m_lru.begin()) {
            --it;
            auto mapIt = m_map.find(*it);
            if (mapIt == m_map.end()) {
                it = m_lru.erase(it);
                continue;
            }
            // Skip pinned entries — still referenced by in-flight GPU work
            if (mapIt->second.pinCount > 0)
                continue;
            if (!includeLoopFrames && mapIt->second.isLoopFrame) {
                continue;
            }
            m_used -= mapIt->second.bytes;
            // A4: recycle the Texture instead of destroying it, if a
            // recycle callback is installed (typically by GpuUploadManager).
            if (m_recycleFn && mapIt->second.texture) {
                m_recycleFn(mapIt->second.texture);
            }
            m_map.erase(mapIt);
            it = m_lru.erase(it);
        }
    };
    walkAndEvict(/*includeLoopFrames=*/false);
    if (m_used + needed > m_budget)
        walkAndEvict(/*includeLoopFrames=*/true);
}

} // namespace rt
