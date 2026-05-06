/*
 * VP9AlphaEncoder — self-contained VP9+alpha WebM writer.
 *
 * Encodes RGBA frames to VP9 with full alpha channel support using
 * FFmpeg's libvpx-vp9 codec in YUVA420P pixel format, muxed into
 * a WebM container.
 *
 * This is NOT part of the export pipeline's Encoder hierarchy — it is
 * a standalone utility for the pre-rendered animation cache system
 * (AnimationVideoCache / SpinePrerenderer).
 *
 * Usage:
 *   VP9AlphaEncoder enc;
 *   enc.open("out.webm", 1024, 2048, 60);
 *   for (each frame) enc.writeFrame(rgbaPixels);
 *   enc.finalize();
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Forward declarations — avoid pulling FFmpeg headers into every TU.
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;

namespace rt {

class VP9AlphaEncoder
{
public:
    VP9AlphaEncoder();
    ~VP9AlphaEncoder();

    VP9AlphaEncoder(const VP9AlphaEncoder&) = delete;
    VP9AlphaEncoder& operator=(const VP9AlphaEncoder&) = delete;

    /// Open a WebM file for writing.
    /// @param path     Output file path (.webm)
    /// @param width    Frame width in pixels (must be even)
    /// @param height   Frame height in pixels (must be even)
    /// @param fps      Frames per second
    /// @param crf      Constant Rate Factor (0-63, lower = better; default 18)
    /// @return true on success
    bool open(const std::filesystem::path& path,
              uint32_t width, uint32_t height,
              int fps = 60, int crf = 18);

    /// Write one RGBA frame (top-down, row-major, 4 bytes/pixel).
    /// Frames must be written sequentially.
    bool writeFrame(const uint8_t* rgbaPixels);

    /// Flush encoder, write trailer, and close the file.
    /// Must be called after the last writeFrame().
    bool finalize();

    /// @return true if the encoder is open and ready for frames
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }

    /// @return number of frames written so far
    [[nodiscard]] int64_t framesWritten() const noexcept { return m_framesWritten; }

    /// @return last error message
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

private:
    bool flushEncoder();

    AVFormatContext* m_fmtCtx{nullptr};
    AVCodecContext*  m_codecCtx{nullptr};
    AVFrame*         m_frame{nullptr};
    AVPacket*        m_packet{nullptr};
    AVStream*        m_stream{nullptr};
    SwsContext*      m_swsCtx{nullptr};

    uint32_t    m_width{0};
    uint32_t    m_height{0};
    int64_t     m_framesWritten{0};
    bool        m_isOpen{false};
    std::string m_lastError;
};

} // namespace rt
