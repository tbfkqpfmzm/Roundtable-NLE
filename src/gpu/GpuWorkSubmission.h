/*
 * GpuWorkSubmission — RAII wrapper for per-frame GPU work submission.
 *
 * Encapsulates the VkFence + VkCommandBuffer + submission/wait pattern
 * that CompositeServiceFrame.cpp currently manages with raw Vulkan calls.
 * Replaces CompositeGpuSlot.
 */

#pragma once

#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <vector>

namespace rt {

/// RAII wrapper for a single GPU work submission slot.
class GpuWorkSubmission
{
public:
    GpuWorkSubmission() = default;
    ~GpuWorkSubmission();

    GpuWorkSubmission(const GpuWorkSubmission&) = delete;
    GpuWorkSubmission& operator=(const GpuWorkSubmission&) = delete;
    GpuWorkSubmission(GpuWorkSubmission&&) noexcept;
    GpuWorkSubmission& operator=(GpuWorkSubmission&&) noexcept;

    /// Initialize with device and command pool.
    /// Allocates the command buffer and creates the fence.
    bool init(VkDevice device, VkCommandPool cmdPool);

    /// Destroy all resources.
    void destroy();

    /// Begin recording commands into the command buffer.
    bool beginRecording();

    /// End recording commands.
    bool endRecording();

    /// Submit the command buffer to a queue, then wait for completion.
    /// @param queue     Queue to submit to.
    /// @param queueLock Optional mutex to lock around vkQueueSubmit.
    /// @return true if the fence signaled successfully.
    bool submitAndWait(VkQueue queue, std::mutex* queueLock = nullptr);

    /// Submit without waiting (caller must waitForCompletion() later).
    bool submit(VkQueue queue, std::mutex* queueLock = nullptr);

    /// Submit with an optional semaphore to signal after command completion.
    /// The semaphore is added to pSignalSemaphores alongside any internal ones.
    bool submit(VkQueue queue, VkSemaphore signalSemaphore, std::mutex* queueLock = nullptr);

    /// Wait for the fence to signal.
    bool waitForCompletion(uint64_t timeoutNs = UINT64_MAX);

    /// Check if the fence has signaled (non-blocking).
    [[nodiscard]] bool isComplete() const;

    /// Add a pipeline barrier to the command buffer.
    void addBarrier(VkPipelineStageFlags srcStage,
                    VkPipelineStageFlags dstStage,
                    VkAccessFlags srcAccess,
                    VkAccessFlags dstAccess);

    // ── Accessors ──────────────────────────────────────────────────

    [[nodiscard]] VkCommandBuffer cmdBuffer() const noexcept { return m_cmdBuffer; }
    [[nodiscard]] VkFence fence() const noexcept { return m_fence; }
    [[nodiscard]] bool isValid() const noexcept { return m_device != VK_NULL_HANDLE; }

private:
    VkDevice        m_device{VK_NULL_HANDLE};
    VkCommandPool   m_cmdPool{VK_NULL_HANDLE};
    VkCommandBuffer m_cmdBuffer{VK_NULL_HANDLE};
    VkFence         m_fence{VK_NULL_HANDLE};
    bool            m_recording{false};
};

} // namespace rt
