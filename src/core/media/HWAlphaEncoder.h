/*
 * HWAlphaEncoder — GPU-accelerated HEVC encoder with packed alpha.
 *
 * Uses NVIDIA NVENC (hevc_nvenc) for hardware-accelerated video encoding
 * with transparency support.  Because NVENC does not support YUVA pixel
 * formats, we use the "packed-alpha" technique:
 *
 *   ┌───────────────────┐
 *   │   RGB (top half)  │  ← original colour pixels
 *   ├───────────────────┤
 *   │ Alpha (bot half)  │  ← alpha channel as greyscale
 *   └───────────────────┘
 *
 * The output is a standard HEVC stream in an MP4 container at double the
 * nominal height.  On decode, the consumer splits the frame in half and
 * reconstructs RGBA.
 *
 * Falls back to CPU libvpx-vp9 encoding via VP9AlphaEncoder if NVENC is
 * not available.
 *
 * Thread-safe: one instance per thread.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Forward declarations
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;
struct AVBufferRef;

namespace rt {

class HWAlphaEncoder
{
public:
    HWAlphaEncoder();
    ~HWAlphaEncoder();

    HWAlphaEncoder(const HWAlphaEncoder&) = delete;
    HWAlphaEncoder& operator=(const HWAlphaEncoder&) = delete;

    /// Check whether NVENC HEVC encoding is available on this system.
    /// Lightweight probe — opens and immediately closes a test encoder.
    static bool isNvencAvailable();

    /// Open an output file for writing.
    /// @param path     Output file path (.mp4)
    /// @param width    Frame width in pixels (must be even)
    /// @param height   Frame height in pixels (must be even) — *nominal* height
    /// @param fps      Frames per second
    /// @param crf      Quality (0-51 for HEVC; default 20)
    /// @return true on success
    bool open(const std::filesystem::path& path,
              uint32_t width, uint32_t height,
              int fps = 60, int crf = 20);

    /// Write one RGBA frame (top-down, row-major, 4 bytes/pixel).
    bool writeFrame(const uint8_t* rgbaPixels);

    /// Flush encoder, write trailer, close file.
    bool finalize();

    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }
    [[nodiscard]] int64_t framesWritten() const noexcept { return m_framesWritten; }
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

    /// Returns true if this instance is using NVENC (GPU) vs software fallback
    [[nodiscard]] bool isHardwareAccelerated() const noexcept { return m_usingNvenc; }

private:
    bool flushEncoder();

    /// Receive encoded packets from the codec and write them to the container.
    /// Returns false on error.
    bool receiveAndWritePackets();

    AVFormatContext* m_fmtCtx{nullptr};
    AVCodecContext*  m_codecCtx{nullptr};
    AVFrame*         m_frame{nullptr};    ///< NV12/YUV420P at width × (height*2)
    AVPacket*        m_packet{nullptr};
    AVStream*        m_stream{nullptr};
    SwsContext*      m_swsCtx{nullptr};
    AVBufferRef*     m_hwDeviceCtx{nullptr};
    AVBufferRef*     m_hwFramesCtx{nullptr};

    uint32_t    m_width{0};
    uint32_t    m_height{0};       ///< Nominal height (actual encoded = 2×)
    int64_t     m_framesWritten{0};
    bool        m_isOpen{false};
    bool        m_usingNvenc{false};
    std::string m_lastError;

    /// Intermediate CPU buffer for the packed (2× height) RGBA frame
    std::vector<uint8_t> m_packedRGBA;
};

} // namespace rt
