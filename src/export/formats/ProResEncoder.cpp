/*
 * ProResEncoder.cpp — ProRes encoding via FFmpeg prores_ks.
 */

#include "formats/ProResEncoder.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

namespace {
struct SwsDel { void operator()(SwsContext* c) { if (c) sws_freeContext(c); } };
thread_local std::unique_ptr<SwsContext, SwsDel> t_swsCtx;
}

ProResEncoder::ProResEncoder() = default;
ProResEncoder::~ProResEncoder() { shutdown(); }

bool ProResEncoder::init(const EncoderConfig& config)
{
    if (m_initialized) shutdown();
    m_config = config;
    m_framesEncoded = 0;
    return initCodec(config);
}

bool ProResEncoder::initCodec(const EncoderConfig& config)
{
    const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_PRORES);
    if (!codec) { m_lastError = "No ProRes encoder found"; spdlog::error("{}", m_lastError); return false; }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) { m_lastError = "Failed to alloc context"; return false; }

    // ProRes uses YUV422P10LE or YUVA444P10LE depending on profile
    bool alpha = config.proresProfile >= ProResProfile::_4444;
    m_codecCtx->pix_fmt   = alpha ? AV_PIX_FMT_YUVA444P10LE : AV_PIX_FMT_YUV422P10LE;
    m_codecCtx->width     = static_cast<int>(config.width);
    m_codecCtx->height    = static_cast<int>(config.height);
    m_codecCtx->time_base = {config.fpsDen, config.fpsNum};
    m_codecCtx->framerate = {config.fpsNum, config.fpsDen};

    // Set ProRes profile
    int profile = 2; // Standard
    switch (config.proresProfile) {
        case ProResProfile::Proxy:    profile = 0; break;
        case ProResProfile::LT:       profile = 1; break;
        case ProResProfile::Standard: profile = 2; break;
        case ProResProfile::HQ:       profile = 3; break;
        case ProResProfile::_4444:    profile = 4; break;
        case ProResProfile::_4444XQ:  profile = 5; break;
        default: break;
    }
    av_opt_set_int(m_codecCtx->priv_data, "profile", profile, 0);

    // Codec params (profile, pix_fmt, color tags) belong in the MOV
    // header so editors read them without scanning the bitstream.
    m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        m_lastError = "ProResEncoder: Failed to open codec";
        spdlog::error("{}", m_lastError);
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_frame = av_frame_alloc();
    m_frame->format = m_codecCtx->pix_fmt;
    m_frame->width  = m_codecCtx->width;
    m_frame->height = m_codecCtx->height;
    av_frame_get_buffer(m_frame, 0);
    m_packet = av_packet_alloc();

    t_swsCtx.reset(sws_getContext(
        m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_RGBA,
        m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr));

    m_initialized = true;
    spdlog::info("ProResEncoder: {}x{} profile={}", config.width, config.height, profile);
    return true;
}

int ProResEncoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_PRORES;
}

bool ProResEncoder::encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex)
{
    if (!m_initialized) return false;
    av_frame_make_writable(m_frame);
    const uint8_t* src[] = { rgbaPixels };
    int stride[] = { static_cast<int>(m_config.width * 4) };
    sws_scale(t_swsCtx.get(), src, stride, 0, m_codecCtx->height, m_frame->data, m_frame->linesize);
    m_frame->pts = frameIndex;
    return sendFrame(m_frame);
}

bool ProResEncoder::sendFrame(AVFrame* frame)
{
    m_pendingPackets.clear();
    m_pktStore.clear();  // invalidates prior m_lastPacket/pending (already consumed)
    if (avcodec_send_frame(m_codecCtx, frame) < 0) { m_lastError = "send error"; return false; }

    bool gotOne = false;
    while (true) {
        if (avcodec_receive_packet(m_codecCtx, m_packet) != 0) break;
        EncodedPacket ep;
        ep.pts        = m_packet->pts;
        ep.dts        = m_packet->dts;
        ep.duration   = (m_packet->duration > 0) ? m_packet->duration : 1;
        ep.isKeyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        retainPacketData(ep, m_packet->data, m_packet->size);
        av_packet_unref(m_packet);
        if (!gotOne) { m_lastPacket = ep; gotOne = true; }
        else { m_pendingPackets.push_back(ep); }
        ++m_framesEncoded;
    }
    return gotOne;
}

int ProResEncoder::flush()
{
    if (!m_initialized) return 0;
    m_flushedPackets.clear();
    m_pktStore.clear();  // prior packets already consumed by the caller
    avcodec_send_frame(m_codecCtx, nullptr);
    int count = 0;
    while (avcodec_receive_packet(m_codecCtx, m_packet) == 0) {
        EncodedPacket ep;
        ep.pts        = m_packet->pts;
        ep.dts        = m_packet->dts;
        ep.duration   = (m_packet->duration > 0) ? m_packet->duration : 1;
        ep.isKeyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        retainPacketData(ep, m_packet->data, m_packet->size);
        av_packet_unref(m_packet);
        m_flushedPackets.push_back(ep);
        ++count; ++m_framesEncoded;
    }
    return count;
}

void ProResEncoder::shutdown()
{
    t_swsCtx.reset();
    if (m_packet) av_packet_free(&m_packet);
    if (m_frame) av_frame_free(&m_frame);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    m_packet = nullptr; m_frame = nullptr; m_codecCtx = nullptr;
    m_initialized = false; m_flushedPackets.clear();
}

#else

ProResEncoder::ProResEncoder() = default;
ProResEncoder::~ProResEncoder() = default;
bool ProResEncoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
bool ProResEncoder::encodeFrame(const uint8_t*, int64_t) { return false; }
int  ProResEncoder::flush() { return 0; }
void ProResEncoder::shutdown() {}
bool ProResEncoder::initCodec(const EncoderConfig&) { return false; }
bool ProResEncoder::sendFrame(AVFrame*) { return false; }

#endif

} // namespace rt
