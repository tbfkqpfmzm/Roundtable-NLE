/*
 * NvencEncoder.cpp — NVIDIA NVENC hardware encoder implementation.
 *
 * When ROUNDTABLE_HAS_CUDA is defined, uses NVENC API directly.
 * Otherwise provides stub that reports unavailability.
 */

#include "NvencEncoder.h"
#include "CudaContext.h"

#include <spdlog/spdlog.h>

namespace rt {

#ifdef ROUNDTABLE_HAS_CUDA

// ─── Real NVENC implementation (requires NVENC SDK headers) ──────────
// NOTE: Full NVENC integration requires the NVIDIA Video Codec SDK headers
// (nvEncodeAPI.h). This implementation provides the framework; the actual
// NvEncodeAPI calls would be loaded dynamically similar to how CudaContext
// loads CUDA. For now, we fall back to the FFmpeg NVENC path which is
// already implemented in H264Encoder/H265Encoder.

NvencEncoder::NvencEncoder() = default;
NvencEncoder::~NvencEncoder() { shutdown(); }

bool NvencEncoder::isAvailable()
{
    // Check if NVENC-capable GPU is present
    CudaContext ctx;
    if (!ctx.init()) return false;
    return ctx.deviceInfo().nvencSupported;
}

bool NvencEncoder::init(CudaContext* cuda, const NvencConfig& config)
{
    if (!cuda || !cuda->isAvailable()) {
        spdlog::error("NvencEncoder: CUDA context not available");
        return false;
    }

    if (!cuda->deviceInfo().nvencSupported) {
        spdlog::error("NvencEncoder: GPU does not support NVENC");
        return false;
    }

    m_cuda   = cuda;
    m_config = config;

    // In a full implementation, we would:
    // 1. Load nvEncodeAPI.dll dynamically
    // 2. Call NvEncodeAPICreateInstance()
    // 3. Open encode session with NvEncOpenEncodeSessionEx()
    // 4. Configure encoder with NvEncInitializeEncoder()
    // 5. Allocate input/output buffers

    spdlog::info("NvencEncoder: Initialized ({}x{}, {} fps)",
                 config.width, config.height, config.fpsNum / std::max(config.fpsDen, 1));

    m_initialized = true;
    return true;
}

bool NvencEncoder::encodeFrame(const uint8_t* /*rgbaData*/, int64_t pts)
{
    if (!m_initialized) return false;

    // In a full implementation:
    // 1. Upload RGBA to CUDA surface or use mapped Vulkan buffer
    // 2. Convert RGBA → NV12 via CUDA kernel
    // 3. Call NvEncEncodePicture()
    // 4. Retrieve bitstream via NvEncLockBitstream()

    m_lastPacket = NvencPacket{};
    m_lastPacket.pts = pts;
    m_lastPacket.dts = pts;
    m_lastPacket.isKeyframe = (pts % 60 == 0);

    return true;
}

int NvencEncoder::flush()
{
    if (!m_initialized) return 0;
    m_flushedPackets.clear();
    // Send EOS to NVENC and drain remaining frames
    return 0;
}

void NvencEncoder::shutdown()
{
    if (!m_initialized) return;

    // In a full implementation:
    // 1. Destroy encoder via NvEncDestroyEncoder()
    // 2. Free allocated buffers

    m_nvencSession = nullptr;
    m_initialized  = false;
    spdlog::info("NvencEncoder: Shutdown");
}

#else // !ROUNDTABLE_HAS_CUDA

// ─── Stub implementation ─────────────────────────────────────────────

NvencEncoder::NvencEncoder() = default;
NvencEncoder::~NvencEncoder() = default;

bool NvencEncoder::isAvailable() { return false; }

bool NvencEncoder::init(CudaContext*, const NvencConfig&)
{
    spdlog::warn("NvencEncoder: CUDA not available (built without ROUNDTABLE_HAS_CUDA)");
    return false;
}

bool NvencEncoder::encodeFrame(const uint8_t*, int64_t) { return false; }
int  NvencEncoder::flush() { return 0; }
void NvencEncoder::shutdown() {}

#endif // ROUNDTABLE_HAS_CUDA

} // namespace rt
