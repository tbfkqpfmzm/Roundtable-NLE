/*
 * GpuWorkSubmission.cpp — Triple-buffered GPU work submission ring.
 *
 * See GpuWorkSubmission.h for architecture.
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
    , m_slots(other.m_slots)
    , m_globalSubmissionIndex(other.m_globalSubmissionIndex)
    , m_currentSlot(other.m_currentSlot)
    , m_recording(other.m_recording)
{
    other.m_device    = VK_NULL_HANDLE;
    other.m_cmdPool   = VK_NULL_HANDLE;
    other.m_slots     = {};
    other.m_globalSubmissionIndex = 0;
    other.m_currentSlot = 0;
    other.m_recording = false;
}

GpuWorkSubmission& GpuWorkSubmission::operator=(GpuWorkSubmission&& other) noexcept
{
    if (this != &other) {
        destroy();
        m_device    = other.m_device;
        m_cmdPool   = other.m_cmdPool;
        m_slots     = other.m_slots;
        m_globalSubmissionIndex = other.m_globalSubmissionIndex;
        m_currentSlot = other.m_currentSlot;
        m_recording = other.m_recording;
        other.m_device    = VK_NULL_HANDLE;
        other.m_cmdPool   = VK_NULL_HANDLE;
        other.m_slots     = {};
        other.m_globalSubmissionIndex = 0;
        other.m_currentSlot = 0;
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

    // Allocate command buffer array and create fences for all 3 slots
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = cmdPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kRingSize;

    VkCommandBuffer rawBuffers[kRingSize];
    if (vkAllocateCommandBuffers(device, &allocInfo, rawBuffers) != VK_SUCCESS)
        return false;

    for (int i = 0; i < kRingSize; ++i) {
        m_slots[i].cmdBuffer = rawBuffers[i];

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // Create fences in signaled state so first beginRecording() doesn't wait
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device, &fenceInfo, nullptr, &m_slots[i].fence) != VK_SUCCESS) {
            // Cleanup already-created fences
            for (int j = 0; j < i; ++j)
                vkDestroyFence(device, m_slots[j].fence, nullptr);
            vkFreeCommandBuffers(device, cmdPool, kRingSize, rawBuffers);
            for (auto& s : m_slots) s = Slot{};
            return false;
        }
    }

    m_currentSlot = 0;
    m_globalSubmissionIndex = 0;
    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void GpuWorkSubmission::destroy()
{
    if (m_device != VK_NULL_HANDLE) {
        // Wait for ALL in-flight slots before destroying resources
        waitForAll();

        for (int i = 0; i < kRingSize; ++i) {
            auto& s = m_slots[i];
            if (s.fence != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, s.fence, nullptr);
                s.fence = VK_NULL_HANDLE;
            }
        }
        if (m_cmdPool != VK_NULL_HANDLE) {
            VkCommandBuffer buffers[kRingSize];
            for (int i = 0; i < kRingSize; ++i)
                buffers[i] = m_slots[i].cmdBuffer;
            vkFreeCommandBuffers(m_device, m_cmdPool, kRingSize, buffers);
        }
    }
    m_device    = VK_NULL_HANDLE;
    m_cmdPool   = VK_NULL_HANDLE;
    m_slots     = {};
    m_recording = false;
    m_currentSlot = 0;
    m_globalSubmissionIndex = 0;
}

// ── beginRecording ──────────────────────────────────────────────────────────

bool GpuWorkSubmission::beginRecording()
{
    Slot& s = slot();
    if (s.cmdBuffer == VK_NULL_HANDLE)
        return false;

    // Wait for this slot's fence if it's still in-flight from a previous
    // submission.  This is the core of triple-buffering: the CPU may have
    // wrapped around and is reusing a slot whose GPU work hasn't finished.
    if (s.inFlight) {
        if (vkWaitForFences(m_device, 1, &s.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return false;
        s.inFlight = false;
    }

    vkResetCommandBuffer(s.cmdBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(s.cmdBuffer, &beginInfo) != VK_SUCCESS)
        return false;

    m_recording = true;
    return true;
}

// ── endRecording ────────────────────────────────────────────────────────────

bool GpuWorkSubmission::endRecording()
{
    Slot& s = slot();
    if (s.cmdBuffer == VK_NULL_HANDLE || !m_recording)
        return false;
    if (vkEndCommandBuffer(s.cmdBuffer) != VK_SUCCESS)
        return false;
    m_recording = false;
    return true;
}

// ── advanceSlot ─────────────────────────────────────────────────────────────

void GpuWorkSubmission::advanceSlot()
{
    m_currentSlot = (m_currentSlot + 1) % kRingSize;
    ++m_globalSubmissionIndex;
}

// ── submitAndWait ───────────────────────────────────────────────────────────

bool GpuWorkSubmission::submitAndWait(VkQueue queue, std::mutex* queueLock)
{
    if (!submit(queue, queueLock))
        return false;
    return waitForCompletion();
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
    Slot& s = slot();
    if (s.cmdBuffer == VK_NULL_HANDLE || s.fence == VK_NULL_HANDLE)
        return false;

    vkResetFences(m_device, 1, &s.fence);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &s.cmdBuffer;

    VkSemaphore signalSemaphores[1]{};
    if (signalSemaphore != VK_NULL_HANDLE) {
        signalSemaphores[0] = signalSemaphore;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores    = signalSemaphores;
    }

    VkResult result;
    if (queueLock) {
        std::lock_guard lock(*queueLock);
        result = vkQueueSubmit(queue, 1, &submitInfo, s.fence);
    } else {
        result = vkQueueSubmit(queue, 1, &submitInfo, s.fence);
    }

    if (result != VK_SUCCESS)
        return false;

    s.inFlight = true;
    s.submissionIndex = m_globalSubmissionIndex;
    advanceSlot();
    return true;
}

// ── waitForCompletion ───────────────────────────────────────────────────────

bool GpuWorkSubmission::waitForCompletion(uint64_t timeoutNs)
{
    // Wait for the previously submitted slot (the one we just advanced from)
    int prevSlot = (m_currentSlot - 1 + kRingSize) % kRingSize;
    auto& s = m_slots[prevSlot];
    if (s.fence == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE)
        return false;
    if (!s.inFlight)
        return true; // Already completed
    VkResult res = vkWaitForFences(m_device, 1, &s.fence, VK_TRUE, timeoutNs);
    if (res == VK_SUCCESS)
        s.inFlight = false;
    return res == VK_SUCCESS;
}

// ── waitForAll ──────────────────────────────────────────────────────────────

void GpuWorkSubmission::waitForAll()
{
    if (m_device == VK_NULL_HANDLE)
        return;

    // Collect all fences that are in-flight
    VkFence fences[kRingSize];
    int count = 0;
    for (int i = 0; i < kRingSize; ++i) {
        if (m_slots[i].inFlight && m_slots[i].fence != VK_NULL_HANDLE) {
            fences[count++] = m_slots[i].fence;
            m_slots[i].inFlight = false;
        }
    }
    if (count > 0)
        vkWaitForFences(m_device, count, fences, VK_TRUE, UINT64_MAX);
}

// ── isComplete ──────────────────────────────────────────────────────────────

bool GpuWorkSubmission::isComplete() const
{
    // Check the most recently submitted slot
    int prevSlot = (m_currentSlot - 1 + kRingSize) % kRingSize;
    const auto& s = m_slots[prevSlot];
    if (s.fence == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE)
        return true;
    if (!s.inFlight)
        return true;
    return vkGetFenceStatus(m_device, s.fence) == VK_SUCCESS;
}

// ── addBarrier ──────────────────────────────────────────────────────────────

void GpuWorkSubmission::addBarrier(VkPipelineStageFlags srcStage,
                                    VkPipelineStageFlags dstStage,
                                    VkAccessFlags srcAccess,
                                    VkAccessFlags dstAccess)
{
    auto& s = slot();
    if (s.cmdBuffer == VK_NULL_HANDLE || !m_recording)
        return;

    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    vkCmdPipelineBarrier(s.cmdBuffer,
                         srcStage, dstStage,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

} // namespace rt
