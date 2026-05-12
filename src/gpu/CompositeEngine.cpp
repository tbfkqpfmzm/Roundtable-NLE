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

    if (device != VK_NULL_HANDLE) {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device, &semInfo, nullptr, &m_compositeSemaphore) != VK_SUCCESS) {
            spdlog::warn("CompositeEngine: failed to create composite semaphore");
            m_compositeSemaphore = VK_NULL_HANDLE;
        }
    }

    m_stagingRing = std::make_unique<StagingRing>();
    m_uploadManager = std::make_unique<GpuUploadManager>(
        GpuContext::get(), *m_stagingRing);
}

void CompositeEngine::shutdown()
{
    if (m_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);

    if (m_compositeSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, m_compositeSemaphore, nullptr);
        m_compositeSemaphore = VK_NULL_HANDLE;
    }

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

void CompositeEngine::resetBackoff() noexcept
{
    m_gpuBackoffAttempts = 0;
    m_gpuBackoffUntil = {};
}

// ============================================================================
// Texture cache
// ============================================================================

void CompositeEngine::clearTextureCache()
{
    clearGpuTexCache();
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
    m_gpuBackoffAttempts = 0;
    m_gpuBackoffUntil = {};
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

    // -- GPU device-lost recovery --
    {
        auto& gpu = GpuContext::get();
        if (gpu.gpuState() == GpuState::DeviceLost) {
            spdlog::warn("[COMPOSITE] GPU device lost detected -- attempting recovery");
            if (gpu.tryRecover()) {
                m_gpuCompositeState = 1;
                m_gpuBackoffAttempts = 0;
                m_gpuBackoffUntil = {};
            } else {
                m_gpuCompositeState = -1;
                return nullptr;
            }
        }
    }

    // -- GPU backoff check --
    if (m_gpuCompositeState == 1) {
        using namespace std::chrono;
        if (m_gpuBackoffAttempts > 0 &&
            steady_clock::now() < m_gpuBackoffUntil) {
            return nullptr;
        }
        if (m_gpuBackoffAttempts > 0) {
            spdlog::info("[COMPOSITE] GPU backoff expired after {} attempts, retrying",
                         m_gpuBackoffAttempts);
        }
    }

    if (m_gpuCompositeState != 1 || !compositor || !compositor->isInitialized())
        return nullptr;

    auto& ctx = GpuContext::get();

    // -- Ensure texture pool is large enough --
    while (m_gpuLayerTextures.size() < layers.size())
        m_gpuLayerTextures.push_back(std::make_unique<Texture>());
    m_gpuLayerTexKeys.resize(m_gpuLayerTextures.size());
    while (m_gpuMaskTextures.size() < layers.size())
        m_gpuMaskTextures.push_back(std::make_unique<Texture>());

    std::vector<CompositorLayer> gpuLayers;
    gpuLayers.reserve(layers.size());
    bool uploadOk = true;

    // -- Set up command buffer --
    if (!m_gpuSubmission) {
        m_gpuSubmission = std::make_unique<GpuWorkSubmission>();
        m_gpuSubmission->init(ctx.vkDevice(), ctx.cmdPool().handle());
    }
    auto& slot = *m_gpuSubmission;
    slot.beginRecording();
    VkCommandBuffer uploadCmd = slot.cmdBuffer();

    m_uploadManager->beginFrame(uploadCmd);
    m_uploadManager->setTextureCache(m_gpuTexCache.get());

    if (m_stagingRing && !m_stagingRing->isInitialized())
        m_stagingRing->init(ctx.allocator().handle(), 64u * 1024u * 1024u);

    if (!m_gpuTexCache) {
        const auto memStats = ctx.allocator().queryStats();
        const size_t gpuVram = memStats.deviceLocalBudgetBytes;
        const size_t budget = std::clamp<size_t>(
            gpuVram / 4, 512ull * 1024 * 1024, 8ull * 1024 * 1024 * 1024);
        m_gpuTexCache = std::make_unique<GpuTextureCache>(budget);
        m_uploadManager->setTextureCache(m_gpuTexCache.get());
        spdlog::info("[PERF] GpuTexCache budget: {:.0f} MB (GPU VRAM: {:.0f} MB)",
                     budget / 1048576.0, gpuVram / 1048576.0);
    }

    // -- Upload each layer --
    for (size_t li = 0; li < layers.size(); ++li) {
        const auto& layer = layers[li];

        if (layer.gpuTextureReady) {
            CompositorLayer cl;
            cl.textureInfo = layer.gpuDescriptor;
            uint32_t srcW = layer.frameWidth  ? layer.frameWidth  : outW;
            uint32_t srcH = layer.frameHeight ? layer.frameHeight : outH;
            if (layer.isPacked && srcH > 1) srcH /= 2;
            cl.transform = Compositor::buildViewportTransform(
                srcW, srcH, outW, outH,
                layer.posX, layer.posY, layer.scX, layer.scY, layer.rot,
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

        auto uploadResult = m_uploadManager->uploadLayer(
            layer, *m_gpuLayerTextures[li],
            m_gpuLayerTexKeys[li].mediaId,
            m_gpuLayerTexKeys[li].frameNumber,
            scrubMode);

        if (uploadResult.success) {
            CompositorLayer cl;
            cl.textureInfo = uploadResult.descriptor;
            cl.transform = Compositor::buildViewportTransform(
                uploadResult.srcW, uploadResult.srcH, outW, outH,
                layer.posX, layer.posY, layer.scX, layer.scY, layer.rot,
                layer.containFit);
            cl.opacity   = layer.opacity;
            cl.blendMode = static_cast<BlendMode>(layer.blendMode);
            cl.enabled   = true;
            cl.isPacked  = uploadResult.cacheHit ? uploadResult.isPacked : layer.isPacked;
            cl.isPMA     = layer.isPMA;
            cl.needsSwapRB = layer.needsSwapRB;
            cl.cropLeft   = layer.cropL / 100.0f;
            cl.cropRight  = layer.cropR / 100.0f;
            cl.cropTop    = layer.cropT / 100.0f;
            cl.cropBottom = layer.cropB / 100.0f;
            gpuLayers.push_back(cl);
        } else {
            spdlog::warn("compositeFrame: layer {} upload failed, skipping", li);
            CompositorLayer emptyCl;
            emptyCl.enabled = false;
            gpuLayers.push_back(emptyCl);
            uploadOk = false;
            break;
        }
    }

    if (!uploadOk) {
        m_uploadManager->endFrame();
        return nullptr;
    }

    // -- Upload mask textures --
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
        }
    }

    // -- Pipeline barrier: transfer -> compute --
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(uploadCmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // -- Effects pass --
    if (effectProcessor && effectProcessor->isInitialized()) {
        for (size_t li = 0; li < layers.size(); ++li) {
            const auto& layer = layers[li];
            if (layer.effects.empty()) continue;

            auto* fx = effectProcessor;
            if (!fx->isInitialized()) continue;

            ++effectLayerCount;
            effectPassCount += static_cast<int>(layer.effects.size());

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

            VkDescriptorImageInfo srcInfo = gpuLayers[li].textureInfo;
            if (fx->process(uploadCmd, srcInfo, layer.effects)) {
                gpuLayers[li].textureInfo = fx->outputDescriptorInfo();
            }

            VkMemoryBarrier fxBarrier{};
            fxBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            fxBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            fxBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(uploadCmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &fxBarrier, 0, nullptr, 0, nullptr);
        }
    }

    // -- Wipe transitions --
    bool compOk = false;
    bool readbackOk = false;

    if (transitionRenderer && transitionRenderer->isInitialized()) {
        for (size_t wi = 0; wi < layers.size(); ++wi) {
            if (layers[wi].wipeProgress < 0.0f)
                continue;

            const bool isSingletonIncoming =
                !layers[wi].isWipeOutgoing
                && layers[wi].wipePeerClipId == 0
                && layers[wi].wipeType == TransitionType::CrossDissolve;
            if (!layers[wi].isWipeOutgoing && !isSingletonIncoming)
                continue;
            if (!gpuLayers[wi].enabled) continue;

            auto* tr = transitionRenderer;
            if (!tr || !tr->isInitialized()) continue;

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
            if (layers[wi].wipePeerClipId == 0) {
                if (layers[wi].wipeType == TransitionType::FadeToWhite) {
                    srcB.textureInfo = tr->whiteDescriptorInfo();
                } else if (layers[wi].wipeType == TransitionType::FadeFromWhite) {
                    srcB = srcA;
                    srcA = TransitionSourceInfo{};
                    srcA.textureInfo = tr->whiteDescriptorInfo();
                } else if (layers[wi].wipeType == TransitionType::FadeToBlack) {
                    srcB.textureInfo = tr->blackDescriptorInfo();
                } else if (layers[wi].wipeType == TransitionType::FadeFromBlack) {
                    srcB = srcA;
                    srcA = TransitionSourceInfo{};
                    srcA.textureInfo = tr->blackDescriptorInfo();
                } else if (layers[wi].wipeType == TransitionType::CrossDissolve
                           && !layers[wi].isWipeOutgoing) {
                    srcB = srcA;
                    srcA = TransitionSourceInfo{};
                    srcA.textureInfo = tr->transparentDescriptorInfo();
                } else {
                    srcB.textureInfo = tr->transparentDescriptorInfo();
                }
            } else {
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

            GpuTransitionType gt = toGpuTransitionType(layers[wi].wipeType);
            int32_t dirOvr = transitionDirectionOverride(layers[wi].wipeType);

            if (tr->render(uploadCmd, srcA, srcB,
                               gt, layers[wi].wipeProgress,
                               dirOvr, 0.0f, layers[wi].wipeSoftness))
            {
                ++transitionCount;
                gpuLayers[wi].textureInfo = tr->outputDescriptorInfo();
                gpuLayers[wi].opacity     = 1.0f;
                gpuLayers[wi].isPacked    = false;
                gpuLayers[wi].isPMA       = false;
                gpuLayers[wi].cropLeft    = 0.0f;
                gpuLayers[wi].cropRight   = 0.0f;
                gpuLayers[wi].cropTop     = 0.0f;
                gpuLayers[wi].cropBottom  = 0.0f;
                gpuLayers[wi].blendMode   = BlendMode::Normal;
                gpuLayers[wi].transform   = Compositor::buildViewportTransform(
                    outW, outH, outW, outH,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, false);
                if (pi != SIZE_MAX)
                    gpuLayers[pi].enabled = false;

                VkMemoryBarrier trBarrier{};
                trBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                trBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                trBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(uploadCmd,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 1, &trBarrier, 0, nullptr, 0, nullptr);
            }
        }
    }

    // -- Build A/B pairs --
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

    // -- Composite + readback --
    compositor->setPairs(abPairs);
    compOk = compositor->composite(uploadCmd);
    if (compOk) {
        if (!gpuDisplayMode || scrubMode)
            readbackOk = compositor->recordReadback(uploadCmd);
    }

    // -- Single submit --
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

    if (!gpuSubmitOk) {
        GpuContext::get().signalDeviceLost();
        int backoffMs = kGpuBackoffInitialMs * (1 << m_gpuBackoffAttempts);
        if (backoffMs > kGpuBackoffMaxMs) backoffMs = kGpuBackoffMaxMs;
        m_gpuBackoffUntil = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(backoffMs);
        ++m_gpuBackoffAttempts;
        spdlog::error("[COMPOSITE] GPU submit/wait failed (attempt {}) -- backoff {}ms",
                      m_gpuBackoffAttempts, backoffMs);
        compOk = false;
        readbackOk = false;
    } else {
        m_gpuBackoffAttempts = 0;
        m_gpuBackoffUntil = {};
    }

    m_uploadManager->endFrame();

    perfTgpuUp = std::chrono::high_resolution_clock::now();

    // -- Build result frame --
    if (compOk) {
        auto result = std::make_shared<CachedFrame>();
        result->width  = outW;
        result->height = outH;
        result->stride = outW * 4;

        if (gpuDisplayMode && !scrubMode) {
            result->gpuReady     = true;
            result->gpuImageView = reinterpret_cast<uint64_t>(compositor->outputImageView());
            result->gpuSampler   = reinterpret_cast<uint64_t>(compositor->outputSampler());
            result->gpuSemaphore = reinterpret_cast<uint64_t>(m_compositeSemaphore);
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
            spdlog::info("[PERF] compositeFrame (GPU): layers={} | "
                         "decode+spine={:.1f}ms  gpu={:.1f}ms  TOTAL={:.1f}ms  "
                         "gpuDisplay={}  effectLayers={}  effectPasses={}  transitions={}",
                         layers.size(),
                         ms(perfT0, perfTlayers),
                         ms(perfTlayers, perfTcomp),
                         ms(perfT0, perfTcomp),
                         gpuDisplayMode,
                         effectLayerCount, effectPassCount, transitionCount);
            int gpuHitCount = 0, uploadCount = 0;
            for (const auto& l : layers) {
                if (l.gpuTextureReady) ++gpuHitCount;
                else ++uploadCount;
            }
            spdlog::info("[PERF]   layer breakdown: gpuCacheHit={} uploaded={}",
                         gpuHitCount, uploadCount);
            if (m_gpuTexCache) {
                spdlog::info("[PERF] GpuTexCache: {} entries, {:.0f}MB / {:.0f}MB budget, "
                             "hits={} misses={}",
                             m_gpuTexCache->entryCount(),
                             m_gpuTexCache->memoryUsed() / 1048576.0,
                             m_gpuTexCache->budget() / 1048576.0,
                             m_gpuTexCache->hits(), m_gpuTexCache->misses());
            }
        }

        return result;
    }

    return nullptr;
}
