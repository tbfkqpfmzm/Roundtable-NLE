/*
 * CompositeService.cpp - Frame compositing pipeline (extracted from TimelineWorkspace).
 * No Qt dependency.
 */

// **Must** come before any header that pulls in <vulkan/vulkan.h> so
// volk can define VK_NO_PROTOTYPES first.  Without this, calls like
// vkQueueWaitIdle resolve to a direct function symbol instead of a
// function-pointer variable, causing an ACCESS_VIOLATION crash.
#include <volk.h>

#include "CompositeService.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "ClipRenderers.h"

// Media / timeline
#include "media/FrameCache.h"
#include "media/MediaPool.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "timeline/OpacityMask.h"

#include "project/Project.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/AnimationVideoCache.h"
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "stb_image.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_set>

// GPU compositing
#include "GpuWorkSubmission.h"
#include "GpuContext.h"
#include "Compositor.h"
#include "GpuTextureCache.h"
#include "EffectProcessor.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"
#include "vulkan/Texture.h"

#ifdef _WIN32
#include <windows.h>
#endif


namespace rt {

std::atomic<bool> CompositeService::s_modalDialogActive{false};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

CompositeService::CompositeService()
{
    m_compositeLru.resize(COMPOSITE_CACHE_SIZE);

    // Create inter-queue binary semaphore for compute→graphics sync.
    // Signaled after the compute queue finishes the compositor's output write;
    // waited on by the graphics queue (VulkanViewport) before reading it.
    // Without this, the two queues race on the compositor's single output
    // texture, causing a GPU data hazard → display freeze on scrub→play.
    if (GpuContext::get().isInitialized()) {
        VkDevice device = GpuContext::get().vkDevice();
        if (device != VK_NULL_HANDLE) {
            VkSemaphoreCreateInfo semInfo{};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(device, &semInfo, nullptr, &m_compositeSemaphore) != VK_SUCCESS) {
                spdlog::warn("CompositeService: failed to create composite semaphore");
                m_compositeSemaphore = VK_NULL_HANDLE;
            }
        }
    }
}

CompositeService::~CompositeService()
{
    // Drain GPU queues before destroying any resources.  In GPU display mode,
    // the VulkanViewport may have an in-flight present or descriptor set that
    // references the compositor's output texture.  If we destroy the texture
    // (via clearGpuTexCache / m_gpuLayerTextures.clear()) without waiting,
    // the next VulkanViewport render pass reads freed memory → access violation.
    if (GpuContext::get().isInitialized()) {
        auto& ctx = GpuContext::get();
        VkDevice device = ctx.vkDevice();
        if (device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(device);
    }

    if (m_compositeSemaphore != VK_NULL_HANDLE && GpuContext::get().isInitialized()) {
        vkDestroySemaphore(GpuContext::get().vkDevice(), m_compositeSemaphore, nullptr);
        m_compositeSemaphore = VK_NULL_HANDLE;
    }

    destroyCompositeSlot();
    clearGpuTexCache();
}

void CompositeService::reset()
{
    invalidateCacheDirect();
    m_compositeBuffer.reset();
    m_openMediaHandles.clear();
    m_videoFallbackCache.clear();
    m_gpuLayerTextures.clear();
    m_gpuLayerTexKeys.clear();
    m_gpuMaskTextures.clear();
    m_gpuCompositeState = 0;
    m_gpuBackoffAttempts = 0;
    m_gpuBackoffUntil = {};
    m_lastActiveClipIds.clear();
    m_prewarmedClipIds.clear();
    m_lastLookaheadScan = {};
    m_stickyLastClipFrame.clear();
    m_stickyLastCharFrame.clear();
#ifdef ROUNDTABLE_HAS_SPINE
    m_spineSharedCache.clear();
    m_animNameCache.clear();
    m_gpuSpineActiveCharKey.clear();
    m_spineCache.clear();
    m_lastPreRenderedSpineFrame.clear();
    {
        std::lock_guard lg(m_spinePendingMutex);
        m_spinePendingKeys.clear();
    }
    m_spineLoadFutures.clear();
    m_animVideoCache.reset();
#endif
}

#ifdef ROUNDTABLE_HAS_SPINE
void CompositeService::initAnimVideoCache(MediaPool* pool)
{
    if (!m_animVideoCache && pool) {
        // Use user data directory for cache (writable when installed to Program Files)
        std::string cacheDir = "assets/cache/animations";
#ifdef _WIN32
        wchar_t appData[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring wdir = std::wstring(appData) + L"/ROUNDTABLE/cache/animations";
            cacheDir = std::string(wdir.begin(), wdir.end());
        }
#endif
        m_animVideoCache = std::make_unique<AnimationVideoCache>(
            pool, cacheDir, "assets");
        m_animVideoCache->scanCacheDirectory();
    }
}

void CompositeService::drainCompletedSpineFutures()
{
    m_spineLoadFutures.erase(
        std::remove_if(m_spineLoadFutures.begin(), m_spineLoadFutures.end(),
                       [](const std::future<void>& f) {
                           return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                       }),
        m_spineLoadFutures.end());
}

void CompositeService::addSpineFuture(std::future<void> f)
{
    m_spineLoadFutures.push_back(std::move(f));
}

std::shared_ptr<CompositeService::SpineSharedData>
CompositeService::findSpineSharedData(const std::string& key) const
{
    auto it = m_spineSharedCache.find(key);
    return (it != m_spineSharedCache.end()) ? it->second : nullptr;
}

void CompositeService::storeSpineSharedData(const std::string& key,
                                            std::shared_ptr<SpineSharedData> data)
{
    m_spineSharedCache[key] = std::move(data);
}

const std::vector<std::string>*
CompositeService::findAnimNames(const std::string& key) const
{
    auto it = m_animNameCache.find(key);
    return (it != m_animNameCache.end()) ? &it->second : nullptr;
}

void CompositeService::storeAnimNames(const std::string& key,
                                      std::vector<std::string> names)
{
    m_animNameCache[key] = std::move(names);
}

void CompositeService::evictSpineState(uint64_t clipId)
{
    m_spineCache.erase(clipId);
}

void CompositeService::purgeDeadSpineStates(const std::unordered_set<uint64_t>& liveIds)
{
    for (auto it = m_spineCache.begin(); it != m_spineCache.end(); ) {
        if (liveIds.find(it->first) == liveIds.end())
            it = m_spineCache.erase(it);
        else
            ++it;
    }
}
#endif

void CompositeService::invalidateCacheDirect()
{
    m_compositeLru.clear();
    m_compositeLru.resize(COMPOSITE_CACHE_SIZE);
    m_compositeLruIdx = 0;
    {
        std::lock_guard lg(m_lastCompositeMtx);
        m_lastGoodComposite.reset();
    }
    m_gpuLayerTexKeys.clear();
    m_cacheInvalidateRequested.store(false, std::memory_order_release);
}

void CompositeService::destroyCompositeSlot()
{
    m_gpuSubmission.reset();
    m_gpuCompositeState = 0;
    m_gpuBackoffAttempts = 0;
    m_gpuBackoffUntil = {};
}

void CompositeService::clearGpuTexCache()
{
    m_gpuTexCache.reset();
}

int CompositeService::vramUsagePercent() const
{
    return m_gpuTexCache ? m_gpuTexCache->usagePercent() : 0;
}

#ifdef ROUNDTABLE_HAS_SPINE
void CompositeService::integrateSpineSharedData(const std::string& key,
                                                 std::shared_ptr<SpineSharedData> shared)
{
    m_spineSharedCache[key] = std::move(shared);
}

void CompositeService::removeSpinePendingKey(const std::string& key)
{
    std::lock_guard lg(m_spinePendingMutex);
    m_spinePendingKeys.erase(key);
}

bool CompositeService::isSpinePending(const std::string& key) const
{
    std::lock_guard lg(m_spinePendingMutex);
    return m_spinePendingKeys.count(key) > 0;
}

bool CompositeService::addSpinePendingKey(const std::string& key)
{
    std::lock_guard lg(m_spinePendingMutex);
    if (m_spinePendingKeys.count(key))
        return false;
    m_spinePendingKeys.insert(key);
    return true;
}
#endif


} // namespace rt