/*
 * CommandPool.cpp — Per-thread command buffer management.
 * Step 2: Vulkan Initialization
 */

#include <volk.h>
#include "vulkan/CommandPool.h"
#include "vulkan/Device.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <utility>

namespace rt {

// ── Destructor ──────────────────────────────────────────────────────────────

CommandPool::~CommandPool()
{
    destroy();
}

// ── Move constructor ────────────────────────────────────────────────────────

CommandPool::CommandPool(CommandPool&& other) noexcept
    : m_device(other.m_device)
    , m_pool(other.m_pool)
    , m_queueMutex(other.m_queueMutex)
{
    other.m_device     = VK_NULL_HANDLE;
    other.m_pool       = VK_NULL_HANDLE;
    other.m_queueMutex = nullptr;
}

// ── Move assignment ─────────────────────────────────────────────────────────

CommandPool& CommandPool::operator=(CommandPool&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_device       = other.m_device;
        m_pool         = other.m_pool;
        m_queueMutex   = other.m_queueMutex;
        other.m_device     = VK_NULL_HANDLE;
        other.m_pool       = VK_NULL_HANDLE;
        other.m_queueMutex = nullptr;
    }
    return *this;
}

// ── create ──────────────────────────────────────────────────────────────────

bool CommandPool::create(VkDevice device, uint32_t queueFamilyIndex,
                         VkCommandPoolCreateFlags flags)
{
    m_device = device;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags            = flags;

    VkResult result = vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_pool);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create command pool (VkResult: {})", static_cast<int>(result));
        return false;
    }

    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void CommandPool::destroy()
{
    if (m_pool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    m_device = VK_NULL_HANDLE;
}

// ── allocateBuffer ──────────────────────────────────────────────────────────

VkCommandBuffer CommandPool::allocateBuffer()
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = m_pool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer buffer;
    VkResult result = vkAllocateCommandBuffers(m_device, &allocInfo, &buffer);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to allocate command buffer");
        return VK_NULL_HANDLE;
    }

    return buffer;
}

// ── allocateBuffers ─────────────────────────────────────────────────────────

std::vector<VkCommandBuffer> CommandPool::allocateBuffers(uint32_t count)
{
    std::vector<VkCommandBuffer> buffers(count);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = m_pool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = count;

    VkResult result = vkAllocateCommandBuffers(m_device, &allocInfo, buffers.data());
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to allocate {} command buffers", count);
        return {};
    }

    return buffers;
}

// ── freeBuffer ──────────────────────────────────────────────────────────────

void CommandPool::freeBuffer(VkCommandBuffer buffer)
{
    if (buffer != VK_NULL_HANDLE)
        vkFreeCommandBuffers(m_device, m_pool, 1, &buffer);
}

// ── reset ───────────────────────────────────────────────────────────────────

void CommandPool::reset(VkCommandPoolResetFlags flags)
{
    vkResetCommandPool(m_device, m_pool, flags);
}

// ── beginSingleTime ─────────────────────────────────────────────────────────

VkCommandBuffer CommandPool::beginSingleTime()
{
    VkCommandBuffer buffer = allocateBuffer();
    if (buffer == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(buffer, &beginInfo);
    return buffer;
}

// ── Helper: submit, wait on fence, destroy fence, free buffer ────────────────

/// Submit a command buffer with optional mutex guard, wait for completion,
/// then destroy the fence and free the buffer.  Replaces duplicated
/// fence boilerplate in both endSingleTime / endSingleTimeWithWait.
static void submitAndWait(VkDevice device, VkQueue queue,
                           VkCommandBuffer buffer, VkSubmitInfo& submitInfo,
                           std::mutex* mtx)
{
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceInfo, nullptr, &fence);

    if (mtx) {
        std::lock_guard lock(*mtx);
        vkQueueSubmit(queue, 1, &submitInfo, fence);
    } else {
        vkQueueSubmit(queue, 1, &submitInfo, fence);
    }

    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device, fence, nullptr);
}

// ── endSingleTime ───────────────────────────────────────────────────────────

void CommandPool::endSingleTime(VkCommandBuffer buffer, VkQueue queue,
                                 std::mutex* queueMutex)
{
    vkEndCommandBuffer(buffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &buffer;

    std::mutex* mtx = queueMutex ? queueMutex : m_queueMutex;
    submitAndWait(m_device, queue, buffer, submitInfo, mtx);
    freeBuffer(buffer);
}

// ── endSingleTimeWithWait ───────────────────────────────────────────────────

void CommandPool::endSingleTimeWithWait(VkCommandBuffer buffer, VkQueue queue,
                                         VkSemaphore waitSemaphore,
                                         uint64_t waitValue,
                                         std::mutex* queueMutex)
{
    vkEndCommandBuffer(buffer);

    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.waitSemaphoreValueCount   = 1;
    timelineInfo.pWaitSemaphoreValues      = &waitValue;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = &timelineInfo;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = &waitSemaphore;
    submitInfo.pWaitDstStageMask    = &waitStage;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &buffer;

    std::mutex* mtx = queueMutex ? queueMutex : m_queueMutex;
    submitAndWait(m_device, queue, buffer, submitInfo, mtx);
    freeBuffer(buffer);
}

} // namespace rt

