/*
 * AV1Encoder.cpp — AV1 encoding via FFmpeg.
 *
 * Tries av1_nvenc first (RTX 4090 native AV1), then libsvtav1 (CPU).
 */

#include "formats/AV1Encoder.h"

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
struct SwsDel { void operator()(SwsContext* c) { if (c) sws_freeContext(c); } };
thread_local std::unique_ptr<SwsContext, SwsDel> t_swsCtx;
}

AV1Encoder::AV1Encoder() = default;
AV1Encoder::~AV1Encoder() { shutdown(); }

bool AV1Encoder::init(const EncoderConfig& config)
{
    if (m_initialized) shutdown();
    m_config = config;
    m_framesEncoded = 0;
    m_hwAccel = false;
    return initCodec(config);
}

bool AV1Encoder::initCodec(const EncoderConfig& config)
{
    // ── Phase 1: Try NVENC with CUDA hardware frames ────────────
    if (config.hwAccel == HardwareAccel::NVENC) {
        const AVCodec* nvenc = avcodec_find_encoder_by_name("av1_nvenc");
        if (nvenc) {
            m_hwDeviceCtx = nullptr;
            int ret = av_hwdevice_ctx_create(&m_hwDeviceCtx,
                                              AV_HWDEVICE_TYPE_CUDA,
                                              nullptr, nullptr, 0);
            if (ret >= 0) {
                spdlog::info("AV1Encoder: CUDA device created, trying HW frames");
                m_codecCtx = avcodec_alloc_context3(nvenc);
                if (m_codecCtx) {
                    m_codecCtx->width       = static_cast<int>(config.width);
                    m_codecCtx->height      = static_cast<int>(config.height);
                    m_codecCtx->time_base   = {config.fpsDen, config.fpsNum};
                    m_codecCtx->framerate   = {config.fpsNum, config.fpsDen};
                    m_codecCtx->pix_fmt     = AV_PIX_FMT_CUDA;
                    m_codecCtx->gop_size    = config.gopSize > 0 ? config.gopSize : config.fpsNum * 2;
                    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);

                    // AV1 sequence header goes in the container, not inline.
                    m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                    if (config.bitrateMbps > 0) {
                        m_codecCtx->bit_rate = static_cast<int64_t>(config.bitrateMbps) * 1000000;
                    }
                    if (config.bt709) {
                        m_codecCtx->color_primaries = AVCOL_PRI_BT709;
                        m_codecCtx->color_trc       = AVCOL_TRC_BT709;
                        m_codecCtx->colorspace      = AVCOL_SPC_BT709;
                    }
                    av_opt_set(m_codecCtx->priv_data, "preset", "p5", 0);
                    av_opt_set(m_codecCtx->priv_data, "tune", "hq", 0);
                    av_opt_set(m_codecCtx->priv_data, "rc", "constqp", 0);
                    av_opt_set_int(m_codecCtx->priv_data, "qp", config.crf, 0);
                    // NVENC CUDA input needs an explicit hw_frames_ctx
                    // before open, else EINVAL (-22) → slow upload path.
                    int hwfErr = attachCudaHwFrames(
                        m_codecCtx, m_hwDeviceCtx,
                        static_cast<int>(config.width),
                        static_cast<int>(config.height));
                    ret = (hwfErr < 0)
                              ? hwfErr
                              : avcodec_open2(m_codecCtx, nvenc, nullptr);
                    if (ret >= 0 && m_codecCtx->hw_frames_ctx) {
                        m_hwAccel = true;
                        spdlog::info("AV1Encoder: NVENC with CUDA HW frames");
                    } else {
                        spdlog::warn("AV1Encoder: CUDA HW frames failed (ret={}), "
                                     "trying NVENC software input", ret);
                        avcodec_free_context(&m_codecCtx);
                        m_codecCtx = nullptr;
                    }
                }
            }
            // ── Phase 2: Try NVENC with software input (YUV420P) ──
            if (!m_codecCtx) {
                if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); m_hwDeviceCtx = nullptr; }
                m_codecCtx = avcodec_alloc_context3(nvenc);
                if (m_codecCtx) {
                    m_codecCtx->width       = static_cast<int>(config.width);
                    m_codecCtx->height      = static_cast<int>(config.height);
                    m_codecCtx->time_base   = {config.fpsDen, config.fpsNum};
                    m_codecCtx->framerate   = {config.fpsNum, config.fpsDen};
                    m_codecCtx->pix_fmt     = AV_PIX_FMT_YUV420P;
                    m_codecCtx->gop_size    = config.gopSize > 0 ? config.gopSize : config.fpsNum * 2;

                    // AV1 sequence header goes in the container, not inline.
                    m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                    if (config.bitrateMbps > 0) {
                        m_codecCtx->bit_rate = static_cast<int64_t>(config.bitrateMbps) * 1000000;
                    }
                    if (config.bt709) {
                        m_codecCtx->color_primaries = AVCOL_PRI_BT709;
                        m_codecCtx->color_trc       = AVCOL_TRC_BT709;
                        m_codecCtx->colorspace      = AVCOL_SPC_BT709;
                    }
                    av_opt_set(m_codecCtx->priv_data, "preset", "p5", 0);
                    av_opt_set(m_codecCtx->priv_data, "tune", "hq", 0);
                    av_opt_set(m_codecCtx->priv_data, "rc", "constqp", 0);
                    av_opt_set_int(m_codecCtx->priv_data, "qp", config.crf, 0);
                    int openRet = avcodec_open2(m_codecCtx, nvenc, nullptr);
                    if (openRet >= 0 && m_codecCtx->pix_fmt == AV_PIX_FMT_YUV420P) {
                        m_hwAccel = true;
                        spdlog::info("AV1Encoder: NVENC with software input (YUV420P)");
                    } else {
                        spdlog::warn("AV1Encoder: NVENC unusable, falling back to CPU");
                        avcodec_free_context(&m_codecCtx); m_codecCtx = nullptr;
                        m_hwAccel = false;
                    }
                }
            }
        }
    }

    // ── Phase 3: CPU fallback ──────────────────────────────────
    if (!m_codecCtx) {
        const AVCodec* codec = avcodec_find_encoder_by_name("libsvtav1");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_AV1);
        if (!codec) {
            m_lastError = "AV1Encoder: No AV1 encoder found";
            spdlog::error("{}", m_lastError);
            return false;
        }
        m_codecCtx = avcodec_alloc_context3(codec);
        if (!m_codecCtx) { m_lastError = "Failed to alloc context"; return false; }
        m_codecCtx->width       = static_cast<int>(config.width);
        m_codecCtx->height      = static_cast<int>(config.height);
        m_codecCtx->time_base   = {config.fpsDen, config.fpsNum};
        m_codecCtx->framerate   = {config.fpsNum, config.fpsDen};
        m_codecCtx->pix_fmt     = AV_PIX_FMT_YUV420P;
        m_codecCtx->gop_size    = config.gopSize > 0 ? config.gopSize : config.fpsNum * 2;

        // AV1 sequence header goes in the container, not inline.
        m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (config.bitrateMbps > 0) {
            m_codecCtx->bit_rate = static_cast<int64_t>(config.bitrateMbps) * 1000000;
        }
        if (config.bt709) {
            m_codecCtx->color_primaries = AVCOL_PRI_BT709;
            m_codecCtx->color_trc       = AVCOL_TRC_BT709;
            m_codecCtx->colorspace      = AVCOL_SPC_BT709;
        }
        {
            int svtPreset = 8;
            switch (config.preset) {
                case EncoderPreset::Ultrafast: svtPreset = 12; break;
                case EncoderPreset::Superfast: svtPreset = 11; break;
                case EncoderPreset::Veryfast:  svtPreset = 10; break;
                case EncoderPreset::Faster:    svtPreset = 9; break;
                case EncoderPreset::Fast:      svtPreset = 8; break;
                case EncoderPreset::Medium:    svtPreset = 6; break;
                case EncoderPreset::Slow:      svtPreset = 4; break;
                case EncoderPreset::Slower:    svtPreset = 2; break;
                case EncoderPreset::Veryslow:  svtPreset = 0; break;
                default: break;
            }
            av_opt_set_int(m_codecCtx->priv_data, "preset", svtPreset, 0);
        }
        if (config.bitrateMbps == 0)
            av_opt_set_int(m_codecCtx->priv_data, "crf", config.crf, 0);
        if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
            m_lastError = "AV1Encoder: Failed to open libsvtav1";
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            return false;
        }
        m_hwAccel = false;
        spdlog::info("AV1Encoder: Using CPU (libsvtav1)");
    }

    // ── Allocate software YUV420P frame ────────────────────────
    m_frame = av_frame_alloc();
    m_frame->format = AV_PIX_FMT_YUV420P;
    m_frame->width  = m_codecCtx->width;
    m_frame->height = m_codecCtx->height;
    av_frame_get_buffer(m_frame, 0);
    m_packet = av_packet_alloc();

    t_swsCtx.reset(sws_getContext(
        m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_BGRA,
        m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr));

    m_initialized = true;
    spdlog::info("AV1Encoder: {}x{} @ {}/{} fps ({})", config.width, config.height,
                 config.fpsNum, config.fpsDen, m_hwAccel ? "NVENC" : "libsvtav1");
    return true;
}

int AV1Encoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_AV1;
}

bool AV1Encoder::encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex)
{
    if (!m_initialized) return false;
    av_frame_make_writable(m_frame);

    const uint8_t* src[] = { rgbaPixels };
    int stride[] = { static_cast<int>(m_config.width * 4) };

    // Use min(config.height, m_frame->height) so we never read past
    // the caller's source buffer.  Extra lines stay zero (black).
    int srcLines = std::min(static_cast<int>(m_config.height), m_frame->height);
    sws_scale(t_swsCtx.get(), src, stride, 0, srcLines, m_frame->data, m_frame->linesize);
    m_frame->pts = frameIndex;

    // ── HW encode path: upload software frame to GPU ──────────────
    if (m_hwAccel && m_codecCtx->hw_frames_ctx) {
        AVFrame* hwFrame = av_frame_alloc();
        hwFrame->format = AV_PIX_FMT_CUDA;
        hwFrame->width  = m_frame->width;
        hwFrame->height = m_frame->height;

        int ret = av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, hwFrame, 0);
        if (ret < 0) {
            spdlog::error("AV1Encoder: Failed to get HW frame buffer");
            av_frame_free(&hwFrame);
            return false;
        }
        ret = av_hwframe_transfer_data(hwFrame, m_frame, 0);
        if (ret < 0) {
            spdlog::error("AV1Encoder: Failed to upload frame to GPU");
            av_frame_free(&hwFrame);
            return false;
        }
        hwFrame->pts = m_frame->pts;
        bool ok = sendFrame(hwFrame);
        av_frame_free(&hwFrame);
        return ok;
    }

    // ── SW encode path: send software frame directly ──────────
    return sendFrame(m_frame);
}

bool AV1Encoder::sendFrame(AVFrame* frame)
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

int AV1Encoder::flush()
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

void AV1Encoder::shutdown()
{
    t_swsCtx.reset();
    if (m_packet) av_packet_free(&m_packet);
    if (m_frame) av_frame_free(&m_frame);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
    if (m_hwFramesCtx) av_buffer_unref(&m_hwFramesCtx);
    m_packet = nullptr; m_frame = nullptr; m_codecCtx = nullptr;
    m_hwDeviceCtx = nullptr; m_hwFramesCtx = nullptr;
    m_initialized = false; m_flushedPackets.clear();
}

#else

AV1Encoder::AV1Encoder() = default;
AV1Encoder::~AV1Encoder() = default;
bool AV1Encoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
bool AV1Encoder::encodeFrame(const uint8_t*, int64_t) { return false; }
int  AV1Encoder::flush() { return 0; }
void AV1Encoder::shutdown() {}
bool AV1Encoder::initCodec(const EncoderConfig&) { return false; }
bool AV1Encoder::sendFrame(AVFrame*) { return false; }

#endif

} // namespace rt
