/*
 * ChromaKeyEncoder — Standard H.264 encoder for chroma-key output.
 *
 * Takes RGBA pixels (with alpha already composited over a solid background
 * color by the caller), and encodes them as a standard YUV420P H.264 MP4
 * file — no packed-alpha, no alpha channel, just a normal video with a
 * solid-color background ready for keying in external editing software.
 *
 * Uses NVENC (h264_nvenc) for GPU acceleration when available, falling
 * back to libx264 software encoding.
 *
 * Thread-safe: one instance per thread.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Forward declarations
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;
struct AVBufferRef;

namespace rt {

class ChromaKeyEncoder
{
public:
    ChromaKeyEncoder();
    ~ChromaKeyEncoder();

    ChromaKeyEncoder(const ChromaKeyEncoder&) = delete;
    ChromaKeyEncoder& operator=(const ChromaKeyEncoder&) = delete;

    /// Check whether NVENC H.264 encoding is available on this system.
    static bool isNvencAvailable();

    /// Open an output file for writing.
    /// @param path     Output file path (.mp4)
    /// @param width    Frame width in pixels (must be even)
    /// @param height   Frame height in pixels (must be even)
    /// @param fps      Frames per second
    /// @param crf      Quality (0-51; default 22)
    /// @return true on success
    bool open(const std::filesystem::path& path,
              uint32_t width, uint32_t height,
              int fps = 60, int crf = 22);

    /// Write one RGBA frame (top-down, row-major, 4 bytes/pixel).
    /// The alpha channel is ignored — only RGB is encoded.
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
    bool receiveAndWritePackets();

    AVFormatContext* m_fmtCtx{nullptr};
    AVCodecContext*  m_codecCtx{nullptr};
    AVFrame*         m_frame{nullptr};    ///< YUV420P at width × height
    AVPacket*        m_packet{nullptr};
    AVStream*        m_stream{nullptr};
    SwsContext*      m_swsCtx{nullptr};
    AVBufferRef*     m_hwDeviceCtx{nullptr};
    AVBufferRef*     m_hwFramesCtx{nullptr};

    uint32_t    m_width{0};
    uint32_t    m_height{0};
    int64_t     m_framesWritten{0};
    bool        m_isOpen{false};
    bool        m_usingNvenc{false};
    std::string m_lastError;
};

} // namespace rt
