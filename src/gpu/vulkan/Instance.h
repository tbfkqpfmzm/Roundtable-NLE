/*
 * Instance — Vulkan instance creation with validation layers.
 *
 * Step 2: Creates VkInstance with optional debug/validation layers.
 * In debug builds, attaches VK_EXT_debug_utils for error reporting.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace rt {

/// Configuration for Vulkan instance creation.
struct InstanceConfig
{
    std::string appName    = "ROUNDTABLE NLE";
    uint32_t    appVersion = VK_MAKE_API_VERSION(0, 2, 0, 0);
#ifdef ROUNDTABLE_DEBUG
    bool        enableValidation = true;
#else
    bool        enableValidation = false;
#endif
    /// Extra instance extensions to request (e.g. surface extensions)
    std::vector<const char*> extraExtensions;
};

/// RAII wrapper for VkInstance + debug messenger.
class Instance
{
public:
    Instance() = default;
    ~Instance();

    // Non-copyable
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) noexcept;
    Instance& operator=(Instance&&) noexcept;

    /// Create the Vulkan instance.
    /// @return true on success.
    bool create(const InstanceConfig& config = {});

    /// Destroy instance and debug messenger.
    void destroy();

    /// Get the raw VkInstance handle.
    [[nodiscard]] VkInstance handle() const noexcept { return m_instance; }

    /// Whether validation layers are active.
    [[nodiscard]] bool validationEnabled() const noexcept { return m_validationEnabled; }

    /// Implicit conversion to VkInstance.
    operator VkInstance() const noexcept { return m_instance; }

private:
    VkInstance               m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    bool                     m_validationEnabled{false};

    bool checkValidationLayerSupport() const;
    void setupDebugMessenger();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
        VkDebugUtilsMessageTypeFlagsEXT             type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void*                                       userData);
};

} // namespace rt
