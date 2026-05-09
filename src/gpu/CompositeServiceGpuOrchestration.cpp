/*
 * CompositeServiceGpuOrchestration.cpp - GPU composite orchestration.
 * Extracted from CompositeServiceFrame.cpp (Step 3 of modularization plan).
 */

#include "CompositeService.h"
#include "Compositor.h"
#include "GpuContext.h"
#include "GpuTextureCache.h"
#include "GpuWorkSubmission.h"
#include "StagingRing.h"
#include "CompositeServiceBlend.h"
#include "vulkan/Texture.h"
#include "TransitionRenderer.h"
#include "EffectProcessor.h"

#include "media/FrameCache.h"

#include "effects/LUT.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt {// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

std::shared_ptr<CachedFrame> CompositeService::tryCompositeOnGpu(
    const std::vector<LayerInfo>& layers,
    uint32_t outW, uint32_t outH,
    int64_t tick, bool scrubMode,
    bool perfLog,
    std::chrono::high_resolution_clock::time_point perfT0,
    std::chrono::high_resolution_clock::time_point& perfTlayers,
    int& effectLayerCount, int& effectPassCount,
    int& transitionCount)
{
    auto perfTgpuUp  = perfT0;
    auto perfTcomp   = perfT0;
    // Try GPU compositing first.  Fall through to CPU on failure.
    if (m_gpuCompositeState == 0) {
        m_gpuCompositeState = GpuContext::get().isInitialized() ? 1 : -1;
    }

    // ── GPU backoff check ──────────────────────────────────────────────
    // If a previous GPU submit failed, skip GPU this time if we're still
    // in the cooldown window.  The backoff doubles each failure (100ms →
    // 10s max) so we don't hammer a broken driver while still recovering
    // automatically as soon as it's healthy again.  No permanent disable.
    if (m_gpuCompositeState == 1) {
        using namespace std::chrono;
        if (m_gpuBackoffAttempts > 0 &&
            steady_clock::now() < m_gpuBackoffUntil) {
            // Still in cooldown — skip GPU this frame, serve stale frame
            return nullptr;
        }
        if (m_gpuBackoffAttempts > 0) {
            // Cooldown expired — retry GPU fresh
            spdlog::info("[COMPOSITE] GPU backoff expired after {} attempts, retrying",
                         m_gpuBackoffAttempts);
        }
    }

    if (m_gpuCompositeState == 1) {
        auto* comp = GpuContext::get().compositor(outW, outH);
        if (comp && comp->isInitialized()) {
            auto& ctx = GpuContext::get();

            // Ensure texture pool is large enough
            while (m_gpuLayerTextures.size() < layers.size())
                m_gpuLayerTextures.push_back(std::make_unique<Texture>());
            m_gpuLayerTexKeys.resize(m_gpuLayerTextures.size());

            std::vector<CompositorLayer> gpuLayers;
            gpuLayers.reserve(layers.size());
            bool uploadOk = true;
            
            
            

            // ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼ RECORDING GPU COMMANDS ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼
            // Record ALL texture uploads, effects, transitions, composite,
            // and readback into a SINGLE command buffer. Submitted once at
            // the end with a persistent fence (no per-frame alloc/destroy).
            // Uses GpuWorkSubmission to encapsulate the VkFence + cmd buffer.
            if (!m_gpuSubmission) {
                m_gpuSubmission = std::make_unique<GpuWorkSubmission>();
                m_gpuSubmission->init(ctx.vkDevice(), ctx.cmdPool().handle());
            }
            auto& slot = *m_gpuSubmission;

            // Begin recording commands.
            slot.beginRecording();
            VkCommandBuffer uploadCmd = slot.cmdBuffer();
            std::vector<Texture::StagingCleanup> stagingCleanups;
            stagingCleanups.reserve(layers.size());

            // Lazily init persistent staging ring (64 MB ├óΓé¼ΓÇ¥ enough for ~8
            // layers at 1920├âΓÇö1080├âΓÇö4).  Reset each frame after fence wait.
            if (!m_stagingRing.isInitialized())
                m_stagingRing.init(ctx.allocator().handle(), 64u * 1024u * 1024u);
            m_stagingRing.reset();

            // Lazily create GPU texture cache with a VRAM budget of 25%
            // of the GPU's actual device-local memory.  Clamped 512 MB-8 GB
            // so we never starve the compositor/swapchain but also never
            // show a false-positive "VRAM 99%" warning.
            if (!m_gpuTexCache) {
                const auto memStats = ctx.allocator().queryStats();
                const size_t gpuVram = memStats.deviceLocalBudgetBytes;
                const size_t budget = std::clamp<size_t>(
                    gpuVram / 4, 512ull * 1024 * 1024, 8ull * 1024 * 1024 * 1024);
                m_gpuTexCache = std::make_unique<GpuTextureCache>(budget);
                spdlog::info("[PERF] GpuTexCache budget: {:.0f} MB (GPU VRAM: {:.0f} MB)",
                             budget / 1048576.0, gpuVram / 1048576.0);
            }

            for (size_t li = 0; li < layers.size(); ++li) {
                const auto& layer = layers[li];

                // GPU-resident layers (e.g. future zero-copy SpineRenderer FBO)
                // skip the CPU upload path entirely.
                if (layer.gpuTextureReady) {
                    CompositorLayer cl;
                    cl.textureInfo = layer.gpuDescriptor;
                    uint32_t srcW = layer.frameWidth  ? layer.frameWidth  : outW;
                    uint32_t srcH = layer.frameHeight ? layer.frameHeight : outH;
                    // Packed-alpha: visible area is the top half of the texture
                    if (layer.isPacked && srcH > 1) srcH /= 2;
                    cl.transform = Compositor::buildViewportTransform(
                        srcW, srcH, outW, outH,
                        layer.posX, layer.posY,
                        layer.scX, layer.scY, layer.rot,
                        layer.containFit);
                    cl.opacity   = layer.opacity;
                    cl.blendMode = static_cast<BlendMode>(layer.blendMode);
                    cl.enabled   = true;
                    cl.isPacked  = layer.isPacked;
                    cl.isPMA     = layer.isPMA;
                    cl.needsSwapRB = layer.needsSwapRB;
                    cl.cropLeft   = layer.cropL / 100.0f;
                    cl.cropRight  = layer.cropR / 100.0f;
                    cl.cropTop    = layer.cropT / 100.0f;
                    cl.cropBottom = layer.cropB / 100.0f;
                    gpuLayers.push_back(cl);
                    continue;
                }

                // ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼ GPU texture cache lookup ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼
                // If this exact frame was uploaded before, skip the PCIe
                // transfer and reuse the cached GPU texture.
                if (layer.frame && layer.frame->mediaId != 0) {
                    auto hit = m_gpuTexCache->get(
                        layer.frame->mediaId, layer.frame->frameNumber);
                    if (hit.found &&
                        hit.width  == layer.frame->width &&
                        hit.height == layer.frame->height)
                    {
                        CompositorLayer cl;
                        cl.textureInfo = hit.descriptor;
                        uint32_t srcW = hit.width;
                        uint32_t srcH = hit.height;
                        // Use the isPacked flag stored in the GPU cache
                        // entry (set at upload time) ├óΓé¼ΓÇ¥ not layer.isPacked
                        // which may differ due to frame-drop fallback.
                        if (hit.isPacked && srcH > 1) srcH /= 2;
                        cl.transform = Compositor::buildViewportTransform(
                            srcW, srcH, outW, outH,
                            layer.posX, layer.posY,
                            layer.scX, layer.scY, layer.rot,
                            layer.containFit);
                        cl.opacity   = layer.opacity;
                        cl.blendMode = static_cast<BlendMode>(layer.blendMode);
                        cl.enabled   = true;
                        cl.isPacked  = hit.isPacked;
                        cl.isPMA     = layer.isPMA;
                        cl.needsSwapRB = layer.needsSwapRB;
                        cl.cropLeft   = layer.cropL / 100.0f;
                        cl.cropRight  = layer.cropR / 100.0f;
                        cl.cropTop    = layer.cropT / 100.0f;
                        cl.cropBottom = layer.cropB / 100.0f;
                        gpuLayers.push_back(cl);
                        continue;
                    }
                }

                const VkDeviceSize dataSize = static_cast<VkDeviceSize>(
                    layer.frame->pixels.size());

                // Skip layer if pixels are empty
                if (dataSize == 0) {
                    spdlog::warn("compositeFrame: layer {} has 0 pixels, skipping", li);
                    continue;
                }

                // Determine if this frame can be GPU-cached for later reuse.
                // Looping animations cycle through the same frame numbers
                // repeatedly ΓÇö caching their GPU textures means the dirty-
                // tracking shortcut (above) skips ALL decode + upload work
                // after the first loop.  For non-looping advancing video,
                // use the faster pool-texture path (reuses VkImage, no VMA alloc).
                //
                // Scrub mode also benefits from caching ΓÇö the user may
                // revisit the same frame repeatedly.  Linear playback of
                // non-loop video does NOT: at 1920├ù1080 each cached
                // texture is 8.1 MB, so a 20-second clip at 30 fps
                // fills a 12 GB VRAM budget in ~25 seconds, then every
                // upload contends with LRU eviction ΓÇö causing exactly
                // the micro-stutters the user is reporting.
                const bool cacheable = (layer.frame->mediaId != 0) &&
                                       (layer.isLoopContent ||
                                        layer.frame->isLoopFrame ||
                                        scrubMode);

                if (perfLog) {
                    spdlog::info("  [PERF] GPU upload: mediaId={} frame={} cacheable={} "
                                 "isLoop={} scrub={} pixels={}KB",
                                 layer.frame->mediaId, layer.frame->frameNumber,
                                 cacheable, layer.isLoopContent, scrubMode,
                                 layer.frame->pixels.size() / 1024);
                }

                VkDescriptorImageInfo uploadedDescInfo{};
                Texture::StagingCleanup staging{};
                bool usedRing = false;

                if (cacheable) {
                    // ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼ Cache-owned texture path ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼
                    // Allocate a NEW texture that will live in the GPU cache.
                    // This avoids pool overwrite and persists across frames,
                    // so future lookups skip the CPU├â┬ó├óΓé¼┬á├óΓé¼ΓäóGPU upload entirely.
                    auto cacheTex = std::make_unique<Texture>();
                    TextureConfig cfg;
                    cfg.width  = layer.frame->width;
                    cfg.height = layer.frame->height;
                    cfg.format = VK_FORMAT_B8G8R8A8_UNORM;
                    cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                    // Try ring-buffer path first (zero VMA alloc)
                    if (m_stagingRing.isInitialized() &&
                        cacheTex->createFromDataRing(
                            ctx.allocator().handle(), ctx.vkDevice(), cfg,
                            layer.frame->pixels.data(), dataSize,
                            uploadCmd, m_stagingRing))
                    {
                        usedRing = true;
                    } else if (!cacheTex->createFromDataBatched(
                            ctx.allocator().handle(), ctx.vkDevice(), cfg,
                            layer.frame->pixels.data(), dataSize,
                            uploadCmd, staging))
                    {
                        spdlog::warn("compositeFrame GPU: cache texture upload failed for layer {}", li);
                        uploadOk = false;
                        break;
                    }
                    uploadedDescInfo = cacheTex->descriptorInfo();

                    // Transfer ownership to the GPU texture cache.
                    // LRU eviction only trims the oldest entries (back of
                    // list), so textures accessed in this frame (spliced to
                    // front by get()) are never evicted mid-batch.
                    m_gpuTexCache->put(layer.frame->mediaId,
                                       layer.frame->frameNumber,
                                       std::move(cacheTex), dataSize,
                                       layer.isPacked, layer.isPMA,
                                       layer.frame->isLoopFrame);
                } else {
                    // Pool texture path (non-cacheable layers)
                    auto& tex = m_gpuLayerTextures[li];
                    auto& poolKey = m_gpuLayerTexKeys[li];

                    // Dirty tracking: if the exact same frame is already
                    // in this pool slot, skip the CPU->GPU upload entirely.
                    const uint64_t fMediaId = layer.frame->mediaId;
                    const int64_t  fFrameNo = layer.frame->frameNumber;
                    if (poolKey.mediaId == fMediaId &&
                        poolKey.frameNumber == fFrameNo &&
                        fMediaId != 0 &&
                        tex->image() != VK_NULL_HANDLE &&
                        tex->width() == layer.frame->width &&
                        tex->height() == layer.frame->height)
                    {
                        // Reuse existing GPU texture as-is
                        uploadedDescInfo = tex->descriptorInfo();
                    } else {
                    if (tex->width() != layer.frame->width ||
                        tex->height() != layer.frame->height ||
                        tex->image() == VK_NULL_HANDLE)
                    {
                        tex->destroy();
                        TextureConfig cfg;
                        cfg.width  = layer.frame->width;
                        cfg.height = layer.frame->height;
                        cfg.format = VK_FORMAT_B8G8R8A8_UNORM;
                        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                        if (m_stagingRing.isInitialized() &&
                            tex->createFromDataRing(
                                ctx.allocator().handle(), ctx.vkDevice(), cfg,
                                layer.frame->pixels.data(), dataSize,
                                uploadCmd, m_stagingRing))
                        {
                            usedRing = true;
                        } else if (!tex->createFromDataBatched(
                                ctx.allocator().handle(), ctx.vkDevice(), cfg,
                                layer.frame->pixels.data(), dataSize,
                                uploadCmd, staging))
                        {
                            spdlog::warn("compositeFrame GPU: texture upload failed for layer {}", li);
                            uploadOk = false;
                            break;
                        }
                    } else {
                        if (m_stagingRing.isInitialized() &&
                            tex->updateDataRing(
                                layer.frame->pixels.data(), dataSize,
                                uploadCmd, m_stagingRing))
                        {
                            usedRing = true;
                        } else if (!tex->updateDataBatched(
                                layer.frame->pixels.data(), dataSize,
                                uploadCmd, staging))
                        {
                            uploadOk = false;
                            break;
                        }
                    }
                    poolKey.mediaId = fMediaId;
                    poolKey.frameNumber = fFrameNo;
                    uploadedDescInfo = tex->descriptorInfo();
                    }
                }

                if (!usedRing && staging.buffer != VK_NULL_HANDLE)
                    stagingCleanups.push_back(staging);

                // Build CompositorLayer ├â┬ó├óΓÇÜ┬¼├óΓé¼┬¥ effects run AFTER upload submit
                CompositorLayer cl;
                cl.textureInfo = uploadedDescInfo;

                // Build fit transform (contain for pre-rendered spine, cover otherwise)
                {
                    uint32_t srcW = layer.frame->width;
                    uint32_t srcH = layer.frame->height;
                    if (layer.isPacked && srcH > 1) srcH /= 2;
                    cl.transform = Compositor::buildViewportTransform(
                        srcW, srcH,
                        outW, outH,
                        layer.posX, layer.posY,
                        layer.scX, layer.scY,
                        layer.rot,
                        layer.containFit);
                }

                cl.opacity   = layer.opacity;
                cl.blendMode = static_cast<BlendMode>(layer.blendMode);
                cl.enabled   = true;
                cl.isPacked  = layer.isPacked;
                cl.isPMA     = layer.isPMA;
                cl.needsSwapRB = layer.needsSwapRB;

                // Crop: CPU uses 0├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇª├óΓé¼┼ô100 percentages, GPU uses 0├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇª├óΓé¼┼ô1 fractions
                cl.cropLeft   = layer.cropL / 100.0f;
                cl.cropRight  = layer.cropR / 100.0f;
                cl.cropTop    = layer.cropT / 100.0f;
                cl.cropBottom = layer.cropB / 100.0f;

                gpuLayers.push_back(cl);
            }

            // -- Upload mask textures for layers that have masks ---------
            while (m_gpuMaskTextures.size() < layers.size())
                m_gpuMaskTextures.push_back(std::make_unique<Texture>());

            for (size_t li = 0; li < layers.size() && li < gpuLayers.size(); ++li) {
                const auto& layer = layers[li];
                if (!layer.clipPtr || layer.clipPtr->maskCount() == 0)
                    continue;
                auto maskPixels = rasterizeMasks(layer.clipPtr->masks(), outW, outH);
                const VkDeviceSize maskDataSize = static_cast<VkDeviceSize>(maskPixels.size());
                auto& maskTex = m_gpuMaskTextures[li];
                Texture::StagingCleanup maskStaging{};
                bool maskUsedRing = false;
                if (maskTex->width() != outW || maskTex->height() != outH ||
                    maskTex->image() == VK_NULL_HANDLE)
                {
                    maskTex->destroy();
                    TextureConfig maskCfg;
                    maskCfg.width  = outW;
                    maskCfg.height = outH;
                    maskCfg.format = VK_FORMAT_R8G8B8A8_UNORM;
                    maskCfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                    if (m_stagingRing.isInitialized() &&
                        maskTex->createFromDataRing(
                            ctx.allocator().handle(), ctx.vkDevice(), maskCfg,
                            maskPixels.data(), maskDataSize,
                            uploadCmd, m_stagingRing))
                    {
                        maskUsedRing = true;
                    } else if (!maskTex->createFromDataBatched(
                            ctx.allocator().handle(), ctx.vkDevice(), maskCfg,
                            maskPixels.data(), maskDataSize,
                            uploadCmd, maskStaging))
                    {
                        spdlog::warn("compositeFrame GPU: mask texture upload failed for layer {}", li);
                        continue;
                    }
                } else {
                    if (m_stagingRing.isInitialized() &&
                        maskTex->updateDataRing(
                            maskPixels.data(), maskDataSize,
                            uploadCmd, m_stagingRing))
                    {
                        maskUsedRing = true;
                    } else if (!maskTex->updateDataBatched(
                            maskPixels.data(), maskDataSize,
                            uploadCmd, maskStaging))
                    {
                        continue;
                    }
                }
                if (!maskUsedRing && maskStaging.buffer != VK_NULL_HANDLE)
                    stagingCleanups.push_back(maskStaging);
                gpuLayers[li].hasMask = true;
                gpuLayers[li].maskTextureInfo = maskTex->descriptorInfo();
            }

            // ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼ MERGED SINGLE-SUBMIT GPU PATH ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼
            // Instead of N+M+2 separate command buffer submits (one per
            // upload batch, effect layer, transition, and composite), we
            // record EVERYTHING into the same command buffer with pipeline
            // barriers between stages. This reduces vkQueueWaitIdle calls
            // from N+M+2 down to exactly ONE per frame.

            // Pipeline barrier: texture uploads (transfer) ├â┬ó├óΓé¼┬á├óΓé¼Γäó effects/composite (compute)
            {
                VkMemoryBarrier transferToCompute{};
                transferToCompute.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                transferToCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                transferToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                                                | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(uploadCmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 1, &transferToCompute, 0, nullptr, 0, nullptr);
            }

            // ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼ Effects pass (recorded into same cmd buffer) ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼
            if (uploadOk) {
                for (size_t li = 0; li < layers.size(); ++li) {
                    const auto& layer = layers[li];
                    // Skip layers with no effects to apply
                    if (layer.effects.empty()) continue;
                    // Skip layers with no GPU texture available
                    if (!layer.gpuTextureReady && !layer.frame) continue;
                    {
                        // Use frameWidth/frameHeight for GPU-resident layers
                        // (their frame pointer is null ├óΓé¼ΓÇ¥ source came from GPU cache)
                        uint32_t fxW = layer.gpuTextureReady ? layer.frameWidth  : layer.frame->width;
                        uint32_t fxH = layer.gpuTextureReady ? layer.frameHeight : layer.frame->height;
                        auto* fx = ctx.effectProcessor(fxW, fxH);
                        if (fx && fx->isInitialized()) {
                            ++effectLayerCount;
                            effectPassCount += static_cast<int>(layer.effects.size());

                            // Upload LUT 3D texture if a LUT effect is present
                            for (const auto& snap : layer.effects) {
                                if (snap.type == EffectType::LUT && layer.clipPtr) {
                                    for (size_t ei = 0; ei < layer.clipPtr->effects().effectCount(); ++ei) {
                                        auto& clipFx = layer.clipPtr->effects().effect(ei);
                                        if (clipFx.effectType() == EffectType::LUT && clipFx.isEnabled()) {
                                            auto* lutFx = static_cast<LUT*>(&clipFx);
                                            if (lutFx->hasLUT())
                                                fx->uploadLUT3D(lutFx->lutData(), lutFx->lutSize());
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }

                            // Use gpuLayers descriptor ├â┬ó├óΓÇÜ┬¼├óΓé¼┬¥ works for pool,
                            // cache-owned, and cache-hit textures alike.
                            VkDescriptorImageInfo srcInfo = gpuLayers[li].textureInfo;
                            if (fx->process(uploadCmd, srcInfo, layer.effects)) {
                                gpuLayers[li].textureInfo = fx->outputDescriptorInfo();
                            }
                            // Barrier between effect passes
                            VkMemoryBarrier fxBarrier{};
                            fxBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                            fxBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                            fxBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                                                    | VK_ACCESS_SHADER_WRITE_BIT;
                            vkCmdPipelineBarrier(uploadCmd,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, 1, &fxBarrier, 0, nullptr, 0, nullptr);
                        }
                    }
                }
            }

            // ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼ Wipe transitions (recorded into same cmd buffer) ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼
            bool compOk = false;
            bool readbackOk = false;

            if (uploadOk) {
                // ├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼ GPU wipe transitions: merge pairs through TransitionRenderer ├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼
                // Scan for outgoing wipe layers, find their incoming peer,
                // and replace the pair with the wipe-blended output.
                for (size_t wi = 0; wi < layers.size(); ++wi) {
                    if (layers[wi].wipeProgress < 0.0f)
                        continue;
                    // Normally only outgoing layers drive the transition
                    // pass (the peer is merged in as srcB).  Exception:
                    // a single-clip CrossDissolve fade-in has no peer and
                    // is flagged as non-outgoing ├óΓé¼ΓÇ¥ it must still render.
                    const bool isSingletonIncoming =
                        !layers[wi].isWipeOutgoing
                        && layers[wi].wipePeerClipId == 0
                        && layers[wi].wipeType == TransitionType::CrossDissolve;
                    if (!layers[wi].isWipeOutgoing && !isSingletonIncoming)
                        continue;
                    if (!gpuLayers[wi].enabled) continue;

                    auto* tr = ctx.transitionRenderer(outW, outH);
                    if (!tr || !tr->isInitialized()) continue;

                    // Determine source textures for the transition.
                    // For two-clip transitions, find the incoming peer layer.
                    // For single-clip fades (wipePeerClipId == 0), use a
                    // solid-color placeholder from TransitionRenderer.
                    TransitionSourceInfo srcA{};
                    TransitionSourceInfo srcB{};
                    srcA.textureInfo = gpuLayers[wi].textureInfo;
                    srcA.transform   = gpuLayers[wi].transform;
                    srcA.crop        = glm::vec4(gpuLayers[wi].cropLeft,
                                                 gpuLayers[wi].cropRight,
                                                 gpuLayers[wi].cropTop,
                                                 gpuLayers[wi].cropBottom);
                    srcA.isPacked    = gpuLayers[wi].isPacked;

                    size_t pi = SIZE_MAX;
                    bool usePlaceholder = false;

                    if (layers[wi].wipePeerClipId == 0) {
                        // Single-clip fade: use placeholder as the "other" side
                        usePlaceholder = true;
                        if (layers[wi].wipeType == TransitionType::FadeToWhite) {
                            srcB.textureInfo = tr->whiteDescriptorInfo();
                        } else if (layers[wi].wipeType == TransitionType::FadeFromWhite) {
                            // Swap: white is sourceA, clip is sourceB
                            srcB = srcA;
                            srcA = TransitionSourceInfo{};
                            srcA.textureInfo = tr->whiteDescriptorInfo();
                        } else if (layers[wi].wipeType == TransitionType::FadeToBlack) {
                            srcB.textureInfo = tr->blackDescriptorInfo();
                        } else if (layers[wi].wipeType == TransitionType::FadeFromBlack) {
                            // Swap: black is sourceA, clip is sourceB
                            srcB = srcA;
                            srcA = TransitionSourceInfo{};
                            srcA.textureInfo = tr->blackDescriptorInfo();
                        } else if (layers[wi].wipeType == TransitionType::CrossDissolve
                                   && !layers[wi].isWipeOutgoing) {
                            // Single-clip CrossDissolve incoming: transparent -> clip.
                            srcB = srcA;
                            srcA = TransitionSourceInfo{};
                            srcA.textureInfo = tr->transparentDescriptorInfo();
                        } else {
                            // Default (including CrossDissolve outgoing):
                            // clip -> transparent.
                            srcB.textureInfo = tr->transparentDescriptorInfo();
                        }
                    } else {
                        // Two-clip transition: find incoming peer
                        for (size_t wj = 0; wj < layers.size(); ++wj) {
                            if (wj != wi && layers[wj].clipId == layers[wi].wipePeerClipId) {
                                pi = wj;
                                break;
                            }
                        }
                        if (pi == SIZE_MAX || !gpuLayers[pi].enabled) continue;
                        srcB.textureInfo = gpuLayers[pi].textureInfo;
                        srcB.transform   = gpuLayers[pi].transform;
                        srcB.crop        = glm::vec4(gpuLayers[pi].cropLeft,
                                                     gpuLayers[pi].cropRight,
                                                     gpuLayers[pi].cropTop,
                                                     gpuLayers[pi].cropBottom);
                        srcB.isPacked    = gpuLayers[pi].isPacked;
                    }

                    // Map timeline TransitionType ├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬á├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├â┬ó├óΓé¼┼╛├é┬ó GpuTransitionType
                    GpuTransitionType gt = GpuTransitionType::Dissolve;
                    switch (layers[wi].wipeType) {
                    case TransitionType::CrossDissolve: gt = GpuTransitionType::Dissolve;  break;
                    case TransitionType::WipeLeft:  gt = GpuTransitionType::WipeLeft;  break;
                    case TransitionType::WipeRight: gt = GpuTransitionType::WipeRight; break;
                    case TransitionType::WipeUp:    gt = GpuTransitionType::WipeUp;    break;
                    case TransitionType::WipeDown:  gt = GpuTransitionType::WipeDown;  break;
                    case TransitionType::PushLeft:  gt = GpuTransitionType::PushLeft;  break;
                    case TransitionType::PushRight: gt = GpuTransitionType::PushRight; break;
                    case TransitionType::PushUp:    gt = GpuTransitionType::PushUp;    break;
                    case TransitionType::PushDown:  gt = GpuTransitionType::PushDown;  break;
                    // Dissolve family
                    case TransitionType::DipToBlack:       gt = GpuTransitionType::DipColor;         break;
                    case TransitionType::DipToWhite:       gt = GpuTransitionType::DipColor;         break;
                    case TransitionType::FilmDissolve:     gt = GpuTransitionType::FilmDissolve;     break;
                    case TransitionType::AdditiveDissolve: gt = GpuTransitionType::AdditiveDissolve; break;
                    // Wipe family
                    case TransitionType::BarnDoor:         gt = GpuTransitionType::BarnDoor;         break;
                    case TransitionType::ClockWipe:        gt = GpuTransitionType::ClockWipe;        break;
                    case TransitionType::RadialWipe:       gt = GpuTransitionType::RadialWipe;       break;
                    case TransitionType::IrisRound:        gt = GpuTransitionType::Iris;             break;
                    case TransitionType::IrisDiamond:      gt = GpuTransitionType::Iris;             break;
                    case TransitionType::IrisCross:        gt = GpuTransitionType::Iris;             break;
                    case TransitionType::DiagonalWipe:     gt = GpuTransitionType::DiagonalWipe;     break;
                    case TransitionType::CheckerWipe:      gt = GpuTransitionType::CheckerWipe;      break;
                    case TransitionType::VenetianBlinds:    gt = GpuTransitionType::VenetianBlinds;   break;
                    case TransitionType::Inset:            gt = GpuTransitionType::Inset;            break;
                    // Slide family
                    case TransitionType::SlideLeft:
                    case TransitionType::SlideRight:
                    case TransitionType::SlideUp:
                    case TransitionType::SlideDown:        gt = GpuTransitionType::Slide;            break;
                    case TransitionType::Split:            gt = GpuTransitionType::SplitWipe;        break;
                    case TransitionType::CenterSplit:      gt = GpuTransitionType::SplitWipe;        break;
                    case TransitionType::Swap:             gt = GpuTransitionType::Swap;             break;
                    // Zoom / motion
                    case TransitionType::Zoom:             gt = GpuTransitionType::ZoomTransition;   break;
                    case TransitionType::CrossZoom:        gt = GpuTransitionType::ZoomTransition;   break;
                    case TransitionType::WhipPan:          gt = GpuTransitionType::WhipPan;          break;
                    // Stylized
                    case TransitionType::RandomBlocks:     gt = GpuTransitionType::RandomBlocks;     break;
                    case TransitionType::MorphCut:         gt = GpuTransitionType::MorphCut;         break;
                    case TransitionType::GradientWipe:     gt = GpuTransitionType::GradientWipe;     break;
                    // Single-clip fades (cross-dissolve with placeholder)
                    case TransitionType::FadeToBlack:      gt = GpuTransitionType::Dissolve;         break;
                    case TransitionType::FadeFromBlack:    gt = GpuTransitionType::Dissolve;         break;
                    case TransitionType::FadeToWhite:      gt = GpuTransitionType::Dissolve;         break;
                    case TransitionType::FadeFromWhite:    gt = GpuTransitionType::Dissolve;         break;
                    default: break;
                    }

                    // Set direction override for multi-variant GPU types
                    int32_t dirOvr = -1;
                    float   extraP = 0.0f;
                    switch (layers[wi].wipeType) {
                    case TransitionType::DipToWhite:   dirOvr = 1; break;
                    case TransitionType::IrisDiamond:  dirOvr = 1; break;
                    case TransitionType::IrisCross:    dirOvr = 2; break;
                    case TransitionType::SlideLeft:    dirOvr = 0; break;
                    case TransitionType::SlideRight:   dirOvr = 1; break;
                    case TransitionType::SlideUp:      dirOvr = 2; break;
                    case TransitionType::SlideDown:    dirOvr = 3; break;
                    case TransitionType::CenterSplit:  dirOvr = 1; break;
                    case TransitionType::CrossZoom:    dirOvr = 1; break;
                    default: break;
                    }

                    // Record transition into merged command buffer
                    if (tr->render(uploadCmd, srcA, srcB,
                                       gt, layers[wi].wipeProgress,
                                       dirOvr, extraP,
                                       layers[wi].wipeSoftness))
                    {
                        ++transitionCount;
                        // Replace outgoing layer with the transition output.
                        // The transition output is a full-viewport RGBA image,
                        // so reset all compositor properties to identity.
                        gpuLayers[wi].textureInfo = tr->outputDescriptorInfo();
                        gpuLayers[wi].opacity     = 1.0f;
                        gpuLayers[wi].isPacked    = false;
                        gpuLayers[wi].isPMA       = false;
                        gpuLayers[wi].cropLeft    = 0.0f;
                        gpuLayers[wi].cropRight   = 0.0f;
                        gpuLayers[wi].cropTop     = 0.0f;
                        gpuLayers[wi].cropBottom  = 0.0f;
                        gpuLayers[wi].blendMode   = BlendMode::Normal;
                        // Transition output is already at viewport resolution ├â┬ó├óΓÇÜ┬¼├óΓé¼┬¥
                        // use identity transform (1:1 mapping).
                        gpuLayers[wi].transform   = Compositor::buildViewportTransform(
                            outW, outH, outW, outH,
                            0.0f, 0.0f, 1.0f, 1.0f, 0.0f, false);
                        if (pi != SIZE_MAX)
                            gpuLayers[pi].enabled = false; // peer is merged

                        // Barrier after each transition output
                        VkMemoryBarrier trBarrier{};
                        trBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                        trBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        trBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                                                | VK_ACCESS_SHADER_WRITE_BIT;
                        vkCmdPipelineBarrier(uploadCmd,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0, 1, &trBarrier, 0, nullptr, 0, nullptr);
                    }
                }

                // ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼ Convert flat layers into A/B pairs ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼
                // Group consecutive layers into (background, foreground) pairs.
                // Transitions have already been rendered above and baked into
                // gpuLayers, so all pairs get type=-1 (cut) here ΓÇö the
                // transition output already replaced the appropriate layer.
                std::vector<ABPair> abPairs;
                abPairs.reserve((gpuLayers.size() + 1) / 2);
                for (size_t pi = 0; pi < gpuLayers.size(); pi += 2) {
                    ABPair pair;
                    pair.background = gpuLayers[pi];
                    pair.background.pairIndex = static_cast<uint32_t>(pi / 2);
                    pair.background.isBackground = true;
                    if (pi + 1 < gpuLayers.size()) {
                        pair.foreground = gpuLayers[pi + 1];
                        pair.foreground.pairIndex = static_cast<uint32_t>(pi / 2);
                        pair.foreground.isBackground = false;
                    } else {
                        // Odd layer count: foreground is a disabled placeholder.
                        pair.foreground.enabled = false;
                        pair.foreground.opacity = 0.0f;
                        pair.foreground.pairIndex = static_cast<uint32_t>(pi / 2);
                        pair.foreground.isBackground = false;
                    }
                    pair.transition.type = -1; // cut ΓÇö transition already rendered
                    abPairs.push_back(pair);
                }

                // ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼ Composite + readback (recorded into same cmd buffer) ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼
                comp->setPairs(abPairs);
                compOk = comp->composite(uploadCmd);
                if (compOk) {
                    // During playback with GPU display, skip the readback
                    // ├óΓé¼ΓÇ¥ VulkanViewport samples the output texture directly.
                    // This saves ~1-3ms of GPU work per frame.  Scrub/pause
                    // always records readback for scopes + CPU display.
                    // Also record readback during GPU display + scrub, since
                    // we skip GPU-direct during scrub (race condition fix).
                    if (!m_gpuDisplayMode || scrubMode)
                        readbackOk = comp->recordReadback(uploadCmd);
                }
            }

            // -- SINGLE SUBMIT -- persistent fence, no per-frame alloc ----
            // Upload + effects + transitions + composite + readback all
            // execute in a single GPU submission via GpuWorkSubmission.
            // The timeline semaphore is signaled after completion so the
            // graphics queue (VulkanViewport) can wait on it before reading
            // the compositor's output texture — prevents scrub→play freeze.
            slot.endRecording();
            bool gpuSubmitOk = false;
            {
                std::lock_guard qLock(ctx.computeQueueMutex());
                VkSemaphore compSem = m_compositeSemaphore;
                if (compSem != VK_NULL_HANDLE) {
                    gpuSubmitOk = slot.submit(ctx.computeQueue(), compSem);
                } else {
                    gpuSubmitOk = slot.submit(ctx.computeQueue());
                }
            }
            if (gpuSubmitOk) {
                gpuSubmitOk = slot.waitForCompletion();
            }
            if (!gpuSubmitOk) {
                // GPU submission failed (e.g. VK_ERROR_DEVICE_LOST).
                // Enter exponential backoff cooldown (100ms → 10s max) so we
                // don't hammer a broken driver.  After the cooldown expires,
                // the next tryCompositeOnGpu call retries automatically.
                // NEVER permanently disable GPU compositing.
                int backoffMs = kGpuBackoffInitialMs * (1 << m_gpuBackoffAttempts);
                if (backoffMs > kGpuBackoffMaxMs) backoffMs = kGpuBackoffMaxMs;
                m_gpuBackoffUntil = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds(backoffMs);
                ++m_gpuBackoffAttempts;
                spdlog::error("[COMPOSITE] GPU submit/wait failed (attempt {}) — "
                              "backoff {}ms, will retry at {}",
                              m_gpuBackoffAttempts, backoffMs,
                              m_gpuBackoffAttempts >= 5 ? "10s cap" : "next interval");
                compOk = false;
                readbackOk = false;
            } else {
                // GPU success — reset backoff so we respond instantly next time
                m_gpuBackoffAttempts = 0;
                m_gpuBackoffUntil = {};
            }
            for (auto& sc : stagingCleanups)
                sc.destroy();
            stagingCleanups.clear();

            // ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼ PERF: GPU work complete ├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼├â┬ó├óΓé¼┬¥├óΓÇÜ┬¼
            perfTgpuUp = std::chrono::high_resolution_clock::now();

            if (compOk) {
                auto result = std::make_shared<CachedFrame>();
                result->width  = outW;
                result->height = outH;
                result->stride = outW * 4;

                // GPU-resident handles for VulkanViewport zero-copy display.
                // SKIP GPU-direct during scrub mode: the single output texture
                // can race with the next compositeFrame() call (producer thread
                // may overwrite it while the presenter's render pass is still
                // reading it on a different GPU queue).  CPU upload via the
                // ring-buffered upload slots in VulkanViewport is race-free.
                //
                // gpuSemaphore is the inter-queue timeline semaphore signaled
                // after the compute queue finishes writing the compositor output.
                // The viewport's graphics queue waits on it before reading.
                if (m_gpuDisplayMode && !scrubMode) {
                    result->gpuReady     = true;
                    result->gpuImageView = reinterpret_cast<uint64_t>(comp->outputImageView());
                    result->gpuSampler   = reinterpret_cast<uint64_t>(comp->outputSampler());
                    result->gpuSemaphore = reinterpret_cast<uint64_t>(m_compositeSemaphore);
                }

                // Map readback staging ├â┬ó├óΓé¼┬á├óΓé¼Γäó CPU pixels.
                // In GPU display mode, defer the staging├â┬ó├óΓé¼┬á├óΓé¼ΓäóCPU copy via
                // lazyReadback so scopes can read pixels on demand without
                // paying 3ms of PCIe transfer on every frame.
                // NOTE: Export (scrubMode=true) MUST use synchronous readback
                // because the compositor pointer captured in lazyReadback can
                // become dangling if the compositor is resized between the
                // composite call and the deferred readback call.
                if (readbackOk && m_gpuDisplayMode && !scrubMode) {
                    auto compPtr = comp;
                    uint32_t rW = outW, rH = outH;
                    result->lazyReadback = [compPtr, rW, rH](std::vector<uint8_t>& px) -> bool {
                        const size_t imgBytes = static_cast<size_t>(rW) * rH * 4;
                        px.resize(imgBytes);
                        return compPtr->mapAndCopyReadback(px);
                    };
                } else if (readbackOk) {
                    const size_t imgBytes = static_cast<size_t>(outW) * outH * 4;
                    if (m_compositeLru.size() >= COMPOSITE_CACHE_SIZE) {
                        auto& victim = m_compositeLru[m_compositeLruIdx];
                        if (victim.frame && victim.frame.use_count() == 1 &&
                            victim.frame->pixels.size() == imgBytes)
                        {
                            result->pixels = std::move(victim.frame->pixels);
                        }
                    }
                    comp->mapAndCopyReadback(result->pixels);
                }

                // Insert into composite result LRU cache.
                // Skip gpuReady frames: their gpuImageView/gpuSampler point
                // into the compositor's single output texture which is
                // overwritten on every composite.  Caching them would return
                // stale handles pointing to the wrong frame data.
                if (!result->gpuReady) {
                if (m_compositeLru.size() < COMPOSITE_CACHE_SIZE)
                    m_compositeLru.push_back({tick, outW, outH, result});
                else {
                    m_compositeLru[m_compositeLruIdx] = {tick, outW, outH, result};
                    m_compositeLruIdx = (m_compositeLruIdx + 1) % COMPOSITE_CACHE_SIZE;
                }
                }

                perfTcomp = std::chrono::high_resolution_clock::now();
                if (perfLog) {
                    auto ms = [](auto a, auto b) {
                        return std::chrono::duration<double, std::milli>(b - a).count();
                    };
                    spdlog::info("[PERF] compositeFrame (GPU): layers={} | "
                                 "decode+spine={:.1f}ms  gpu={:.1f}ms  TOTAL={:.1f}ms  "
                                 "gpuDisplay={}  effectLayers={}  effectPasses={}  transitions={}",
                                 layers.size(),
                                 ms(perfT0, perfTlayers),
                                 ms(perfTlayers, perfTcomp),
                                 ms(perfT0, perfTcomp),
                                 m_gpuDisplayMode,
                                 effectLayerCount,
                                 effectPassCount,
                                 transitionCount);
                    // Count how many layers were GPU-cache hits vs uploads
                    int gpuHitCount = 0, uploadCount = 0, loopCount = 0;
                    for (const auto& l : layers) {
                        if (l.gpuTextureReady) ++gpuHitCount;
                        else ++uploadCount;
                        if (l.isLoopContent) ++loopCount;
                    }
                    spdlog::info("[PERF]   layer breakdown: gpuCacheHit={} uploaded={} loopContent={}",
                                 gpuHitCount, uploadCount, loopCount);
                    // Also log GpuTexCache stats for VRAM monitoring
                    if (m_gpuTexCache) {
                        spdlog::info("[PERF] GpuTexCache: {} entries, {:.0f}MB / {:.0f}MB budget, "
                                     "hits={} misses={}",
                                     m_gpuTexCache->entryCount(),
                                     m_gpuTexCache->memoryUsed() / 1048576.0,
                                     m_gpuTexCache->budget() / 1048576.0,
                                     m_gpuTexCache->hits(), m_gpuTexCache->misses());
                    }
                }

                // ΓöÇΓöÇ Rolling per-second perf summary + slow-frame trigger ΓöÇΓöÇ
                // Always-on (no perfLog gate). Logs a 1-line summary every
                // second of playback, and force-logs any single frame that
                // exceeds 33ms at WARN level so it stands out.
                {
                    const double frameMs = std::chrono::duration<double, std::milli>(
                        perfTcomp - perfT0).count();
                    int gpuHitNow = 0, uploadNow = 0;
                    for (const auto& l : layers) {
                        if (l.gpuTextureReady) ++gpuHitNow;
                        else ++uploadNow;
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (m_perfWindow.windowStart.time_since_epoch().count() == 0)
                        m_perfWindow.windowStart = now;

                    ++m_perfWindow.frameCount;
                    m_perfWindow.totalMs += frameMs;
                    m_perfWindow.maxMs = std::max(m_perfWindow.maxMs, frameMs);
                    m_perfWindow.layerSum += static_cast<int>(layers.size());
                    m_perfWindow.gpuHitSum += gpuHitNow;
                    m_perfWindow.uploadSum += uploadNow;
                    m_perfWindow.effectPassSum += effectPassCount;
                    m_perfWindow.transitionSum += transitionCount;
                    if (frameMs > 33.0) ++m_perfWindow.slowFrameCount;
                    if (frameMs > 50.0) ++m_perfWindow.veryslowFrameCount;

                    // Force-log this single slow frame so it's easy to spot.
                    if (frameMs > 33.0) {
                        spdlog::warn("[PERF SLOW] frame={:.1f}ms layers={} gpuHit={} upload={} "
                                     "effectPasses={} transitions={} tick={}",
                                     frameMs, layers.size(), gpuHitNow, uploadNow,
                                     effectPassCount, transitionCount, tick);
                    }

                    // 1-second roll-up.
                    auto windowMs = std::chrono::duration<double, std::milli>(
                        now - m_perfWindow.windowStart).count();
                    if (windowMs >= 1000.0 && m_perfWindow.frameCount > 0) {
                        const double avgMs = m_perfWindow.totalMs / m_perfWindow.frameCount;
                        const double avgLayers = static_cast<double>(m_perfWindow.layerSum)
                                                 / m_perfWindow.frameCount;
                        const double effectiveFps = m_perfWindow.frameCount * 1000.0 / windowMs;
                        spdlog::info("[PERF/1s] frames={} fps={:.1f} avg={:.1f}ms max={:.1f}ms "
                                     "slow(>33)={} verySlow(>50)={} avgLayers={:.1f} "
                                     "gpuHit={} upload={} fxPasses={} trans={}",
                                     m_perfWindow.frameCount,
                                     effectiveFps,
                                     avgMs,
                                     m_perfWindow.maxMs,
                                     m_perfWindow.slowFrameCount,
                                     m_perfWindow.veryslowFrameCount,
                                     avgLayers,
                                     m_perfWindow.gpuHitSum,
                                     m_perfWindow.uploadSum,
                                     m_perfWindow.effectPassSum,
                                     m_perfWindow.transitionSum);
                        m_perfWindow = PlaybackPerfWindow{};
                        m_perfWindow.windowStart = now;
                    }
                }
                {
                    std::lock_guard lg(m_lastCompositeMtx);
                    m_lastGoodComposite = result;
                    m_lastGoodCompositeTick = tick;
                }
                return result;
            }

            // GPU failed ├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥ fall through to CPU path
            spdlog::warn("compositeFrame: GPU composite failed, falling back to CPU");
        }
    }

    // ├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼ CPU compositing path (fallback) ├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼├â╞Æ├åΓÇÖ├âΓÇÜ├é┬ó├â╞Æ├é┬ó├â┬ó├óΓé¼┼í├é┬¼├âΓÇÜ├é┬¥├â╞Æ├é┬ó├â┬ó├óΓÇÜ┬¼├à┬í├âΓÇÜ├é┬¼
        return nullptr;
}

} // namespace rt