/*
 * ICompositor — Abstract compositor interface.
 *
 * Decouples CompositeService from the concrete Vulkan Compositor
 * implementation.  Allows swapping backends and enables unit testing
 * with a mock compositor.
 */

#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace rt {

struct ABPair;
struct CompositorLayer;
struct CompositorStats;

/// Abstract interface for multi-layer GPU compositing.
class ICompositor
{
public:
    virtual ~ICompositor() = default;

    // ── Lifecycle ───────────────────────────────────────────────────

    /// Shut down and release all GPU resources.
    virtual void shutdown() = 0;

    [[nodiscard]] virtual bool isInitialized() const noexcept = 0;

    // ── Layer management ────────────────────────────────────────────

    /// Set layers for the next composite dispatch (max 32).
    virtual void setLayers(const std::vector<CompositorLayer>& layers) = 0;

    /// Set A/B track pairs for the next composite dispatch.
    virtual void setPairs(const std::vector<ABPair>& pairs) = 0;

    /// Clear all layers.
    virtual void clearLayers() = 0;

    /// Get current layer count.
    [[nodiscard]] virtual uint32_t layerCount() const noexcept = 0;

    // ── Compositing ────────────────────────────────────────────────

    /// Dispatch composite compute shader.
    virtual bool composite(VkCommandBuffer cmd) = 0;

    /// Composite using an internal command buffer (synchronous).
    virtual bool compositeSync() = 0;

    // ── Resize ─────────────────────────────────────────────────────

    virtual bool resize(uint32_t width, uint32_t height) = 0;

    // ── Output access ──────────────────────────────────────────────

    [[nodiscard]] virtual VkDescriptorImageInfo outputDescriptorInfo() const = 0;

    /// The output image view (for GPU-resident display).
    [[nodiscard]] virtual VkImageView outputImageView() const noexcept = 0;

    /// The output sampler (for GPU-resident display).
    [[nodiscard]] virtual VkSampler outputSampler() const noexcept = 0;

    /// Read back output pixels (for export). Synchronous.
    virtual bool readbackOutput(std::vector<uint8_t>& outPixels) = 0;

    /// Record readback into an external command buffer.
    virtual bool recordReadback(VkCommandBuffer cmd) = 0;

    /// Map the persistent readback staging buffer and copy pixels out.
    virtual bool mapAndCopyReadback(std::vector<uint8_t>& outPixels) = 0;

    // ── Statistics ─────────────────────────────────────────────────

    [[nodiscard]] virtual const CompositorStats& stats() const noexcept = 0;
};

} // namespace rt
