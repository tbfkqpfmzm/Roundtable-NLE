/*
 * MediaPoolPrefetchConvertGpu.cpp — UPGRADE_PLAN Phase 4.
 *
 * Implements MediaPool::convertDecodedToCacheGpu: the GPU-resident
 * sibling of convertDecodedToCache.  Routes NV12 / YUV420P → BGRA
 * through Nv12Converter into a per-frame pooled VkImage so the
 * compositor (Phase 5) can sample without a CPU bounce.
 *
 * Lives in its own TU so the rest of core/media stays Vulkan-free.
 * Also defines WorkerGpuState's destructor (the only spot that needs
 * vkDestroySemaphore).
 *
 * Gated by CompositeService::gpuResidentDecodeEnabled() at the call
 * site (MediaPoolPrefetchDecode.cpp); this file does not check the
 * flag itself — callers that reach in are committing to the GPU path.
 */

#include "MediaPool.h"
#include "MediaPoolPrefetchGpu.h"
#include "PrefetchTexturePool.h"

#include "CompositeService.h"     // feature flag (gpuResidentDecodeEnabled)
#include "GpuContext.h"
#include "GpuScheduler.h"
#include "Nv12Converter.h"
#include "vulkan/Texture.h"
#include "vulkan/Device.h"
#include "vulkan/Allocator.h"

#include <spdlog/spdlog.h>

#include <volk.h>

#include <algorithm>
#include <vector>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavutil/pixfmt.h>
}
#endif

namespace rt {

// ─────────────────────────────────────────────────────────────────────────
// WorkerGpuState destructor — needs vkDestroySemaphore, hence Vulkan TU.
// ─────────────────────────────────────────────────────────────────────────
WorkerGpuState::~WorkerGpuState()
{
    // Drain any in-flight submissions. The worker thread has exited by
    // the time this runs (MediaPool::stopPrefetchThread joins workers
    // before MediaPool dtor runs to completion).  Wait on each pending
    // fence so the GPU finishes before we free staging buffers and
    // command buffers — anything else would crash the driver mid-DMA.
    if (device != VK_NULL_HANDLE) {
        for (auto& p : pending) {
            if (p.fence != VK_NULL_HANDLE) {
                vkWaitForFences(device, 1, &p.fence, VK_TRUE, UINT64_MAX);
                vkDestroyFence(device, p.fence, nullptr);
            }
            if (p.cmdBuf != VK_NULL_HANDLE) {
                cmdPool.freeBuffer(p.cmdBuf);
            }
            for (auto& s : p.staging) s.destroy();
        }
    }
    pending.clear();

    if (signalSem != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, signalSem, nullptr);
    }
    // cmdPool: RAII via rt::CommandPool destructor.
}

// ─────────────────────────────────────────────────────────────────────────
// WorkerGpuState::pollAndCleanup — non-blocking sweep of completed work.
// ─────────────────────────────────────────────────────────────────────────
void WorkerGpuState::pollAndCleanup()
{
    if (device == VK_NULL_HANDLE) return;
    while (!pending.empty()) {
        auto& front = pending.front();
        if (front.fence == VK_NULL_HANDLE) {
            // Defensive: malformed entry, drop it.
            pending.pop_front();
            continue;
        }
        const VkResult r = vkGetFenceStatus(device, front.fence);
        if (r != VK_SUCCESS) {
            // VK_NOT_READY: fence still pending — stop here.  Later
            // entries were submitted after this one and on the same
            // queue, so their fences cannot have signalled yet.
            // Any other code (e.g. VK_ERROR_DEVICE_LOST): leave the
            // entry for the destructor to drain; this avoids freeing
            // a cmd buffer the driver thinks is still in use.
            break;
        }
        vkDestroyFence(device, front.fence, nullptr);
        if (front.cmdBuf != VK_NULL_HANDLE) {
            cmdPool.freeBuffer(front.cmdBuf);
        }
        for (auto& s : front.staging) s.destroy();
        pending.pop_front();
    }
}

// ─────────────────────────────────────────────────────────────────────────
// MediaPool::convertDecodedToCacheGpu
// ─────────────────────────────────────────────────────────────────────────
std::shared_ptr<CachedFrame> MediaPool::convertDecodedToCacheGpu(
    PrefetchDecoderState& state,
    const PrefetchTask&   task,
    DecodedFrame&         decoded,
    int64_t               frameNumber,
    WorkerGpuState&       wgs)
{
    // ── Eligibility (UPGRADE_PLAN H.2, plus L.1 device-lost check) ──────
    auto& ctx = GpuContext::get();
    if (!ctx.isInitialized() || !ctx.isOperational()) return nullptr;
    if (!wgs.ready())                                  return nullptr;
    if (!m_prefetchTexPool)                            return nullptr;
    if (task.packedAlpha)                              return nullptr;

    // Non-blocking sweep of completed prior submissions.  Frees their
    // fence + staging + cmd buffer so we don't accumulate them.  Each
    // entry walks at most once across two prefetch calls.
    wgs.pollAndCleanup();

    // Hardware → CPU transfer.  Same step the CPU path takes (NVDEC
    // surfaces are not directly accessible to Nv12Converter today).
    if (decoded.isHardware) {
        DecodedFrame cpuFrame;
        if (!state.decoder->transferHardwareFrame(decoded, cpuFrame))
            return nullptr;
        decoded = std::move(cpuFrame);
    }
    if (!decoded.data[0] || decoded.width == 0 || decoded.height == 0)
        return nullptr;

#ifdef ROUNDTABLE_HAS_FFMPEG
    AVPixelFormat srcFmt = AV_PIX_FMT_NONE;
    if (decoded.rawFormat >= 0) {
        srcFmt = static_cast<AVPixelFormat>(decoded.rawFormat);
    } else {
        switch (decoded.format) {
            case PixelFormat::YUV420P: srcFmt = AV_PIX_FMT_YUV420P; break;
            case PixelFormat::NV12:    srcFmt = AV_PIX_FMT_NV12;    break;
            default:                   return nullptr;
        }
    }
    if (srcFmt != AV_PIX_FMT_NV12 && srcFmt != AV_PIX_FMT_YUV420P)
        return nullptr;
#else
    return nullptr;
#endif

    const int srcW = static_cast<int>(decoded.width);
    const int srcH = static_cast<int>(decoded.height);

    // ── Tier-clamp identical to convertDecodedToCache so cached frames
    //    interleave correctly with whatever the CPU path produces. ───────
    int maxDim = 1920;
    switch (task.tier) {
        case ResolutionTier::Half:    maxDim =  960; break;
        case ResolutionTier::Quarter: maxDim =  480; break;
        default:                      maxDim = 1920; break;
    }
    int dstW = srcW, dstH = srcH;
    if (srcW > maxDim || srcH > maxDim) {
        const float scale = std::min(static_cast<float>(maxDim) / srcW,
                                     static_cast<float>(maxDim) / srcH);
        dstW = std::max(2, static_cast<int>(srcW * scale) & ~1);
        dstH = std::max(2, static_cast<int>(srcH * scale) & ~1);
    }
    if (dstW > 16384 || dstH > 16384) return nullptr;

    // ── Acquire (or lazily create) the Nv12Converter sized for dst.  ────
    // Different output sizes get distinct converter instances per
    // GpuContext, so per-size workers do not contend on apiMutex.
    Nv12Converter* conv = ctx.nv12Converter(
        static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH));
    if (!conv || !conv->isInitialized()) return nullptr;

    // ── Acquire a pooled destination texture (BGRA, concurrent sharing
    //    so compute writes + graphics reads need no queue-ownership
    //    transfer barrier).  See PrefetchTexturePool.h. ─────────────────
    const auto& qf = ctx.device().queueFamilies();
    const uint32_t computeFamily  = qf.compute.value_or(qf.graphics.value_or(0));
    const uint32_t graphicsFamily = qf.graphics.value_or(0);
    std::vector<uint32_t> concurrent;
    if (computeFamily != graphicsFamily) {
        concurrent.push_back(computeFamily);
        concurrent.push_back(graphicsFamily);
    }
    constexpr VkImageUsageFlags kDstUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // TRANSFER_SRC for lazy readback
    auto dstTex = makePooledTexture(*m_prefetchTexPool, ctx,
                                    static_cast<uint32_t>(dstW),
                                    static_cast<uint32_t>(dstH),
                                    VK_FORMAT_B8G8R8A8_UNORM,
                                    kDstUsage,
                                    std::move(concurrent));
    if (!dstTex) return nullptr;

    // ── Lock Nv12Converter for the duration of record + submit + wait.
    //    The converter's internal planar textures + descriptor set are
    //    shared mutable state across recorders.  Mirrors what the
    //    existing convertAndReadback* methods do.  ────────────────────────
    std::lock_guard apiLock(conv->apiMutex());

    // ── Open per-worker command buffer.  beginSingleTime allocates + ────
    //    begins with ONE_TIME_SUBMIT flag set.  ─────────────────────────
    VkCommandBuffer cmd = wgs.cmdPool.beginSingleTime();
    if (cmd == VK_NULL_HANDLE) return nullptr;

    std::vector<Texture::StagingCleanup> stagingOut;

    // ── Record NV12 / YUV420P → BGRA into cmd.  No submit. ──────────────
    bool recordOk = false;
    if (srcFmt == AV_PIX_FMT_NV12) {
        recordOk = conv->recordConvertScaled(
            cmd,
            decoded.data[0], decoded.linesize[0],
            decoded.data[1], decoded.linesize[1],
            static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
            static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
            stagingOut);
    } else {
        recordOk = conv->recordConvertYuv420pScaled(
            cmd,
            decoded.data[0], decoded.linesize[0],
            decoded.data[1], decoded.linesize[1],
            decoded.data[2], decoded.linesize[2],
            static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
            static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
            stagingOut);
    }
    if (!recordOk) {
        vkEndCommandBuffer(cmd);
        wgs.cmdPool.freeBuffer(cmd);
        for (auto& s : stagingOut) s.destroy();
        return nullptr;
    }

    // ── Transition dstTex UNDEFINED → TRANSFER_DST_OPTIMAL.
    //    oldLayout=UNDEFINED discards the previous contents — correct
    //    for both freshly created textures and recycled ones (we're
    //    about to overwrite the entire image via vkCmdCopyImage). ───────
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = dstTex->image();
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ── vkCmdCopyImage: Nv12Converter output (GENERAL) → dstTex
    //    (TRANSFER_DST_OPTIMAL).  Sizes match because Nv12Converter was
    //    sized for dstW×dstH and the converter renders at that size. ────
    {
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent         = {static_cast<uint32_t>(dstW),
                                 static_cast<uint32_t>(dstH), 1};
        vkCmdCopyImage(cmd,
            conv->outputTexture().image(), VK_IMAGE_LAYOUT_GENERAL,
            dstTex->image(),                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &region);
    }

    // ── Transition dstTex TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.
    //    dstStageMask must be valid on the COMPUTE queue family this cmd
    //    buffer belongs to — FRAGMENT_SHADER_BIT is graphics-only and
    //    fails VUID-vkCmdPipelineBarrier-dstStageMask-06462 on dedicated
    //    compute queues.  BOTTOM_OF_PIPE_BIT + dstAccessMask=0 says
    //    "complete the layout transition by the end of this submission;
    //    later queues handle their own memory visibility on first use."
    //    The sampling queue (compositor) issues its own implicit
    //    visibility when binding the descriptor.
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask       = 0;
        b.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = dstTex->image();
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
    // Note: Texture::m_currentLayout is now out-of-sync with reality
    // (UNDEFINED instead of SHADER_READ_ONLY_OPTIMAL).  Acceptable
    // because the only consumers — compositor sampling via the saved
    // gpuImageView and GpuContext::readbackTexture (which hardcodes
    // SHADER_READ_ONLY_OPTIMAL as oldLayout) — do not use the tracker.

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        wgs.cmdPool.freeBuffer(cmd);
        for (auto& s : stagingOut) s.destroy();
        return nullptr;
    }

    // ── Per-call fence for deferred CPU-side cleanup of staging
    //    buffers + cmd buffer.  NOT used for compositor ordering:
    //    compositor submits on the same compute queue (via the same
    //    GpuScheduler) so Vulkan's per-queue FIFO ordering already
    //    guarantees this convert+copy completes before the compositor's
    //    later sample.  See WorkerGpuState::pending in MediaPoolPrefetchGpu.h.
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(ctx.vkDevice(), &fci, nullptr, &fence) != VK_SUCCESS) {
        wgs.cmdPool.freeBuffer(cmd);
        for (auto& s : stagingOut) s.destroy();
        return nullptr;
    }

    GpuSubmission sub{};
    sub.cmd             = cmd;
    sub.queue           = GpuQueueKind::Compute;
    sub.completionFence = fence;
    sub.tag             = "prefetch-decode-convert";

    const VkResult sr = ctx.scheduler().submit(sub);
    if (sr != VK_SUCCESS) {
        spdlog::warn("convertDecodedToCacheGpu: submit failed vk={} handle={} frame={}",
                     static_cast<int>(sr), task.handle, frameNumber);
        vkDestroyFence(ctx.vkDevice(), fence, nullptr);
        wgs.cmdPool.freeBuffer(cmd);
        for (auto& s : stagingOut) s.destroy();
        return nullptr;
    }

    // Inline-wait + immediate cleanup.
    //
    // We must NOT release apiMutex (held via apiLock above) until the
    // GPU has finished executing this submission, because the next
    // worker through this function will call vkUpdateDescriptorSets on
    // the converter's shared descriptor set, and the spec forbids that
    // while a pending command buffer still references the descriptor
    // set (VUID-vkUpdateDescriptorSets-None-03047).  Without the wait
    // here, validation layers flag every prefetched frame, and the
    // descriptor set's contents can be clobbered mid-execution —
    // producing the wrong source pixels in the converted output.
    //
    // The deferred-cleanup infrastructure (wgs.pending, pollAndCleanup,
    // dtor drain) is kept in place even though the deque always stays
    // empty under this design: a future change that gives each worker
    // its own descriptor set would lift the wait without disturbing the
    // surrounding code.
    vkWaitForFences(ctx.vkDevice(), 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(ctx.vkDevice(), fence, nullptr);
    wgs.cmdPool.freeBuffer(cmd);
    for (auto& s : stagingOut) s.destroy();

    m_perf.gpuResidentDecoded.fetch_add(1, std::memory_order_relaxed);

    // ── Build the GPU-resident CachedFrame.  ────────────────────────────
    // Same deleter pattern as convertDecodedToCache: recycle the empty
    // (or filled-by-lazy-readback) pixel buffer into the pixel pool.
    auto pool = m_pixelPool;
    auto cached = std::shared_ptr<CachedFrame>(
        new CachedFrame, [pool](CachedFrame* f) {
            pool->recycle(std::move(f->pixels));
            delete f;
        });
    cached->mediaId         = task.handle;
    cached->frameNumber     = frameNumber;
    cached->width           = static_cast<uint32_t>(dstW);
    cached->height          = static_cast<uint32_t>(dstH);
    cached->tier            = task.tier;
    cached->stride          = static_cast<uint32_t>(dstW) * 4;
    cached->isKeyframe      = decoded.isKeyframe;
    cached->timestamp       = decoded.timestamp;
    cached->pinned          = (task.info.frameCount <= 1);
    cached->isLoopFrame     = task.isLoop;
    cached->gpuReady        = true;
    cached->gpuImageView    = reinterpret_cast<uint64_t>(dstTex->imageView());
    cached->gpuSampler      = reinterpret_cast<uint64_t>(dstTex->sampler());
    cached->gpuTextureOwner = dstTex;   // shared_ptr — co-owned with the cache
    // cached->gpuSemaphore stays 0 in PR-4 (Phase 6 wires it).
    // cached->pixels stays empty; lazyReadback materialises on demand.

    // Lazy CPU readback for disk cache + export.  Captures the texture
    // shared_ptr by value so the readback can outlive the original
    // gpuTextureOwner if needed.  GpuContext::readbackTexture is
    // thread-safe (UPGRADE_PLAN Phase 7 K.4).
    cached->lazyReadback = [texOwner = dstTex](std::vector<uint8_t>& outPixels) {
        if (!texOwner) return false;
        const uint32_t w = texOwner->width();
        const uint32_t h = texOwner->height();
        return GpuContext::get().readbackTexture(
            const_cast<Texture*>(texOwner.get()), w, h, outPixels);
    };

    return cached;
}

// ─────────────────────────────────────────────────────────────────────────
// tryConvertDecodedToCacheGpu — see header.
// ─────────────────────────────────────────────────────────────────────────
std::shared_ptr<CachedFrame> tryConvertDecodedToCacheGpu(
    MediaPool&            pool,
    PrefetchDecoderState& state,
    const PrefetchTask&   task,
    DecodedFrame&         decoded,
    int64_t               frameNumber,
    WorkerGpuState*       wgs)
{
    if (!wgs || !wgs->ready())                          return nullptr;
    if (!CompositeService::gpuResidentDecodeEnabled())  return nullptr;
    return pool.convertDecodedToCacheGpu(state, task, decoded, frameNumber, *wgs);
}

} // namespace rt
