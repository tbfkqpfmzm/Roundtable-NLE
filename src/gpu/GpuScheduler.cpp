/*
 * GpuScheduler.cpp — Central queue submission authority.
 * See GpuScheduler.h for the architecture rationale.
 */

#include "GpuScheduler.h"

#include <volk.h>
#include <spdlog/spdlog.h>

#include <cassert>

namespace rt {

bool GpuScheduler::init(
    VkDevice    device,
    VkQueue     graphicsQueue, std::mutex* graphicsQueueMutex,
    VkQueue     computeQueue,  std::mutex* computeQueueMutex,
    VkQueue     transferQueue, std::mutex* transferQueueMutex)
{
    if (m_device != VK_NULL_HANDLE) {
        spdlog::warn("GpuScheduler: init called twice — ignoring");
        return true;
    }
    if (device == VK_NULL_HANDLE || graphicsQueue == VK_NULL_HANDLE) {
        spdlog::error("GpuScheduler: init requires at minimum a graphics queue");
        return false;
    }
    m_device = device;

    m_graphics.queue = graphicsQueue;
    m_graphics.mutex = graphicsQueueMutex;

    // Fall through to graphics if compute/transfer aren't separately
    // available.  Submissions still go through this scheduler so the
    // queue mutex is honored — they just land on the graphics queue.
    m_compute.queue  = (computeQueue != VK_NULL_HANDLE) ? computeQueue : graphicsQueue;
    m_compute.mutex  = (computeQueue != VK_NULL_HANDLE) ? computeQueueMutex : graphicsQueueMutex;

    m_transfer.queue = (transferQueue != VK_NULL_HANDLE) ? transferQueue : graphicsQueue;
    m_transfer.mutex = (transferQueue != VK_NULL_HANDLE) ? transferQueueMutex : graphicsQueueMutex;

    spdlog::info("GpuScheduler: initialized (graphics, compute={}, transfer={})",
                 (computeQueue != VK_NULL_HANDLE  && computeQueue  != graphicsQueue) ? "dedicated" : "shared",
                 (transferQueue != VK_NULL_HANDLE && transferQueue != graphicsQueue) ? "dedicated" : "shared");
    return true;
}

void GpuScheduler::shutdown()
{
    if (m_device == VK_NULL_HANDLE) return;
    spdlog::info("GpuScheduler: total submissions = {} (graphics={}, compute={}, transfer={})",
                 m_totalSubmissions.load(std::memory_order_relaxed),
                 m_graphics.submissions.load(std::memory_order_relaxed),
                 m_compute.submissions.load(std::memory_order_relaxed),
                 m_transfer.submissions.load(std::memory_order_relaxed));
    m_device = VK_NULL_HANDLE;
    // QueueSlot holds a std::atomic so it can't be copy-assigned with {};
    // clear fields in place instead.
    auto clearSlot = [](QueueSlot& s) {
        s.queue = VK_NULL_HANDLE;
        s.mutex = nullptr;
        s.submissions.store(0, std::memory_order_relaxed);
    };
    clearSlot(m_graphics);
    clearSlot(m_compute);
    clearSlot(m_transfer);
    m_totalSubmissions.store(0, std::memory_order_relaxed);
}

GpuScheduler::QueueSlot& GpuScheduler::slotFor(GpuQueueKind kind) noexcept
{
    switch (kind) {
        case GpuQueueKind::Graphics: return m_graphics;
        case GpuQueueKind::Compute:  return m_compute;
        case GpuQueueKind::Transfer: return m_transfer;
    }
    return m_graphics;  // unreachable; quiets compiler.
}

const GpuScheduler::QueueSlot& GpuScheduler::slotFor(GpuQueueKind kind) const noexcept
{
    switch (kind) {
        case GpuQueueKind::Graphics: return m_graphics;
        case GpuQueueKind::Compute:  return m_compute;
        case GpuQueueKind::Transfer: return m_transfer;
    }
    return m_graphics;
}

uint64_t GpuScheduler::submissionsOn(GpuQueueKind kind) const noexcept
{
    return slotFor(kind).submissions.load(std::memory_order_relaxed);
}

VkResult GpuScheduler::submit(const GpuSubmission& sub)
{
    if (m_device == VK_NULL_HANDLE) {
        spdlog::error("GpuScheduler: submit() called before init()");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (sub.cmd == VK_NULL_HANDLE) {
        spdlog::error("GpuScheduler: submit() with VK_NULL_HANDLE command buffer (tag={})",
                      sub.tag ? sub.tag : "<none>");
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    if (sub.waitSemaphoreCount > 0 && (!sub.waitSemaphores || !sub.waitStages)) {
        spdlog::error("GpuScheduler: submit() with waitSemaphoreCount={} but null arrays (tag={})",
                      sub.waitSemaphoreCount, sub.tag ? sub.tag : "<none>");
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    if (sub.signalSemaphoreCount > 0 && !sub.signalSemaphores) {
        spdlog::error("GpuScheduler: submit() with signalSemaphoreCount={} but null array (tag={})",
                      sub.signalSemaphoreCount, sub.tag ? sub.tag : "<none>");
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }

    QueueSlot& slot = slotFor(sub.queue);
    if (slot.queue == VK_NULL_HANDLE) {
        spdlog::error("GpuScheduler: no queue available for kind={} (tag={})",
                      static_cast<int>(sub.queue), sub.tag ? sub.tag : "<none>");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = sub.pNext;
    submitInfo.waitSemaphoreCount   = sub.waitSemaphoreCount;
    submitInfo.pWaitSemaphores      = sub.waitSemaphores;
    submitInfo.pWaitDstStageMask    = sub.waitStages;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &sub.cmd;
    submitInfo.signalSemaphoreCount = sub.signalSemaphoreCount;
    submitInfo.pSignalSemaphores    = sub.signalSemaphores;

    VkResult result;
    if (slot.mutex) {
        std::lock_guard lock(*slot.mutex);
        result = vkQueueSubmit(slot.queue, 1, &submitInfo, sub.completionFence);
    } else {
        result = vkQueueSubmit(slot.queue, 1, &submitInfo, sub.completionFence);
    }

    slot.submissions.fetch_add(1, std::memory_order_relaxed);
    m_totalSubmissions.fetch_add(1, std::memory_order_relaxed);

    if (result != VK_SUCCESS) {
        // -4 = VK_ERROR_DEVICE_LOST.  Surface the code so the rest of
        // the stack (CompositeEngine recovery, etc.) can react.
        spdlog::error("[GpuScheduler] vkQueueSubmit failed: VkResult={} kind={} tag={}",
                      static_cast<int>(result),
                      static_cast<int>(sub.queue),
                      sub.tag ? sub.tag : "<none>");
    }
    return result;
}

} // namespace rt
