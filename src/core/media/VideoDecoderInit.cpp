/*
 * VideoDecoderInit.cpp — Open/close and decoder initialization for VideoDecoder.
 * Extracted from VideoDecoder.cpp for maintainability.
 */

#ifdef ROUNDTABLE_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "media/VideoDecoder.h"
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cstring>
#include <cmath>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <windows.h>
#endif

namespace rt {

// ── Shared-mode file open (the source-file un-locking machinery) ────────
//
// FFmpeg's default file: protocol opens with the platform-default sharing
// mode. On Windows that DENIES other processes write/delete while we have
// the handle open — which we do for as long as the decoder is cached in
// MediaPool. The user can't replace a .png in Explorer or delete a
// finished .mp4 because of us. Premiere keeps the lock cooperative; we
// match that by opening the file ourselves with FILE_SHARE_READ |
// FILE_SHARE_WRITE | FILE_SHARE_DELETE and feeding FFmpeg the bytes via a
// custom AVIOContext. On POSIX, fopen() doesn't take exclusive locks at
// all — the call here collapses to a plain fopen and is a no-op.

namespace {

constexpr int kAvioBufSize = 32 * 1024;   // FFmpeg recommended

// On Windows we bypass the CRT entirely and drive Win32 ReadFile /
// SetFilePointerEx directly from the AVIO callbacks. _open_osfhandle +
// _fdopen LOOK like a clean wrapper around our shared-mode HANDLE, but
// some MSVCRT builds silently re-acquire the share state when the CRT
// touches the descriptor, which would defeat FILE_SHARE_DELETE. Using
// the raw HANDLE end-to-end guarantees the share mode set at CreateFile
// time is what the kernel sees for the entire lifetime of the open.
// On POSIX, FILE* is fine (no exclusive sharing happens there).

#ifdef _WIN32
using SharedFileHandle = HANDLE;
// const (not constexpr): INVALID_HANDLE_VALUE casts (LONG_PTR)-1 to a
// pointer, which isn't a core constant expression under MSVC C++17/20.
const SharedFileHandle kInvalidShared = INVALID_HANDLE_VALUE;
#else
using SharedFileHandle = FILE*;
constexpr SharedFileHandle kInvalidShared = nullptr;
#endif

SharedFileHandle openSharedRead(const std::filesystem::path& p)
{
#ifdef _WIN32
    HANDLE h = ::CreateFileW(p.wstring().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    // Share-mode self-test. If h was really opened with FILE_SHARE_DELETE,
    // we can immediately reopen the SAME file from our own process with
    // DELETE access (kernel-enforced). If this fails, our first open
    // didn't actually get FILE_SHARE_DELETE — that's a bug we own. If it
    // succeeds, the file is shared-delete-able and any "locked" complaint
    // from Explorer must be coming from a different process or a Windows
    // service (Defender, indexer, etc.).
    if (h != INVALID_HANDLE_VALUE) {
        HANDLE h2 = ::CreateFileW(p.wstring().c_str(),
            DELETE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h2 != INVALID_HANDLE_VALUE) {
            spdlog::info("VideoDecoder: share-mode self-test PASSED '{}'",
                         p.string());
            ::CloseHandle(h2);
        } else {
            DWORD err = ::GetLastError();
            spdlog::warn("VideoDecoder: share-mode self-test FAILED '{}' "
                         "GetLastError={} — FILE_SHARE_DELETE not active",
                         p.string(), err);
        }
    }
    return h;
#else
    return std::fopen(p.string().c_str(), "rb");
#endif
}

void closeShared(SharedFileHandle h)
{
    if (h == kInvalidShared) return;
#ifdef _WIN32
    ::CloseHandle(h);
#else
    std::fclose(h);
#endif
}

int avioReadShared(void* opaque, uint8_t* buf, int bufSize)
{
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(opaque);
    DWORD got = 0;
    if (!::ReadFile(h, buf, static_cast<DWORD>(bufSize), &got, nullptr))
        return AVERROR(EIO);
    if (got == 0) return AVERROR_EOF;
    return static_cast<int>(got);
#else
    FILE* fp = static_cast<FILE*>(opaque);
    size_t n = std::fread(buf, 1, static_cast<size_t>(bufSize), fp);
    if (n == 0) {
        if (std::feof(fp)) return AVERROR_EOF;
        return AVERROR(errno ? errno : EIO);
    }
    return static_cast<int>(n);
#endif
}

int64_t avioSeekShared(void* opaque, int64_t offset, int whence)
{
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(opaque);
    if (whence == AVSEEK_SIZE) {
        LARGE_INTEGER sz;
        if (!::GetFileSizeEx(h, &sz)) return AVERROR(EIO);
        return sz.QuadPart;
    }
    DWORD mode = FILE_BEGIN;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: mode = FILE_BEGIN;   break;
        case SEEK_CUR: mode = FILE_CURRENT; break;
        case SEEK_END: mode = FILE_END;     break;
        default: return AVERROR(EINVAL);
    }
    LARGE_INTEGER off;    off.QuadPart    = offset;
    LARGE_INTEGER newPos; newPos.QuadPart = 0;
    if (!::SetFilePointerEx(h, off, &newPos, mode))
        return AVERROR(EIO);
    return newPos.QuadPart;
#else
    FILE* fp = static_cast<FILE*>(opaque);
    if (whence == AVSEEK_SIZE) {
        long cur = std::ftell(fp);
        if (std::fseek(fp, 0, SEEK_END) != 0) return AVERROR(errno);
        long sz = std::ftell(fp);
        std::fseek(fp, cur, SEEK_SET);
        return sz < 0 ? AVERROR(errno) : sz;
    }
    int stdWhence = SEEK_SET;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: stdWhence = SEEK_SET; break;
        case SEEK_CUR: stdWhence = SEEK_CUR; break;
        case SEEK_END: stdWhence = SEEK_END; break;
        default: return AVERROR(EINVAL);
    }
    if (std::fseek(fp, static_cast<long>(offset), stdWhence) != 0)
        return AVERROR(errno);
    return std::ftell(fp);
#endif
}

} // namespace

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
    case AV_PIX_FMT_CUDA:    return PixelFormat::NV12;
    case AV_PIX_FMT_QSV:    return PixelFormat::NV12;
    default:                  return PixelFormat::Unknown;
    }
}

// ── Open ────────────────────────────────────────────────────────────────────

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

    // ── Open the file ourselves in shared mode (read | write | delete)
    //    and feed FFmpeg via a custom AVIOContext. This is what stops
    //    Explorer from refusing overwrite/delete on the source file.
    SharedFileHandle sh = openSharedRead(path);
    if (sh == kInvalidShared) {
        m_lastError = "Cannot open file (shared mode): " + pathStr;
        spdlog::error("VideoDecoder: {}", m_lastError);
        return false;
    }
    m_avioFile = static_cast<void*>(sh);
    // Diagnostic: confirms the shared-mode AVIO path was actually used
    // (vs. some older binary or fallback). One line per real open so the
    // log stays readable; visible in logs/perf_log.txt.
    spdlog::info("VideoDecoder: shared-mode open '{}' (FILE_SHARE_READ|WRITE|DELETE, raw HANDLE)",
                 pathStr);

    auto* buf = static_cast<uint8_t*>(av_malloc(kAvioBufSize));
    if (!buf) {
        closeShared(sh);
        m_avioFile = nullptr;
        m_lastError = "av_malloc(AVIO buffer) failed";
        spdlog::error("VideoDecoder: {}", m_lastError);
        return false;
    }
    m_avioBuf = buf;

    m_avioCtx = avio_alloc_context(buf, kAvioBufSize,
                                   /*write_flag=*/0,
                                   /*opaque=*/static_cast<void*>(sh),
                                   &avioReadShared,
                                   /*write_packet=*/nullptr,
                                   &avioSeekShared);
    if (!m_avioCtx) {
        av_free(buf);
        m_avioBuf = nullptr;
        closeShared(sh);
        m_avioFile = nullptr;
        m_lastError = "avio_alloc_context failed";
        spdlog::error("VideoDecoder: {}", m_lastError);
        return false;
    }

    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) {
        av_freep(&m_avioCtx->buffer);
        avio_context_free(&m_avioCtx);
        m_avioBuf = nullptr;
        closeShared(sh);
        m_avioFile = nullptr;
        m_lastError = "avformat_alloc_context failed";
        spdlog::error("VideoDecoder: {}", m_lastError);
        return false;
    }
    m_fmtCtx->pb    = m_avioCtx;
    m_fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Open container. m_fmtCtx->pb is set so FFmpeg uses our AVIO callbacks
    // exclusively — the filename is only used as a hint for format probing
    // (init_input early-returns after av_probe_input_buffer2 when pb is set,
    // never calling io_open). FFmpeg therefore opens ZERO file handles of
    // its own. The HANDLE we hold is the only one for this decoder.
    int ret = avformat_open_input(&m_fmtCtx, pathStr.c_str(), nullptr, nullptr);
    if (ret < 0)
    {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        m_lastError = "Cannot open file: " + std::string(err);
        spdlog::error("VideoDecoder: {}", m_lastError);
        // avformat_open_input frees m_fmtCtx on failure; the AVIOContext
        // and HANDLE we created independently still need cleanup.
        if (m_avioCtx) {
            av_freep(&m_avioCtx->buffer);
            avio_context_free(&m_avioCtx);
        }
        m_avioBuf = nullptr;
        closeShared(sh);
        m_avioFile = nullptr;
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
        m_info.fps = 30.0;

    // VFR detection
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

    if (vs->nb_frames > 0)
        m_info.frameCount = vs->nb_frames;
    else
        m_info.frameCount = static_cast<int64_t>(m_info.duration * m_info.fps);
    m_info.containerFormat = m_fmtCtx->iformat->name;

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    m_info.codecName = codec ? codec->name : "unknown";

    // Alpha channel detection
    bool hasAlpha = false;
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(
            static_cast<AVPixelFormat>(codecpar->format));
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA))
            hasAlpha = true;

        AVDictionaryEntry* alphaMode = av_dict_get(vs->metadata, "alpha_mode", nullptr, 0);
        if (alphaMode && std::string(alphaMode->value) == "1")
            hasAlpha = true;

        if (codecpar->codec_id == AV_CODEC_ID_PRORES) {
            uint32_t tag = codecpar->codec_tag;
            if (tag == MKTAG('a','p','4','h') || tag == MKTAG('a','p','4','x'))
                hasAlpha = true;
        }
    }

    m_info.hasAlpha = hasAlpha;

    // Packed-alpha detection
    {
        AVDictionaryEntry* pa = av_dict_get(m_fmtCtx->metadata, "packed_alpha", nullptr, 0);
        if (pa && std::string(pa->value) == "1") {
            m_info.packedAlpha = true;
            spdlog::info("VideoDecoder: '{}' is packed-alpha ({}x{}, nominal {}x{})",
                         path.filename().string(), m_info.width, m_info.height,
                         m_info.width, m_info.height / 2);
        }
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
        if (forceSoftwareDecode()) {
            spdlog::info("VideoDecoder: '{}' — user preference is software-only",
                         path.filename().string());
            if (!initSoftwareDecoder()) {
                avformat_close_input(&m_fmtCtx);
                return false;
            }
            goto done_open;
        }

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

// ── Close ───────────────────────────────────────────────────────────────────

void VideoDecoder::close()
{
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
    // Tear down the custom AVIO + shared FILE handle. With
    // AVFMT_FLAG_CUSTOM_IO set, avformat_close_input does NOT free our
    // AVIOContext or the buffer it owns, so the caller must.
    if (m_avioCtx)
    {
        // av_freep(&m_avioCtx->buffer) — the buffer pointer may have been
        // grown/replaced by FFmpeg during probing, so always free the
        // CURRENT buffer pointer rather than the one we passed in.
        av_freep(&static_cast<AVIOContext*>(m_avioCtx)->buffer);
        AVIOContext* tmp = m_avioCtx;
        avio_context_free(&tmp);
        m_avioCtx = nullptr;
    }
    m_avioBuf = nullptr;
    if (m_avioFile)
    {
        closeShared(static_cast<SharedFileHandle>(m_avioFile));
        m_avioFile = nullptr;
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

// ── Hardware decoder init ───────────────────────────────────────────────────

bool VideoDecoder::initHardwareDecoder(int deviceTypeInt)
{
    auto deviceType = static_cast<AVHWDeviceType>(deviceTypeInt);
    AVStream* vs = m_fmtCtx->streams[m_info.videoStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) return false;

    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
    if (deviceType == AV_HWDEVICE_TYPE_CUDA)
        hwPixFmt = AV_PIX_FMT_CUDA;
    else if (deviceType == AV_HWDEVICE_TYPE_D3D11VA)
        hwPixFmt = AV_PIX_FMT_D3D11;
    else if (deviceType == AV_HWDEVICE_TYPE_QSV)
        hwPixFmt = AV_PIX_FMT_QSV;

    for (int i = 0;; i++)
    {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == deviceType)
        {
            hwPixFmt = config->pix_fmt;
            break;
        }
    }

    if (hwPixFmt == AV_PIX_FMT_NONE)
        return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    int ret = avcodec_parameters_to_context(m_codecCtx, vs->codecpar);
    if (ret < 0) { avcodec_free_context(&m_codecCtx); return false; }

    m_codecCtx->thread_count = 1;
    m_codecCtx->thread_type = FF_THREAD_SLICE;

    ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, deviceType, nullptr, nullptr, 0);
    if (ret < 0) { avcodec_free_context(&m_codecCtx); return false; }

    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    m_codecCtx->get_format = getHwFormat;

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0)
    {
        avcodec_free_context(&m_codecCtx);
        av_buffer_unref(&m_hwDeviceCtx);
        return false;
    }

    m_hwAccel = true;
    m_hwPixFmt = hwPixFmt;

    spdlog::info("VideoDecoder: hardware decoder initialized ({})",
                 av_hwdevice_get_type_name(deviceType));
    return true;
}

// ── Software decoder init ───────────────────────────────────────────────────

bool VideoDecoder::initSoftwareDecoder(const char* preferredDecoderName)
{
    AVStream* vs = m_fmtCtx->streams[m_info.videoStreamIndex];

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

    m_codecCtx->thread_count = m_maxThreads;
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

} // namespace rt
#endif // ROUNDTABLE_HAS_FFMPEG
