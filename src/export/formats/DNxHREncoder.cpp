/*
 * DNxHREncoder.cpp — Avid DNxHR encoding via FFmpeg dnxhd codec.
 */

#include "formats/DNxHREncoder.h"

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

DNxHREncoder::DNxHREncoder() = default;
DNxHREncoder::~DNxHREncoder() { shutdown(); }

bool DNxHREncoder::init(const EncoderConfig& config)
{
    if (m_initialized) shutdown();
    m_config = config;
    m_framesEncoded = 0;
    return initCodec(config);
}

bool DNxHREncoder::initCodec(const EncoderConfig& config)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_DNXHD);
    if (!codec) { m_lastError = "No DNxHD/DNxHR encoder found"; spdlog::error("{}", m_lastError); return false; }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) { m_lastError = "Failed to alloc context"; return false; }

    // DNxHR uses YUV422P (8-bit) or YUV422P10LE (10-bit) or YUVA444P10LE (444)
    bool is10bit = (m_profile == DNxHRProfile::HQX || m_profile == DNxHRProfile::_444);
    bool is444   = (m_profile == DNxHRProfile::_444);

    if (is444)
        m_codecCtx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
    else if (is10bit)
        m_codecCtx->pix_fmt = AV_PIX_FMT_YUV422P10LE;
    else
        m_codecCtx->pix_fmt = AV_PIX_FMT_YUV422P;

    m_codecCtx->width     = static_cast<int>(config.width);
    m_codecCtx->height    = static_cast<int>(config.height);
    m_codecCtx->time_base = {config.fpsDen, config.fpsNum};
    m_codecCtx->framerate = {config.fpsNum, config.fpsDen};

    // Set DNxHR profile via the "profile" option
    const char* profileStr = "dnxhr_hq";
    switch (m_profile) {
        case DNxHRProfile::LB:   profileStr = "dnxhr_lb";  break;
        case DNxHRProfile::SQ:   profileStr = "dnxhr_sq";  break;
        case DNxHRProfile::HQ:   profileStr = "dnxhr_hq";  break;
        case DNxHRProfile::HQX:  profileStr = "dnxhr_hqx"; break;
        case DNxHRProfile::_444: profileStr = "dnxhr_444";  break;
        default: break;
    }
    av_opt_set(m_codecCtx->priv_data, "profile", profileStr, 0);

    // Codec params belong in the container header so editors read
    // them without scanning the bitstream.
    m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        m_lastError = "DNxHREncoder: Failed to open codec";
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
    spdlog::info("DNxHREncoder: {}x{} profile={}", config.width, config.height, profileStr);
    return true;
}

int DNxHREncoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_DNXHD;
}

bool DNxHREncoder::encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex)
{
    if (!m_initialized) return false;
    av_frame_make_writable(m_frame);
    const uint8_t* src[] = { rgbaPixels };
    int stride[] = { static_cast<int>(m_config.width * 4) };
    sws_scale(t_swsCtx.get(), src, stride, 0, m_codecCtx->height, m_frame->data, m_frame->linesize);
    m_frame->pts = frameIndex;
    return sendFrame(m_frame);
}

bool DNxHREncoder::sendFrame(AVFrame* frame)
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

int DNxHREncoder::flush()
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

void DNxHREncoder::shutdown()
{
    t_swsCtx.reset();
    if (m_packet) av_packet_free(&m_packet);
    if (m_frame) av_frame_free(&m_frame);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    m_packet = nullptr; m_frame = nullptr; m_codecCtx = nullptr;
    m_initialized = false; m_flushedPackets.clear();
}

#else

DNxHREncoder::DNxHREncoder() = default;
DNxHREncoder::~DNxHREncoder() = default;
bool DNxHREncoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
bool DNxHREncoder::encodeFrame(const uint8_t*, int64_t) { return false; }
int  DNxHREncoder::flush() { return 0; }
void DNxHREncoder::shutdown() {}
bool DNxHREncoder::initCodec(const EncoderConfig&) { return false; }
bool DNxHREncoder::sendFrame(AVFrame*) { return false; }

#endif

} // namespace rt
