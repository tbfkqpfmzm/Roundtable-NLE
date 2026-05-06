/*
 * WhisperCuda — CUDA backend for whisper.cpp acceleration.
 *
 * Uses NVIDIA tensor cores on RTX 4090 for fast inference.
 * Conditionally compiled when both ROUNDTABLE_HAS_CUDA and
 * ROUNDTABLE_HAS_WHISPER are defined.
 *
 * Without those defines, falls back to CPU inference automatically.
 */

#pragma once

#include <cstdint>
#include <string>

namespace rt {

/// CUDA acceleration info for Whisper inference.
struct WhisperCudaInfo
{
    bool   available{false};
    int    deviceIndex{-1};
    std::string deviceName;
    size_t freeMemoryMB{0};
    bool   tensorCoresAvailable{false};  // SM >= 7.0
    bool   fp16Supported{false};         // SM >= 7.0
};

/// CUDA backend for whisper.cpp.
/// Manages CUDA device selection and memory for whisper inference.
class WhisperCuda
{
public:
    WhisperCuda();
    ~WhisperCuda();

    // Non-copyable
    WhisperCuda(const WhisperCuda&) = delete;
    WhisperCuda& operator=(const WhisperCuda&) = delete;

    /// Initialize CUDA backend — detect GPU, check capabilities.
    /// \return true if CUDA is available and suitable for whisper.
    bool init();

    /// Shut down, release CUDA resources.
    void shutdown();

    /// True if CUDA acceleration is available for whisper.
    [[nodiscard]] bool isAvailable() const noexcept { return m_info.available; }

    /// Get device info.
    [[nodiscard]] const WhisperCudaInfo& info() const noexcept { return m_info; }

    /// Recommended compute type based on GPU capability.
    /// Returns "float16" for tensor-core GPUs, "float32" otherwise.
    [[nodiscard]] const char* recommendedComputeType() const noexcept;

    /// Estimate if the GPU has enough VRAM for the given model.
    /// Returns estimated VRAM usage in MB.
    [[nodiscard]] size_t estimateModelVRAM(const std::string& modelSize) const noexcept;

    /// True if GPU has enough VRAM for the given model.
    [[nodiscard]] bool canFitModel(const std::string& modelSize) const noexcept;

private:
    WhisperCudaInfo m_info;
};

} // namespace rt
