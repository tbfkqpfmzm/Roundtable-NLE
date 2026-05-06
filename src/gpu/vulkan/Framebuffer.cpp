/*
 * Framebuffer.cpp — Offscreen render target with dynamic rendering.
 * Step 2: Vulkan Initialization
 */

#include <volk.h>
#include "vulkan/Framebuffer.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>

#include <utility>

namespace rt {

// ── Destructor ──────────────────────────────────────────────────────────────

Framebuffer::~Framebuffer()
{
    destroy();
}

// ── Move constructor ────────────────────────────────────────────────────────

Framebuffer::Framebuffer(Framebuffer&& other) noexcept
    : m_config(other.m_config)
    , m_allocator(other.m_allocator)
    , m_device(other.m_device)
    , m_colorImage(other.m_colorImage)
    , m_colorView(other.m_colorView)
    , m_colorAllocation(other.m_colorAllocation)
    , m_colorLayout(other.m_colorLayout)
    , m_depthImage(other.m_depthImage)
    , m_depthView(other.m_depthView)
    , m_depthAllocation(other.m_depthAllocation)
{
    other.m_allocator       = nullptr;
    other.m_device          = VK_NULL_HANDLE;
    other.m_colorImage      = VK_NULL_HANDLE;
    other.m_colorView       = VK_NULL_HANDLE;
    other.m_colorAllocation = nullptr;
    other.m_colorLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    other.m_depthImage      = VK_NULL_HANDLE;
    other.m_depthView       = VK_NULL_HANDLE;
    other.m_depthAllocation = nullptr;
}

// ── Move assignment ─────────────────────────────────────────────────────────

Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_config          = other.m_config;
        m_allocator       = other.m_allocator;
        m_device          = other.m_device;
        m_colorImage      = other.m_colorImage;
        m_colorView       = other.m_colorView;
        m_colorAllocation = other.m_colorAllocation;
        m_colorLayout     = other.m_colorLayout;
        m_depthImage      = other.m_depthImage;
        m_depthView       = other.m_depthView;
        m_depthAllocation = other.m_depthAllocation;

        other.m_allocator       = nullptr;
        other.m_device          = VK_NULL_HANDLE;
        other.m_colorImage      = VK_NULL_HANDLE;
        other.m_colorView       = VK_NULL_HANDLE;
        other.m_colorAllocation = nullptr;
        other.m_colorLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        other.m_depthImage      = VK_NULL_HANDLE;
        other.m_depthView       = VK_NULL_HANDLE;
        other.m_depthAllocation = nullptr;
    }
    return *this;
}

// ── create ──────────────────────────────────────────────────────────────────

bool Framebuffer::create(VmaAllocator allocator, VkDevice device,
                          const FramebufferConfig& config)
{
    m_allocator = allocator;
    m_device    = device;
    m_config    = config;

    if (!createColorAttachment()) return false;

    if (config.hasDepth)
    {
        if (!createDepthAttachment())
        {
            destroy();
            return false;
        }
    }

    spdlog::debug("Created framebuffer {}x{}", config.width, config.height);
    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void Framebuffer::destroy()
{
    if (m_device != VK_NULL_HANDLE)
    {
        if (m_depthView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, m_depthView, nullptr);
            m_depthView = VK_NULL_HANDLE;
        }
        if (m_colorView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, m_colorView, nullptr);
            m_colorView = VK_NULL_HANDLE;
        }
    }

    if (m_allocator != nullptr)
    {
        if (m_depthImage != VK_NULL_HANDLE)
        {
            vmaDestroyImage(m_allocator, m_depthImage, m_depthAllocation);
            m_depthImage      = VK_NULL_HANDLE;
            m_depthAllocation = nullptr;
        }
        if (m_colorImage != VK_NULL_HANDLE)
        {
            vmaDestroyImage(m_allocator, m_colorImage, m_colorAllocation);
            m_colorImage      = VK_NULL_HANDLE;
            m_colorAllocation = nullptr;
            m_colorLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }

    m_device    = VK_NULL_HANDLE;
    m_allocator = nullptr;
}

// ── resize ──────────────────────────────────────────────────────────────────

bool Framebuffer::resize(uint32_t width, uint32_t height)
{
    if (width == m_config.width && height == m_config.height) return true;

    auto allocator = m_allocator;
    auto device    = m_device;
    auto config    = m_config;
    config.width   = width;
    config.height  = height;

    destroy();
    return create(allocator, device, config);
}

// ── createColorAttachment ───────────────────────────────────────────────────

bool Framebuffer::createColorAttachment()
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width  = m_config.width;
    imageInfo.extent.height = m_config.height;
    imageInfo.extent.depth  = 1;
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.format        = m_config.colorFormat;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;

    if (!m_config.concurrentFamilies.empty()) {
        imageInfo.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = static_cast<uint32_t>(m_config.concurrentFamilies.size());
        imageInfo.pQueueFamilyIndices   = m_config.concurrentFamilies.data();
    } else {
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (m_config.sampled)
        imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

    // Also allow as transfer source for readback / compositing
    imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Storage bit for compute shader compositing
    imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkResult result = vmaCreateImage(m_allocator, &imageInfo, &allocInfo,
                                      &m_colorImage, &m_colorAllocation, nullptr);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create framebuffer color image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_colorImage;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = m_config.colorFormat;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    result = vkCreateImageView(m_device, &viewInfo, nullptr, &m_colorView);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create framebuffer color image view");
        return false;
    }

    return true;
}

// ── createDepthAttachment ───────────────────────────────────────────────────

bool Framebuffer::createDepthAttachment()
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width  = m_config.width;
    imageInfo.extent.height = m_config.height;
    imageInfo.extent.depth  = 1;
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.format        = m_config.depthFormat;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkResult result = vmaCreateImage(m_allocator, &imageInfo, &allocInfo,
                                      &m_depthImage, &m_depthAllocation, nullptr);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create framebuffer depth image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_depthImage;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = m_config.depthFormat;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    result = vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthView);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create framebuffer depth image view");
        return false;
    }

    return true;
}

// ── beginRendering (VK 1.3 dynamic rendering) ──────────────────────────────

void Framebuffer::beginRendering(VkCommandBuffer cmd, const VkClearValue* clearColor)
{
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView   = m_colorView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp      = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    if (clearColor)
        colorAttachment.clearValue = *clearColor;

    VkRenderingAttachmentInfo depthAttachment{};
    if (m_config.hasDepth && m_depthView != VK_NULL_HANDLE)
    {
        depthAttachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView   = m_depthView;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};
    }

    VkRenderingInfo renderInfo{};
    renderInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea.offset    = {0, 0};
    renderInfo.renderArea.extent    = {m_config.width, m_config.height};
    renderInfo.layerCount           = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments    = &colorAttachment;
    if (m_config.hasDepth && m_depthView != VK_NULL_HANDLE)
        renderInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);
}

// ── endRendering ────────────────────────────────────────────────────────────

void Framebuffer::endRendering(VkCommandBuffer cmd)
{
    vkCmdEndRendering(cmd);
}

// ── transitionToShaderRead ──────────────────────────────────────────────────

void Framebuffer::transitionToShaderRead(VkCommandBuffer cmd)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = m_colorImage;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;
    barrier.srcAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    m_colorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// ── transitionToColorAttachment ─────────────────────────────────────────────

void Framebuffer::transitionToColorAttachment(VkCommandBuffer cmd)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = m_colorLayout;
    barrier.newLayout                       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = m_colorImage;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    VkPipelineStageFlags srcStage;
    if (m_colorLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcAccessMask = 0;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
                         srcStage,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    m_colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

// ── descriptorInfo ──────────────────────────────────────────────────────────

VkDescriptorImageInfo Framebuffer::descriptorInfo(VkSampler sampler) const
{
    VkDescriptorImageInfo info{};
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView   = m_colorView;
    info.sampler     = sampler;
    return info;
}

} // namespace rt

