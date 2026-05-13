/*
 * RenderResource.h — Resource table entry for the render graph DAG.
 *
 * Resources are declared in a flat table and referenced by ResourceId.
 * The table tracks Vulkan handles, image layout state, access patterns,
 * and lifetime metadata.  Automatic barrier computation uses the current
 * layout/access tracking to emit only the barriers that are strictly
 * necessary between passes.
 *
 * Belongs to rt::render_graph namespace.
 */

#pragma once

#include <cstdint>
#include <string>

#include <volk.h>

namespace rt::render_graph {

// ── Resource type ──────────────────────────────────────────────────────────

enum class ResourceType : uint8_t
{
    Texture,              ///< VkImage with VkImageView
    Buffer,               ///< VkBuffer
    Sampler,              ///< VkSampler (immutable)
    CombinedImageSampler, ///< VkDescriptorImageInfo (sampler + imageView)
    StorageImage,         ///< VkImage with VK_IMAGE_USAGE_STORAGE_BIT
    AccelerationStructure ///< VkAccelerationStructureKHR (future RT)
};

[[nodiscard]] inline const char* toString(ResourceType type) noexcept
{
    switch (type) {
    case ResourceType::Texture:              return "Texture";
    case ResourceType::Buffer:               return "Buffer";
    case ResourceType::Sampler:              return "Sampler";
    case ResourceType::CombinedImageSampler: return "CombinedImageSampler";
    case ResourceType::StorageImage:         return "StorageImage";
    case ResourceType::AccelerationStructure: return "AccelerationStructure";
    }
    return "Unknown";
}

// ── Resource access mode (for barrier computation) ─────────────────────────

enum class ResourceAccess : uint8_t
{
    Undefined,
    TransferWrite,
    TransferRead,
    ShaderWrite,
    ShaderRead,
    ShaderReadWrite,
    Present,
    ColorAttachment,
    DepthStencilAttachment
};

/// Map ResourceAccess → VkAccessFlags
[[nodiscard]] inline VkAccessFlags toVkAccessFlags(ResourceAccess access) noexcept
{
    switch (access) {
    case ResourceAccess::Undefined:             return 0;
    case ResourceAccess::TransferWrite:         return VK_ACCESS_TRANSFER_WRITE_BIT;
    case ResourceAccess::TransferRead:          return VK_ACCESS_TRANSFER_READ_BIT;
    case ResourceAccess::ShaderWrite:           return VK_ACCESS_SHADER_WRITE_BIT;
    case ResourceAccess::ShaderRead:            return VK_ACCESS_SHADER_READ_BIT;
    case ResourceAccess::ShaderReadWrite:       return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    case ResourceAccess::Present:               return VK_ACCESS_MEMORY_READ_BIT;
    case ResourceAccess::ColorAttachment:       return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case ResourceAccess::DepthStencilAttachment: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    return 0;
}

/// Map ResourceAccess → VkPipelineStageFlags
[[nodiscard]] inline VkPipelineStageFlags toVkStageFlags(ResourceAccess access) noexcept
{
    switch (access) {
    case ResourceAccess::Undefined:             return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    case ResourceAccess::TransferWrite:         return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case ResourceAccess::TransferRead:          return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case ResourceAccess::ShaderWrite:           return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case ResourceAccess::ShaderRead:            return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case ResourceAccess::ShaderReadWrite:       return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case ResourceAccess::Present:               return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    case ResourceAccess::ColorAttachment:       return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case ResourceAccess::DepthStencilAttachment: return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
    return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
}

/// Map ResourceAccess → VkImageLayout
[[nodiscard]] inline VkImageLayout toVkImageLayout(ResourceAccess access) noexcept
{
    switch (access) {
    case ResourceAccess::Undefined:              return VK_IMAGE_LAYOUT_UNDEFINED;
    case ResourceAccess::TransferWrite:          return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case ResourceAccess::TransferRead:           return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case ResourceAccess::ShaderWrite:            return VK_IMAGE_LAYOUT_GENERAL;
    case ResourceAccess::ShaderRead:             return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case ResourceAccess::ShaderReadWrite:        return VK_IMAGE_LAYOUT_GENERAL;
    case ResourceAccess::Present:                return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case ResourceAccess::ColorAttachment:        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case ResourceAccess::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}

// ── Resource table entry ───────────────────────────────────────────────────

/// A single resource in the render graph's resource table.
/// Resources are either:
///   - Transient: allocated per-frame, freed when all referencing passes done
///   - Persistent: owned externally (cache, swapchain), graph only borrows them
struct RenderResource
{
    ResourceId              id{kInvalidResource};
    ResourceType            type{ResourceType::Texture};
    std::string             name;

    // ── Vulkan handles (owned or aliased) ─────────────────────────
    VkImage                 image{VK_NULL_HANDLE};
    VkImageView             imageView{VK_NULL_HANDLE};
    VkBuffer                buffer{VK_NULL_HANDLE};
    VkSampler               sampler{VK_NULL_HANDLE};
    VkDescriptorImageInfo   descriptor{};     ///< Pre-built for CombinedImageSampler
    VkDeviceMemory          memory{VK_NULL_HANDLE};

    // ── Image properties ──────────────────────────────────────────
    uint32_t                width{0};
    uint32_t                height{0};
    uint32_t                depth{1};
    VkFormat                format{VK_FORMAT_R8G8B8A8_UNORM};
    VkImageUsageFlags       usageFlags{VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
    VkImageTiling           tiling{VK_IMAGE_TILING_OPTIMAL};

    // ── Buffer properties ─────────────────────────────────────────
    VkDeviceSize            bufferSize{0};
    VkBufferUsageFlags      bufferUsageFlags{VK_BUFFER_USAGE_TRANSFER_DST_BIT};

    // ── State tracking (updated by GpuRenderGraph after each pass) ─
    VkImageLayout           currentLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    ResourceAccess          currentAccess{ResourceAccess::Undefined};
    VkPipelineStageFlags    currentStage{0};

    // ── Lifetime ──────────────────────────────────────────────────
    bool                    transient{false};  ///< Freed after frame completes
    bool                    external{false};   ///< Owned by caller (cache, swapchain)
    uint32_t                refCount{0};       ///< Number of passes referencing this
};

} // namespace rt::render_graph
