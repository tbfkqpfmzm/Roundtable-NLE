/*
 * Nv12Converter — GPU compute-shader NV12 → BGRA color conversion.
 *
 * Replaces CPU-side sws_scale for the most common NVDEC output format.
 * Uses BT.709 color matrix (HDTV standard, correct for most H.264/H.265).
 *
 * Usage:
 *   1. Call init() once with the Vulkan context.
 *   2. Call convert() or convertSync() per frame.
 *   3. Read the BGRA result from outputDescriptorInfo() (for GPU consumers)
 *      or readbackOutput() (for CPU consumers).
 *
 * The converter owns two persistent textures (Y plane R8, UV plane RG8)
 * and one BGRA output storage image.  Dimensions are auto-resized.
 */

#pragma once

#include "vulkan/Device.h"
#include "vulkan/Allocator.h"
#include "vulkan/CommandPool.h"
#include "vulkan/Texture.h"
#include "vulkan/Buffer.h"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace rt {

struct Nv12ConverterConfig
{
    uint32_t width{1920};
    uint32_t height{1080};
};

class Nv12Converter
{
public:
    Nv12Converter();
    ~Nv12Converter();

    Nv12Converter(const Nv12Converter&) = delete;
    Nv12Converter& operator=(const Nv12Converter&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    bool init(Device& device, Allocator& allocator, CommandPool& cmdPool,
              VkQueue computeQueue, const Nv12ConverterConfig& config = {});
    void shutdown();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // ── Conversion ──────────────────────────────────────────────────────

    /// Upload NV12 planes and record conversion dispatch into an external
    /// command buffer.  After submit + wait:
    ///   - outputDescriptorInfo() samples the BGRA result
    ///   - readbackOutput() copies BGRA pixels to CPU
    ///
    /// @param cmd        External command buffer (must be in recording state).
    /// @param yData      Y plane pixels (W × H bytes, one byte per pixel).
    /// @param yLinesize  Row stride of Y plane in bytes.
    /// @param uvData     UV plane pixels (W/2 × H/2 × 2 bytes, interleaved CB/CR).
    /// @param uvLinesize Row stride of UV plane in bytes.
    /// @param width      Frame width in pixels.
    /// @param height     Frame height in pixels.
    /// @param stagingOut Staging buffers that must be freed AFTER cmd completes.
    /// @return true on success.
    bool convert(VkCommandBuffer cmd,
                 const uint8_t* yData, int yLinesize,
                 const uint8_t* uvData, int uvLinesize,
                 uint32_t width, uint32_t height,
                 std::vector<Texture::StagingCleanup>& stagingOut);

    /// Synchronous version — allocates its own command buffer, submits, waits.
    bool convertSync(const uint8_t* yData, int yLinesize,
                     const uint8_t* uvData, int uvLinesize,
                     uint32_t width, uint32_t height);

    /// Synchronous NV12→BGRA with integrated downscale.
    /// Input planes are srcW × srcH; output BGRA is dstW × dstH.
    /// The compute shader samples input textures with bilinear filtering
    /// at normalized coordinates, producing a downscaled BGRA result.
    bool convertSyncScaled(const uint8_t* yData, int yLinesize,
                           const uint8_t* uvData, int uvLinesize,
                           uint32_t srcW, uint32_t srcH,
                           uint32_t dstW, uint32_t dstH);

    // ── YUV420P (3-plane) conversion ─────────────────────────────────

    /// Synchronous YUV420P→BGRA conversion.  3 separate planes.
    bool convertYuv420pSync(const uint8_t* yData, int yLinesize,
                            const uint8_t* uData, int uLinesize,
                            const uint8_t* vData, int vLinesize,
                            uint32_t width, uint32_t height);

    /// Synchronous YUV420P→BGRA with integrated downscale.
    bool convertYuv420pSyncScaled(const uint8_t* yData, int yLinesize,
                                  const uint8_t* uData, int uLinesize,
                                  const uint8_t* vData, int vLinesize,
                                  uint32_t srcW, uint32_t srcH,
                                  uint32_t dstW, uint32_t dstH);

    /// Convert NV12 data from a Vulkan buffer (GPU→GPU, no CPU staging).
    /// Used for CUDA-Vulkan interop: NVDEC writes NV12 to a shared VkBuffer,
    /// then this method copies to Y/UV textures and dispatches the compute
    /// shader — all in GPU memory, zero PCIe transfers.
    ///
    /// @param cmd          External command buffer (recording state).
    /// @param nv12Buffer   VkBuffer containing NV12 data.
    /// @param width        Frame width in pixels.
    /// @param height       Frame height in pixels.
    /// @param yOffset      Byte offset of Y plane in the buffer.
    /// @param yRowPitch    Row pitch of Y plane in bytes (0 = tightly packed = width).
    /// @param uvOffset     Byte offset of UV plane in the buffer.
    /// @param uvRowPitch   Row pitch of UV plane in bytes (0 = tightly packed = width).
    /// @return true on success.
    bool convertFromVkBuffer(VkCommandBuffer cmd,
                             VkBuffer nv12Buffer,
                             uint32_t width, uint32_t height,
                             VkDeviceSize yOffset  = 0, uint32_t yRowPitch  = 0,
                             VkDeviceSize uvOffset = 0, uint32_t uvRowPitch = 0);

    // ── Resize ──────────────────────────────────────────────────────────

    bool resize(uint32_t width, uint32_t height);

    // ── Output access ───────────────────────────────────────────────────

    [[nodiscard]] uint32_t outputWidth()  const noexcept { return m_config.width; }
    [[nodiscard]] uint32_t outputHeight() const noexcept { return m_config.height; }
    [[nodiscard]] VkDescriptorImageInfo outputDescriptorInfo() const;

    /// Raw output texture — needed for GPU→GPU image copy when producing
    /// GPU-resident CachedFrames (avoids CPU readback + re-upload).
    [[nodiscard]] const Texture& outputTexture() const noexcept { return m_outputTexture; }

    /// Read back BGRA pixels to CPU.  Synchronous (allocates cmd + waits).
    bool readbackOutput(std::vector<uint8_t>& outPixels);

private:
    bool createTextures();
    bool createDescriptorResources();
    bool createPipeline();
    bool createYuv420pPipeline();
    bool ensureOutputSize(uint32_t w, uint32_t h);

    Device*      m_device{nullptr};
    Allocator*   m_allocator{nullptr};
    CommandPool* m_cmdPool{nullptr};
    VkQueue      m_queue{VK_NULL_HANDLE};

    Nv12ConverterConfig m_config;
    bool m_initialized{false};

    // Input planes (uploaded per frame)
    Texture m_yTexture;   // R8 UNORM, W × H
    Texture m_uvTexture;  // RG8 UNORM, W/2 × H/2 (NV12)
    Texture m_uTexture;   // R8 UNORM,  W/2 × H/2 (YUV420P Cb)
    Texture m_vTexture;   // R8 UNORM,  W/2 × H/2 (YUV420P Cr)

    // Output BGRA image (shared by NV12 and YUV420P)
    Texture m_outputTexture; // RGBA8 (BGRA swizzle in shader), W × H

    // NV12 descriptors
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};

    // NV12 pipeline
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline       m_pipeline{VK_NULL_HANDLE};
    VkShaderModule   m_shaderModule{VK_NULL_HANDLE};

    // YUV420P descriptors + pipeline
    VkDescriptorSetLayout m_yuv420pDescSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_yuv420pDescPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_yuv420pDescSet{VK_NULL_HANDLE};
    VkPipelineLayout      m_yuv420pPipeLayout{VK_NULL_HANDLE};
    VkPipeline            m_yuv420pPipeline{VK_NULL_HANDLE};
    VkShaderModule        m_yuv420pShaderModule{VK_NULL_HANDLE};

};

} // namespace rt
