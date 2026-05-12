/*
 * GpuWorkSubmission — Triple-buffered GPU work submission ring.
 *
 * Holds a ring of 3 command buffer + fence slots.  Recording frame N
 * does NOT overwrite the commands for frame N-1 that the GPU is still
 * executing.  beginRecording() implicitly waits for the current slot's
 * fence before resetting; submit() advances to the next slot.
 * waitForAll() drains all in-flight submissions before destruction.
 */

#pragma once

#include <vulkan/vulkan_core.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rt {

/// Triple-buffered GPU work submission ring.
class GpuWorkSubmission
{
public:
    static constexpr int kRingSize = 3;

    struct Slot {
        VkCommandBuffer cmdBuffer{VK_NULL_HANDLE};
        VkFence         fence{VK_NULL_HANDLE};
        uint64_t        submissionIndex{0};
        bool            inFlight{false};
    };

    GpuWorkSubmission() = default;
    ~GpuWorkSubmission();

    GpuWorkSubmission(const GpuWorkSubmission&) = delete;
    GpuWorkSubmission& operator=(const GpuWorkSubmission&) = delete;
    GpuWorkSubmission(GpuWorkSubmission&&) noexcept;
    GpuWorkSubmission& operator=(GpuWorkSubmission&&) noexcept;

    /// Initialize with device and command pool.
    /// Allocates 3 command buffers and creates 3 fences.
    bool init(VkDevice device, VkCommandPool cmdPool);

    /// Destroy all resources.  Calls waitForAll() first.
    void destroy();

    /// Begin recording into the current ring slot.
    /// Implicitly waits for this slot's fence (if inFlight) before
    /// resetting the command buffer, ensuring the GPU is done with
    /// the previous submission that used this slot.
    bool beginRecording();

    /// End recording commands into the current slot.
    bool endRecording();

    /// Submit the current slot's command buffer and advance the ring.
    bool submit(VkQueue queue, std::mutex* queueLock = nullptr);

    /// Submit with an optional semaphore to signal after command completion.
    bool submit(VkQueue queue, VkSemaphore signalSemaphore, std::mutex* queueLock = nullptr);

    /// Submit and wait for completion (legacy single-shot usage).
    bool submitAndWait(VkQueue queue, std::mutex* queueLock = nullptr);

    /// Wait for the most recently submitted slot's fence.
    bool waitForCompletion(uint64_t timeoutNs = UINT64_MAX);

    /// Wait for ALL in-flight slots across the ring.
    void waitForAll();

    /// Check if the most recently submitted slot has signaled.
    [[nodiscard]] bool isComplete() const;

    /// Add a pipeline barrier to the current slot's command buffer.
    void addBarrier(VkPipelineStageFlags srcStage,
                    VkPipelineStageFlags dstStage,
                    VkAccessFlags srcAccess,
                    VkAccessFlags dstAccess);

    // ── Accessors ──────────────────────────────────────────────────

    /// Returns the current recording slot's command buffer.
    [[nodiscard]] VkCommandBuffer cmdBuffer() const noexcept {
        return m_slots[m_currentSlot].cmdBuffer;
    }

    /// Returns the current slot's fence.
    [[nodiscard]] VkFence fence() const noexcept {
        return m_slots[m_currentSlot].fence;
    }

    [[nodiscard]] bool isValid() const noexcept { return m_device != VK_NULL_HANDLE; }
    [[nodiscard]] int currentSlot() const noexcept { return m_currentSlot; }
    [[nodiscard]] uint64_t globalSubmissionIndex() const noexcept { return m_globalSubmissionIndex; }

private:
    VkDevice                m_device{VK_NULL_HANDLE};
    VkCommandPool           m_cmdPool{VK_NULL_HANDLE};
    std::array<Slot, kRingSize> m_slots{};
    uint64_t                m_globalSubmissionIndex{0};
    int                     m_currentSlot{0};
    bool                    m_recording{false};

    Slot& slot() { return m_slots[m_currentSlot]; }
    const Slot& slot() const { return m_slots[m_currentSlot]; }
    void advanceSlot();
};

} // namespace rt
