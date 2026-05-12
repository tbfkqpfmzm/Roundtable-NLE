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
#include "CompositeEngine.h"
#include "GpuContext.h"
#include "Compositor.h"
#include "EffectProcessor.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"

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
    // Create the composite engine (owns GPU compositing pipeline).
    m_engine = std::make_unique<CompositeEngine>();
    if (GpuContext::get().isInitialized()) {
        VkDevice device = GpuContext::get().vkDevice();
        if (device != VK_NULL_HANDLE)
            m_engine->init(device);
    }

    // ── Start the background prewarm thread ───────────────────────────
    // prewarmPlaybackResources() now enqueues work on this thread instead
    // of blocking the UI thread at play-start.  The thread loop waits on
    // a condition variable and wakes up when a new prewarm request arrives
    // or when m_destroying is set.
    m_prewarmThread = std::thread(&CompositeService::prewarmThreadLoop, this);
}

CompositeService::~CompositeService()
{
    // ── Signal prewarm thread to exit ─────────────────────────────────
    m_destroying.store(true);
    {
        std::lock_guard lock(m_prewarmMutex);
        m_prewarmCv.notify_one();
    }
    if (m_prewarmThread.joinable())
        m_prewarmThread.join();

    // Destroy the composite engine — it drains GPU queues and frees all
    // GPU resources (submission, staging ring, texture cache, layers).
    m_engine.reset();
}

void CompositeService::reset()
{
    invalidateCacheDirect();
    m_compositeBuffer.reset();
    m_openMediaHandles.clear();
    m_videoFallbackCache.clear();
    if (m_engine) {
        m_engine->clearLru();
        m_engine->resetBackoff();
    }
    m_safeMode.store(false, std::memory_order_release);
    m_lastSafeModeComposite = {};
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
        // Cache directory is always under the program's assets/ folder
        // (resolved relative to the working directory in dev, or to the
        //  app directory in installed builds — the same as how assets/
        //  itself is resolved).
        m_animVideoCache = std::make_unique<AnimationVideoCache>(
            pool, "assets/Converted", "assets");
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
    if (m_engine) {
        m_engine->clearLru();
    }
    {
        std::lock_guard lg(m_lastCompositeMtx);
        m_lastGoodComposite.reset();
    }
    m_cacheInvalidateRequested.store(false, std::memory_order_release);
}

void CompositeService::shutdown()
{
    m_shutdown.store(true, std::memory_order_release);

    // Shut down the composite engine — waits for GPU idle, destroys
    // all GPU resources (submission, staging ring, texture cache, layers).
    if (m_engine) {
        m_engine->shutdown();
    }

    spdlog::info("CompositeService::shutdown() — complete");
}

int CompositeService::vramUsagePercent() const
{
    return m_engine ? m_engine->vramUsagePercent() : 0;
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