/*
 * NvdecDecoder — Direct NVDEC hardware video decoder (optional).
 *
 * Uses NVIDIA CUVID API for zero-copy video decode on the GPU.
 * Decoded frames stay in GPU memory and can be shared with Vulkan
 * via CudaVulkanInterop for true zero-copy rendering.
 *
 * Requires ROUNDTABLE_HAS_CUDA. When not available, the primary
 * decode path (VideoDecoder via FFmpeg hwaccel) is the fallback.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rt {

// Forward
class CudaContext;

/// Info about decoded NVDEC output
struct NvdecFrameInfo
{
    void*    devicePtr{nullptr};  // CUdeviceptr — GPU memory pointer
    uint32_t width{0};
    uint32_t height{0};
    uint32_t pitch{0};           // Row stride in bytes on GPU
    int64_t  pts{0};             // Presentation timestamp
    int64_t  frameIndex{0};
    bool     isKeyframe{false};
};

/// Codec supported by NVDEC
enum class NvdecCodec : uint8_t
{
    H264,
    HEVC,
    VP9,
    AV1,
};

class NvdecDecoder
{
public:
    explicit NvdecDecoder(CudaContext& cuda);
    ~NvdecDecoder();

    // Non-copyable
    NvdecDecoder(const NvdecDecoder&) = delete;
    NvdecDecoder& operator=(const NvdecDecoder&) = delete;

    /// Open a video stream for NVDEC decoding.
    bool open(const std::string& filePath, NvdecCodec codec = NvdecCodec::H264);

    /// Close the decoder and free GPU resources.
    void close();

    /// Check if decoder is open and functional.
    [[nodiscard]] bool isOpen() const noexcept { return m_open; }

    /// Decode the next frame. Frame data stays on GPU.
    bool decodeNext(NvdecFrameInfo& outFrame);

    /// Seek to a timestamp (seconds).
    bool seek(double timestamp);

    /// Get decoded frame dimensions.
    [[nodiscard]] uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] uint32_t height() const noexcept { return m_height; }

private:
    CudaContext& m_cuda;
    bool         m_open{false};
    uint32_t     m_width{0};
    uint32_t     m_height{0};

    // Opaque handles to CUVID parser/decoder (when ROUNDTABLE_HAS_CUDA)
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace rt
