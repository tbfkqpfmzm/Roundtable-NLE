/*
 * CompositeEngine.cpp -- GPU compositing pipeline.
 *
 * Encapsulates the single-submit GPU pipeline: upload -> effects ->
 * transitions -> composite -> readback, plus all associated GPU resources.
 */

// Must come before any header that pulls in vulkan.h so volk can
// define VK_NO_PROTOTYPES first.
#include <volk.h>

#include "CompositeEngine.h"
#include "render_graph/GpuRenderGraph.h"
#include "media/CacheCoordinator.h"
#include "StagingRing.h"
#include "CompositeServiceLayerBuild.h"  // rt::LayerInfo
#include "CompositeServiceBlend.h"       // rasterizeMasks
#include "Compositor.h"
#include "GpuContext.h"
#include "GpuTextureCache.h"
#include "GpuWorkSubmission.h"
#include "GpuUploadManager.h"
#include "vulkan/Texture.h"
#include "TransitionRenderer.h"
#include "EffectProcessor.h"
#include "media/FrameCache.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/LUT.h"
#include "timeline/Transition.h"
#include "timeline/Clip.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using namespace rt;

// s_useRenderGraph was removed in T3.14 — the DAG path is now the only path.

// ============================================================================
// Static helpers: Timeline TransitionType -> GpuTransitionType mapping
// ============================================================================

static GpuTransitionType toGpuTransitionType(TransitionType tt) noexcept
{
    switch (tt) {
    case TransitionType::CrossDissolve: return GpuTransitionType::Dissolve;
    case TransitionType::WipeLeft:  return GpuTransitionType::WipeLeft;
    case TransitionType::WipeRight: return GpuTransitionType::WipeRight;
    case TransitionType::WipeUp:    return GpuTransitionType::WipeUp;
    case TransitionType::WipeDown:  return GpuTransitionType::WipeDown;
    case TransitionType::PushLeft:  return GpuTransitionType::PushLeft;
    case TransitionType::PushRight: return GpuTransitionType::PushRight;
    case TransitionType::PushUp:    return GpuTransitionType::PushUp;
    case TransitionType::PushDown:  return GpuTransitionType::PushDown;
    case TransitionType::DipToBlack:
    case TransitionType::DipToWhite:       return GpuTransitionType::DipColor;
    case TransitionType::FilmDissolve:     return GpuTransitionType::FilmDissolve;
    case TransitionType::AdditiveDissolve: return GpuTransitionType::AdditiveDissolve;
    case TransitionType::BarnDoor:         return GpuTransitionType::BarnDoor;
    case TransitionType::ClockWipe:        return GpuTransitionType::ClockWipe;
    case TransitionType::RadialWipe:       return GpuTransitionType::RadialWipe;
    case TransitionType::IrisRound:
    case TransitionType::IrisDiamond:
    case TransitionType::IrisCross:        return GpuTransitionType::Iris;
    case TransitionType::DiagonalWipe:     return GpuTransitionType::DiagonalWipe;
    case TransitionType::CheckerWipe:      return GpuTransitionType::CheckerWipe;
    case TransitionType::VenetianBlinds:   return GpuTransitionType::VenetianBlinds;
    case TransitionType::Inset:            return GpuTransitionType::Inset;
    case TransitionType::SlideLeft:
    case TransitionType::SlideRight:
    case TransitionType::SlideUp:
    case TransitionType::SlideDown:        return GpuTransitionType::Slide;
    case TransitionType::Split:
    case TransitionType::CenterSplit:      return GpuTransitionType::SplitWipe;
    case TransitionType::Swap:             return GpuTransitionType::Swap;
    case TransitionType::Zoom:
    case TransitionType::CrossZoom:        return GpuTransitionType::ZoomTransition;
    case TransitionType::WhipPan:          return GpuTransitionType::WhipPan;
    case TransitionType::RandomBlocks:     return GpuTransitionType::RandomBlocks;
    case TransitionType::MorphCut:         return GpuTransitionType::MorphCut;
    case TransitionType::GradientWipe:     return GpuTransitionType::GradientWipe;
    case TransitionType::FadeToBlack:
    case TransitionType::FadeFromBlack:
    case TransitionType::FadeToWhite:
    case TransitionType::FadeFromWhite:
    default: return GpuTransitionType::Dissolve;
    }
}

static int32_t transitionDirectionOverride(TransitionType tt) noexcept
{
    switch (tt) {
    case TransitionType::DipToWhite:   return 1;
    case TransitionType::IrisDiamond:  return 1;
    case TransitionType::IrisCross:    return 2;
    case TransitionType::SlideLeft:    return 0;
    case TransitionType::SlideRight:   return 1;
    case TransitionType::SlideUp:      return 2;
    case TransitionType::SlideDown:    return 3;
    case TransitionType::CenterSplit:  return 1;
    case TransitionType::CrossZoom:    return 1;
    default: return -1;
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

CompositeEngine::CompositeEngine()
{
    m_compositeLru.resize(kCacheSize);
}

CompositeEngine::~CompositeEngine()
{
    shutdown();
}

void CompositeEngine::init(VkDevice device)
{
    m_device = device;

    // The inter-queue semaphore pool now lives on GpuContext (so the
    // presenter can return semaphores to it across threads).  We don't
    // pre-populate; the first acquire allocates on demand and the pool
    // self-warms within the first 3-4 frames.

    m_stagingRing = std::make_unique<StagingRing>();
    m_uploadManager = std::make_unique<GpuUploadManager>(
        GpuContext::get(), *m_stagingRing);

    initTimingPools(device);
}

// ── GPU timing query pools (per-ring-slot) ──────────────────────────────────

void CompositeEngine::initTimingPools(VkDevice device)
{
    if (device == VK_NULL_HANDLE) return;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(GpuContext::get().physicalDevice(), &props);
    m_timestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);

    // Driver may not support GPU timestamps on the compute queue.  If the
    // valid bits for the compute queue family are 0, we just skip telemetry.
    const auto& families = GpuContext::get().device().queueFamilies();
    const uint32_t computeQF = families.compute.value_or(families.graphics.value_or(0));
    VkQueueFamilyProperties qprops{};
    {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(GpuContext::get().physicalDevice(), &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> all(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(GpuContext::get().physicalDevice(), &qCount, all.data());
        if (computeQF < qCount) qprops = all[computeQF];
    }
    if (qprops.timestampValidBits == 0) {
        spdlog::info("[GPU-TIMING] Compute queue does not support timestamps — skipping telemetry");
        return;
    }

    VkQueryPoolCreateInfo qpci{};
    qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = kTimingMarkers;

    for (int i = 0; i < kTimingRingSize; ++i) {
        if (vkCreateQueryPool(device, &qpci, nullptr, &m_timingPools[i]) != VK_SUCCESS) {
            spdlog::warn("[GPU-TIMING] vkCreateQueryPool failed for slot {}", i);
            // Tear down any pools already created and bail out — telemetry
            // is best-effort, never fatal.
            destroyTimingPools();
            return;
        }
        m_timingPoolUsed[i] = false;
    }
    m_timingInitialized = true;
    spdlog::info("[GPU-TIMING] Query pools initialized "
                 "(period={:.2f}ns, ring={})",
                 m_timestampPeriodNs, kTimingRingSize);
}

void CompositeEngine::destroyTimingPools()
{
    for (int i = 0; i < kTimingRingSize; ++i) {
        if (m_timingPools[i] != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_timingPools[i], nullptr);
            m_timingPools[i] = VK_NULL_HANDLE;
        }
        m_timingPoolUsed[i] = false;
    }
    m_timingInitialized = false;
}

void CompositeEngine::resolveTimingsForSlot(int slotIdx)
{
    if (!m_timingInitialized || slotIdx < 0 || slotIdx >= kTimingRingSize) return;
    if (!m_timingPoolUsed[slotIdx]) return;  // pool not yet populated

    uint64_t ts[kTimingMarkers]{};
    VkResult r = vkGetQueryPoolResults(
        m_device, m_timingPools[slotIdx],
        0, kTimingMarkers,
        sizeof(ts), ts, sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return;  // not ready (driver bug?) — skip

    const double tickToMs = m_timestampPeriodNs * 1e-6;
    GpuStageTimings t;
    t.uploadMs  = static_cast<double>(ts[1] - ts[0]) * tickToMs;
    t.effectMs  = static_cast<double>(ts[2] - ts[1]) * tickToMs;
    t.composeMs = static_cast<double>(ts[3] - ts[2]) * tickToMs;
    t.frameMs   = static_cast<double>(ts[3] - ts[0]) * tickToMs;
    t.valid     = true;
    {
        std::lock_guard l(m_timingMtx);
        m_lastTimings = t;
    }

    static int s_logCounter = 0;
    if (++s_logCounter % 30 == 0) {
        spdlog::info("[GPU-TIMING] upload={:.2f}ms effect={:.2f}ms "
                     "compose={:.2f}ms frame={:.2f}ms",
                     t.uploadMs, t.effectMs, t.composeMs, t.frameMs);
    }
}

VkSemaphore CompositeEngine::acquireFrameSemaphore()
{
    // Delegate to the GpuContext-owned pool so the presenter (VulkanViewport,
    // running on the GUI thread) can return semaphores to the same pool we
    // pull from here on the FrameProducer thread.  The previous design had
    // the pool living locally on CompositeEngine, with the presenter pushing
    // consumed semaphores onto an unrelated `m_recycledSemaphores` vector
    // that was never drained — the result was a ~60 VkSemaphore/sec leak
    // over the whole session.
    return GpuContext::get().acquireBinarySemaphore();
}

void CompositeEngine::releaseFrameSemaphore(VkSemaphore sem)
{
    GpuContext::get().releaseBinarySemaphore(sem);
}

void CompositeEngine::shutdown()
{
    if (m_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);

    // Inter-queue semaphores are owned by GpuContext now and destroyed
    // from GpuContext::shutdown after device waitIdle — see the matching
    // block there.  Nothing to do for them here.

    destroyTimingPools();
    destroyCompositeSlot();
    clearGpuTexCache();
    m_gpuLayerTextures.clear();
    m_gpuMaskTextures.clear();
    m_compositeLru.clear();
    m_stagingRing.reset();
}

// ============================================================================
// LRU cache
// ============================================================================

std::shared_ptr<CachedFrame> CompositeEngine::checkLru(
    int64_t tick, uint32_t w, uint32_t h) const
{
    for (const auto& ce : m_compositeLru) {
        if (ce.frame && ce.frame->gpuReady) continue;
        if (ce.tick == tick && ce.w == w && ce.h == h && ce.frame)
            return ce.frame;
    }
    return nullptr;
}

void CompositeEngine::insertLru(int64_t tick, uint32_t w, uint32_t h,
                                std::shared_ptr<CachedFrame> frame)
{
    if (m_compositeLru.size() < kCacheSize)
        m_compositeLru.push_back({tick, w, h, std::move(frame)});
    else {
        m_compositeLru[m_compositeLruIdx] = {tick, w, h, std::move(frame)};
        m_compositeLruIdx = (m_compositeLruIdx + 1) % kCacheSize;
    }
}

void CompositeEngine::flushLruOnResize(uint32_t w, uint32_t h)
{
    if (!m_compositeLru.empty() &&
        (m_compositeLru.front().w != w || m_compositeLru.front().h != h))
    {
        m_compositeLru.clear();
        m_compositeLru.resize(kCacheSize);
        m_compositeLruIdx = 0;
    }
}

void CompositeEngine::clearLru()
{
    m_compositeLru.clear();
    m_compositeLru.resize(kCacheSize);
    m_compositeLruIdx = 0;
}

// A3: Only invalidate composite entries whose tick falls inside [fromTick, toTick].
// Used by edit commands that mutate a known time range — keeps cached
// frames from the rest of the timeline alive so seeking elsewhere doesn't
// trigger a full re-composite chain.
void CompositeEngine::invalidateLruRange(int64_t fromTick, int64_t toTick)
{
    if (fromTick > toTick) std::swap(fromTick, toTick);
    for (auto& ce : m_compositeLru) {
        if (ce.frame && ce.tick >= fromTick && ce.tick <= toTick) {
            ce.frame.reset();
            ce.tick = -1;
            ce.w = 0;
            ce.h = 0;
        }
    }
}

// ============================================================================
// GPU state
// ============================================================================

bool CompositeEngine::isGpuAvailable() const noexcept
{
    return m_gpuCompositeState > 0;
}

void CompositeEngine::notifyDeviceLost() noexcept
{
    m_gpuCompositeState = -1;
}

// resetBackoff was removed in P2 — see CLAUDE_IMPROVEMENT_PLAN.

// ============================================================================
// Texture cache
// ============================================================================

void CompositeEngine::clearTextureCache()
{
    clearGpuTexCache();
}

void CompositeEngine::invalidateMediaPoolSlots(uint64_t mediaId)
{
    if (mediaId == 0) return;
    size_t reset = 0;
    for (auto& key : m_gpuLayerTexKeys) {
        if (key.mediaId == mediaId) {
            key.mediaId     = 0;
            key.frameNumber = -1;
            key.framePtr    = nullptr;
            ++reset;
        }
    }
    if (reset > 0) {
        spdlog::warn("[LIVE-RELOAD] CompositeEngine: reset {} pool-slot "
                     "dirty-tracking key(s) for mediaId={} — next upload "
                     "will re-stage new pixels", reset, mediaId);
    }
}

void CompositeEngine::clearGpuTexCache()
{
    m_gpuTexCache.reset();
}

int CompositeEngine::vramUsagePercent() const noexcept
{
    return m_gpuTexCache ? m_gpuTexCache->usagePercent() : 0;
}

void CompositeEngine::destroyCompositeSlot()
{
    m_gpuSubmission.reset();
    m_gpuCompositeState = 0;
}

// ============================================================================
// Main GPU compositing entry point
// ============================================================================

std::shared_ptr<CachedFrame> CompositeEngine::composite(
    const std::vector<LayerInfo>& layers,
    uint32_t outW, uint32_t outH,
    int64_t tick, bool scrubMode,
    bool gpuDisplayMode,
    Compositor* compositor,
    EffectProcessor* effectProcessor,
    TransitionRenderer* transitionRenderer,
    bool perfLog,
    std::chrono::high_resolution_clock::time_point perfT0,
    std::chrono::high_resolution_clock::time_point& perfTlayers,
    std::chrono::high_resolution_clock::time_point& perfTgpuUp,
    std::chrono::high_resolution_clock::time_point& perfTcomp,
    int& effectLayerCount, int& effectPassCount,
    int& transitionCount)
{
    if (m_gpuCompositeState == 0) {
        m_gpuCompositeState = GpuContext::get().isInitialized() ? 1 : -1;
    }

    // -- GPU device-lost / failed --
    // tryRecover() now transitions to Failed and fires the fatal-failure
    // callback exactly once.  We never resume the GPU path on the same
    // process — the user is expected to restart.  Returning nullptr lets
    // CompositeService fall through to safe-mode CPU compositing.
    {
        auto& gpu = GpuContext::get();
        const GpuState st = gpu.gpuState();
        if (st == GpuState::DeviceLost) {
            spdlog::warn("[COMPOSITE] GPU device lost detected");
            gpu.tryRecover();  // marks Failed + fires fatal callback
            m_gpuCompositeState = -1;
            return nullptr;
        }
        if (st == GpuState::Failed) {
            m_gpuCompositeState = -1;
            return nullptr;
        }
    }

    // P2: GPU backoff retry was deleted.  GPU submit failures are now
    // surfaced as device-lost in the submit path itself (see below).
    if (m_gpuCompositeState != 1 || !compositor || !compositor->isInitialized()) {
        return nullptr;
    }

    // The DAG-based path is now the only path.  The legacy monolithic
    // composite() body (~450 lines of inline command-buffer recording) was
    // deleted after the DAG path reached parity (May 2026).  See
    // RENDER_GRAPH_PLAN.txt Phase 6 / Section I.2 ("Feature Flag Lifecycle
    // — Phase 6+: Remove toggle, delete old code").
    return compositeViaRenderGraph(
        layers, outW, outH, tick, scrubMode, gpuDisplayMode,
        compositor, effectProcessor, transitionRenderer,
        perfLog, perfT0, perfTlayers, perfTgpuUp, perfTcomp,
        effectLayerCount, effectPassCount, transitionCount);
}

// ============================================================================
// Render graph path (formerly the "alternative path"; now the only path)
// ============================================================================

std::shared_ptr<CachedFrame> CompositeEngine::compositeViaRenderGraph(
    const std::vector<LayerInfo>& layers,
    uint32_t outW, uint32_t outH,
    int64_t tick, bool scrubMode,
    bool gpuDisplayMode,
    Compositor* compositor,
    EffectProcessor* effectProcessor,
    TransitionRenderer* transitionRenderer,
    bool perfLog,
    std::chrono::high_resolution_clock::time_point perfT0,
    std::chrono::high_resolution_clock::time_point& perfTlayers,
    std::chrono::high_resolution_clock::time_point& perfTgpuUp,
    std::chrono::high_resolution_clock::time_point& perfTcomp,
    int& effectLayerCount, int& effectPassCount,
    int& transitionCount)
{
    using namespace render_graph;

    auto& ctx = GpuContext::get();

    // ── Ensure texture pool is large enough ──────────────────────────
    while (m_gpuLayerTextures.size() < layers.size())
        m_gpuLayerTextures.push_back(std::make_unique<Texture>());
    m_gpuLayerTexKeys.resize(m_gpuLayerTextures.size());
    while (m_gpuMaskTextures.size() < layers.size())
        m_gpuMaskTextures.push_back(std::make_unique<Texture>());

    // ── Set up command buffer (reuse existing triple-buffer slot) ────
    if (!m_gpuSubmission) {
        m_gpuSubmission = std::make_unique<GpuWorkSubmission>();
        m_gpuSubmission->init(ctx.vkDevice(), ctx.cmdPool().handle());
    }
    auto& slot = *m_gpuSubmission;
    if (!slot.beginRecording()) {
        // beginRecording() fails when the previous frame's fence cannot be
        // waited on — almost always VK_ERROR_DEVICE_LOST surfacing through
        // vkWaitForFences.  If we ignore this and keep recording, every
        // subsequent vkCmd* call writes into an unstarted command buffer
        // and the driver raises an SEH exception inside FrameProducer
        // ("Unknown exception in produceFrame").  Surface device-lost so
        // the next composite call short-circuits into CPU safe mode and
        // we stop spamming the same failure 60+ times per second.
        GpuContext::get().signalDeviceLost();
        m_gpuCompositeState = -1;
        return nullptr;
    }
    VkCommandBuffer cmd = slot.cmdBuffer();
    const int timingSlot = slot.currentSlot();

    // Resolve last frame's timestamps from this ring slot.  beginRecording()
    // above waited for the fence so the queries are guaranteed ready.
    resolveTimingsForSlot(timingSlot);

    // Reset + write start timestamp for THIS frame.
    if (m_timingInitialized && m_timingPools[timingSlot] != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, m_timingPools[timingSlot], 0, kTimingMarkers);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            m_timingPools[timingSlot], 0);
    }

    m_uploadManager->beginFrame(cmd, slot.currentSlot());
    m_uploadManager->setTextureCache(m_gpuTexCache.get());

    if (m_stagingRing && !m_stagingRing->isInitialized())
        m_stagingRing->init(ctx.allocator().handle(), 64u * 1024u * 1024u);

    // ── Initialize GPU texture cache if needed (same logic as old path) ─
    if (!m_gpuTexCache) {
        const auto memStats = ctx.allocator().queryStats();
        const size_t gpuVram = memStats.deviceLocalBudgetBytes;
        size_t budget;
        if (m_cacheCoordinator) {
            budget = m_cacheCoordinator->recommendedGpuTexCacheBudget(gpuVram);
            m_cacheCoordinator->onGpuAvailable(gpuVram);
        } else {
            budget = std::clamp<size_t>(
                gpuVram / 4, 512ull * 1024 * 1024, 8ull * 1024 * 1024 * 1024);
        }
        m_gpuTexCache = std::make_unique<GpuTextureCache>(budget);
        m_uploadManager->setTextureCache(m_gpuTexCache.get());
        if (m_cacheCoordinator) {
            m_cacheCoordinator->setVramPressureFn(
                [this](size_t* budgetOut) -> bool {
                    if (!m_gpuTexCache) return false;
                    *budgetOut = m_gpuTexCache->budget();
                    return m_gpuTexCache->isUnderPressure();
                });
            // Bidirectional pressure: when the CPU FrameCache is over its
            // high-water mark, CacheCoordinator calls this to shrink the
            // GPU budget (which evicts cold textures and releases their
            // shared_ptr ownerships).  GpuTextureCache::setBudget triggers
            // eviction down to the new size.  Called back to restore the
            // original budget when CPU pressure subsides.
            m_cacheCoordinator->setGpuBudgetFn(
                [this](size_t newBudget) {
                    if (m_gpuTexCache)
                        m_gpuTexCache->setBudget(newBudget);
                });
        }
        spdlog::info("[PERF] GpuTexCache budget: {:.0f} MB (GPU VRAM: {:.0f} MB)",
                     budget / 1048576.0, gpuVram / 1048576.0);
    }

    // ══════════════════════════════════════════════════════════════════
    // STEP 1: Build the Render Graph
    // ══════════════════════════════════════════════════════════════════

    GpuRenderGraph graph;

    // ── Declare the final composite output resource ────────────────
    ResourceId outputTexId = graph.declareResource({
        .type = ResourceType::StorageImage,
        .name = "compositeOutput",
        .width = outW,
        .height = outH,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usageFlags = VK_IMAGE_USAGE_STORAGE_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                    | VK_IMAGE_USAGE_SAMPLED_BIT,
        .transient = true,
        .external = true,    // owned by Compositor
    });

    // ── Declare per-layer textures and build pass lists ────────────
    struct LayerPassInfo {
        ResourceId layerTexId{kInvalidResource};
        ResourceId maskTexId{kInvalidResource};
        uint32_t   uploadPassIdx{UINT32_MAX};
        std::vector<uint32_t> effectPassIdxs;
        uint32_t   transitionPassIdx{UINT32_MAX};
    };
    std::vector<LayerPassInfo> layerInfo(layers.size());

    for (size_t li = 0; li < layers.size(); ++li) {
        const auto& layer = layers[li];

        // ── Declare this layer's texture resource ──────────────────
        ResourceId layerTexId = graph.declareResource({
            .type = ResourceType::Texture,
            .name = "layer" + std::to_string(li) + "_tex",
            .image = m_gpuLayerTextures[li] ? m_gpuLayerTextures[li]->image() : VK_NULL_HANDLE,
            .width = outW,
            .height = outH,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_STORAGE_BIT
                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .currentLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .currentAccess = ResourceAccess::Undefined,
            .transient = true,
            .external = true,    // owned by m_gpuLayerTextures[li]
        });
        layerInfo[li].layerTexId = layerTexId;

        if (!layer.gpuTextureReady) {
            // ── Add Upload pass for this layer ─────────────────────
            std::vector<VkBufferImageCopy> regions(1);
            regions[0].bufferOffset = 0;
            regions[0].bufferRowLength = 0;
            regions[0].bufferImageHeight = 0;
            regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[0].imageSubresource.mipLevel = 0;
            regions[0].imageSubresource.baseArrayLayer = 0;
            regions[0].imageSubresource.layerCount = 1;
            regions[0].imageOffset = {0, 0, 0};
            regions[0].imageExtent = {outW, outH, 1};

            layerInfo[li].uploadPassIdx = graph.addUploadPass(
                "uploadLayer" + std::to_string(li),
                layerTexId,
                VK_NULL_HANDLE, // staging buffer assigned at execution time
                0, regions, true);
        }

        // ── Declare mask texture if needed ─────────────────────────
        if (layer.clipPtr && layer.clipPtr->maskCount() > 0) {
            ResourceId maskTexId = graph.declareResource({
                .type = ResourceType::Texture,
                .name = "layer" + std::to_string(li) + "_mask",
                .image = m_gpuMaskTextures[li] ? m_gpuMaskTextures[li]->image() : VK_NULL_HANDLE,
                .width = outW,
                .height = outH,
                .format = VK_FORMAT_R8_UNORM,
                .usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT
                            | VK_IMAGE_USAGE_STORAGE_BIT
                            | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .currentLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .currentAccess = ResourceAccess::Undefined,
                .transient = true,
                .external = true,
            });
            layerInfo[li].maskTexId = maskTexId;

            std::vector<VkBufferImageCopy> maskRegions(1);
            maskRegions[0].bufferOffset = 0;
            maskRegions[0].bufferRowLength = 0;
            maskRegions[0].bufferImageHeight = 0;
            maskRegions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            maskRegions[0].imageSubresource.mipLevel = 0;
            maskRegions[0].imageSubresource.baseArrayLayer = 0;
            maskRegions[0].imageSubresource.layerCount = 1;
            maskRegions[0].imageOffset = {0, 0, 0};
            maskRegions[0].imageExtent = {outW, outH, 1};

            (void)graph.addUploadPass(
                "uploadMask" + std::to_string(li),
                maskTexId, VK_NULL_HANDLE, 0, maskRegions, true);
        }

        // ── Add Effect passes for this layer ───────────────────────
        ResourceId effectInput = layerTexId;
        for (size_t ei = 0; ei < layer.effects.size(); ++ei) {
            ResourceId effectOutput = graph.declareResource({
                .type = ResourceType::StorageImage,
                .name = "layer" + std::to_string(li) + "_effect" + std::to_string(ei),
                .width = outW,
                .height = outH,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .usageFlags = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .transient = true,
                .external = true,    // managed by EffectProcessor
            });

            uint32_t epIdx = graph.addComputePass(
                "effect_" + std::to_string(li) + "_" + std::to_string(ei),
                PassType::Effect,
                {effectInput}, {effectOutput},
                VK_NULL_HANDLE, VK_NULL_HANDLE, {}, {}, 1, 1, 1,
                true,   // optional — skip if effect fails
                false); // not fatal
            layerInfo[li].effectPassIdxs.push_back(epIdx);
            effectInput = effectOutput;
        }
    }

    // ── Add Transition passes ──────────────────────────────────────
    for (size_t wi = 0; wi < layers.size(); ++wi) {
        if (layers[wi].wipeProgress < 0.0f)
            continue;
        if (!layers[wi].isWipeOutgoing && layers[wi].wipePeerClipId != 0)
            continue;

        if (wi < layerInfo.size() && layerInfo[wi].layerTexId != kInvalidResource) {
            ResourceId transitionOutput = graph.declareResource({
                .type = ResourceType::StorageImage,
                .name = "transition" + std::to_string(wi) + "_out",
                .width = outW, .height = outH,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .usageFlags = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .transient = true,
                .external = true,    // managed by TransitionRenderer
            });

            uint32_t tpIdx = graph.addComputePass(
                "transition_" + std::to_string(wi),
                PassType::Transition,
                {layerInfo[wi].layerTexId}, {transitionOutput},
                VK_NULL_HANDLE, VK_NULL_HANDLE, {}, {}, 1, 1, 1,
                true, false);
            layerInfo[wi].transitionPassIdx = tpIdx;
        }
    }

    // ── Collect final layer texture IDs for composite pass ─────────
    std::vector<ResourceId> compositeInputs;
    compositeInputs.reserve(layers.size());
    for (auto& li : layerInfo) {
        // If the layer had a transition, use the transition output;
        // if it had effects, use the last effect output;
        // otherwise use the layer texture.
        ResourceId finalTex = li.layerTexId;
        if (li.transitionPassIdx != UINT32_MAX)
            finalTex = graph.pass(li.transitionPassIdx).outputs[0];
        else if (!li.effectPassIdxs.empty())
            finalTex = graph.pass(li.effectPassIdxs.back()).outputs[0];

        if (finalTex != kInvalidResource)
            compositeInputs.push_back(finalTex);
    }

    // ── Add Composite pass ─────────────────────────────────────────
    (void)graph.addComputePass(
        "finalComposite", PassType::Composite,
        compositeInputs, {outputTexId},
        VK_NULL_HANDLE, VK_NULL_HANDLE, {}, {}, 1, 1, 1,
        false, true);  // not optional, fatal

    // ── Add Readback pass (if needed) ──────────────────────────────
    bool needsReadback = !gpuDisplayMode || scrubMode;
    if (needsReadback) {
        std::vector<VkBufferImageCopy> readbackRegions(1);
        readbackRegions[0].bufferOffset = 0;
        readbackRegions[0].bufferRowLength = 0;
        readbackRegions[0].bufferImageHeight = 0;
        readbackRegions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        readbackRegions[0].imageSubresource.mipLevel = 0;
        readbackRegions[0].imageSubresource.baseArrayLayer = 0;
        readbackRegions[0].imageSubresource.layerCount = 1;
        readbackRegions[0].imageOffset = {0, 0, 0};
        readbackRegions[0].imageExtent = {outW, outH, 1};

        (void)graph.addReadbackPass(
            "readback", outputTexId,
            VK_NULL_HANDLE, 0, readbackRegions, true);
    }

    // ══════════════════════════════════════════════════════════════════
    // STEP 2: Compile the graph (topo sort + barriers)
    // ══════════════════════════════════════════════════════════════════

    if (!graph.compile(ctx.vkDevice())) {
        spdlog::warn("[RENDER_GRAPH] Graph compilation failed — falling back");
        slot.endRecording();
        m_uploadManager->endFrame();
        return nullptr;
    }

    // ══════════════════════════════════════════════════════════════════
    // STEP 3: Execute passes in topological order
    // ══════════════════════════════════════════════════════════════════

    // Local state needed during execution
    std::vector<CompositorLayer> gpuLayers;
    gpuLayers.reserve(layers.size());
    bool uploadOk = true;
    bool compOk = false;
    bool readbackOk = false;
    bool uploadsSeen = false;

    // Pre-allocate gpuLayers from layer info.
    // For layers with gpuTextureReady=true (Spine renders, cached textures),
    // populate the CompositorLayer immediately — these won't have an
    // Upload pass in the graph.
    for (size_t li = 0; li < layers.size(); ++li) {
        const auto& layer = layers[li];
        CompositorLayer cl;
        cl.enabled = true;
        cl.opacity = layer.opacity;
        cl.blendMode = static_cast<BlendMode>(layer.blendMode);
        cl.isPacked = layer.isPacked;
        cl.isPMA = layer.isPMA;
        cl.needsSwapRB = layer.needsSwapRB;
        cl.cropLeft  = layer.cropL / 100.0f;
        cl.cropRight = layer.cropR / 100.0f;
        cl.cropTop   = layer.cropT / 100.0f;
        cl.cropBottom= layer.cropB / 100.0f;

        if (layer.gpuTextureReady) {
            uint32_t srcW = layer.frameWidth  ? layer.frameWidth  : outW;
            uint32_t srcH = layer.frameHeight ? layer.frameHeight : outH;
            if (layer.isPacked && srcH > 1) srcH /= 2;
            cl.textureInfo = layer.gpuDescriptor;
            cl.transform = Compositor::buildViewportTransform(
                srcW, srcH, outW, outH,
                layer.posX, layer.posY, layer.scX, layer.scY, layer.rot,
                layer.containFit);
        }

        gpuLayers.push_back(cl);
    }

    // ── Upload mask textures (same logic as old monolithic path) ──
    for (size_t li = 0; li < layers.size() && li < gpuLayers.size(); ++li) {
        const auto& layer = layers[li];
        if (!layer.clipPtr || layer.clipPtr->maskCount() == 0)
            continue;
        auto maskPixels = rasterizeMasks(layer.clipPtr->masks(), outW, outH);
        VkDescriptorImageInfo maskDesc{};
        if (m_uploadManager->uploadMask(
                maskPixels, *m_gpuMaskTextures[li], outW, outH, maskDesc))
        {
            gpuLayers[li].hasMask = true;
            gpuLayers[li].maskTextureInfo = maskDesc;
            uploadsSeen = true;
        }
    }

    // Walk passes in topological order.
    // NOTE: Graph-computed barriers are NOT used yet — the existing
    // renderers manage their own internal barriers.  We insert only
    // the global transfer→compute barrier that the old monolithic path
    // had between upload and compute stages.
    //
    // GPU-TIMING stage markers (written into m_timingPools[timingSlot]):
    //   0: frame start (already written before the loop)
    //   1: after all uploads, before first effect/transition
    //   2: after all effects+transitions, before composite
    //   3: after composite+readback (written after the loop)
    bool uploadTimestampWritten = false;
    bool effectTimestampWritten = false;
    const bool timingActive = m_timingInitialized &&
                              m_timingPools[timingSlot] != VK_NULL_HANDLE;
    for (uint32_t passIdx : graph.topologicalOrder()) {
        const auto& pass = graph.pass(passIdx);

        // Phase D (CLAUDE_IMPROVEMENT_PLAN): session-disabled passes.
        // Optional passes (Effect, Transition) that failed earlier in
        // this session are skipped entirely.  gpuLayers[] keeps its
        // pre-pass state — effectively a passthrough.  Prevents the
        // engine from re-dispatching a known-broken shader 60 fps.
        if (pass.optional && !m_disabledPasses.empty() &&
            m_disabledPasses.count(pass.name) > 0)
        {
            continue;
        }

        // Insert global transfer→compute barrier before first compute pass
        // when uploads have been recorded (mirrors old monolithic path).
        if (uploadsSeen && pass.type != PassType::Upload && pass.type != PassType::External) {
            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &barrier, 0, nullptr, 0, nullptr);
            uploadsSeen = false; // only insert once
        }

        // Stage-boundary timestamps.  Written *before* the first pass of
        // each stage so the delta from the previous marker measures the
        // previous stage's GPU work.
        if (timingActive && !uploadTimestampWritten &&
            pass.type != PassType::Upload && pass.type != PassType::External)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 1);
            uploadTimestampWritten = true;
        }
        if (timingActive && !effectTimestampWritten &&
            (pass.type == PassType::Composite || pass.type == PassType::Readback))
        {
            // First non-upload marker may not have fired (no effects this
            // frame).  Ensure marker 1 is written before marker 2.
            if (!uploadTimestampWritten) {
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    m_timingPools[timingSlot], 1);
                uploadTimestampWritten = true;
            }
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 2);
            effectTimestampWritten = true;
        }

        // ── Execute the pass ─────────────────────────────────────
        switch (pass.type) {
        case PassType::Upload: {
            uploadsSeen = true;
            // Find which layer this upload belongs to
            for (size_t li = 0; li < layers.size(); ++li) {
                if (layerInfo[li].uploadPassIdx == passIdx) {
                    const auto& layer = layers[li];

                    if (layer.gpuTextureReady) {
                        // GPU-resident texture — set up CompositorLayer directly
                        uint32_t srcW = layer.frameWidth  ? layer.frameWidth  : outW;
                        uint32_t srcH = layer.frameHeight ? layer.frameHeight : outH;
                        if (layer.isPacked && srcH > 1) srcH /= 2;
                        gpuLayers[li].textureInfo = layer.gpuDescriptor;
                        gpuLayers[li].transform = Compositor::buildViewportTransform(
                            srcW, srcH, outW, outH,
                            layer.posX, layer.posY, layer.scX, layer.scY, layer.rot,
                            layer.containFit);
                    } else {
                        // Upload via existing GpuUploadManager
                        auto uploadResult = m_uploadManager->uploadLayer(
                            layer, *m_gpuLayerTextures[li],
                            m_gpuLayerTexKeys[li].mediaId,
                            m_gpuLayerTexKeys[li].frameNumber,
                            m_gpuLayerTexKeys[li].framePtr,
                            scrubMode);

                        if (uploadResult.success) {
                            gpuLayers[li].textureInfo = uploadResult.descriptor;
                            gpuLayers[li].transform = Compositor::buildViewportTransform(
                                uploadResult.srcW, uploadResult.srcH, outW, outH,
                                layer.posX, layer.posY, layer.scX, layer.scY, layer.rot,
                                layer.containFit);
                        } else {
                            spdlog::warn("[RENDER_GRAPH] layer {} upload failed", li);
                            gpuLayers[li].enabled = false;
                            uploadOk = false;
                        }
                    }
                    break;
                }
            }
            break;
        }

        case PassType::Effect: {
            // Find which layer/effect this pass corresponds to
            for (size_t li = 0; li < layers.size(); ++li) {
                for (size_t ei = 0; ei < layerInfo[li].effectPassIdxs.size(); ++ei) {
                    if (layerInfo[li].effectPassIdxs[ei] == passIdx) {
                        const auto& layer = layers[li];
                        if (!effectProcessor || !effectProcessor->isInitialized())
                            break;

                        // Fault isolation: skip the effect if its source
                        // layer's upload failed (gpuLayers[li].enabled was
                        // cleared in the Upload case).  Sampling a null
                        // descriptor would either crash the driver or
                        // produce garbage that contaminates the composite.
                        if (!gpuLayers[li].enabled)
                            break;

                        ++effectLayerCount;
                        effectPassCount += static_cast<int>(layer.effects.size());

                        // Handle LUT upload for this layer's effects
                        for (const auto& snap : layer.effects) {
                            if (snap.type == EffectType::LUT && layer.clipPtr) {
                                for (size_t cfi = 0; cfi < layer.clipPtr->effects().effectCount(); ++cfi) {
                                    auto& clipFx = layer.clipPtr->effects().effect(cfi);
                                    if (clipFx.effectType() == EffectType::LUT && clipFx.isEnabled()) {
                                        auto* lutFx = static_cast<LUT*>(&clipFx);
                                        if (lutFx->hasLUT())
                                            effectProcessor->uploadLUT3D(lutFx->lutData(), lutFx->lutSize());
                                        break;
                                    }
                                }
                                break;
                            }
                        }

                        VkDescriptorImageInfo srcInfo = gpuLayers[li].textureInfo;
                        if (effectProcessor->process(cmd, srcInfo, layer.effects)) {
                            gpuLayers[li].textureInfo = effectProcessor->outputDescriptorInfo();
                        } else if (pass.optional) {
                            // Phase D: effect failed.  Disable this pass
                            // for the rest of the session — the input
                            // descriptor stays bound (implicit passthrough).
                            if (m_disabledPasses.insert(pass.name).second) {
                                spdlog::warn("[RENDER_GRAPH] Effect pass '{}' failed "
                                             "— disabled for the rest of this session",
                                             pass.name);
                            }
                        }
                        break;
                    }
                }
            }
            break;
        }

        case PassType::Transition: {
            for (size_t wi = 0; wi < layers.size(); ++wi) {
                if (layerInfo[wi].transitionPassIdx != passIdx)
                    continue;
                if (!transitionRenderer || !transitionRenderer->isInitialized())
                    break;
                // Fault isolation: skip transition if the A side never
                // uploaded (peer-side enabled is checked further down).
                if (!gpuLayers[wi].enabled)
                    break;

                const auto& layer = layers[wi];
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
                if (layer.wipePeerClipId == 0) {
                    // Singleton transition (fade to/from color)
                    if (layer.wipeType == TransitionType::FadeToWhite) {
                        srcB.textureInfo = transitionRenderer->whiteDescriptorInfo();
                    } else if (layer.wipeType == TransitionType::FadeFromWhite) {
                        srcB = srcA;
                        srcA = TransitionSourceInfo{};
                        srcA.textureInfo = transitionRenderer->whiteDescriptorInfo();
                    } else if (layer.wipeType == TransitionType::FadeToBlack) {
                        srcB.textureInfo = transitionRenderer->blackDescriptorInfo();
                    } else if (layer.wipeType == TransitionType::FadeFromBlack) {
                        srcB = srcA;
                        srcA = TransitionSourceInfo{};
                        srcA.textureInfo = transitionRenderer->blackDescriptorInfo();
                    } else if (layer.wipeType == TransitionType::CrossDissolve) {
                        // Single-clip dissolve. Honor direction the same way
                        // FadeFrom*/FadeTo* do, so the boundary frame is
                        // correct:
                        //   incoming (fade-in at clip head, leftClipId==0):
                        //     mix(transparent, clip, p) — p=0 → transparent,
                        //     p=1 → clip. Without the swap, p=0 rendered
                        //     mix(clip, transparent, 0)=clip, i.e. a 1-frame
                        //     fully-opaque flash before the fade-in.
                        //   outgoing (fade-out at clip tail): mix(clip,
                        //     transparent, p) — p=0 → clip, p=1 → transparent.
                        if (!layer.isWipeOutgoing) {
                            srcB = srcA;
                            srcA = TransitionSourceInfo{};
                            srcA.textureInfo =
                                transitionRenderer->transparentDescriptorInfo();
                        } else {
                            srcB.textureInfo =
                                transitionRenderer->transparentDescriptorInfo();
                        }
                    } else {
                        srcB.textureInfo = transitionRenderer->transparentDescriptorInfo();
                    }
                } else {
                    // Find peer layer
                    for (size_t wj = 0; wj < layers.size(); ++wj) {
                        if (wj != wi && layers[wj].clipId == layer.wipePeerClipId) {
                            pi = wj;
                            break;
                        }
                    }
                    if (pi == SIZE_MAX || !gpuLayers[pi].enabled) break;
                    srcB.textureInfo = gpuLayers[pi].textureInfo;
                    srcB.transform   = gpuLayers[pi].transform;
                    srcB.crop        = glm::vec4(gpuLayers[pi].cropLeft,
                                                 gpuLayers[pi].cropRight,
                                                 gpuLayers[pi].cropTop,
                                                 gpuLayers[pi].cropBottom);
                    srcB.isPacked    = gpuLayers[pi].isPacked;
                }

                GpuTransitionType gt = toGpuTransitionType(layer.wipeType);
                int32_t dirOvr = transitionDirectionOverride(layer.wipeType);

                if (transitionRenderer->render(cmd, srcA, srcB,
                    gt, layer.wipeProgress, dirOvr, 0.0f, layer.wipeSoftness))
                {
                    ++transitionCount;
                    gpuLayers[wi].textureInfo = transitionRenderer->outputDescriptorInfo();
                    gpuLayers[wi].opacity    = 1.0f;
                    gpuLayers[wi].isPacked   = false;
                    gpuLayers[wi].isPMA      = false;
                    gpuLayers[wi].cropLeft   = 0.0f;
                    gpuLayers[wi].cropRight  = 0.0f;
                    gpuLayers[wi].cropTop    = 0.0f;
                    gpuLayers[wi].cropBottom = 0.0f;
                    gpuLayers[wi].blendMode  = BlendMode::Normal;
                    gpuLayers[wi].transform  = Compositor::buildViewportTransform(
                        outW, outH, outW, outH,
                        0.0f, 0.0f, 1.0f, 1.0f, 0.0f, false);
                    if (pi != SIZE_MAX)
                        gpuLayers[pi].enabled = false;
                } else if (pass.optional) {
                    // Phase D: transition failed.  Disable for the rest
                    // of the session (gpuLayers[wi] keeps pre-transition
                    // state — clean visual passthrough).
                    if (m_disabledPasses.insert(pass.name).second) {
                        spdlog::warn("[RENDER_GRAPH] Transition pass '{}' failed "
                                     "— disabled for the rest of this session",
                                     pass.name);
                    }
                }
                break;
            }
            break;
        }

        case PassType::Composite: {
            // Build A/B pairs from gpuLayers and dispatch
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
                    pair.foreground.enabled = false;
                    pair.foreground.opacity = 0.0f;
                    pair.foreground.pairIndex = static_cast<uint32_t>(pi / 2);
                    pair.foreground.isBackground = false;
                }
                pair.transition.type = -1;
                abPairs.push_back(pair);
            }

            compositor->setPairs(abPairs);
            compOk = compositor->composite(cmd);
            break;
        }

        case PassType::Readback: {
            if (compOk) {
                readbackOk = compositor->recordReadback(cmd);
            }
            break;
        }

        default:
            break;
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // STEP 4: Submit and build result (same as old path)
    // ══════════════════════════════════════════════════════════════════

    // Final GPU-TIMING marker (frame end).  Backfill any markers we did
    // not reach (e.g. no effects this frame) so deltas remain monotonic.
    if (timingActive) {
        if (!uploadTimestampWritten) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 1);
        }
        if (!effectTimestampWritten) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 2);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            m_timingPools[timingSlot], 3);
        m_timingPoolUsed[timingSlot] = true;
    }

    // Per-pass fault isolation: a failed Upload pass disables only that
    // layer (see PassType::Upload case above, which sets gpuLayers[li].
    // enabled = false on failure).  The compositor renders the surviving
    // layers — the user sees one missing layer instead of a blank frame
    // or a CPU-fallback regression.  This matches RenderPass::optional=true.
    // The frame is only killed if the Composite pass itself failed below.
    if (!uploadOk) {
        spdlog::warn("[RENDER_GRAPH] one or more uploads failed — compositing "
                     "surviving layers (fault-isolated)");
    }

    // Acquire a dedicated inter-queue semaphore for this frame
    VkSemaphore frameSem = acquireFrameSemaphore();
    const bool endOk = slot.endRecording();
    bool gpuSubmitOk = false;
    if (endOk) {
        // Note: no manual ctx.computeQueueMutex() lock here.  GpuWork-
        // Submission::submit routes through GpuScheduler (P1.1), which
        // owns the queue mutex.  Locking it manually here would re-enter
        // a non-recursive std::mutex on the same thread and throw
        // resource_deadlock_would_occur — exactly the regression that
        // caused the program monitor to stay blank.
        gpuSubmitOk = slot.submit(ctx.computeQueue(), frameSem, nullptr);
    }

    if (!gpuSubmitOk) {
        // P2: no retry/backoff.  Submit failure means the device is lost
        // or wedged; signalling it propagates to the fatal-failure modal.
        if (frameSem != VK_NULL_HANDLE)
            releaseFrameSemaphore(frameSem);
        spdlog::error("[RENDER_GRAPH] GPU submit failed — signalling device lost");
        GpuContext::get().signalDeviceLost();
        compOk = false;
        readbackOk = false;
    }

    m_uploadManager->endFrame();

    perfTgpuUp = std::chrono::high_resolution_clock::now();

    // ── Build result frame ───────────────────────────────────────────
    if (compOk) {
        auto result = std::make_shared<CachedFrame>();
        result->width  = outW;
        result->height = outH;
        result->stride = outW * 4;

        if (gpuDisplayMode && !scrubMode) {
            result->gpuReady     = true;
            result->gpuImageView = reinterpret_cast<uint64_t>(compositor->outputImageView());
            result->gpuSampler   = reinterpret_cast<uint64_t>(compositor->outputSampler());
            result->gpuSemaphore = reinterpret_cast<uint64_t>(frameSem);
            // Keep the compositor's output texture alive until all frames
            // referencing it are consumed by the viewport.
            result->gpuTextureOwner = compositor->outputTextureOwner();
        }

        if (readbackOk && gpuDisplayMode && !scrubMode) {
            auto compPtr = compositor;
            uint32_t rW = outW, rH = outH;
            result->lazyReadback = [compPtr, rW, rH](std::vector<uint8_t>& px) -> bool {
                const size_t imgBytes = static_cast<size_t>(rW) * rH * 4;
                px.resize(imgBytes);
                return compPtr->mapAndCopyReadback(px);
            };
        } else if (readbackOk) {
            const size_t imgBytes = static_cast<size_t>(outW) * outH * 4;
            if (m_compositeLru.size() >= kCacheSize) {
                auto& victim = m_compositeLru[m_compositeLruIdx];
                if (victim.frame && victim.frame.use_count() == 1 &&
                    victim.frame->pixels.size() == imgBytes)
                {
                    result->pixels = std::move(victim.frame->pixels);
                }
            }
            compositor->mapAndCopyReadback(result->pixels);
        }

        if (!result->gpuReady) {
            insertLru(tick, outW, outH, result);
        }

        perfTcomp = std::chrono::high_resolution_clock::now();

        if (perfLog) {
            auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            spdlog::info("[RENDER_GRAPH] compositeFrame (DAG): layers={} | "
                         "gpu={:.1f}ms  TOTAL={:.1f}ms  "
                         "gpuDisplay={}  effectLayers={}  effectPasses={}  transitions={}",
                         layers.size(),
                         ms(perfTlayers, perfTcomp),
                         ms(perfT0, perfTcomp),
                         gpuDisplayMode,
                         effectLayerCount, effectPassCount, transitionCount);
        }

        if (m_cacheCoordinator)
            m_cacheCoordinator->onFrameCompleted();

        return result;
    }

    return nullptr;
}
