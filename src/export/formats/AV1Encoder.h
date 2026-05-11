/*
 * AV1Encoder — AV1 encoding via FFmpeg libsvtav1 or NVENC av1_nvenc (RTX 4090).
 */

#pragma once

#include "Encoder.h"

namespace rt {

class AV1Encoder : public Encoder
{
public:
    AV1Encoder();
    ~AV1Encoder() override;

    bool init(const EncoderConfig& config) override;
    bool encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex) override;
    int  flush() override;
    void shutdown() override;

    [[nodiscard]] const EncodedPacket& lastPacket() const override { return m_lastPacket; }
    [[nodiscard]] const std::vector<EncodedPacket>& flushedPackets() const override { return m_flushedPackets; }
    [[nodiscard]] const std::vector<EncodedPacket>& pendingPackets() const override { return m_pendingPackets; }
    [[nodiscard]] EncoderCodec codec() const noexcept override { return EncoderCodec::AV1; }
    [[nodiscard]] int avCodecId() const noexcept override;
    [[nodiscard]] bool isInitialized() const noexcept override { return m_initialized; }
    [[nodiscard]] bool isHardwareAccelerated() const noexcept override { return m_hwAccel; }
    [[nodiscard]] const std::string& lastError() const noexcept override { return m_lastError; }
    [[nodiscard]] int64_t framesEncoded() const noexcept override { return m_framesEncoded; }

private:
    bool initCodec(const EncoderConfig& config);
    bool sendFrame(AVFrame* frame);

    AVCodecContext* m_codecCtx{nullptr};
    AVFrame*        m_frame{nullptr};
    AVPacket*       m_packet{nullptr};
    AVBufferRef*    m_hwDeviceCtx{nullptr};
    AVBufferRef*    m_hwFramesCtx{nullptr};

    EncoderConfig              m_config;
    EncodedPacket              m_lastPacket;
    std::vector<EncodedPacket> m_flushedPackets;
    std::vector<EncodedPacket> m_pendingPackets;
    std::string                m_lastError;
    int64_t                    m_framesEncoded{0};
    bool                       m_initialized{false};
    bool                       m_hwAccel{false};
};

} // namespace rt
