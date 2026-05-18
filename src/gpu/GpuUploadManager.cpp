/*
 * GpuUploadManager.cpp - Texture upload orchestration for GPU compositing.
 *
 * Extracted from CompositeServiceGpuOrchestration.cpp (Phase 4.1).
 * Handles all CPU→GPU texture transfer: cacheable uploads via staging ring,
 * pool texture uploads, mask uploads, and staging buffer cleanup.
 */

#include "GpuUploadManager.h"

#include "GpuContext.h"
#include "GpuTextureCache.h"
#include "StagingRing.h"
#include "CompositeServiceLayerBuild.h"  // LayerInfo
#include "vulkan/Texture.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace rt {

// ── Constructor / Destructor ──────────────────────────────────────────────

GpuUploadManager::GpuUploadManager(GpuContext& ctx, StagingRing& ring)
    : m_ctx(ctx)
    , m_ring(ring)
{
}

GpuUploadManager::~GpuUploadManager()
{
    shutdown();
}

// ── Frame lifecycle ──────────────────────────────────────────────────────

void GpuUploadManager::beginFrame(VkCommandBuffer cmd, int submissionSlot)
{
    m_cmd = cmd;
    m_stagingCleanups.clear();
    m_currentSubmissionSlot = submissionSlot;

    // Unpin only the textures that were pinned during this slot's previous
    // submission.  Since beginFrame() is called after the slot's fence wait
    // (the triple-buffered slot guarantees GPU completion before reuse), we
    // know all textures pinned by this slot's prior use are safe to unpin.
    //
    // Critically, this does NOT unpin textures pinned by OTHER in-flight
    // slots — those textures remain protected until their own slot recycles.
    releaseSlotPins(submissionSlot);

    // Reset the staging ring for this frame's allocations.
    m_ring.reset();
}

void GpuUploadManager::endFrame()
{
    // Destroy any staging buffers allocated via the batched fallback path.
    // Ring-buffer allocations are automatically reclaimed on next reset().
    for (auto& sc : m_stagingCleanups)
        sc.destroy();
    m_stagingCleanups.clear();
    m_cmd = VK_NULL_HANDLE;
}

// ── Layer texture upload ─────────────────────────────────────────────────

GpuUploadResult GpuUploadManager::uploadLayer(
    const LayerInfo& layer,
    Texture& poolTex,
    uint64_t& poolMediaId,
    int64_t& poolFrameNo,
    const void*& poolFramePtr,
    bool scrubMode)
{
    GpuUploadResult result;

    // ── GPU-cache hit path ──────────────────────────────────────────────
    // If this exact frame was uploaded before, skip the PCIe transfer and
    // reuse the cached GPU texture.
    if (m_texCache && layer.frame && layer.frame->mediaId != 0) {
        auto hit = m_texCache->get(
            layer.frame->mediaId, layer.frame->frameNumber,
            static_cast<uint8_t>(layer.frame->tier));
        if (hit.found &&
            hit.width  == layer.frame->width &&
            hit.height == layer.frame->height)
        {
            // Pin so the texture survives until the GPU fence signals.
            // Also record the pin in the current slot's tracker so that
            // only this slot's pins are released on recycle — other
            // in-flight slots' pins remain intact.
            recordPin(layer.frame->mediaId, layer.frame->frameNumber,
                      static_cast<uint8_t>(layer.frame->tier));

            result.descriptor = hit.descriptor;
            result.success    = true;
            result.cacheHit   = true;
            result.isPacked   = hit.isPacked;
            result.isPMA      = hit.isPMA;
            result.srcW       = hit.width;
            result.srcH       = hit.isPacked && hit.height > 1
                                    ? hit.height / 2 : hit.height;
            return result;
        }
    }

    // No frame data — cannot upload.
    if (!layer.frame || layer.frame->pixels.empty()) {
        return result;  // success=false
    }

    const VkDeviceSize dataSize = static_cast<VkDeviceSize>(
        layer.frame->pixels.size());

    // Determine if this frame can be GPU-cached for later reuse.
    const bool cacheable = (layer.frame->mediaId != 0) &&
                           (layer.isLoopContent ||
                            layer.frame->isLoopFrame ||
                            scrubMode);

    VkDescriptorImageInfo uploadedDescInfo{};

    if (cacheable && m_texCache) {
        // ── Cache-owned texture path ────────────────────────────────────
        TextureConfig cfg;
        cfg.width  = layer.frame->width;
        cfg.height = layer.frame->height;
        cfg.format = VK_FORMAT_B8G8R8A8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        // A4: try the recycled-texture pool first.  Eviction from
        // GpuTextureCache returns textures here via setRecycleFn — under
        // steady-state churn (scrub through a long timeline) almost every
        // allocation is a pool hit and we avoid vmaCreateImage entirely.
        auto cacheTex = acquireFromPool(cfg.width, cfg.height,
                                         cfg.format, cfg.usage);
        const bool poolHit = (cacheTex != nullptr);
        if (!cacheTex)
            cacheTex = std::make_unique<Texture>();

        // First-time install of the recycle hook.  Cheap (one if-check
        // per upload) but unconditional install would lose the callback
        // on re-binding to a different cache (does not happen today).
        if (!m_recycleHookInstalled) {
            const VkFormat poolFmt = cfg.format;
            const VkImageUsageFlags poolUsage = cfg.usage;
            m_texCache->setRecycleFn(
                [this, poolFmt, poolUsage](std::unique_ptr<Texture>& evicted) {
                    if (!evicted) return;
                    const uint32_t w = evicted->width();
                    const uint32_t h = evicted->height();
                    if (w == 0 || h == 0) return;
                    releaseToPool(std::move(evicted), w, h, poolFmt, poolUsage);
                });
            m_recycleHookInstalled = true;
        }

        bool usedRing = false;
        bool uploadOk = false;
        if (poolHit) {
            // Reuse the existing VkImage — just stream new pixels via the
            // staging ring.  Skips vmaCreateImage entirely.
            uploadOk = updateViaRingOrBatched(*cacheTex,
                                              layer.frame->pixels.data(),
                                              dataSize);
            usedRing = uploadOk;
        } else {
            uploadOk = uploadViaRingOrBatched(*cacheTex,
                layer.frame->pixels.data(), dataSize,
                cfg.width, cfg.height, cfg.format, cfg.usage, true);
            usedRing = uploadOk;
        }
        if (!uploadOk) {
            spdlog::warn("[UPLOAD] cache texture upload failed for mediaId={} frame={}",
                         layer.frame->mediaId, layer.frame->frameNumber);
            return result;
        }

        uploadedDescInfo = cacheTex->descriptorInfo();

        // Transfer ownership to the GPU texture cache.
        m_texCache->put(layer.frame->mediaId, layer.frame->frameNumber,
                        static_cast<uint8_t>(layer.frame->tier),
                        std::move(cacheTex), static_cast<size_t>(dataSize),
                        layer.isPacked, layer.isPMA,
                        layer.frame->isLoopFrame);

        // Pin the newly cached texture.  This prevents it from being
        // evicted during subsequent put() calls in the same frame (e.g.
        // for later layers) that might trigger LRU eviction.  The pin
        // is tracked per-slot and released when the slot recycles.
        recordPin(layer.frame->mediaId, layer.frame->frameNumber,
                  static_cast<uint8_t>(layer.frame->tier));

        result.descriptor = uploadedDescInfo;
        result.success    = true;
        result.usedRing   = usedRing;
        result.srcW       = layer.frame->width;
        result.srcH       = layer.isPacked && layer.frame->height > 1
                                ? layer.frame->height / 2 : layer.frame->height;
        return result;
    }

    // ── Pool texture path (non-cacheable layers) ────────────────────────
    const uint64_t fMediaId = layer.frame->mediaId;
    const int64_t  fFrameNo = layer.frame->frameNumber;

    // Dirty tracking: if the exact same frame is already in this pool slot,
    // skip the CPU→GPU upload entirely.
    //
    // Identity check includes the CachedFrame pointer: live file replacement
    // produces a NEW CachedFrame under the SAME (mediaId, frameNumber) but
    // with different pixels.  Comparing (mediaId, frameNumber) alone caused
    // the skip to fire incorrectly, leaving the OLD pool texture in place
    // after an overwrite (the "needs scrub" symptom).
    const void* curFramePtr = static_cast<const void*>(layer.frame.get());
    if (poolMediaId == fMediaId &&
        poolFrameNo == fFrameNo &&
        poolFramePtr == curFramePtr &&
        fMediaId != 0 &&
        poolTex.image() != VK_NULL_HANDLE &&
        poolTex.width() == layer.frame->width &&
        poolTex.height() == layer.frame->height)
    {
        // Reuse existing GPU texture as-is
        result.descriptor = poolTex.descriptorInfo();
        result.success    = true;
        result.cacheHit   = false;  // not from cache, but no transfer needed
        result.srcW       = layer.frame->width;
        result.srcH       = layer.isPacked && layer.frame->height > 1
                                ? layer.frame->height / 2 : layer.frame->height;
        return result;
    }

    // Need to upload (either new texture or update existing)
    bool usedRing = false;
    if (poolTex.width() != layer.frame->width ||
        poolTex.height() != layer.frame->height ||
        poolTex.image() == VK_NULL_HANDLE)
    {
        // Destroy old texture and create a new one with correct dimensions.
        poolTex.destroy();

        TextureConfig cfg;
        cfg.width  = layer.frame->width;
        cfg.height = layer.frame->height;
        cfg.format = VK_FORMAT_B8G8R8A8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        if (!uploadViaRingOrBatched(poolTex,
                layer.frame->pixels.data(), dataSize,
                cfg.width, cfg.height, cfg.format, cfg.usage, true))
        {
            spdlog::warn("[UPLOAD] pool texture create failed for layer mediaId={} frame={}",
                         fMediaId, fFrameNo);
            return result;
        }
        usedRing = true;
    } else {
        // Update existing texture with new pixel data (same dimensions).
        if (!updateViaRingOrBatched(poolTex,
                layer.frame->pixels.data(), dataSize))
        {
            spdlog::warn("[UPLOAD] pool texture update failed for layer mediaId={} frame={}",
                         fMediaId, fFrameNo);
            return result;
        }
        usedRing = true;
    }

    // Update dirty-tracking key (including frame pointer for re-decode
    // detection — same (mediaId, frame) but new CachedFrame means new pixels).
    poolMediaId  = fMediaId;
    poolFrameNo  = fFrameNo;
    poolFramePtr = curFramePtr;

    result.descriptor = poolTex.descriptorInfo();
    result.success    = true;
    result.usedRing   = usedRing;
    result.srcW       = layer.frame->width;
    result.srcH       = layer.isPacked && layer.frame->height > 1
                            ? layer.frame->height / 2 : layer.frame->height;
    return result;
}

// ── Mask texture upload ──────────────────────────────────────────────────

bool GpuUploadManager::uploadMask(
    const std::vector<uint8_t>& maskPixels,
    Texture& maskTex,
    uint32_t outW, uint32_t outH,
    VkDescriptorImageInfo& outMaskDesc)
{
    if (maskPixels.empty()) {
        return false;
    }

    const VkDeviceSize maskDataSize = static_cast<VkDeviceSize>(maskPixels.size());
    bool maskUsedRing = false;

    if (maskTex.width() != outW || maskTex.height() != outH ||
        maskTex.image() == VK_NULL_HANDLE)
    {
        maskTex.destroy();

        TextureConfig maskCfg;
        maskCfg.width  = outW;
        maskCfg.height = outH;
        maskCfg.format = VK_FORMAT_R8G8B8A8_UNORM;
        maskCfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        if (!uploadViaRingOrBatched(maskTex,
                maskPixels.data(), maskDataSize,
                maskCfg.width, maskCfg.height, maskCfg.format, maskCfg.usage, true))
        {
            spdlog::warn("[UPLOAD] mask texture create failed for {}x{}", outW, outH);
            return false;
        }
        maskUsedRing = true;
    } else {
        if (!updateViaRingOrBatched(maskTex,
                maskPixels.data(), maskDataSize))
        {
            spdlog::warn("[UPLOAD] mask texture update failed for {}x{}", outW, outH);
            return false;
        }
        maskUsedRing = true;
    }

    outMaskDesc = maskTex.descriptorInfo();
    return true;
}

// ── Per-slot pin tracking ──────────────────────────────────────────────

void GpuUploadManager::recordPin(uint64_t mediaId, int64_t frameNumber, uint8_t tier)
{
    if (!m_texCache) return;

    // A9: bound the per-slot pin count.  A 50-layer composite would pin
    // 50 textures × 3 ring slots = 150 simultaneously-pinned textures
    // which can prevent the LRU from evicting anything and push the cache
    // into oversubscription.  When the slot reaches kMaxPinsPerSlot,
    // unpin the OLDEST pin in this slot (FIFO) before adding the new one.
    // The old texture's CachedFrame still keeps a shared_ptr while it's
    // referenced, so unpinning here only allows LRU eviction once the
    // last reference drops — not immediate destruction mid-flight.
    if (m_currentSubmissionSlot >= 0 && m_currentSubmissionSlot < kRingSize) {
        auto& pins = m_slotPins[m_currentSubmissionSlot];
        if (pins.size() >= kMaxPinsPerSlot) {
            auto& oldest = pins.front();
            m_texCache->unpin(oldest.mediaId, oldest.frameNumber, oldest.tier);
            pins.erase(pins.begin());
        }
        m_texCache->pin(mediaId, frameNumber, tier);
        pins.push_back({mediaId, frameNumber, tier});
    } else {
        // No slot context — just pin without tracking.  This path is
        // exercised only by tests / non-composite uploads.
        m_texCache->pin(mediaId, frameNumber, tier);
    }
}

void GpuUploadManager::releaseSlotPins(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= kRingSize) return;
    if (!m_texCache) {
        m_slotPins[slotIndex].clear();
        return;
    }
    for (const auto& pk : m_slotPins[slotIndex]) {
        m_texCache->unpin(pk.mediaId, pk.frameNumber, pk.tier);
    }
    m_slotPins[slotIndex].clear();
}

// ── Shutdown ─────────────────────────────────────────────────────────────

void GpuUploadManager::shutdown()
{
    endFrame();
    if (m_texCache) {
        // Drop the recycle hook before m_texCache is cleared — it
        // captures `this` and we don't want eviction during the cache's
        // own destruction to dereference a destroyed pool.
        m_texCache->setRecycleFn(nullptr);
    }
    m_texPool.clear();
    m_texCache = nullptr;
}

// ── A4: recycled-texture pool ────────────────────────────────────────────

std::unique_ptr<Texture> GpuUploadManager::acquireFromPool(
    uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage)
{
    PoolKey key{w, h, fmt, usage};
    auto it = m_texPool.find(key);
    if (it == m_texPool.end() || it->second.empty())
        return nullptr;
    std::unique_ptr<Texture> tex = std::move(it->second.back());
    it->second.pop_back();
    return tex;
}

void GpuUploadManager::releaseToPool(std::unique_ptr<Texture> tex,
                                     uint32_t w, uint32_t h,
                                     VkFormat fmt, VkImageUsageFlags usage)
{
    if (!tex) return;
    PoolKey key{w, h, fmt, usage};
    auto& bucket = m_texPool[key];
    if (bucket.size() < kMaxPoolPerShape) {
        bucket.push_back(std::move(tex));
    }
    // Else: drop on the floor — tex's destructor releases VMA memory.
}

// ── Private helpers ──────────────────────────────────────────────────────

bool GpuUploadManager::uploadViaRingOrBatched(
    Texture& tex,
    const void* data, VkDeviceSize dataSize,
    uint32_t width, uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    bool isNewTexture)
{
    (void)isNewTexture;

    TextureConfig cfg;
    cfg.width  = width;
    cfg.height = height;
    cfg.format = format;
    cfg.usage  = usage;

    // Try ring-buffer path first (zero VMA alloc per frame).
    if (m_ring.isInitialized() &&
        tex.createFromDataRing(
            m_ctx.allocator().handle(), m_ctx.vkDevice(), cfg,
            data, dataSize,
            m_cmd, m_ring))
    {
        return true;
    }

    // Fall back to batched upload (creates a staging buffer via VMA).
    Texture::StagingCleanup staging;
    if (!tex.createFromDataBatched(
            m_ctx.allocator().handle(), m_ctx.vkDevice(), cfg,
            data, dataSize,
            m_cmd, staging))
    {
        return false;
    }

    if (staging.buffer != VK_NULL_HANDLE)
        m_stagingCleanups.push_back(staging);
    return true;
}

bool GpuUploadManager::updateViaRingOrBatched(
    Texture& tex,
    const void* data, VkDeviceSize dataSize)
{
    // Try ring-buffer path first.
    if (m_ring.isInitialized() &&
        tex.updateDataRing(data, dataSize, m_cmd, m_ring))
    {
        return true;
    }

    // Fall back to batched update.
    Texture::StagingCleanup staging;
    if (!tex.updateDataBatched(data, dataSize, m_cmd, staging))
        return false;

    if (staging.buffer != VK_NULL_HANDLE)
        m_stagingCleanups.push_back(staging);
    return true;
}

} // namespace rt
