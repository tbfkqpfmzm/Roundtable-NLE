/*
 * H264Encoder.cpp — H.264/AVC encoding via FFmpeg.
 *
 * Tries h264_nvenc first (NVIDIA), then libx264 (CPU).
 * Converts RGBA input → YUV420P via swscale.
 */

#include "formats/H264Encoder.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

namespace {

struct SwsContextDeleter {
    void operator()(SwsContext* ctx) { if (ctx) sws_freeContext(ctx); }
};

thread_local std::unique_ptr<SwsContext, SwsContextDeleter> t_swsCtx;

} // namespace

H264Encoder::H264Encoder() = default;

H264Encoder::~H264Encoder() { shutdown(); }

bool H264Encoder::init(const EncoderConfig& config)
{
    if (m_initialized) shutdown();
    m_config = config;
    m_framesEncoded = 0;
    m_hwAccel = false;

    return initCodec(config);
}

bool H264Encoder::initCodec(const EncoderConfig& config)
{
    const AVCodec* codec = nullptr;
    if (config.hwAccel == HardwareAccel::NVENC) {
        codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (codec) {
            spdlog::info("H264Encoder: Using NVENC hardware encoder");
            m_hwAccel = true;
        }
    }
    if (!codec) {
        codec = avcodec_find_encoder_by_name("libx264");
        m_hwAccel = false;
    }
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        m_lastError = "H264Encoder: No H.264 encoder found";
        spdlog::error("{}", m_lastError);
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        m_lastError = "H264Encoder: Failed to allocate codec context";
        return false;
    }

    m_codecCtx->width     = static_cast<int>(config.width);
    m_codecCtx->height    = static_cast<int>(config.height);
    m_codecCtx->time_base = {config.fpsDen, config.fpsNum};
    m_codecCtx->framerate = {config.fpsNum, config.fpsDen};
    m_codecCtx->pix_fmt   = AV_PIX_FMT_YUV420P;
    m_codecCtx->gop_size  = config.gopSize > 0 ? config.gopSize : config.fpsNum * 2;
    m_codecCtx->max_b_frames = 2;

    if (config.bitrateMbps > 0) {
        m_codecCtx->bit_rate     = static_cast<int64_t>(config.bitrateMbps) * 1000000;
        m_codecCtx->rc_max_rate  = config.maxBitrateMbps > 0
            ? static_cast<int64_t>(config.maxBitrateMbps) * 1000000
            : m_codecCtx->bit_rate * 2;
        m_codecCtx->rc_buffer_size = static_cast<int>(m_codecCtx->rc_max_rate);
    }

    if (config.bt709) {
        m_codecCtx->color_primaries = AVCOL_PRI_BT709;
        m_codecCtx->color_trc       = AVCOL_TRC_BT709;
        m_codecCtx->colorspace      = AVCOL_SPC_BT709;
    }

    if (!m_hwAccel) {
        static const char* presetNames[] = {
            "ultrafast", "superfast", "veryfast", "faster", "fast",
            "medium", "slow", "slower", "veryslow"
        };
        int idx = static_cast<int>(config.preset);
        if (idx < 9) av_opt_set(m_codecCtx->priv_data, "preset", presetNames[idx], 0);
        if (config.bitrateMbps == 0) {
            av_opt_set_int(m_codecCtx->priv_data, "crf", config.crf, 0);
        }
    } else {
        av_opt_set(m_codecCtx->priv_data, "preset", "p5", 0);
        av_opt_set(m_codecCtx->priv_data, "tune", "hq", 0);
        av_opt_set(m_codecCtx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(m_codecCtx->priv_data, "qp", config.crf, 0);
    }

    // SINGLE call to avcodec_open2.  pix_fmt is still YUV420P here.
    // If NVENC supports software input, avcodec_open2 accepts it and
    // pix_fmt stays YUV420P → software path works fine.
    // If NVENC insists on CUDA, avcodec_open2 changes pix_fmt to CUDA.
    int openRet = avcodec_open2(m_codecCtx, codec, nullptr);

    // Check if the pixel format was changed to a hardware format.
    // If so, NVENC needs CUDA frames → we can't easily support this
    // without a full HW upload path.  Fall back to CPU encoding.
    if (openRet >= 0 && m_codecCtx->pix_fmt != AV_PIX_FMT_YUV420P) {
        spdlog::warn("H264Encoder: NVENC changed pix_fmt to {} (CUDA/hardware) — "
                     "falling back to CPU encoding",
                     static_cast<int>(m_codecCtx->pix_fmt));
        avcodec_free_context(&m_codecCtx);
        m_hwAccel = false;
        // Retry with CPU encoder (libx264)
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec)
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            m_lastError = "H264Encoder: No H.264 encoder found";
            spdlog::error("{}", m_lastError);
            return false;
        }
        m_codecCtx = avcodec_alloc_context3(codec);
        if (!m_codecCtx) {
            m_lastError = "H264Encoder: Failed to allocate codec context";
            return false;
        }
        m_codecCtx->width     = static_cast<int>(config.width);
        m_codecCtx->height    = static_cast<int>(config.height);
        m_codecCtx->time_base = {config.fpsDen, config.fpsNum};
        m_codecCtx->framerate = {config.fpsNum, config.fpsDen};
        m_codecCtx->pix_fmt   = AV_PIX_FMT_YUV420P;
        m_codecCtx->gop_size  = config.gopSize > 0 ? config.gopSize : config.fpsNum * 2;
        m_codecCtx->max_b_frames = 2;
        if (config.bitrateMbps > 0) {
            m_codecCtx->bit_rate     = static_cast<int64_t>(config.bitrateMbps) * 1000000;
            m_codecCtx->rc_max_rate  = config.maxBitrateMbps > 0
                ? static_cast<int64_t>(config.maxBitrateMbps) * 1000000
                : m_codecCtx->bit_rate * 2;
            m_codecCtx->rc_buffer_size = static_cast<int>(m_codecCtx->rc_max_rate);
        }
        if (config.bt709) {
            m_codecCtx->color_primaries = AVCOL_PRI_BT709;
            m_codecCtx->color_trc       = AVCOL_TRC_BT709;
            m_codecCtx->colorspace      = AVCOL_SPC_BT709;
        }
        {
            static const char* presetNames[] = {
                "ultrafast", "superfast", "veryfast", "faster", "fast",
                "medium", "slow", "slower", "veryslow"
            };
            int idx2 = static_cast<int>(config.preset);
            if (idx2 < 9) av_opt_set(m_codecCtx->priv_data, "preset", presetNames[idx2], 0);
        }
        if (config.bitrateMbps == 0)
            av_opt_set_int(m_codecCtx->priv_data, "crf", config.crf, 0);
        if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
            m_lastError = "H264Encoder: Failed to open libx264";
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            return false;
        }
    } else if (openRet < 0) {
        m_lastError = "H264Encoder: Failed to open codec";
        spdlog::error("{}", m_lastError);
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    // ── Allocate a software YUV420P frame for sws_scale output ────
    m_frame = av_frame_alloc();
    m_frame->format = AV_PIX_FMT_YUV420P;
    m_frame->width  = static_cast<int>(config.width);
    m_frame->height = static_cast<int>(config.height);
    spdlog::info("H264Encoder: frame alloc wxh={}x{}  codecCtx wxh={}x{}  pix_fmt={} hw={}",
                 m_frame->width, m_frame->height,
                 m_codecCtx->width, m_codecCtx->height,
                 static_cast<int>(m_codecCtx->pix_fmt), m_hwAccel);
    av_frame_get_buffer(m_frame, 0);

    m_packet = av_packet_alloc();

    int swsW = static_cast<int>(config.width);
    int swsH = static_cast<int>(config.height);
    t_swsCtx.reset(sws_getContext(
        swsW, swsH, AV_PIX_FMT_RGBA,
        swsW, swsH, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!t_swsCtx) {
        m_lastError = "H264Encoder: Failed to create sws context";
        spdlog::error("{} pix_fmt={} wxh={}x{}", m_lastError,
                      static_cast<int>(m_codecCtx->pix_fmt),
                      m_codecCtx->width, m_codecCtx->height);
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_initialized = true;
    spdlog::info("H264Encoder: Initialized {}x{} @ {}/{} fps ({}) pix_fmt={}",
                 config.width, config.height, config.fpsNum, config.fpsDen,
                 m_hwAccel ? "NVENC" : "libx264",
                 static_cast<int>(m_codecCtx->pix_fmt));
    return true;
}

bool H264Encoder::encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex)
{
    if (!m_initialized) { spdlog::error("H264Enc: !initialized"); return false; }
    av_frame_make_writable(m_frame);

    const uint8_t* srcSlice[] = { rgbaPixels };
    int srcStride[] = { static_cast<int>(m_config.width * 4) };

    // Use m_frame->height (config height) NOT m_codecCtx->height.
    // NVENC may align height to 16px boundary, but the source RGBA
    // buffer only has the original (config) height.
    sws_scale(t_swsCtx.get(), srcSlice, srcStride, 0, m_frame->height,
              m_frame->data, m_frame->linesize);

    m_frame->pts = frameIndex;

    // ── HW encode path: upload software frame to GPU ──────────────────
    // m_codecCtx->hw_frames_ctx may be null if avcodec_open2 rejected
    // our hw_frames_ctx and reverted to software frames.  In that case
    // fall through to the software path below.
    if (m_hwAccel && m_codecCtx->hw_frames_ctx) {
        AVFrame* hwFrame = av_frame_alloc();
        hwFrame->format = AV_PIX_FMT_CUDA;
        hwFrame->width  = m_frame->width;
        hwFrame->height = m_frame->height;

        int ret = av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, hwFrame, 0);
        if (ret < 0) {
            spdlog::error("H264Encoder: Failed to get HW frame buffer");
            av_frame_free(&hwFrame);
            return false;
        }

        ret = av_hwframe_transfer_data(hwFrame, m_frame, 0);
        if (ret < 0) {
            spdlog::error("H264Encoder: Failed to upload frame to GPU");
            av_frame_free(&hwFrame);
            return false;
        }

        hwFrame->pts    = m_frame->pts;
        bool ok = sendFrame(hwFrame);
        av_frame_free(&hwFrame);
        return ok;
    }

    // ── SW encode path: send software frame directly ──────────────────
    return sendFrame(m_frame);
}

bool H264Encoder::sendFrame(AVFrame* frame)
{
    m_pendingPackets.clear();
    int ret = avcodec_send_frame(m_codecCtx, frame);
    if (ret < 0) { m_lastError = "H264Encoder: Error sending frame"; return false; }

    // Drain ALL available packets from the encoder.  The encoder may
    // produce multiple packets from one send (B-frame reordering) or
    // may produce none (EAGAIN — needs more frames).
    bool gotOne = false;
    while (true) {
        ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            m_lastError = "H264Encoder: receive error";
            return gotOne;
        }

        EncodedPacket ep;
        ep.data       = m_packet->data;
        ep.size       = m_packet->size;
        ep.pts        = m_packet->pts;
        ep.dts        = m_packet->dts;
        ep.duration   = (m_packet->duration > 0) ? m_packet->duration : 1;
        ep.isKeyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        ep.ownsData   = false;

        if (!gotOne) {
            // First packet goes to m_lastPacket for the caller
            m_lastPacket = ep;
            gotOne = true;
        } else {
            m_pendingPackets.push_back(ep);
        }
        ++m_framesEncoded;
    }
    return gotOne;
}

int H264Encoder::flush()
{
    if (!m_initialized) return 0;
    m_flushedPackets.clear();
    avcodec_send_frame(m_codecCtx, nullptr);

    int count = 0;
    while (avcodec_receive_packet(m_codecCtx, m_packet) == 0) {
        EncodedPacket ep;
        ep.data = m_packet->data; ep.size = m_packet->size;
        ep.pts = m_packet->pts; ep.dts = m_packet->dts;
        ep.duration = (m_packet->duration > 0) ? m_packet->duration : 1;
        ep.isKeyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        m_flushedPackets.push_back(ep);
        ++count; ++m_framesEncoded;
    }
    return count;
}

int H264Encoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_H264;
}

void H264Encoder::shutdown()
{
    t_swsCtx.reset();
    if (m_packet)   { av_packet_free(&m_packet); }
    if (m_frame)    { av_frame_free(&m_frame); }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); }
    if (m_hwFramesCtx) { av_buffer_unref(&m_hwFramesCtx); }
    if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); }
    m_packet = nullptr; m_frame = nullptr; m_codecCtx = nullptr;
    m_hwFramesCtx = nullptr; m_hwDeviceCtx = nullptr;
    m_initialized = false;
    m_flushedPackets.clear();
}

#else // !ROUNDTABLE_HAS_FFMPEG

H264Encoder::H264Encoder() = default;
H264Encoder::~H264Encoder() = default;
bool H264Encoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
bool H264Encoder::encodeFrame(const uint8_t*, int64_t) { return false; }
int  H264Encoder::flush() { return 0; }
void H264Encoder::shutdown() {}
bool H264Encoder::initCodec(const EncoderConfig&) { return false; }
bool H264Encoder::sendFrame(AVFrame*) { return false; }

#endif

} // namespace rt
