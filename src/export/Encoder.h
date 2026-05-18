/*
 * Encoder — abstract base class for video/image encoders.
 *
 * All encoder implementations (H264, H265, AV1, ProRes, ImageSequence)
 * derive from this interface. The export pipeline writes frames through
 * this abstraction so the muxer/render queue is codec-agnostic.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// Forward declarations for FFmpeg types
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;

namespace rt {

// ── Encoder codec enum ──────────────────────────────────────────────────────

enum class EncoderCodec : uint8_t
{
    H264,
    H265,
    AV1,
    ProRes,
    DNxHR,
    ImageSequence,  // Not a real codec — writes individual frames
    Count
};

[[nodiscard]] const char* encoderCodecName(EncoderCodec codec) noexcept;

// ── Encoder configuration ───────────────────────────────────────────────────

/// Quality preset (maps to x264/x265/SVT-AV1 presets)
enum class EncoderPreset : uint8_t
{
    Ultrafast,
    Superfast,
    Veryfast,
    Faster,
    Fast,
    Medium,
    Slow,
    Slower,
    Veryslow,
    Count
};

/// Hardware acceleration mode
enum class HardwareAccel : uint8_t
{
    None,       // CPU-only encoding (libx264, libx265, etc.)
    NVENC,      // NVIDIA hardware encoder
    QSV,        // Intel Quick Sync
    AMF,        // AMD Advanced Media Framework
    Count
};

/// ProRes profile
enum class ProResProfile : uint8_t
{
    Proxy,
    LT,
    Standard,
    HQ,
    _4444,      // ProRes 4444
    _4444XQ,    // ProRes 4444 XQ
    Count
};

/// Image sequence format
enum class ImageFormat : uint8_t
{
    PNG,
    TIFF,
    EXR,
    BMP,
    JPEG,
    Count
};

/// Full encoder configuration
struct EncoderConfig
{
    // ── Video ───────────────────────────────────────────────────────────
    uint32_t       width{1920};
    uint32_t       height{1080};
    double         fps{30.0};
    int            fpsNum{30};          // Numerator of exact FPS fraction
    int            fpsDen{1};           // Denominator
    EncoderCodec   codec{EncoderCodec::H264};
    EncoderPreset  preset{EncoderPreset::Medium};
    HardwareAccel  hwAccel{HardwareAccel::None};

    // ── Quality ─────────────────────────────────────────────────────────
    int            crf{23};             // Constant Rate Factor (0-51, lower=better)
    int            bitrateMbps{0};      // 0 = use CRF mode; >0 = CBR/VBR in Mbps
    int            maxBitrateMbps{0};   // 0 = uncapped; for VBR
    int            gopSize{0};          // 0 = auto (2 * fps)

    // ── ProRes specific ─────────────────────────────────────────────────
    ProResProfile  proresProfile{ProResProfile::HQ};

    // ── Image sequence specific ─────────────────────────────────────────
    ImageFormat    imageFormat{ImageFormat::PNG};
    int            jpegQuality{95};     // 0-100

    // ── Color ───────────────────────────────────────────────────────────
    bool           bt709{true};         // BT.709 color space (standard HD)
};

// ── Encoded packet ──────────────────────────────────────────────────────────

/// A single encoded video packet ready for muxing.
struct EncodedPacket
{
    const uint8_t* data{nullptr};
    int            size{0};
    int64_t        pts{0};          // Presentation timestamp
    int64_t        dts{0};          // Decode timestamp
    int64_t        duration{0};
    bool           isKeyframe{false};

    // Ownership: if ownsData is true, data was malloc'd and must be freed.
    bool           ownsData{false};
};

// ═════════════════════════════════════════════════════════════════════════════

/// Abstract video encoder interface.
class Encoder
{
public:
    virtual ~Encoder() = default;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /// Initialize the encoder with the given configuration.
    /// Returns true on success.
    virtual bool init(const EncoderConfig& config) = 0;

    /// Encode a single frame (RGBA pixels, top-down, row-major).
    /// Returns true if an encoded packet is produced (may be delayed).
    virtual bool encodeFrame(const uint8_t* rgbaPixels,
                             int64_t frameIndex) = 0;

    /// Flush remaining buffered frames. Call after the last frame.
    /// Returns the number of flushed packets.
    virtual int flush() = 0;

    /// Shutdown and release all resources.
    virtual void shutdown() = 0;

    // ── Packet retrieval ────────────────────────────────────────────────

    /// Get the last encoded packet (valid after encodeFrame/flush returns true).
    /// The packet data is valid until the next encode/flush call.
    [[nodiscard]] virtual const EncodedPacket& lastPacket() const = 0;

    /// Get all buffered packets (for flush — may produce multiple).
    [[nodiscard]] virtual const std::vector<EncodedPacket>& flushedPackets() const = 0;

    /// Get extra packets produced by the last encodeFrame call.
    /// Some encoders output multiple packets per input frame (B-frame
    /// delay). Collect these after each encodeFrame.
    [[nodiscard]] virtual const std::vector<EncodedPacket>& pendingPackets() const {
        static const std::vector<EncodedPacket> s_empty;
        return s_empty;
    }

    // ── Queries ─────────────────────────────────────────────────────────

    [[nodiscard]] virtual EncoderCodec codec() const noexcept = 0;
    /// Return the FFmpeg AVCodecID for this encoder (or 0 if not available).
    [[nodiscard]] virtual int avCodecId() const noexcept = 0;
    /// Return the underlying FFmpeg AVCodecContext, or nullptr if not opened.
    /// The muxer reads this to copy codec params + extradata (SPS/PPS, etc.)
    /// into the container's stream codecpar so MP4/MOV files have a valid
    /// avcC/hvcC box. Without this, files only play in lenient demuxers
    /// (VLC) and fail in strict editors (Premiere, Resolve, browsers).
    [[nodiscard]] virtual AVCodecContext* avCodecContext() const noexcept { return nullptr; }
    [[nodiscard]] virtual bool isInitialized() const noexcept = 0;
    [[nodiscard]] virtual bool isHardwareAccelerated() const noexcept = 0;
    [[nodiscard]] virtual const std::string& lastError() const noexcept = 0;
    [[nodiscard]] virtual int64_t framesEncoded() const noexcept = 0;

    // ── Factory ─────────────────────────────────────────────────────────

    /// Create an encoder for the given codec. Tries hardware acceleration first.
    static std::unique_ptr<Encoder> create(EncoderCodec codec,
                                            HardwareAccel hwAccel = HardwareAccel::None);

    /// Resolve the "Auto" UI option: returns the best hardware backend
    /// whose FFmpeg encoder is actually available for `codec` (NVENC →
    /// QSV → AMF), or None (CPU) if none exist. Codecs with no hardware
    /// path (ProRes/DNxHR/ImageSequence) always return None.
    [[nodiscard]] static HardwareAccel detectBestHardware(EncoderCodec codec) noexcept;

protected:
    // Stable backing store for packet payloads. FFmpeg reuses a single
    // AVPacket buffer across avcodec_receive_packet() calls, so pointing
    // an EncodedPacket directly at AVPacket::data dangles as soon as the
    // next packet is received (B-frame drains, flush). Each subclass
    // clears this at the start of sendFrame()/flush() and routes every
    // packet's bytes through retainPacketData() so the data outlives the
    // drain loop. std::deque is required: pointers to existing elements
    // stay valid across push_back (std::vector would reallocate).
    std::deque<std::vector<uint8_t>> m_pktStore;

    /// Copy `size` bytes into stable storage and repoint ep.data at it.
    /// Sets ep.size and clears ep.ownsData (the encoder owns the buffer
    /// until the next sendFrame()/flush()).
    void retainPacketData(EncodedPacket& ep, const uint8_t* data, int size);
};

} // namespace rt
