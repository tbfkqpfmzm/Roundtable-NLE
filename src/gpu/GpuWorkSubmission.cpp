/*
 * GpuWorkSubmission.cpp — See GpuWorkSubmission.h for architecture.
 */

#include "GpuWorkSubmission.h"

#include <volk.h>
#include <utility>

namespace rt {

// ── Destructor ──────────────────────────────────────────────────────────────

GpuWorkSubmission::~GpuWorkSubmission()
{
    destroy();
}

// ── Move ────────────────────────────────────────────────────────────────────

GpuWorkSubmission::GpuWorkSubmission(GpuWorkSubmission&& other) noexcept
    : m_device(other.m_device)
    , m_cmdPool(other.m_cmdPool)
    , m_cmdBuffer(other.m_cmdBuffer)
    , m_fence(other.m_fence)
    , m_recording(other.m_recording)
{
    other.m_device    = VK_NULL_HANDLE;
    other.m_cmdPool   = VK_NULL_HANDLE;
    other.m_cmdBuffer = VK_NULL_HANDLE;
    other.m_fence     = VK_NULL_HANDLE;
    other.m_recording = false;
}

GpuWorkSubmission& GpuWorkSubmission::operator=(GpuWorkSubmission&& other) noexcept
{
    if (this != &other) {
        destroy();
        m_device    = other.m_device;
        m_cmdPool   = other.m_cmdPool;
        m_cmdBuffer = other.m_cmdBuffer;
        m_fence     = other.m_fence;
        m_recording = other.m_recording;
        other.m_device    = VK_NULL_HANDLE;
        other.m_cmdPool   = VK_NULL_HANDLE;
        other.m_cmdBuffer = VK_NULL_HANDLE;
        other.m_fence     = VK_NULL_HANDLE;
        other.m_recording = false;
    }
    return *this;
}

// ── init ────────────────────────────────────────────────────────────────────

bool GpuWorkSubmission::init(VkDevice device, VkCommandPool cmdPool)
{
    if (device == VK_NULL_HANDLE || cmdPool == VK_NULL_HANDLE)
        return false;

    m_device  = device;
    m_cmdPool = cmdPool;

    // Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = cmdPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(device, &allocInfo, &m_cmdBuffer) != VK_SUCCESS)
        return false;

    // Create fence (initially unsignaled)
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, cmdPool, 1, &m_cmdBuffer);
        m_cmdBuffer = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void GpuWorkSubmission::destroy()
{
    if (m_device != VK_NULL_HANDLE) {
        // Wait for any pending work to complete before destroying.
        if (m_fence != VK_NULL_HANDLE) {
            vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(m_device, m_fence, nullptr);
            m_fence = VK_NULL_HANDLE;
        }
        if (m_cmdBuffer != VK_NULL_HANDLE && m_cmdPool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(m_device, m_cmdPool, 1, &m_cmdBuffer);
            m_cmdBuffer = VK_NULL_HANDLE;
        }
    }
    m_device  = VK_NULL_HANDLE;
    m_cmdPool = VK_NULL_HANDLE;
    m_recording = false;
}

// ── beginRecording ──────────────────────────────────────────────────────────

bool GpuWorkSubmission::beginRecording()
{
    if (m_cmdBuffer == VK_NULL_HANDLE)
        return false;

    vkResetCommandBuffer(m_cmdBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(m_cmdBuffer, &beginInfo) != VK_SUCCESS)
        return false;

    m_recording = true;
    return true;
}

// ── endRecording ────────────────────────────────────────────────────────────

bool GpuWorkSubmission::endRecording()
{
    if (m_cmdBuffer == VK_NULL_HANDLE || !m_recording)
        return false;
    if (vkEndCommandBuffer(m_cmdBuffer) != VK_SUCCESS)
        return false;
    m_recording = false;
    return true;
}

// ── submitAndWait ───────────────────────────────────────────────────────────

bool GpuWorkSubmission::submitAndWait(VkQueue queue, std::mutex* queueLock)
{
    if (m_cmdBuffer == VK_NULL_HANDLE || m_fence == VK_NULL_HANDLE)
        return false;

    // Reset fence before submission
    vkResetFences(m_device, 1, &m_fence);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &m_cmdBuffer;

    VkResult result;
    if (queueLock) {
        std::lock_guard lock(*queueLock);
        result = vkQueueSubmit(queue, 1, &submitInfo, m_fence);
    } else {
        result = vkQueueSubmit(queue, 1, &submitInfo, m_fence);
    }

    if (result != VK_SUCCESS)
        return false;

    // Wait for completion
    result = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    return result == VK_SUCCESS;
}

// ── submit ──────────────────────────────────────────────────────────────────

bool GpuWorkSubmission::submit(VkQueue queue, std::mutex* queueLock)
{
    return submit(queue, VK_NULL_HANDLE, queueLock);
}

// ── submit (with signal semaphore) ──────────────────────────────────────────

bool GpuWorkSubmission::submit(VkQueue queue, VkSemaphore signalSemaphore,
                                std::mutex* queueLock)
{
    if (m_cmdBuffer == VK_NULL_HANDLE || m_fence == VK_NULL_HANDLE)
        return false;

    vkResetFences(m_device, 1, &m_fence);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &m_cmdBuffer;

    VkSemaphore signalSemaphores[1]{};
    if (signalSemaphore != VK_NULL_HANDLE) {
        signalSemaphores[0] = signalSemaphore;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores    = signalSemaphores;
    }

    if (queueLock) {
        std::lock_guard lock(*queueLock);
        return vkQueueSubmit(queue, 1, &submitInfo, m_fence) == VK_SUCCESS;
    }
    return vkQueueSubmit(queue, 1, &submitInfo, m_fence) == VK_SUCCESS;
}

// ── waitForCompletion ───────────────────────────────────────────────────────

bool GpuWorkSubmission::waitForCompletion(uint64_t timeoutNs)
{
    if (m_fence == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE)
        return false;
    return vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, timeoutNs) == VK_SUCCESS;
}

// ── isComplete ──────────────────────────────────────────────────────────────

bool GpuWorkSubmission::isComplete() const
{
    if (m_fence == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE)
        return true; // No pending work = complete
    return vkGetFenceStatus(m_device, m_fence) == VK_SUCCESS;
}

// ── addBarrier ──────────────────────────────────────────────────────────────

void GpuWorkSubmission::addBarrier(VkPipelineStageFlags srcStage,
                                    VkPipelineStageFlags dstStage,
                                    VkAccessFlags srcAccess,
                                    VkAccessFlags dstAccess)
{
    if (m_cmdBuffer == VK_NULL_HANDLE || !m_recording)
        return;

    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    vkCmdPipelineBarrier(m_cmdBuffer,
                         srcStage, dstStage,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

} // namespace rt
