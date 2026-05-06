/*
 * NvencEncoder.h — NVIDIA NVENC hardware video encoder.
 *
 * Direct NVENC API access for GPU-resident encoding without
 * going through FFmpeg. Requires CUDA context and NVENC SDK headers.
 *
 * This class does NOT inherit from Encoder (which lives in roundtable_export)
 * to avoid circular dependencies. Instead, the FFmpeg-based H264/H265 encoders
 * already support NVENC via h264_nvenc / hevc_nvenc codecs. This class provides
 * a direct NVENC SDK path for zero-copy GPU encoding scenarios.
 *
 * When ROUNDTABLE_HAS_CUDA is not defined, provides a stub.
 */

#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace rt {

class CudaContext;

/// Encoded packet from NVENC (mirrors the export EncodedPacket structure).
struct NvencPacket
{
    const uint8_t* data{nullptr};
    int            size{0};
    int64_t        pts{0};
    int64_t        dts{0};
    bool           isKeyframe{false};
};

/// NVENC encoder configuration.
struct NvencConfig
{
    uint32_t width{1920};
    uint32_t height{1080};
    int      fpsNum{30};
    int      fpsDen{1};
    int      crf{23};
    int      bitrateMbps{0};
    int      gopSize{0};
    bool     useH265{false};   ///< false = H.264, true = H.265
};

/// Direct NVENC encoder — bypasses FFmpeg for lower latency GPU encoding.
class NvencEncoder
{
public:
    NvencEncoder();
    ~NvencEncoder();

    // Non-copyable
    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    /// Initialize with CUDA context and encoder config.
    bool init(CudaContext* cuda, const NvencConfig& config);

    /// Encode a single RGBA frame.
    bool encodeFrame(const uint8_t* rgbaData, int64_t pts);

    /// Flush remaining buffered frames.
    int flush();

    /// Shutdown and release all resources.
    void shutdown();

    /// Check if NVENC is available on this system.
    [[nodiscard]] static bool isAvailable();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    /// Access the last encoded packet.
    [[nodiscard]] const NvencPacket& lastPacket() const { return m_lastPacket; }

    /// Access all flushed packets.
    [[nodiscard]] const std::vector<NvencPacket>& flushedPackets() const { return m_flushedPackets; }

private:
    NvencConfig     m_config;
    NvencPacket     m_lastPacket;
    std::vector<NvencPacket> m_flushedPackets;

    // Opaque NVENC handles (only valid with ROUNDTABLE_HAS_CUDA)
    void*           m_nvencSession{nullptr};
    CudaContext*    m_cuda{nullptr};
    bool            m_initialized{false};
};

} // namespace rt
