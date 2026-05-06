/*
 * EffectProcessor — GPU compute-shader effects pipeline.
 *
 * Step 22: Processes effects on clip images using Vulkan compute shaders.
 *
 * Each effect type maps to a compute shader:
 *   - color_correct.comp: Brightness, contrast, saturation, etc.
 *   - blur.comp: Separable Gaussian blur
 *   (Additional shaders dispatched for sharpen, glow, chroma key, transform2D)
 *
 * Usage:
 *   1. Call init() with Vulkan resources
 *   2. Call process() with source image + EffectStack snapshot
 *   3. Result is in outputImage()
 *
 * The processor applies effects in stack order, ping-ponging between
 * two internal storage images (avoiding extra allocations).
 */

#pragma once

#include "vulkan/Allocator.h"
#include "vulkan/Buffer.h"
#include "vulkan/CommandPool.h"
#include "vulkan/Device.h"
#include "vulkan/Pipeline.h"
#include "vulkan/Texture.h"

#include "effects/Effect.h"
#include "effects/EffectStack.h"

#include <cstdint>
#include <vector>

namespace rt {

// ── Configuration ───────────────────────────────────────────────────────────

struct EffectProcessorConfig
{
    uint32_t width{1920};
    uint32_t height{1080};
    VkFormat format{VK_FORMAT_R8G8B8A8_UNORM};
};

// ── Push constants for effect shaders ───────────────────────────────────────

struct EffectPushConstants
{
    int32_t width;
    int32_t height;
    int32_t paramCount;
    int32_t _pad;
    float   params[28];  // Up to 28 floats per effect (128 bytes total)
};
static_assert(sizeof(EffectPushConstants) == 128);

// ── Statistics ──────────────────────────────────────────────────────────────

struct EffectProcessorStats
{
    int   effectsApplied{0};
    float gpuTimeMs{0.0f};
};

// ═════════════════════════════════════════════════════════════════════════════

class EffectProcessor
{
public:
    EffectProcessor();
    ~EffectProcessor();

    EffectProcessor(const EffectProcessor&) = delete;
    EffectProcessor& operator=(const EffectProcessor&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    bool init(Device& device,
              Allocator& allocator,
              CommandPool& cmdPool,
              VkQueue computeQueue,
              const EffectProcessorConfig& config = {});

    void shutdown();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // ── Processing ──────────────────────────────────────────────────────

    /// Apply effects from a snapshot (evaluated EffectStack) to a source image.
    /// Records commands into the provided command buffer.
    bool process(VkCommandBuffer cmd,
                 const VkDescriptorImageInfo& sourceImage,
                 const std::vector<EffectStack::EffectSnapshot>& effects);

    /// Synchronous version — creates its own command buffer.
    bool processSync(const VkDescriptorImageInfo& sourceImage,
                     const std::vector<EffectStack::EffectSnapshot>& effects);

    // ── Resize ──────────────────────────────────────────────────────────

    bool resize(uint32_t width, uint32_t height);

    // ── Output access ───────────────────────────────────────────────────

    [[nodiscard]] VkImage       outputImage()     const noexcept;
    [[nodiscard]] VkImageView   outputImageView() const noexcept;
    [[nodiscard]] uint32_t      outputWidth()     const noexcept { return m_config.width; }
    [[nodiscard]] uint32_t      outputHeight()    const noexcept { return m_config.height; }

    [[nodiscard]] VkDescriptorImageInfo outputDescriptorInfo() const;

    /// Read back output pixels (for testing).
    bool readbackOutput(std::vector<uint8_t>& outPixels);

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] const EffectProcessorStats& stats() const noexcept { return m_stats; }

private:
    bool createStorageTextures();
    bool createPipelines();
    bool createDescriptorResources();

    /// Dispatch a single effect — writes to pingPong[targetIdx].
    bool dispatchEffect(VkCommandBuffer cmd,
                        EffectType type,
                        const std::vector<float>& params,
                        int sourceIdx, int targetIdx);

    /// Ultra Key multi-pass dispatch (matte → cleanup → finalize).
    /// Returns the final targetIdx written to.
    int dispatchUltraKey(VkCommandBuffer cmd,
                         const std::vector<float>& params,
                         int sourceIdx, int targetIdx);

    /// Helper: dispatch a single compute pass with given pipeline.
    void dispatchPass(VkCommandBuffer cmd, VkPipeline pipeline,
                      VkDescriptorSet ds,
                      const std::vector<float>& params);

    VkPipeline getPipeline(EffectType type) const;

    // ── Vulkan handles ──────────────────────────────────────────────────

    Device*       m_device{nullptr};
    Allocator*    m_allocator{nullptr};
    CommandPool*  m_cmdPool{nullptr};
    VkQueue       m_queue{VK_NULL_HANDLE};

    EffectProcessorConfig m_config;
    bool                  m_initialized{false};

    // Ping-pong storage images for multi-effect chains
    Texture m_storageTextures[2];
    int     m_currentOutput{0}; // which storage texture holds the final result

    // Pipelines (one per effect type)
    PipelineManager  m_pipelineManager;
    VkPipeline       m_colorCorrectPipeline{VK_NULL_HANDLE};
    VkPipeline       m_blurPipeline{VK_NULL_HANDLE};
    VkPipeline       m_sharpenPipeline{VK_NULL_HANDLE};
    VkPipeline       m_glowPipeline{VK_NULL_HANDLE};
    VkPipeline       m_chromaKeyPipeline{VK_NULL_HANDLE};       // legacy (unused)
    VkPipeline       m_ultraKeyMattePipeline{VK_NULL_HANDLE};   // Pass 1: matte generation
    VkPipeline       m_ultraKeyCleanupPipeline{VK_NULL_HANDLE}; // Pass 2: matte cleanup
    VkPipeline       m_ultraKeyFinalizePipeline{VK_NULL_HANDLE};// Pass 3: spill + CC + output
    VkPipeline       m_transform2dPipeline{VK_NULL_HANDLE};
    VkPipeline       m_vignettePipeline{VK_NULL_HANDLE};
    VkPipeline       m_lutPipeline{VK_NULL_HANDLE};
    VkPipeline       m_letterboxPipeline{VK_NULL_HANDLE};
    VkPipeline       m_lumetriColorPipeline{VK_NULL_HANDLE};
    VkPipeline       m_otsPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSets[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorSet       m_sourceDescriptorSet{VK_NULL_HANDLE};

    // Placeholder texture
    Texture m_placeholderTexture;

    // Reusable fence for processSync (avoids create/destroy per call)
    VkFence m_syncFence{VK_NULL_HANDLE};

    // Cached source descriptor to skip redundant vkUpdateDescriptorSets
    VkImageView m_lastSourceImageView{VK_NULL_HANDLE};

    // GPU timing
    VkQueryPool m_queryPool{VK_NULL_HANDLE};
    float       m_timestampPeriod{0.0f};

    // Stats
    EffectProcessorStats m_stats;
};

} // namespace rt
