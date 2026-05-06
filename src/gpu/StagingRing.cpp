/*
 * StagingRing.cpp — Persistent staging buffer with linear sub-allocation.
 */

#include "StagingRing.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>

namespace rt {

bool StagingRing::init(VmaAllocator allocator, VkDeviceSize capacity)
{
    if (m_buffer != VK_NULL_HANDLE)
        destroy();

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size  = capacity;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo resultInfo{};
    VkResult result = vmaCreateBuffer(allocator, &bufInfo, &allocInfo,
                                       &m_buffer, &m_allocation, &resultInfo);
    if (result != VK_SUCCESS) {
        spdlog::error("StagingRing::init: vmaCreateBuffer failed ({})", static_cast<int>(result));
        return false;
    }

    m_allocator = allocator;
    m_mapped    = resultInfo.pMappedData;
    m_capacity  = capacity;
    m_offset    = 0;
    return true;
}

void StagingRing::destroy()
{
    if (m_buffer != VK_NULL_HANDLE && m_allocator) {
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
    }
    m_buffer     = VK_NULL_HANDLE;
    m_allocation = nullptr;
    m_allocator  = nullptr;
    m_mapped     = nullptr;
    m_capacity   = 0;
    m_offset     = 0;
}

StagingRing::Alloc StagingRing::alloc(const void* data, VkDeviceSize size)
{
    // Align to 256 bytes (safe for any Vulkan buffer-image copy alignment)
    constexpr VkDeviceSize ALIGN = 256;
    VkDeviceSize aligned = (m_offset + ALIGN - 1) & ~(ALIGN - 1);

    if (aligned + size > m_capacity)
        return {}; // not enough room — caller should fall back

    std::memcpy(static_cast<char*>(m_mapped) + aligned, data, static_cast<size_t>(size));

    Alloc a;
    a.buffer = m_buffer;
    a.offset = aligned;
    a.valid  = true;

    m_offset = aligned + size;
    return a;
}

} // namespace rt
