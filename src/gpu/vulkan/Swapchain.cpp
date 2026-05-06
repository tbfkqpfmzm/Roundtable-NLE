/*
 * Swapchain.cpp — Vulkan swapchain (presentation engine).
 * Step 2: Vulkan Initialization
 */

#include <volk.h>
#include "vulkan/Swapchain.h"
#include "vulkan/Device.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>

namespace rt {

// ── Destructor / Move ───────────────────────────────────────────────────────

Swapchain::~Swapchain()
{
    // Cannot destroy here without a VkDevice handle. Caller must call destroy().
}

Swapchain::Swapchain(Swapchain&& other) noexcept
    : m_swapchain(other.m_swapchain),
      m_format(other.m_format),
      m_extent(other.m_extent),
      m_surface(other.m_surface),
      m_vsync(other.m_vsync),
      m_needsRecreation(other.m_needsRecreation),
      m_preferredFormat(other.m_preferredFormat),
      m_desiredImageCount(other.m_desiredImageCount),
      m_images(std::move(other.m_images)),
      m_imageViews(std::move(other.m_imageViews))
{
    other.m_swapchain = VK_NULL_HANDLE;
}

Swapchain& Swapchain::operator=(Swapchain&& other) noexcept
{
    if (this != &other)
    {
        m_swapchain         = other.m_swapchain;
        m_format            = other.m_format;
        m_extent            = other.m_extent;
        m_surface           = other.m_surface;
        m_vsync             = other.m_vsync;
        m_needsRecreation   = other.m_needsRecreation;
        m_preferredFormat   = other.m_preferredFormat;
        m_desiredImageCount = other.m_desiredImageCount;
        m_images            = std::move(other.m_images);
        m_imageViews        = std::move(other.m_imageViews);
        other.m_swapchain   = VK_NULL_HANDLE;
    }
    return *this;
}

// ── create ──────────────────────────────────────────────────────────────────

bool Swapchain::create(const Device& device, const SwapchainConfig& config)
{
    m_surface           = config.surface;
    m_vsync             = config.vsync;
    m_preferredFormat   = config.preferredFormat;
    m_desiredImageCount = config.imageCount;

    VkPhysicalDevice physDevice = device.physicalDevice();
    VkDevice         logDevice  = device.logicalDevice();

    // ── Surface capabilities ────────────────────────────────────────────
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDevice, m_surface, &caps);

    auto surfaceFormat = chooseSurfaceFormat(physDevice);
    auto presentMode   = choosePresentMode(physDevice);
    auto extent        = chooseExtent(caps, config.width, config.height);

    m_format = surfaceFormat.format;
    m_extent = extent;

    uint32_t imageCount = std::max(m_desiredImageCount, caps.minImageCount);
    if (caps.maxImageCount > 0)
        imageCount = std::min(imageCount, caps.maxImageCount);

    // ── Create swapchain ────────────────────────────────────────────────
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface          = m_surface;
    createInfo.minImageCount    = imageCount;
    createInfo.imageFormat      = m_format;
    createInfo.imageColorSpace  = surfaceFormat.colorSpace;
    createInfo.imageExtent      = m_extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Queue family sharing
    auto& families = device.queueFamilies();
    uint32_t graphicsFamily = families.graphics.value();
    uint32_t presentFamily  = families.present.value();

    if (graphicsFamily != presentFamily)
    {
        uint32_t queueFamilyIndices[] = {graphicsFamily, presentFamily};
        createInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices   = queueFamilyIndices;
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform   = caps.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode    = presentMode;
    createInfo.clipped        = VK_TRUE;
    createInfo.oldSwapchain   = m_swapchain; // For recreation

    VkResult result = vkCreateSwapchainKHR(logDevice, &createInfo, nullptr, &m_swapchain);

    // Destroy old swapchain if we were recreating
    if (createInfo.oldSwapchain != VK_NULL_HANDLE)
    {
        destroyImageViews(logDevice);
        vkDestroySwapchainKHR(logDevice, createInfo.oldSwapchain, nullptr);
    }

    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create swapchain (VkResult: {})", static_cast<int>(result));
        return false;
    }

    // ── Retrieve swapchain images ───────────────────────────────────────
    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(logDevice, m_swapchain, &actualCount, nullptr);
    m_images.resize(actualCount);
    vkGetSwapchainImagesKHR(logDevice, m_swapchain, &actualCount, m_images.data());

    createImageViews(logDevice);

    m_needsRecreation = false;

    spdlog::info("Swapchain created: {}x{}, {} images, format={}, present={}",
                 m_extent.width, m_extent.height, m_images.size(),
                 static_cast<int>(m_format),
                 m_vsync ? "FIFO (vsync)" : "MAILBOX (immediate)");

    return true;
}

// ── recreate ────────────────────────────────────────────────────────────────

bool Swapchain::recreate(const Device& device, uint32_t width, uint32_t height)
{
    device.waitIdle();

    SwapchainConfig config{};
    config.surface         = m_surface;
    config.width           = width;
    config.height          = height;
    config.vsync           = m_vsync;
    config.preferredFormat = m_preferredFormat;
    config.imageCount      = m_desiredImageCount;

    return create(device, config);
}

// ── destroy ─────────────────────────────────────────────────────────────────

void Swapchain::destroy(VkDevice device)
{
    destroyImageViews(device);

    if (m_swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
        spdlog::info("Swapchain destroyed");
    }
}

// ── acquireNextImage ────────────────────────────────────────────────────────

uint32_t Swapchain::acquireNextImage(VkDevice device, VkSemaphore imageAvailable,
                                      uint64_t timeout)
{
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(device, m_swapchain, timeout,
                                             imageAvailable, VK_NULL_HANDLE,
                                             &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        m_needsRecreation = true;
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            return UINT32_MAX;
    }
    else if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to acquire swapchain image (VkResult: {})",
                      static_cast<int>(result));
        return UINT32_MAX;
    }

    return imageIndex;
}

// ── present ─────────────────────────────────────────────────────────────────

VkResult Swapchain::present(VkQueue presentQueue, uint32_t imageIndex,
                             VkSemaphore renderFinished)
{
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &renderFinished;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &m_swapchain;
    presentInfo.pImageIndices      = &imageIndex;

    VkResult result = vkQueuePresentKHR(presentQueue, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        m_needsRecreation = true;
    }

    return result;
}

// ── chooseSurfaceFormat ─────────────────────────────────────────────────────

VkSurfaceFormatKHR Swapchain::chooseSurfaceFormat(VkPhysicalDevice physDevice) const
{
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDevice, m_surface, &formatCount, nullptr);

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDevice, m_surface, &formatCount, formats.data());

    // Prefer our desired format + sRGB
    for (const auto& f : formats)
    {
        if (f.format == m_preferredFormat &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return f;
        }
    }

    // Fallback: prefer BGRA8 SRGB
    for (const auto& f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return f;
        }
    }

    // Last resort: first available
    return formats[0];
}

// ── choosePresentMode ───────────────────────────────────────────────────────

VkPresentModeKHR Swapchain::choosePresentMode(VkPhysicalDevice physDevice) const
{
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physDevice, m_surface, &modeCount, nullptr);

    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physDevice, m_surface, &modeCount, modes.data());

    if (!m_vsync)
    {
        // Prefer mailbox (triple buffered, no tearing, lowest latency)
        for (auto mode : modes)
        {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                return mode;
        }
        // Fallback: immediate (may tear but lowest latency)
        for (auto mode : modes)
        {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
                return mode;
        }
    }

    // FIFO is always available (vsync)
    return VK_PRESENT_MODE_FIFO_KHR;
}

// ── chooseExtent ────────────────────────────────────────────────────────────

VkExtent2D Swapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& caps,
                                    uint32_t width, uint32_t height) const
{
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
    {
        return caps.currentExtent;
    }

    VkExtent2D extent{width, height};
    extent.width  = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return extent;
}

// ── createImageViews ────────────────────────────────────────────────────────

void Swapchain::createImageViews(VkDevice device)
{
    m_imageViews.resize(m_images.size());

    for (size_t i = 0; i < m_images.size(); ++i)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                           = m_images[i];
        viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                          = m_format;
        viewInfo.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        VkResult result = vkCreateImageView(device, &viewInfo, nullptr, &m_imageViews[i]);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create swapchain image view {} (VkResult: {})",
                          i, static_cast<int>(result));
        }
    }
}

// ── destroyImageViews ───────────────────────────────────────────────────────

void Swapchain::destroyImageViews(VkDevice device)
{
    for (auto& view : m_imageViews)
    {
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    m_imageViews.clear();
}

} // namespace rt

