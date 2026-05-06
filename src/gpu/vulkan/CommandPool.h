/*
 * CommandPool — Per-thread Vulkan command buffer management.
 *
 * Step 2: Provides command buffer allocation and submission helpers.
 * Each thread gets its own pool for lock-free command recording.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>
#include <mutex>

namespace rt {

class Device;

/// RAII wrapper for VkCommandPool + command buffer helpers.
class CommandPool
{
public:
    CommandPool() = default;
    ~CommandPool();

    CommandPool(const CommandPool&) = delete;
    CommandPool& operator=(const CommandPool&) = delete;
    CommandPool(CommandPool&&) noexcept;
    CommandPool& operator=(CommandPool&&) noexcept;

    /// Create a command pool for a given queue family.
    bool create(VkDevice device, uint32_t queueFamilyIndex,
                VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    /// Destroy the pool and all allocated command buffers.
    void destroy();

    /// Allocate a primary command buffer.
    VkCommandBuffer allocateBuffer();

    /// Allocate multiple primary command buffers.
    std::vector<VkCommandBuffer> allocateBuffers(uint32_t count);

    /// Free a command buffer back to the pool.
    void freeBuffer(VkCommandBuffer buffer);

    /// Reset the entire pool (reclaim all allocated buffers).
    void reset(VkCommandPoolResetFlags flags = 0);

    // ── One-shot command helpers ────────────────────────────────────────

    /// Begin a one-shot command buffer (ONE_TIME_SUBMIT).
    VkCommandBuffer beginSingleTime();

    /// End and submit a one-shot command buffer, then wait for completion.
    /// If a queue mutex was set via setQueueMutex(), it will be locked
    /// around the vkQueueSubmit call automatically.
    /// @param queueMutex  Optional per-call override mutex.
    void endSingleTime(VkCommandBuffer buffer, VkQueue queue,
                       std::mutex* queueMutex = nullptr);

    /// End and submit with a timeline semaphore wait (e.g. CUDA→Vulkan sync).
    /// Vulkan will wait at COMPUTE stage until the semaphore reaches waitValue.
    void endSingleTimeWithWait(VkCommandBuffer buffer, VkQueue queue,
                               VkSemaphore waitSemaphore, uint64_t waitValue,
                               std::mutex* queueMutex = nullptr);

    /// Set a mutex to lock around all queue submissions from this pool.
    /// Used when the pool operates on a shared queue from a worker thread.
    void setQueueMutex(std::mutex* mtx) noexcept { m_queueMutex = mtx; }

    // ── Accessors ───────────────────────────────────────────────────────
    [[nodiscard]] VkCommandPool handle() const noexcept { return m_pool; }

    operator VkCommandPool() const noexcept { return m_pool; }

private:
    VkDevice      m_device{VK_NULL_HANDLE};
    VkCommandPool m_pool{VK_NULL_HANDLE};
    std::mutex*   m_queueMutex{nullptr};  ///< Optional: lock around queue submits
};

} // namespace rt
