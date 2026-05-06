/*
 * CompositeGpuSlot.h — Persistent Vulkan resources for the composite path.
 * Single fence + command buffer reused each frame (no per-frame alloc/destroy).
 */
#pragma once
#include <volk.h>

namespace rt {

struct CompositeGpuSlotEntry {
    VkFence         fence       = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffer   = VK_NULL_HANDLE;
};

struct CompositeGpuSlot {
    CompositeGpuSlotEntry slot0;
    VkDevice              device = VK_NULL_HANDLE;

    void destroy() {
        if (slot0.fence != VK_NULL_HANDLE)
            vkDestroyFence(device, slot0.fence, nullptr);
        slot0.fence     = VK_NULL_HANDLE;
        slot0.cmdBuffer = VK_NULL_HANDLE;
    }
};

} // namespace rt
