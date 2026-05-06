/*
 * Swapchain — Vulkan presentation engine.
 *
 * Step 2: Creates and manages the VkSwapchainKHR for displaying rendered
 * frames. Handles resize, format selection, present mode, and image views.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

namespace rt {

class Device;

/// Swapchain configuration.
struct SwapchainConfig
{
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    uint32_t     width{1920};
    uint32_t     height{1080};
    bool         vsync{true};                           // VK_PRESENT_MODE_FIFO vs MAILBOX
    VkFormat     preferredFormat{VK_FORMAT_B8G8R8A8_SRGB};
    uint32_t     imageCount{3};                         // Triple buffering
};

/// RAII wrapper for VkSwapchainKHR.
class Swapchain
{
public:
    Swapchain() = default;
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&&) noexcept;
    Swapchain& operator=(Swapchain&&) noexcept;

    /// Create or recreate the swapchain.
    bool create(const Device& device, const SwapchainConfig& config);

    /// Recreate after window resize.
    bool recreate(const Device& device, uint32_t width, uint32_t height);

    /// Destroy swapchain and image views.
    void destroy(VkDevice device);

    // ── Acquire / Present ───────────────────────────────────────────────

    /// Acquire the next image for rendering.
    /// @return Image index, or UINT32_MAX on error / out of date.
    uint32_t acquireNextImage(VkDevice device, VkSemaphore imageAvailable,
                              uint64_t timeout = UINT64_MAX);

    /// Present rendered image to the screen.
    VkResult present(VkQueue presentQueue, uint32_t imageIndex,
                     VkSemaphore renderFinished);

    // ── Accessors ───────────────────────────────────────────────────────

    [[nodiscard]] VkSwapchainKHR handle()    const noexcept { return m_swapchain; }
    [[nodiscard]] VkFormat       format()    const noexcept { return m_format; }
    [[nodiscard]] VkExtent2D     extent()    const noexcept { return m_extent; }
    [[nodiscard]] uint32_t       imageCount() const noexcept { return static_cast<uint32_t>(m_images.size()); }

    [[nodiscard]] VkImage     image(uint32_t index)     const { return m_images[index]; }
    [[nodiscard]] VkImageView imageView(uint32_t index) const { return m_imageViews[index]; }

    [[nodiscard]] const std::vector<VkImage>&     images()     const noexcept { return m_images; }
    [[nodiscard]] const std::vector<VkImageView>& imageViews() const noexcept { return m_imageViews; }

    [[nodiscard]] bool needsRecreation() const noexcept { return m_needsRecreation; }

    operator VkSwapchainKHR() const noexcept { return m_swapchain; }

private:
    VkSwapchainKHR          m_swapchain{VK_NULL_HANDLE};
    VkFormat                m_format{VK_FORMAT_B8G8R8A8_SRGB};
    VkExtent2D              m_extent{0, 0};
    VkSurfaceKHR            m_surface{VK_NULL_HANDLE};
    bool                    m_vsync{true};
    bool                    m_needsRecreation{false};
    VkFormat                m_preferredFormat{VK_FORMAT_B8G8R8A8_SRGB};
    uint32_t                m_desiredImageCount{3};

    std::vector<VkImage>     m_images;
    std::vector<VkImageView> m_imageViews;

    VkSurfaceFormatKHR chooseSurfaceFormat(VkPhysicalDevice physDevice) const;
    VkPresentModeKHR   choosePresentMode(VkPhysicalDevice physDevice) const;
    VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR& caps,
                                    uint32_t width, uint32_t height) const;

    void createImageViews(VkDevice device);
    void destroyImageViews(VkDevice device);
};

} // namespace rt
