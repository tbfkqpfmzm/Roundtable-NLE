/*
 * NvdecDecoder.cpp — Direct NVDEC via CUVID API
 *
 * Two implementations:
 *   1. ROUNDTABLE_HAS_CUDA: native CUVID decode (zero-copy GPU frames)
 *   2. Fallback: stub — relies on FFmpeg hwaccel path in VideoDecoder
 */

#include "NvdecDecoder.h"
#include "CudaContext.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_CUDA

#include <cuda.h>

// NVIDIA Video Codec SDK headers (nvcuvid.h, cuviddec.h) are NOT part of
// the CUDA Toolkit — they require a separate download from NVIDIA.
// We check for them at CMake time via ROUNDTABLE_HAS_VIDEO_CODEC_SDK.
#ifdef ROUNDTABLE_HAS_VIDEO_CODEC_SDK
#include <nvcuvid.h>
#include <cuviddec.h>
#define HAS_CUVID 1
#else
#define HAS_CUVID 0
#endif

namespace rt {

struct NvdecDecoder::Impl
{
#if HAS_CUVID
    CUvideoparser   parser{nullptr};
    CUvideodecoder  decoder{nullptr};
#endif
    CUstream        stream{nullptr};

    // Decoded frame queue (GPU pointers)
    std::vector<NvdecFrameInfo> pendingFrames;
    int64_t currentFrame{0};
};

NvdecDecoder::NvdecDecoder(CudaContext& cuda)
    : m_cuda(cuda)
    , m_impl(std::make_unique<Impl>())
{
}

NvdecDecoder::~NvdecDecoder()
{
    close();
}

bool NvdecDecoder::open(const std::string& filePath, NvdecCodec codec)
{
    if (!m_cuda.isAvailable()) {
        spdlog::warn("NvdecDecoder: CUDA not available");
        return false;
    }

#if HAS_CUVID
    // Set CUDA context
    cuCtxPushCurrent(static_cast<CUcontext>(m_cuda.nativeContext()));

    // Map our codec enum to CUVID codec type
    cudaVideoCodec cuCodec;
    switch (codec) {
        case NvdecCodec::H264: cuCodec = cudaVideoCodec_H264; break;
        case NvdecCodec::HEVC: cuCodec = cudaVideoCodec_HEVC; break;
        case NvdecCodec::VP9:  cuCodec = cudaVideoCodec_VP9;  break;
        case NvdecCodec::AV1:  cuCodec = cudaVideoCodec_AV1;  break;
        default:               cuCodec = cudaVideoCodec_H264; break;
    }

    // TODO: Set up CUVIDPARSERPARAMS, create parser + decoder
    (void)cuCodec;

    spdlog::info("NvdecDecoder: opened '{}' codec={}", filePath, static_cast<int>(codec));
    m_open = true;

    CUcontext dummy;
    cuCtxPopCurrent(&dummy);
    return true;
#else
    spdlog::info("NvdecDecoder: NVIDIA Video Codec SDK not available. "
                 "Using FFmpeg hwaccel path. ({})", filePath);
    (void)codec;
    return false;
#endif
}

void NvdecDecoder::close()
{
    if (!m_open) return;

    if (m_cuda.isAvailable()) {
        cuCtxPushCurrent(static_cast<CUcontext>(m_cuda.nativeContext()));

#if HAS_CUVID
        if (m_impl->decoder) {
            cuvidDestroyDecoder(m_impl->decoder);
            m_impl->decoder = nullptr;
        }
        if (m_impl->parser) {
            cuvidDestroyVideoParser(m_impl->parser);
            m_impl->parser = nullptr;
        }
#endif

        CUcontext dummy;
        cuCtxPopCurrent(&dummy);
    }

    m_open = false;
    m_width = 0;
    m_height = 0;
}

bool NvdecDecoder::decodeNext(NvdecFrameInfo& outFrame)
{
    if (!m_open) return false;

    // TODO: Feed packet data to CUVID parser, receive decoded frame
    // The parser callback delivers decoded frames asynchronously.
    // For now, return false (no frames) — actual implementation will
    // populate outFrame with GPU device pointer.

    (void)outFrame;
    return false;
}

bool NvdecDecoder::seek(double /*timestamp*/)
{
    if (!m_open) return false;
    // TODO: Flush parser/decoder, seek in demuxer
    return false;
}

} // namespace rt

#else // !ROUNDTABLE_HAS_CUDA

namespace rt {

struct NvdecDecoder::Impl {};

NvdecDecoder::NvdecDecoder(CudaContext& cuda)
    : m_cuda(cuda)
    , m_impl(std::make_unique<Impl>())
{
}

NvdecDecoder::~NvdecDecoder()
{
    close();
}

bool NvdecDecoder::open(const std::string& /*filePath*/, NvdecCodec /*codec*/)
{
    spdlog::info("NvdecDecoder: direct NVDEC not available (no CUDA Toolkit). "
                 "Using FFmpeg hwaccel path instead.");
    return false;
}

void NvdecDecoder::close()
{
    m_open = false;
}

bool NvdecDecoder::decodeNext(NvdecFrameInfo& /*outFrame*/)
{
    return false;
}

bool NvdecDecoder::seek(double /*timestamp*/)
{
    return false;
}

} // namespace rt

#endif // ROUNDTABLE_HAS_CUDA

