/*
 * DNxHREncoder — Avid DNxHR encoding via FFmpeg dnxhd.
 */

#pragma once

#include "Encoder.h"

namespace rt {

/// DNxHR quality profile
enum class DNxHRProfile : uint8_t
{
    LB,     // Low Bandwidth (8-bit 4:2:2, ~36 Mbps @ 1080p30)
    SQ,     // Standard Quality (8-bit 4:2:2, ~145 Mbps)
    HQ,     // High Quality (8-bit 4:2:2, ~220 Mbps)
    HQX,    // High Quality 10-bit (10-bit 4:2:2, ~220 Mbps)
    _444,   // 4:4:4 10-bit (~350 Mbps)
    Count
};

class DNxHREncoder : public Encoder
{
public:
    DNxHREncoder();
    ~DNxHREncoder() override;

    bool init(const EncoderConfig& config) override;
    bool encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex) override;
    int  flush() override;
    void shutdown() override;

    [[nodiscard]] const EncodedPacket& lastPacket() const override { return m_lastPacket; }
    [[nodiscard]] const std::vector<EncodedPacket>& flushedPackets() const override { return m_flushedPackets; }
    [[nodiscard]] const std::vector<EncodedPacket>& pendingPackets() const override { return m_pendingPackets; }
    [[nodiscard]] EncoderCodec codec() const noexcept override { return EncoderCodec::DNxHR; }
    [[nodiscard]] int avCodecId() const noexcept override;
    [[nodiscard]] AVCodecContext* avCodecContext() const noexcept override { return m_codecCtx; }
    [[nodiscard]] bool isInitialized() const noexcept override { return m_initialized; }
    [[nodiscard]] bool isHardwareAccelerated() const noexcept override { return false; }
    [[nodiscard]] const std::string& lastError() const noexcept override { return m_lastError; }
    [[nodiscard]] int64_t framesEncoded() const noexcept override { return m_framesEncoded; }

    void setProfile(DNxHRProfile p) noexcept { m_profile = p; }
    [[nodiscard]] DNxHRProfile profile() const noexcept { return m_profile; }

private:
    bool initCodec(const EncoderConfig& config);
    bool sendFrame(AVFrame* frame);

    AVCodecContext* m_codecCtx{nullptr};
    AVFrame*        m_frame{nullptr};
    AVPacket*       m_packet{nullptr};

    EncoderConfig              m_config;
    EncodedPacket              m_lastPacket;
    std::vector<EncodedPacket> m_flushedPackets;
    std::vector<EncodedPacket> m_pendingPackets;
    std::string                m_lastError;
    int64_t                    m_framesEncoded{0};
    bool                       m_initialized{false};
    DNxHRProfile               m_profile{DNxHRProfile::HQ};
};

} // namespace rt
