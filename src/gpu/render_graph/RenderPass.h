/*
 * RenderPass.h — GPU render pass node definition for the render graph DAG.
 *
 * Each node in the DAG is a render pass with explicit inputs, outputs,
 * pipeline state, and fault-isolation metadata.  Passes are executed in
 * topological order by GpuRenderGraph.
 *
 * Belongs to rt::render_graph namespace.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

namespace rt::render_graph {

// ── Pass type enumeration ──────────────────────────────────────────────────

enum class PassType : uint8_t
{
    External     = 0,  ///< Input resource from outside the graph (decoded frame, etc.)
    Upload       = 1,  ///< CPU buffer → GPU texture (vkCmdCopyBufferToImage)
    Effect       = 2,  ///< Texture → compute shader → texture
    Transition   = 3,  ///< Two textures → blended texture
    Composite    = 4,  ///< N textures → final output (via Compositor)
    Present      = 5,  ///< Final texture → swapchain image (signal semaphore)
    Readback     = 6,  ///< GPU texture → CPU staging buffer (vkCmdCopyImageToBuffer)
    Custom       = 7   ///< User-defined (future extension)
};

[[nodiscard]] inline const char* toString(PassType type) noexcept
{
    switch (type) {
    case PassType::External:   return "External";
    case PassType::Upload:     return "Upload";
    case PassType::Effect:     return "Effect";
    case PassType::Transition: return "Transition";
    case PassType::Composite:  return "Composite";
    case PassType::Present:    return "Present";
    case PassType::Readback:   return "Readback";
    case PassType::Custom:     return "Custom";
    }
    return "Unknown";
}

// ── Resource ID (index into GpuRenderGraph's resource table) ───────────────

using ResourceId = uint32_t;
inline constexpr ResourceId kInvalidResource = UINT32_MAX;

// ── Image barrier descriptor (computed automatically) ──────────────────────

struct ImageBarrier
{
    ResourceId          resourceId{kInvalidResource};
    VkImageLayout       oldLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageLayout       newLayout{VK_IMAGE_LAYOUT_GENERAL};
    VkAccessFlags       srcAccessMask{0};
    VkAccessFlags       dstAccessMask{0};
    VkPipelineStageFlags srcStageMask{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkPipelineStageFlags dstStageMask{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
};

// ── Render pass node ───────────────────────────────────────────────────────

/// A single node in the render graph DAG.  Declares what resources it
/// consumes, what it produces, and how to execute it.
struct RenderPass
{
    std::string                 name;           ///< Debug label
    PassType                    type{PassType::External};
    uint32_t                    passIndex{0};   ///< Position in graph (set during compile)

    // ── Resource bindings ──────────────────────────────────────────
    std::vector<ResourceId>     inputs;          ///< Resources consumed (read)
    std::vector<ResourceId>     outputs;         ///< Resources produced (written)

    // ── Automatic barriers (computed by GpuRenderGraph::compile) ───
    std::vector<ImageBarrier>   preBarriers;     ///< Barriers to issue before pass
    std::vector<ImageBarrier>   postBarriers;    ///< Barriers to issue after pass

    // ── Pipeline state ─────────────────────────────────────────────
    VkPipelineBindPoint         bindPoint{VK_PIPELINE_BIND_POINT_COMPUTE};
    VkPipeline                  pipeline{VK_NULL_HANDLE};
    VkPipelineLayout            pipelineLayout{VK_NULL_HANDLE};
    VkShaderModule              shaderModule{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> descriptorSets;

    // ── Push constants (for compute shader parameters) ─────────────
    std::vector<uint8_t>        pushConstants;

    // ── Dispatch dimensions (compute only) ─────────────────────────
    uint32_t                    groupCountX{1};
    uint32_t                    groupCountY{1};
    uint32_t                    groupCountZ{1};

    // ── Graphics-specific state (future use — Spine, text, waveform) ─
    VkRenderPass                graphicsRenderPass{VK_NULL_HANDLE};
    VkFramebuffer               framebuffer{VK_NULL_HANDLE};
    VkExtent2D                  renderArea{};
    std::vector<VkClearValue>   clearValues;
    uint32_t                    subpassIndex{0};

    // ── Copy-specific state (Upload / Readback) ────────────────────
    VkBuffer                    copyBuffer{VK_NULL_HANDLE};
    VkDeviceSize                copyBufferOffset{0};
    std::vector<VkBufferImageCopy> copyRegions;

    // ── Fault isolation ────────────────────────────────────────────
    bool                        optional{false}; ///< true = skip on failure, continue
    bool                        fatal{true};     ///< true = frame fails if this pass fails

    // ── GPU timestamp query ────────────────────────────────────────
    uint32_t                    timestampQueryIndex{UINT32_MAX};
};

} // namespace rt::render_graph
