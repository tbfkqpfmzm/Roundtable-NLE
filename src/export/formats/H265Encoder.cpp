/*
 * H265Encoder.cpp — H.265/HEVC encoding via FFmpeg.
 *
 * Tries hevc_nvenc first (NVIDIA), then libx265 (CPU).
 */

#include "formats/H265Encoder.h"

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
struct SwsCtxDel { void operator()(SwsContext* c) { if (c) sws_freeContext(c); } };
thread_local std::unique_ptr<SwsContext, SwsCtxDel> t_swsCtx;
}

H265Encoder::H265Encoder() = default;
H265Encoder::~H265Encoder() { shutdown(); }

bool H265Encoder::init(const EncoderConfig& config)
{
    if (m_initialized) shutdown();
    m_config = config;
    m_framesEncoded = 0;
    m_hwAccel = false;
    return initCodec(config);
}

bool H265Encoder::initCodec(const EncoderConfig& config)
{
    const AVCodec* codec = nullptr;
    if (config.hwAccel == HardwareAccel::NVENC) {
        codec = avcodec_find_encoder_by_name("hevc_nvenc");
        if (codec) { spdlog::info("H265Encoder: Using NVENC"); m_hwAccel = true; }
    }
    if (!codec) { codec = avcodec_find_encoder_by_name("libx265"); m_hwAccel = false; }
    if (!codec) { codec = avcodec_find_encoder(AV_CODEC_ID_HEVC); }
    if (!codec) { m_lastError = "No H.265 encoder found"; spdlog::error("{}", m_lastError); return false; }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) { m_lastError = "Failed to alloc context"; return false; }

    m_codecCtx->width     = static_cast<int>(config.width);
    m_codecCtx->height    = static_cast<int>(config.height);
    m_codecCtx->time_base = {config.fpsDen, config.fpsNum};
    m_codecCtx->framerate = {config.fpsNum, config.fpsDen};
    m_codecCtx->pix_fmt   = AV_PIX_FMT_YUV420P;
    m_codecCtx->gop_size  = config.gopSize > 0 ? config.gopSize : config.fpsNum * 2;

    if (config.bitrateMbps > 0) {
        m_codecCtx->bit_rate = static_cast<int64_t>(config.bitrateMbps) * 1000000;
    }
    if (config.bt709) {
        m_codecCtx->color_primaries = AVCOL_PRI_BT709;
        m_codecCtx->color_trc       = AVCOL_TRC_BT709;
        m_codecCtx->colorspace      = AVCOL_SPC_BT709;
    }

    if (!m_hwAccel) {
        static const char* presets[] = {
            "ultrafast","superfast","veryfast","faster","fast","medium","slow","slower","veryslow"
        };
        int idx = static_cast<int>(config.preset);
        if (idx < 9) av_opt_set(m_codecCtx->priv_data, "preset", presets[idx], 0);
        if (config.bitrateMbps == 0) av_opt_set_int(m_codecCtx->priv_data, "crf", config.crf, 0);
    } else {
        av_opt_set(m_codecCtx->priv_data, "preset", "p5", 0);
        av_opt_set(m_codecCtx->priv_data, "tune", "hq", 0);
        av_opt_set(m_codecCtx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(m_codecCtx->priv_data, "qp", config.crf, 0);
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        m_lastError = "H265Encoder: Failed to open codec";
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
        m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr));

    m_initialized = true;
    spdlog::info("H265Encoder: {}x{} @ {}/{} fps ({})", config.width, config.height,
                 config.fpsNum, config.fpsDen, m_hwAccel ? "NVENC" : "libx265");
    return true;
}

int H265Encoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_HEVC;
}

bool H265Encoder::encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex)
{
    if (!m_initialized) return false;
    av_frame_make_writable(m_frame);
    const uint8_t* src[] = { rgbaPixels };
    int stride[] = { static_cast<int>(m_config.width * 4) };
    sws_scale(t_swsCtx.get(), src, stride, 0, m_codecCtx->height, m_frame->data, m_frame->linesize);
    m_frame->pts = frameIndex;
    return sendFrame(m_frame);
}

bool H265Encoder::sendFrame(AVFrame* frame)
{
    m_pendingPackets.clear();
    if (avcodec_send_frame(m_codecCtx, frame) < 0) { m_lastError = "send error"; return false; }

    bool gotOne = false;
    while (true) {
        if (avcodec_receive_packet(m_codecCtx, m_packet) != 0) break;
        EncodedPacket ep;
        ep.data       = m_packet->data;
        ep.size       = m_packet->size;
        ep.pts        = m_packet->pts;
        ep.dts        = m_packet->dts;
        ep.duration   = (m_packet->duration > 0) ? m_packet->duration : 1;
        ep.isKeyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        ep.ownsData   = false;
        if (!gotOne) { m_lastPacket = ep; gotOne = true; }
        else { m_pendingPackets.push_back(ep); }
        ++m_framesEncoded;
    }
    return gotOne;
}

int H265Encoder::flush()
{
    if (!m_initialized) return 0;
    m_flushedPackets.clear();
    avcodec_send_frame(m_codecCtx, nullptr);
    int count = 0;
    while (avcodec_receive_packet(m_codecCtx, m_packet) == 0) {
        EncodedPacket ep;
        ep.data       = m_packet->data;
        ep.size       = m_packet->size;
        ep.pts        = m_packet->pts;
        ep.dts        = m_packet->dts;
        ep.duration   = (m_packet->duration > 0) ? m_packet->duration : 1;
        ep.isKeyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        ep.ownsData   = false;
        m_flushedPackets.push_back(ep);
        ++count; ++m_framesEncoded;
    }
    return count;
}

void H265Encoder::shutdown()
{
    t_swsCtx.reset();
    if (m_packet) av_packet_free(&m_packet);
    if (m_frame) av_frame_free(&m_frame);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    m_packet = nullptr; m_frame = nullptr; m_codecCtx = nullptr;
    m_initialized = false; m_flushedPackets.clear();
}

#else

H265Encoder::H265Encoder() = default;
H265Encoder::~H265Encoder() = default;
bool H265Encoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
bool H265Encoder::encodeFrame(const uint8_t*, int64_t) { return false; }
int  H265Encoder::flush() { return 0; }
void H265Encoder::shutdown() {}
bool H265Encoder::initCodec(const EncoderConfig&) { return false; }
bool H265Encoder::sendFrame(AVFrame*) { return false; }

#endif

} // namespace rt
