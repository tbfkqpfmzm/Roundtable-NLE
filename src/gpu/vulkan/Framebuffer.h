/*
 * Framebuffer — Offscreen render target management.
 *
 * Step 2: Manages offscreen framebuffers for compositing.
 * Uses dynamic rendering (VK 1.3) — no VkRenderPass needed.
 * Each framebuffer owns its color and optional depth images.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

// Forward declare VMA types
struct VmaAllocator_T;
struct VmaAllocation_T;
using VmaAllocator  = VmaAllocator_T*;
using VmaAllocation = VmaAllocation_T*;

namespace rt {

/// Offscreen render target configuration.
struct FramebufferConfig
{
    uint32_t  width{1920};
    uint32_t  height{1080};
    VkFormat  colorFormat{VK_FORMAT_R8G8B8A8_UNORM};
    bool      hasDepth{false};
    VkFormat  depthFormat{VK_FORMAT_D32_SFLOAT};
    bool      sampled{true};   // true = can be used as shader input after rendering

    /// Queue family indices for CONCURRENT sharing (cross-queue sampling).
    /// When non-empty, the color image uses VK_SHARING_MODE_CONCURRENT.
    std::vector<uint32_t> concurrentFamilies;
};

/// RAII wrapper for an offscreen render target.
class Framebuffer
{
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    Framebuffer(Framebuffer&&) noexcept;
    Framebuffer& operator=(Framebuffer&&) noexcept;

    /// Create the offscreen render target.
    bool create(VmaAllocator allocator, VkDevice device, const FramebufferConfig& config);

    /// Destroy all resources.
    void destroy();

    /// Resize (destroys and recreates).
    bool resize(uint32_t width, uint32_t height);

    // ── Dynamic rendering helpers ───────────────────────────────────────

    /// Begin dynamic rendering to this framebuffer.
    /// Call between vkBeginCommandBuffer and vkEndCommandBuffer.
    void beginRendering(VkCommandBuffer cmd, const VkClearValue* clearColor = nullptr);

    /// End dynamic rendering.
    void endRendering(VkCommandBuffer cmd);

    /// Transition color image for shader reading after rendering.
    void transitionToShaderRead(VkCommandBuffer cmd);

    /// Transition color image back to color attachment for rendering.
    void transitionToColorAttachment(VkCommandBuffer cmd);

    // ── Accessors ───────────────────────────────────────────────────────

    [[nodiscard]] VkImage       colorImage()     const noexcept { return m_colorImage; }
    [[nodiscard]] VkImageView   colorImageView() const noexcept { return m_colorView; }
    [[nodiscard]] VkImage       depthImage()     const noexcept { return m_depthImage; }
    [[nodiscard]] VkImageView   depthImageView() const noexcept { return m_depthView; }
    [[nodiscard]] uint32_t      width()          const noexcept { return m_config.width; }
    [[nodiscard]] uint32_t      height()         const noexcept { return m_config.height; }
    [[nodiscard]] VkFormat      colorFormat()    const noexcept { return m_config.colorFormat; }
    [[nodiscard]] VkExtent2D    extent()         const noexcept { return {m_config.width, m_config.height}; }

    /// Descriptor image info for sampling the color output.
    [[nodiscard]] VkDescriptorImageInfo descriptorInfo(VkSampler sampler) const;

private:
    FramebufferConfig m_config;
    VmaAllocator      m_allocator{nullptr};
    VkDevice          m_device{VK_NULL_HANDLE};

    VkImage           m_colorImage{VK_NULL_HANDLE};
    VkImageView       m_colorView{VK_NULL_HANDLE};
    VmaAllocation     m_colorAllocation{nullptr};
    VkImageLayout     m_colorLayout{VK_IMAGE_LAYOUT_UNDEFINED};

    VkImage           m_depthImage{VK_NULL_HANDLE};
    VkImageView       m_depthView{VK_NULL_HANDLE};
    VmaAllocation     m_depthAllocation{nullptr};

    bool createColorAttachment();
    bool createDepthAttachment();
};

} // namespace rt
