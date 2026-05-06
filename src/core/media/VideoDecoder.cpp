/*
 * VideoDecoder — FFmpeg-based video decoder with NVDEC hardware acceleration.
 *
 * Uses FFmpeg 7.x API (avcodec 61.x).
 * Hardware decode: AV_HWDEVICE_TYPE_CUDA → NVDEC on RTX 4090.
 */

#ifdef ROUNDTABLE_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "media/VideoDecoder.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace rt {

// ── Global user preference ──────────────────────────────────────────────
static bool s_forceSoftwareDecode = false;

void setForceSoftwareDecode(bool force) { s_forceSoftwareDecode = force; }
bool forceSoftwareDecode() { return s_forceSoftwareDecode; }

// ── Helpers ─────────────────────────────────────────────────────────────────

static AVPixelFormat getHwFormat(AVCodecContext* /*ctx*/,
                                  const enum AVPixelFormat* pixFmts)
{
    for (const AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == AV_PIX_FMT_CUDA || *p == AV_PIX_FMT_D3D11 || *p == AV_PIX_FMT_QSV)
            return *p;
    }
    return AV_PIX_FMT_NONE;
}

static PixelFormat convertPixFmt(AVPixelFormat fmt)
{
    switch (fmt)
    {
    case AV_PIX_FMT_NV12:    return PixelFormat::NV12;
    case AV_PIX_FMT_YUV420P: return PixelFormat::YUV420P;
    case AV_PIX_FMT_BGRA:    return PixelFormat::BGRA;
    case AV_PIX_FMT_RGBA:    return PixelFormat::RGBA;
    case AV_PIX_FMT_CUDA:    return PixelFormat::NV12; // NVDEC outputs NV12
    case AV_PIX_FMT_QSV:    return PixelFormat::NV12; // QSV outputs NV12
    default:                  return PixelFormat::Unknown;
    }
}

// ── Constructor / Destructor ────────────────────────────────────────────────

VideoDecoder::VideoDecoder()
{
    m_frame   = av_frame_alloc();
    m_hwFrame = av_frame_alloc();
    m_packet  = av_packet_alloc();

    if (!m_frame || !m_hwFrame || !m_packet) {
        spdlog::error("VideoDecoder: Failed to allocate AVFrame/AVPacket");
    }
}

VideoDecoder::~VideoDecoder()
{
    close();
    av_frame_free(&m_frame);
    av_frame_free(&m_hwFrame);
    av_packet_free(&m_packet);
}

// ── Open / Close ────────────────────────────────────────────────────────────

bool VideoDecoder::open(const std::filesystem::path& path, bool forceSoftware,
                        int maxThreads, bool sliceOnlyThreading)
{
    std::lock_guard lock(m_mutex);
    close();
    m_maxThreads = maxThreads;
    m_sliceOnlyThreading = sliceOnlyThreading;

    if (!m_frame || !m_hwFrame || !m_packet) {
        m_lastError = "Internal frame/packet allocation failed";
        spdlog::error("VideoDecoder: {}", m_lastError);
        return false;
    }

    std::string pathStr = path.string();

    // Open container
    int ret = avformat_open_input(&m_fmtCtx, pathStr.c_str(), nullptr, nullptr);
    if (ret < 0)
    {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        m_lastError = "Cannot open file: " + std::string(err);
        spdlog::error("VideoDecoder: {}", m_lastError);
        return false;
    }

    // Read stream info
    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0)
    {
        m_lastError = "Cannot find stream info";
        spdlog::error("VideoDecoder: {}", m_lastError);
        avformat_close_input(&m_fmtCtx);
        return false;
    }

    // Find video stream
    m_info.videoStreamIndex = -1;
    m_info.audioStreamIndex = -1;
    for (unsigned i = 0; i < m_fmtCtx->nb_streams; ++i)
    {
        if (m_fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
            && m_info.videoStreamIndex < 0)
        {
            m_info.videoStreamIndex = static_cast<int>(i);
        }
        else if (m_fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
                 && m_info.audioStreamIndex < 0)
        {
            m_info.audioStreamIndex = static_cast<int>(i);
            m_info.hasAudio = true;
        }
    }

    if (m_info.videoStreamIndex < 0)
    {
        // Audio-only file: populate what we can from the audio stream
        if (m_info.audioStreamIndex >= 0) {
            AVStream* as = m_fmtCtx->streams[m_info.audioStreamIndex];
            if (m_fmtCtx->duration > 0)
                m_info.duration = static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE;
            else if (as->duration > 0)
                m_info.duration = static_cast<double>(as->duration) * av_q2d(as->time_base);
            m_info.containerFormat = m_fmtCtx->iformat->name;
            m_audioOnly = true;
            m_isOpen = true;
            spdlog::info("VideoDecoder: audio-only file '{}' ({:.1f}s)",
                         pathStr, m_info.duration);
            return true;
        }
        m_lastError = "No video stream found";
        spdlog::error("VideoDecoder: {}", m_lastError);
        avformat_close_input(&m_fmtCtx);
        return false;
    }

    // Extract stream info
    AVStream* vs = m_fmtCtx->streams[m_info.videoStreamIndex];
    AVCodecParameters* codecpar = vs->codecpar;

    m_info.width = static_cast<uint32_t>(codecpar->width);
    m_info.height = static_cast<uint32_t>(codecpar->height);
    m_info.timebaseNum = vs->time_base.num;
    m_info.timebaseDen = vs->time_base.den;
    m_info.bitrate = codecpar->bit_rate;

    if (vs->avg_frame_rate.den > 0)
        m_info.fps = av_q2d(vs->avg_frame_rate);
    else if (vs->r_frame_rate.den > 0)
        m_info.fps = av_q2d(vs->r_frame_rate);
    else
        m_info.fps = 30.0; // fallback

    // ── VFR detection ────────────────────────────────────────────────
    // Compare avg_frame_rate (average FPS across entire file) with
    // r_frame_rate (codec-level real base framerate). If they differ
    // by more than 5%, the file likely has variable frame rate.
    // Common with screen recordings and phone videos.
    if (vs->avg_frame_rate.den > 0 && vs->r_frame_rate.den > 0) {
        const double avgFps  = av_q2d(vs->avg_frame_rate);
        const double realFps = av_q2d(vs->r_frame_rate);
        if (realFps > 0.0 && std::abs(avgFps - realFps) / realFps > 0.05) {
            m_info.isVFR = true;
            spdlog::warn("VideoDecoder: VFR detected — avg={:.2f} real={:.2f} ({})",
                         avgFps, realFps, pathStr);
        }
    }

    if (m_fmtCtx->duration > 0)
        m_info.duration = static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE;
    else if (vs->duration > 0)
        m_info.duration = static_cast<double>(vs->duration) * av_q2d(vs->time_base);

    // Prefer the container's nb_frames when available — it's the exact
    // number of packets in the stream.  The fallback (duration × fps)
    // can drift due to rounding: e.g. file written at 60fps but decoder
    // reads avg_frame_rate as 60.38fps → computed count overshoots the
    // actual packet count, causing decode failures for non-existent
    // trailing frames.
    if (vs->nb_frames > 0)
        m_info.frameCount = vs->nb_frames;
    else
        m_info.frameCount = static_cast<int64_t>(m_info.duration * m_info.fps);
    m_info.containerFormat = m_fmtCtx->iformat->name;

    // Resolve codec name early for diagnostics
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    m_info.codecName = codec ? codec->name : "unknown";

    // Check if the video has an alpha channel — if so, NVDEC will silently
    // drop it (hw decoders output NV12/YUV420P, no alpha plane).
    // Detect alpha from: pixel format with alpha (yuva*), or WebM alpha_mode metadata.
    bool hasAlpha = false;
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(
            static_cast<AVPixelFormat>(codecpar->format));
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA))
            hasAlpha = true;

        // Also check WebM alpha_mode metadata
        AVDictionaryEntry* alphaMode = av_dict_get(vs->metadata, "alpha_mode", nullptr, 0);
        if (alphaMode && std::string(alphaMode->value) == "1")
            hasAlpha = true;

        // ProRes 4444 codec tags (ap4h / ap4x) always carry alpha even
        // if codecpar->format hasn't been updated to a YUVA format yet.
        if (codecpar->codec_id == AV_CODEC_ID_PRORES) {
            uint32_t tag = codecpar->codec_tag;
            // 'ap4h' = ProRes 4444, 'ap4x' = ProRes 4444 XQ
            if (tag == MKTAG('a','p','4','h') || tag == MKTAG('a','p','4','x'))
                hasAlpha = true;
        }
    }

    m_info.hasAlpha = hasAlpha;

    // Check for packed-alpha metadata (written by HWAlphaEncoder).
    // Packed-alpha videos are standard HEVC/H264 at 2× height with
    // RGB in the top half and alpha-as-greyscale in the bottom half.
    // These do NOT have native alpha — NVDEC can decode them normally.
    {
        AVDictionaryEntry* pa = av_dict_get(m_fmtCtx->metadata, "packed_alpha", nullptr, 0);
        if (pa && std::string(pa->value) == "1") {
            m_info.packedAlpha = true;
            spdlog::info("VideoDecoder: '{}' is packed-alpha ({}x{}, nominal {}x{})",
                         path.filename().string(), m_info.width, m_info.height,
                         m_info.width, m_info.height / 2);
        }
        // Heuristic fallback: if height > 2× width and HEVC/H264 codec,
        // assume packed-alpha even without metadata (some MOV containers
        // lose custom metadata during remux).
        if (!m_info.packedAlpha && !hasAlpha &&
            m_info.height > 0 && m_info.width > 0 &&
            m_info.height >= m_info.width * 2 &&
            (codecpar->codec_id == AV_CODEC_ID_HEVC ||
             codecpar->codec_id == AV_CODEC_ID_H264)) {
            m_info.packedAlpha = true;
            spdlog::info("VideoDecoder: '{}' heuristic packed-alpha ({}x{}, "
                         "height >= 2*width, codec={})",
                         path.filename().string(), m_info.width, m_info.height,
                         avcodec_get_name(codecpar->codec_id));
        }
    }

    if (hasAlpha)
    {
        spdlog::debug("VideoDecoder: '{}' has alpha channel — forcing software decode "
                      "(NVDEC drops alpha)", path.filename().string());

        // The native vp8/vp9 decoders in some FFmpeg builds do NOT handle
        // WebM alpha (BlockAdditional data).  The libvpx / libvpx-vp9
        // decoders process alpha correctly and produce yuva420p frames.
        // Force libvpx decoder when alpha is detected for VP8/VP9.
        const char* preferredDecoder = nullptr;
        if (codecpar->codec_id == AV_CODEC_ID_VP9)
            preferredDecoder = "libvpx-vp9";
        else if (codecpar->codec_id == AV_CODEC_ID_VP8)
            preferredDecoder = "libvpx";

        if (!initSoftwareDecoder(preferredDecoder))
        {
            avformat_close_input(&m_fmtCtx);
            return false;
        }
    }
    else if (forceSoftware)
    {
        // Caller explicitly requested software decode (e.g. prefetch workers
        // that must not consume NVDEC sessions).
        spdlog::debug("VideoDecoder: '{}' force-software (no NVDEC session)",
                      path.filename().string());
        if (!initSoftwareDecoder())
        {
            avformat_close_input(&m_fmtCtx);
            return false;
        }
    }
    else
    {
        // Check user preference: if "Software only" was selected in Preferences,
        // skip hardware decode even when CUDA is available.
        if (forceSoftwareDecode()) {
            spdlog::info("VideoDecoder: '{}' — user preference is software-only",
                         path.filename().string());
            if (!initSoftwareDecoder()) {
                avformat_close_input(&m_fmtCtx);
                return false;
            }
            goto done_open;
        }

        // Try hardware decode: NVDEC/CUDA → D3D11VA → Intel QSV → software
        if (!initHardwareDecoder(AV_HWDEVICE_TYPE_CUDA))
        {
            spdlog::info("VideoDecoder: NVDEC/CUDA not available for codec '{}', trying D3D11VA",
                         m_info.codecName);
            if (!initHardwareDecoder(AV_HWDEVICE_TYPE_D3D11VA))
            {
                spdlog::info("VideoDecoder: D3D11VA not available for codec '{}', trying Intel QSV",
                             m_info.codecName);
                if (!initHardwareDecoder(AV_HWDEVICE_TYPE_QSV))
                {
                    spdlog::info("VideoDecoder: no GPU decoder for codec '{}' — using software decode",
                                 m_info.codecName);
                    if (!initSoftwareDecoder())
                    {
                        avformat_close_input(&m_fmtCtx);
                        return false;
                    }
                }
            }
        }
    }
done_open:

    m_isOpen = true;
    m_currentFrame = 0;
    spdlog::info("VideoDecoder: opened '{}' — {}x{} @ {:.2f}fps, {:.1f}s, codec={}, hw={}",
                 path.filename().string(), m_info.width, m_info.height,
                 m_info.fps, m_info.duration, m_info.codecName,
                 m_hwAccel ? "GPU-accelerated" : "software");
    return true;
}

void VideoDecoder::close()
{
    // No mutex lock — caller holds lock, or called from destructor
    m_isOpen = false;
    m_hwAccel = false;
    m_currentFrame = 0;
    m_hwPixFmt = -1;

    if (m_codecCtx)
    {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_fmtCtx)
    {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    if (m_hwDeviceCtx)
    {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }

    m_info = {};
}

bool VideoDecoder::isOpen() const noexcept
{
    return m_isOpen;
}

const VideoStreamInfo& VideoDecoder::info() const noexcept
{
    return m_info;
}

// ── Hardware / Software init ────────────────────────────────────────────────

bool VideoDecoder::initHardwareDecoder(int deviceTypeInt)
{
    auto deviceType = static_cast<AVHWDeviceType>(deviceTypeInt);
    AVStream* vs = m_fmtCtx->streams[m_info.videoStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) return false;

    // Determine the expected pixel format for this device type
    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
    if (deviceType == AV_HWDEVICE_TYPE_CUDA)
        hwPixFmt = AV_PIX_FMT_CUDA;
    else if (deviceType == AV_HWDEVICE_TYPE_D3D11VA)
        hwPixFmt = AV_PIX_FMT_D3D11;
    else if (deviceType == AV_HWDEVICE_TYPE_QSV)
        hwPixFmt = AV_PIX_FMT_QSV;

    // Check if this codec supports the requested hw device
    bool hasSupport = false;
    for (int i = 0;; ++i)
    {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
            && config->device_type == deviceType)
        {
            hasSupport = true;
            if (config->pix_fmt != AV_PIX_FMT_NONE)
                hwPixFmt = config->pix_fmt;
            break;
        }
    }

    // If the default decoder doesn't support this hw device, try the
    // dedicated hw-accelerated decoder (e.g. av1_cuvid, h264_qsv).
    // Critical for AV1 where libdav1d is the default and has no hw support.
    if (!hasSupport && (deviceType == AV_HWDEVICE_TYPE_CUDA || deviceType == AV_HWDEVICE_TYPE_QSV))
    {
        const char* suffix = (deviceType == AV_HWDEVICE_TYPE_CUDA) ? "cuvid" : "qsv";
        const char* codecPrefix = nullptr;
        switch (vs->codecpar->codec_id)
        {
        case AV_CODEC_ID_AV1:        codecPrefix = "av1";   break;
        case AV_CODEC_ID_H264:       codecPrefix = "h264";  break;
        case AV_CODEC_ID_HEVC:       codecPrefix = "hevc";  break;
        case AV_CODEC_ID_VP9:        codecPrefix = "vp9";   break;
        case AV_CODEC_ID_VP8:        codecPrefix = "vp8";   break;
        case AV_CODEC_ID_MPEG2VIDEO: codecPrefix = "mpeg2"; break;
        case AV_CODEC_ID_VC1:        codecPrefix = "vc1";   break;
        default: break;
        }

        const char* hwDecName = nullptr;
        if (codecPrefix)
        {
            char nameBuf[32];
            snprintf(nameBuf, sizeof(nameBuf), "%s_%s", codecPrefix, suffix);
            hwDecName = avcodec_find_decoder_by_name(nameBuf) ? nameBuf : nullptr;
        }
        else if (deviceType == AV_HWDEVICE_TYPE_CUDA)
        {
            switch (vs->codecpar->codec_id)
            {
            case AV_CODEC_ID_MPEG1VIDEO: hwDecName = "mpeg1_cuvid"; break;
            case AV_CODEC_ID_MPEG4:      hwDecName = "mpeg4_cuvid"; break;
            default: break;
            }
        }

        if (hwDecName)
        {
            const AVCodec* hwCodec = avcodec_find_decoder_by_name(hwDecName);
            if (hwCodec)
            {
                for (int i = 0;; ++i)
                {
                    const AVCodecHWConfig* config = avcodec_get_hw_config(hwCodec, i);
                    if (!config) break;
                    if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
                        && config->device_type == deviceType)
                    {
                        hasSupport = true;
                        codec = hwCodec;
                        if (config->pix_fmt != AV_PIX_FMT_NONE)
                            hwPixFmt = config->pix_fmt;
                        spdlog::info("VideoDecoder: using dedicated hw decoder '{}' for accel",
                                     hwDecName);
                        break;
                    }
                }
            }
        }
    }

    if (!hasSupport)
    {
        spdlog::debug("VideoDecoder: codec '{}' has no hw config for device type {}",
                      codec->name, static_cast<int>(deviceType));
        return false;
    }

    // Create hw device context
    int ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, deviceType,
                                      nullptr, nullptr, 0);
    if (ret < 0)
    {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        spdlog::warn("VideoDecoder: failed to create hw device context (type {}): {}",
                     static_cast<int>(deviceType), err);
        return false;
    }

    // Store the expected hw pixel format for get_format callback
    m_hwPixFmt = hwPixFmt;

    // Create codec context
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx)
    {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
        return false;
    }

    ret = avcodec_parameters_to_context(m_codecCtx, vs->codecpar);
    if (ret < 0)
    {
        avcodec_free_context(&m_codecCtx);
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
        return false;
    }

    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    m_codecCtx->get_format = getHwFormat;

    // Multi-threaded decode (even with hw accel, threading helps with parsing)
    m_codecCtx->thread_count = 0; // auto

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0)
    {
        avcodec_free_context(&m_codecCtx);
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
        m_hwPixFmt = AV_PIX_FMT_NONE;
        return false;
    }

    m_hwAccel = true;
    const char* typeName = (deviceType == AV_HWDEVICE_TYPE_CUDA)   ? "NVDEC/CUDA" :
                           (deviceType == AV_HWDEVICE_TYPE_D3D11VA) ? "D3D11VA"    :
                           (deviceType == AV_HWDEVICE_TYPE_QSV)     ? "Intel QSV"  :
                                                                      "hardware";
    spdlog::info("VideoDecoder: {} acceleration enabled for codec '{}'",
                 typeName, codec->name);
    return true;
}

bool VideoDecoder::initSoftwareDecoder(const char* preferredDecoderName)
{
    AVStream* vs = m_fmtCtx->streams[m_info.videoStreamIndex];

    // Try the preferred decoder first (e.g. "libvpx-vp9" for alpha), fall
    // back to the default decoder for the codec ID.
    const AVCodec* codec = nullptr;
    if (preferredDecoderName)
    {
        codec = avcodec_find_decoder_by_name(preferredDecoderName);
        if (codec)
            spdlog::info("VideoDecoder: using preferred decoder '{}'", preferredDecoderName);
        else
            spdlog::warn("VideoDecoder: preferred decoder '{}' not found, using default",
                         preferredDecoderName);
    }
    if (!codec)
        codec = avcodec_find_decoder(vs->codecpar->codec_id);

    if (!codec)
    {
        m_lastError = "No decoder found for codec";
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    int ret = avcodec_parameters_to_context(m_codecCtx, vs->codecpar);
    if (ret < 0)
    {
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    // Slice-level threading — avoid FF_THREAD_FRAME which buffers
    // N frames internally and requires a complex push/pull loop to avoid
    // packet loss.  m_maxThreads limits CPU consumption for prefetch
    // workers (prevents 4 workers × all-cores from saturating the CPU
    // and starving the main thread's sws_scale).
    m_codecCtx->thread_count = m_maxThreads;  // 0 = auto
    // Prefetch workers (maxThreads > 0) use frame+slice threading for
    // maximum H.264 throughput on sequential decode.  Scrub decoders
    // and other seek-heavy paths MUST use slice-only to avoid H.264
    // reference frame corruption after avcodec_flush_buffers.
    if (m_sliceOnlyThreading) {
        m_codecCtx->thread_type = FF_THREAD_SLICE;
    } else {
        m_codecCtx->thread_type = (m_maxThreads > 0)
            ? (FF_THREAD_FRAME | FF_THREAD_SLICE) : FF_THREAD_SLICE;
    }

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0)
    {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        m_lastError = "Cannot open codec: " + std::string(err);
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_hwAccel = false;
    return true;
}

// ── Seek ────────────────────────────────────────────────────────────────────

bool VideoDecoder::seek(double timeSeconds, SeekMode mode)
{
    std::lock_guard lock(m_mutex);
    if (!m_isOpen) return false;

    AVStream* vs = m_fmtCtx->streams[m_info.videoStreamIndex];

    // Convert seconds → stream timebase
    int64_t targetTs = static_cast<int64_t>(timeSeconds / av_q2d(vs->time_base));

    int flags = AVSEEK_FLAG_BACKWARD; // Always seek to keyframe before target
    int ret = av_seek_frame(m_fmtCtx, m_info.videoStreamIndex, targetTs, flags);
    if (ret < 0)
    {
        // Try container-level seek
        int64_t tsAv = static_cast<int64_t>(timeSeconds * AV_TIME_BASE);
        ret = av_seek_frame(m_fmtCtx, -1, tsAv, flags);
        if (ret < 0)
        {
            m_lastError = "Seek failed";
            return false;
        }
    }

    avcodec_flush_buffers(m_codecCtx);
    m_draining = false; // Reset drain state after seek

    // For precise seeking, decode forward until we reach the target frame
    if (mode == SeekMode::Precise)
    {
        DecodedFrame dummy;
        while (true)
        {
            int rdret = av_read_frame(m_fmtCtx, m_packet);
            if (rdret < 0) break;
            if (m_packet->stream_index != m_info.videoStreamIndex) {
                av_packet_unref(m_packet);
                continue;
            }
            bool ok = decodePacket(m_packet, dummy);
            av_packet_unref(m_packet);
            if (ok && dummy.timestamp >= timeSeconds - (0.5 / m_info.fps))
                break;
        }
        // Flush decoder so it can accept new packets from decodeNext()
        avcodec_flush_buffers(m_codecCtx);
    }

    m_currentFrame = secondsToFrame(timeSeconds);
    return true;
}

bool VideoDecoder::seekToFrame(int64_t frameNumber, SeekMode mode)
{
    double time = frameToSeconds(frameNumber);
    return seek(time, mode);
}

// ── Decode ──────────────────────────────────────────────────────────────────

bool VideoDecoder::decodeNext(DecodedFrame& outFrame)
{
    std::lock_guard lock(m_mutex);
    if (!m_isOpen) return false;

    // FFmpeg decode is asynchronous (push packets → pull frames).
    // We always try receive_frame FIRST to avoid send_packet EAGAIN
    // (buffer full → packet loss → frame drops / decoder corruption).

    while (true)
    {
        // ── Step 1: Try to receive a decoded frame ──────────────────
        int ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == 0) {
            fillFrameInfo(outFrame, m_frame);
            return true;
        }
        if (ret == AVERROR_EOF) {
            // Decoder fully drained — no more frames
            m_draining = false;
            return false;
        }
        // AVERROR(EAGAIN): decoder needs more input — fall through

        // ── Step 2: If draining, nothing more to send ───────────────
        if (m_draining) {
            // receive_frame returned EAGAIN during drain — shouldn't
            // happen, but treat as done to avoid infinite loop.
            m_draining = false;
            return false;
        }

        // ── Step 3: Read next packet from container ─────────────────
        ret = av_read_frame(m_fmtCtx, m_packet);
        if (ret < 0)
        {
            if (ret == AVERROR_EOF) {
                // End of stream — signal decoder to flush buffered frames.
                avcodec_send_packet(m_codecCtx, nullptr);
                m_draining = true;
                continue;  // loop back to receive drained frames
            }
            return false;
        }

        if (m_packet->stream_index != m_info.videoStreamIndex)
        {
            av_packet_unref(m_packet);
            continue;
        }

        // ── Step 4: Send packet to decoder ──────────────────────────
        ret = avcodec_send_packet(m_codecCtx, m_packet);
        av_packet_unref(m_packet);

        if (ret == AVERROR(EAGAIN)) {
            // send_packet EAGAIN after receive_frame EAGAIN — this
            // should not happen with the receive-first pattern, but
            // log it for diagnostics.  The packet is lost.
            spdlog::warn("VideoDecoder: send_packet EAGAIN after receive EAGAIN "
                         "— possible packet loss");
        } else if (ret < 0 && ret != AVERROR_EOF) {
            return false;
        }
        // Loop back to step 1 (receive the decoded frame)
    }
}

bool VideoDecoder::decodeAt(double timeSeconds, DecodedFrame& outFrame)
{
    if (!seek(timeSeconds, SeekMode::Precise))
        return false;
    return decodeNext(outFrame);
}

bool VideoDecoder::decodePacket(AVPacket* packet, DecodedFrame& outFrame)
{
    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        return false;

    ret = avcodec_receive_frame(m_codecCtx, m_frame);
    if (ret < 0)
        return false;

    fillFrameInfo(outFrame, m_frame);
    return true;
}

void VideoDecoder::fillFrameInfo(DecodedFrame& outFrame, AVFrame* frame)
{
    outFrame.width = static_cast<uint32_t>(frame->width);
    outFrame.height = static_cast<uint32_t>(frame->height);
    outFrame.pts = frame->pts;
    outFrame.timestamp = ptsToSeconds(frame->pts);
    outFrame.frameIndex = m_currentFrame++;
    outFrame.isKeyframe = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
    outFrame.avFrame = frame;

    if (frame->format == AV_PIX_FMT_CUDA || frame->format == AV_PIX_FMT_D3D11)
    {
        // Hardware frame — data is on GPU.
        outFrame.isHardware = true;
        outFrame.format = PixelFormat::NV12;
        outFrame.rawFormat = frame->format;

        // For CUDA frames, data[0] = Y plane device ptr, data[1] = UV plane
        // device ptr, linesize[0] = Y pitch, linesize[1] = UV pitch.
        // These are needed by the CUDA→Vulkan zero-copy path.
        if (frame->format == AV_PIX_FMT_CUDA && frame->data[0]) {
            for (int i = 0; i < 4; ++i) {
                outFrame.data[i] = frame->data[i];
                outFrame.linesize[i] = frame->linesize[i];
            }
        } else {
            std::memset(outFrame.data, 0, sizeof(outFrame.data));
            std::memset(outFrame.linesize, 0, sizeof(outFrame.linesize));
        }
    }
    else
    {
        // Software frame — data is in CPU memory
        outFrame.isHardware = false;
        outFrame.format = convertPixFmt(static_cast<AVPixelFormat>(frame->format));
        outFrame.rawFormat = frame->format;
        for (int i = 0; i < 4; ++i)
        {
            outFrame.data[i] = frame->data[i];
            outFrame.linesize[i] = frame->linesize[i];
        }
    }
}

// ── Hardware frame transfer ─────────────────────────────────────────────────

bool VideoDecoder::transferHardwareFrame(const DecodedFrame& hwFrame,
                                          DecodedFrame& cpuFrame)
{
    if (!hwFrame.isHardware || !hwFrame.avFrame)
        return false;

    int ret = av_hwframe_transfer_data(m_hwFrame, hwFrame.avFrame, 0);
    if (ret < 0)
    {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        m_lastError = "HW transfer failed: " + std::string(err);
        return false;
    }

    fillFrameInfo(cpuFrame, m_hwFrame);
    cpuFrame.isHardware = false;
    return true;
}

// ── Utilities ───────────────────────────────────────────────────────────────

bool VideoDecoder::isHardwareAccelerated() const noexcept
{
    return m_hwAccel;
}

const std::string& VideoDecoder::lastError() const noexcept
{
    return m_lastError;
}

int64_t VideoDecoder::secondsToFrame(double seconds) const noexcept
{
    if (m_info.fps <= 0) return 0;
    return static_cast<int64_t>(seconds * m_info.fps + 0.5);
}

double VideoDecoder::frameToSeconds(int64_t frame) const noexcept
{
    if (m_info.fps <= 0) return 0.0;
    return static_cast<double>(frame) / m_info.fps;
}

double VideoDecoder::ptsToSeconds(int64_t pts) const noexcept
{
    if (m_info.timebaseDen <= 0) return 0.0;
    return static_cast<double>(pts) * m_info.timebaseNum / m_info.timebaseDen;
}

} // namespace rt

#else // !ROUNDTABLE_HAS_FFMPEG

// ── Stub implementations when FFmpeg is not available ───────────────────────
#include "media/VideoDecoder.h"
#include <spdlog/spdlog.h>

namespace rt {

VideoDecoder::VideoDecoder() {}
VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(const std::filesystem::path&, bool /*forceSoftware*/,
                        int /*maxThreads*/, bool /*sliceOnlyThreading*/)
{
    m_lastError = "FFmpeg not available — video decode disabled";
    spdlog::error("VideoDecoder: {}", m_lastError);
    return false;
}

void VideoDecoder::close() { m_isOpen = false; }
bool VideoDecoder::isOpen() const noexcept { return false; }
const VideoStreamInfo& VideoDecoder::info() const noexcept { return m_info; }
bool VideoDecoder::seek(double, SeekMode) { return false; }
bool VideoDecoder::seekToFrame(int64_t, SeekMode) { return false; }
bool VideoDecoder::decodeNext(DecodedFrame&) { return false; }
bool VideoDecoder::decodeAt(double, DecodedFrame&) { return false; }
bool VideoDecoder::isHardwareAccelerated() const noexcept { return false; }
bool VideoDecoder::transferHardwareFrame(const DecodedFrame&, DecodedFrame&) { return false; }
const std::string& VideoDecoder::lastError() const noexcept { return m_lastError; }
int64_t VideoDecoder::secondsToFrame(double) const noexcept { return 0; }
double VideoDecoder::frameToSeconds(int64_t) const noexcept { return 0.0; }
double VideoDecoder::ptsToSeconds(int64_t) const noexcept { return 0.0; }

// Private stubs
bool VideoDecoder::initHardwareDecoder(int) { return false; }
bool VideoDecoder::initSoftwareDecoder(const char*) { return false; }
bool VideoDecoder::decodePacket(AVPacket*, DecodedFrame&) { return false; }
void VideoDecoder::fillFrameInfo(DecodedFrame&, AVFrame*) {}

} // namespace rt

#endif // ROUNDTABLE_HAS_FFMPEG

