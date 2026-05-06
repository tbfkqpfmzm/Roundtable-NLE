/*
 * Device — Physical and logical Vulkan device management.
 *
 * Step 2: Selects the best GPU (prefers discrete / RTX 4090), finds queue
 * families (graphics, compute, transfer, present), creates the logical device
 * with required extensions and features.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <optional>
#include <string>
#include <vector>

namespace rt {

class Instance;

/// Queue family indices discovered during device selection.
struct QueueFamilyIndices
{
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> compute;   // Dedicated compute (if available)
    std::optional<uint32_t> transfer;  // Dedicated transfer (if available)
    std::optional<uint32_t> present;   // Presentation

    /// True if all required families found (graphics + present at minimum).
    [[nodiscard]] bool isComplete() const noexcept
    {
        return graphics.has_value() && present.has_value();
    }

    /// Dedicated async compute queue (separate from graphics).
    [[nodiscard]] bool hasAsyncCompute() const noexcept
    {
        return compute.has_value() && compute != graphics;
    }

    /// Dedicated transfer queue (DMA engine).
    [[nodiscard]] bool hasDedicatedTransfer() const noexcept
    {
        return transfer.has_value() && transfer != graphics && transfer != compute;
    }
};

/// GPU capabilities discovered at device selection.
struct GPUInfo
{
    std::string            name;
    uint32_t               vendorId{0};
    uint32_t               deviceId{0};
    VkPhysicalDeviceType   deviceType{VK_PHYSICAL_DEVICE_TYPE_OTHER};
    uint32_t               apiVersion{0};
    uint32_t               driverVersion{0};
    VkDeviceSize           vramSize{0};       // Total device-local memory in bytes
    uint32_t               maxComputeWorkGroupSize[3]{};
    bool                   supportsTimelineSemaphore{false};
    bool                   supportsDynamicRendering{false};
    bool                   supportsDescriptorIndexing{false};
    bool                   supportsSynchronization2{false};
};

/// RAII wrapper for Vulkan physical device + logical device.
class Device
{
public:
    Device() = default;
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;

    /// Select physical device and create logical device.
    /// @param instance   The Vulkan instance.
    /// @param surface    Optional surface for present queue selection.
    /// @return true on success.
    bool create(const Instance& instance, VkSurfaceKHR surface = VK_NULL_HANDLE);

    /// Destroy logical device.
    void destroy();

    // ── Handles ─────────────────────────────────────────────────────────
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return m_physicalDevice; }
    [[nodiscard]] VkDevice         logicalDevice()  const noexcept { return m_device; }
    [[nodiscard]] VkDevice         handle()         const noexcept { return m_device; }

    operator VkDevice() const noexcept { return m_device; }

    // ── Queues ──────────────────────────────────────────────────────────
    [[nodiscard]] VkQueue graphicsQueue() const noexcept { return m_graphicsQueue; }
    [[nodiscard]] VkQueue computeQueue()  const noexcept { return m_computeQueue; }
    [[nodiscard]] VkQueue transferQueue() const noexcept { return m_transferQueue; }
    [[nodiscard]] VkQueue presentQueue()  const noexcept { return m_presentQueue; }

    [[nodiscard]] const QueueFamilyIndices& queueFamilies() const noexcept { return m_queueFamilies; }

    // ── GPU Info ─────────────────────────────────────────────────────────
    [[nodiscard]] const GPUInfo& gpuInfo() const noexcept { return m_gpuInfo; }

    /// Get physical device memory properties (for VMA or manual allocation).
    [[nodiscard]] const VkPhysicalDeviceMemoryProperties& memoryProperties() const noexcept
    {
        return m_memoryProperties;
    }

    /// Wait for the device to be idle (drain all queues).
    void waitIdle() const;

    /// Find a memory type matching requirements.
    [[nodiscard]] uint32_t findMemoryType(uint32_t typeFilter,
                                          VkMemoryPropertyFlags properties) const;

    /// Find a supported format from candidates.
    [[nodiscard]] VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates,
                                               VkImageTiling tiling,
                                               VkFormatFeatureFlags features) const;

private:
    VkPhysicalDevice                   m_physicalDevice{VK_NULL_HANDLE};
    VkDevice                           m_device{VK_NULL_HANDLE};
    QueueFamilyIndices                 m_queueFamilies;
    GPUInfo                            m_gpuInfo;
    VkPhysicalDeviceMemoryProperties   m_memoryProperties{};

    VkQueue m_graphicsQueue{VK_NULL_HANDLE};
    VkQueue m_computeQueue{VK_NULL_HANDLE};
    VkQueue m_transferQueue{VK_NULL_HANDLE};
    VkQueue m_presentQueue{VK_NULL_HANDLE};

    /// Score a physical device for suitability (higher = better).
    int rateDevice(VkPhysicalDevice device, VkSurfaceKHR surface) const;

    /// Find queue family indices for a given physical device.
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device,
                                         VkSurfaceKHR surface) const;

    /// Populate m_gpuInfo from physical device properties.
    void queryGPUInfo(VkPhysicalDevice device);

    /// Required device extensions.
    static std::vector<const char*> requiredExtensions();
};

} // namespace rt
