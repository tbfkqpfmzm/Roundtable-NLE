#include "GpuResourceManager.h"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace rt {

GpuWaitPolicy GpuResourceManager::playbackCompositePolicy() noexcept
{
    return GpuWaitPolicy{
        std::chrono::milliseconds(16),
        "playback-composite-fence",
        true
    };
}

GpuWaitPolicy GpuResourceManager::exactCompositePolicy() noexcept
{
    return GpuWaitPolicy{
        std::chrono::milliseconds(250),
        "exact-composite-fence",
        false
    };
}

GpuWaitOutcome GpuResourceManager::waitForFence(
    VkDevice device,
    VkFence fence,
    const GpuWaitPolicy& policy) noexcept
{
    if (device == VK_NULL_HANDLE || fence == VK_NULL_HANDLE || policy.timeout.count() <= 0) {
        return GpuWaitOutcome::InvalidHandle;
    }

    using Clock = std::chrono::steady_clock;
    const auto waitStart = Clock::now();
    const auto timeoutNs = static_cast<uint64_t>(policy.timeout.count());
    const VkResult result = vkWaitForFences(device, 1, &fence, VK_TRUE, timeoutNs);
    const auto waitEnd = Clock::now();
    const double waitMs = std::chrono::duration<double, std::milli>(waitEnd - waitStart).count();

    ++m_stats.boundedFenceWaits;
    m_stats.lastFenceWaitMs = waitMs;
    m_stats.maxFenceWaitMs = std::max(m_stats.maxFenceWaitMs, waitMs);

    if (result == VK_SUCCESS) {
        return GpuWaitOutcome::Signaled;
    }
    if (result == VK_TIMEOUT) {
        ++m_stats.fenceTimeouts;
        if (policy.allowHeldFrameOnTimeout) {
            ++m_stats.heldFrames;
        }
        return GpuWaitOutcome::Timeout;
    }

    ++m_stats.fenceErrors;
    return GpuWaitOutcome::Error;
}

GpuWaitOutcome GpuResourceManager::pollFence(VkDevice device, VkFence fence) noexcept
{
    if (device == VK_NULL_HANDLE || fence == VK_NULL_HANDLE) {
        return GpuWaitOutcome::InvalidHandle;
    }

    const VkResult result = vkGetFenceStatus(device, fence);
    if (result == VK_SUCCESS) {
        return GpuWaitOutcome::Signaled;
    }
    if (result == VK_NOT_READY) {
        return GpuWaitOutcome::Timeout;
    }

    ++m_stats.fenceErrors;
    return GpuWaitOutcome::Error;
}

bool GpuResourceManager::requiresPresentationDrain(VkQueue computeQueue, VkQueue graphicsQueue) const noexcept
{
    return computeQueue != VK_NULL_HANDLE &&
           graphicsQueue != VK_NULL_HANDLE &&
           computeQueue != graphicsQueue;
}

const char* toString(GpuWaitOutcome outcome) noexcept
{
    switch (outcome) {
    case GpuWaitOutcome::Signaled:      return "Signaled";
    case GpuWaitOutcome::Timeout:       return "Timeout";
    case GpuWaitOutcome::Error:         return "Error";
    case GpuWaitOutcome::InvalidHandle: return "InvalidHandle";
    }
    return "Unknown";
}

void GpuResourceManager::initStagingRing(VmaAllocator allocator, VkDeviceSize capacity)
{
    if (m_stagingRing) {
        spdlog::warn("[GpuResourceManager] StagingRing already initialized");
        return;
    }
    m_stagingRing = std::make_unique<StagingRing>();
    if (!m_stagingRing->init(allocator, capacity)) {
        spdlog::error("[GpuResourceManager] Failed to initialize shared StagingRing");
        m_stagingRing.reset();
        return;
    }
    spdlog::info("[GpuResourceManager] Shared StagingRing initialized ({} MB)",
                 capacity / (1024 * 1024));
}

} // namespace rt
