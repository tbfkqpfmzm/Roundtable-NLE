/*
 * VideoUploader — uploads decoded video frames to Vulkan textures.
 *
 * Two paths:
 *   1. Zero-copy (CUDA interop): NVDEC → shared memory → VkImage
 *   2. CPU staging: decoded frame → staging buffer → VkImage
 *
 * The uploader manages a pool of staging buffers and Vulkan textures
 * for efficient frame upload during playback.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rt {

// Forward declarations  
struct CachedFrame;

/// A Vulkan texture containing an uploaded video frame
struct GpuFrame
{
    void*    vkImage{nullptr};        // VkImage
    void*    vkImageView{nullptr};    // VkImageView
    void*    vkSampler{nullptr};      // VkSampler
    void*    vkDescriptorSet{nullptr};// VkDescriptorSet (if applicable)
    uint32_t width{0};
    uint32_t height{0};
    uint64_t mediaId{0};
    int64_t  frameNumber{0};
    bool     valid{false};
    bool     isZeroCopy{false};       // True if backed by CUDA shared memory
};

/// Configuration for the uploader
struct VideoUploaderConfig
{
    uint32_t maxStagingBuffers{4};     // Number of staging buffers in pool
    uint32_t maxTextureSlots{8};       // Number of VkImage slots for frames
    bool     preferZeroCopy{true};     // Prefer CUDA interop when available
    bool     useYuvShader{true};       // Convert YUV→RGB in fragment shader
};

class VideoUploader
{
public:
    VideoUploader();
    ~VideoUploader();

    // Non-copyable
    VideoUploader(const VideoUploader&) = delete;
    VideoUploader& operator=(const VideoUploader&) = delete;

    /// Initialize with Vulkan device handles.
    /// vkDevice, vkPhysicalDevice, vkQueue, vkCommandPool are raw Vulkan handles.
    bool init(void* vkDevice, void* vkPhysicalDevice,
              void* vkQueue, void* vkCommandPool,
              const VideoUploaderConfig& config = {});

    /// Shut down and free all GPU resources.
    void shutdown();

    /// Upload a CPU-decoded frame to a Vulkan texture.
    /// Returns a GpuFrame with valid VkImage/VkImageView.
    [[nodiscard]] std::shared_ptr<GpuFrame> upload(const CachedFrame& frame);

    /// Check if a frame is already uploaded (texture cache).
    [[nodiscard]] std::shared_ptr<GpuFrame> findUploaded(
        uint64_t mediaId, int64_t frameNumber) const;

    /// Release a specific uploaded frame texture back to the pool.
    void release(std::shared_ptr<GpuFrame> frame);

    /// Release all uploaded frames.
    void releaseAll();

    /// Number of frames currently uploaded.
    [[nodiscard]] size_t uploadedCount() const;

    /// Check if initialized.
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

private:
    bool createStagingBuffers();
    bool createTextureSlots();

    bool     m_initialized{false};
    void*    m_vkDevice{nullptr};
    void*    m_vkPhysicalDevice{nullptr};
    void*    m_vkQueue{nullptr};
    void*    m_vkCommandPool{nullptr};
    VideoUploaderConfig m_config;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace rt
