/*
 * Allocator.cpp — VMA (Vulkan Memory Allocator) integration.
 * Step 2: Vulkan Initialization
 *
 * NOTE: VMA is a single-header library. The VMA_IMPLEMENTATION define
 * must appear in exactly ONE .cpp file. We do it here.
 */

#include <volk.h>

// Configure VMA before including
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION           1003000  // Vulkan 1.3

// VMA implementation (header-only library — defined once)
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include "vulkan/Allocator.h"
#include "vulkan/Instance.h"
#include "vulkan/Device.h"

#include <spdlog/spdlog.h>

namespace rt {

// ── Destructor / Move ───────────────────────────────────────────────────────

Allocator::~Allocator()
{
    destroy();
}

Allocator::Allocator(Allocator&& other) noexcept
    : m_allocator(other.m_allocator)
{
    other.m_allocator = nullptr;
}

Allocator& Allocator::operator=(Allocator&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_allocator       = other.m_allocator;
        other.m_allocator = nullptr;
    }
    return *this;
}

// ── create ──────────────────────────────────────────────────────────────────

bool Allocator::create(const Instance& instance, const Device& device)
{
    if (m_allocator != nullptr)
    {
        spdlog::warn("Allocator::create called when already initialized");
        return true;
    }

    // Provide Vulkan function pointers for VMA
    VmaVulkanFunctions vulkanFunctions{};
    vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocInfo{};
    allocInfo.flags            = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
                                 VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocInfo.physicalDevice   = device.physicalDevice();
    allocInfo.device           = device.logicalDevice();
    allocInfo.instance         = instance.handle();
    allocInfo.pVulkanFunctions = &vulkanFunctions;
    allocInfo.vulkanApiVersion = VK_API_VERSION_1_3;

    VkResult result = vmaCreateAllocator(&allocInfo, &m_allocator);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create VMA allocator (VkResult: {})", static_cast<int>(result));
        return false;
    }

    spdlog::info("VMA allocator created (Vulkan 1.3, budget tracking enabled)");
    logMemoryUsage();

    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void Allocator::destroy()
{
    if (m_allocator != nullptr)
    {
        vmaDestroyAllocator(m_allocator);
        m_allocator = nullptr;
        spdlog::info("VMA allocator destroyed");
    }
}

// ── queryStats ──────────────────────────────────────────────────────────────

MemoryStats Allocator::queryStats() const
{
    MemoryStats stats{};
    if (m_allocator == nullptr) return stats;

    VmaTotalStatistics vmaStats{};
    vmaCalculateStatistics(m_allocator, &vmaStats);

    stats.totalAllocatedBytes = vmaStats.total.statistics.blockBytes;
    stats.totalUsedBytes      = vmaStats.total.statistics.allocationBytes;
    stats.allocationCount     = vmaStats.total.statistics.allocationCount;

    // Query memory budget for heap 0 (typically device-local/VRAM)
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(m_allocator, budgets);

    // Heap 0 is usually the device-local (VRAM) heap
    stats.deviceLocalUsedBytes   = budgets[0].statistics.allocationBytes;
    stats.deviceLocalBudgetBytes = budgets[0].budget;

    return stats;
}

// ── logMemoryUsage ──────────────────────────────────────────────────────────

void Allocator::logMemoryUsage() const
{
    auto stats = queryStats();

    spdlog::info("GPU Memory: {:.1f} MB used / {:.1f} MB budget ({} allocations)",
                 static_cast<double>(stats.deviceLocalUsedBytes) / (1024.0 * 1024.0),
                 static_cast<double>(stats.deviceLocalBudgetBytes) / (1024.0 * 1024.0),
                 stats.allocationCount);
}

} // namespace rt

