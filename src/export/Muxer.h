/*
 * Muxer — FFmpeg-based multiplexer for combining video and audio streams.
 *
 * Supports MP4, MOV, MKV, WebM containers.
 * Writes encoded video packets + audio to a container file.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVStream;

namespace rt {

// Forward declarations
struct EncodedPacket;
struct EncoderConfig;
struct MixdownResult;

// ── Container format ────────────────────────────────────────────────────────

enum class ContainerFormat : uint8_t
{
    MP4,
    MOV,
    MKV,
    WebM,
    AVI,
    Count
};

[[nodiscard]] const char* containerFormatName(ContainerFormat fmt) noexcept;
[[nodiscard]] const char* containerFormatExtension(ContainerFormat fmt) noexcept;

// ── Muxer configuration ────────────────────────────────────────────────────

struct MuxerConfig
{
    std::filesystem::path outputPath;
    ContainerFormat       format{ContainerFormat::MP4};

    // Video stream info
    uint32_t videoWidth{1920};
    uint32_t videoHeight{1080};
    int      videoFpsNum{30};
    int      videoFpsDen{1};
    int      videoCodecId{0};        // AV_CODEC_ID_H264, etc.

    // Audio stream info
    uint32_t audioSampleRate{48000};
    uint16_t audioChannels{2};
    int      audioBitrate{192000};
    bool     hasAudio{true};
};

// ═════════════════════════════════════════════════════════════════════════════

class Muxer
{
public:
    Muxer();
    ~Muxer();

    Muxer(const Muxer&) = delete;
    Muxer& operator=(const Muxer&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /// Open the output file and initialize streams.
    /// If deferHeader is true, the caller must call writeHeader() manually.
    bool open(const MuxerConfig& config, bool deferHeader = false);

    /// Write the container header (call after configuring codec params).
    bool writeHeader();

    /// Write a video packet.
    bool writeVideoPacket(const EncodedPacket& packet);

    /// Write raw audio data (interleaved float).
    /// For containers that need encoded audio, this encodes internally.
    bool writeAudioData(const float* samples, int64_t frameCount,
                        uint32_t sampleRate, uint16_t channels);

    /// Write pre-encoded audio packet.
    bool writeAudioPacket(const EncodedPacket& packet);

    /// Write the trailer and close the file.
    bool close();

    // ── Convenience ─────────────────────────────────────────────────────

    /// Mux video packets + audio mix result into a container.
    /// This is the simple all-in-one mux path.
    static bool muxFile(const std::filesystem::path& outputPath,
                        const std::vector<EncodedPacket>& videoPackets,
                        const MixdownResult* audioResult,
                        const MuxerConfig& config);

    // ── Queries ─────────────────────────────────────────────────────────

    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }
    [[nodiscard]] int64_t videoPtsWritten() const noexcept { return m_videoPtsWritten; }
    [[nodiscard]] int64_t audioPtsWritten() const noexcept { return m_audioPtsWritten; }
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

private:
    AVFormatContext* m_fmtCtx{nullptr};
    AVStream*        m_videoStream{nullptr};
    AVStream*        m_audioStream{nullptr};

    // Persistent AAC encoder for writeAudioData()
    struct AudioEncoder;
    AudioEncoder* m_audioEnc{nullptr};
    void initAudioEncoder(uint32_t sampleRate, uint16_t channels);
    void flushAudioEncoder();

    MuxerConfig m_config;
    int64_t     m_videoPtsWritten{0};
    int64_t     m_audioPtsWritten{0};
    bool        m_isOpen{false};
    bool        m_headerWritten{false};
    std::string m_lastError;
};

} // namespace rt
