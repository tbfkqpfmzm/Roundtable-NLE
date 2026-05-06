#pragma once

#include "StagingRing.h"

#include <volk.h>

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <memory>

namespace rt {

enum class GpuWaitOutcome : uint8_t
{
    Signaled,
    Timeout,
    Error,
    InvalidHandle
};

struct GpuWaitPolicy
{
    std::chrono::nanoseconds timeout{0};
    const char* label{"gpu-wait"};
    bool allowHeldFrameOnTimeout{true};
};

struct GpuResourceSnapshot
{
    size_t textureBytesUsed{0};
    size_t textureBudgetBytes{0};
    size_t textureEntries{0};
    size_t stagingBytesUsed{0};
    size_t stagingCapacityBytes{0};
    bool underVramPressure{false};
};

struct GpuResourceStats
{
    uint64_t boundedFenceWaits{0};
    uint64_t fenceTimeouts{0};
    uint64_t fenceErrors{0};
    uint64_t heldFrames{0};
    uint64_t queueIdleAvoided{0};
    double lastFenceWaitMs{0.0};
    double maxFenceWaitMs{0.0};
    GpuResourceSnapshot lastSnapshot;
};

class GpuResourceManager
{
public:
    [[nodiscard]] static GpuWaitPolicy playbackCompositePolicy() noexcept;
    [[nodiscard]] static GpuWaitPolicy exactCompositePolicy() noexcept;

    [[nodiscard]] GpuWaitOutcome waitForFence(
        VkDevice device,
        VkFence fence,
        const GpuWaitPolicy& policy) noexcept;

    [[nodiscard]] GpuWaitOutcome pollFence(VkDevice device, VkFence fence) noexcept;

    [[nodiscard]] bool requiresPresentationDrain(VkQueue computeQueue, VkQueue graphicsQueue) const noexcept;
    void noteQueueIdleAvoided() noexcept { ++m_stats.queueIdleAvoided; }
    void noteHeldFrame() noexcept { ++m_stats.heldFrames; }

    void updateSnapshot(GpuResourceSnapshot snapshot) noexcept { m_stats.lastSnapshot = snapshot; }
    [[nodiscard]] GpuResourceStats stats() const noexcept { return m_stats; }
    void resetStats() noexcept { m_stats = {}; }

    // ── Shared staging ring ─────────────────────────────────────────

    /// Initialize the shared staging ring (call once after VMA is ready).
    void initStagingRing(VmaAllocator allocator, VkDeviceSize capacity);

    /// Get the shared staging ring for GPU uploads.
    [[nodiscard]] StagingRing* stagingRing() noexcept { return m_stagingRing.get(); }
    [[nodiscard]] const StagingRing* stagingRing() const noexcept { return m_stagingRing.get(); }

    [[nodiscard]] bool hasStagingRing() const noexcept { return m_stagingRing != nullptr; }

private:
    GpuResourceStats m_stats;
    std::unique_ptr<StagingRing> m_stagingRing;
};

[[nodiscard]] const char* toString(GpuWaitOutcome outcome) noexcept;

} // namespace rt
