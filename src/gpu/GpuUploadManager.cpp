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

void GpuUploadManager::beginFrame(VkCommandBuffer cmd)
{
    m_cmd = cmd;
    m_stagingCleanups.clear();

    // Unpin all previously-pinned textures.  Since beginFrame() is called
    // after the slot's fence wait (in the triple-buffering scheme), the
    // previous GPU work using this slot is guaranteed complete, so all
    // previously-pinned textures are safe to unpin.
    if (m_texCache)
        m_texCache->unpinAll();

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
    bool scrubMode)
{
    GpuUploadResult result;

    // ── GPU-cache hit path ──────────────────────────────────────────────
    // If this exact frame was uploaded before, skip the PCIe transfer and
    // reuse the cached GPU texture.
    if (m_texCache && layer.frame && layer.frame->mediaId != 0) {
        auto hit = m_texCache->get(
            layer.frame->mediaId, layer.frame->frameNumber);
        if (hit.found &&
            hit.width  == layer.frame->width &&
            hit.height == layer.frame->height)
        {
            // Pin so the texture survives until the GPU fence signals.
            m_texCache->pin(layer.frame->mediaId, layer.frame->frameNumber);

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
        auto cacheTex = std::make_unique<Texture>();
        TextureConfig cfg;
        cfg.width  = layer.frame->width;
        cfg.height = layer.frame->height;
        cfg.format = VK_FORMAT_B8G8R8A8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        bool usedRing = false;
        if (uploadViaRingOrBatched(*cacheTex,
                layer.frame->pixels.data(), dataSize,
                cfg.width, cfg.height, cfg.format, cfg.usage, true))
        {
            usedRing = true;
        } else {
            spdlog::warn("[UPLOAD] cache texture upload failed for mediaId={} frame={}",
                         layer.frame->mediaId, layer.frame->frameNumber);
            return result;
        }

        uploadedDescInfo = cacheTex->descriptorInfo();

        // Transfer ownership to the GPU texture cache.
        m_texCache->put(layer.frame->mediaId, layer.frame->frameNumber,
                        std::move(cacheTex), static_cast<size_t>(dataSize),
                        layer.isPacked, layer.isPMA,
                        layer.frame->isLoopFrame);

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
    if (poolMediaId == fMediaId &&
        poolFrameNo == fFrameNo &&
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

    // Update dirty-tracking key.
    poolMediaId = fMediaId;
    poolFrameNo = fFrameNo;

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

// ── Shutdown ─────────────────────────────────────────────────────────────

void GpuUploadManager::shutdown()
{
    endFrame();
    m_texCache = nullptr;
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
