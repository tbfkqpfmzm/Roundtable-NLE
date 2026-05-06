/*
 * SmartRenderAnalyzer — determines which export frames can be passed through
 * (raw packet copy) vs. which must be composited and re-encoded.
 *
 * A frame is passthrough-eligible when:
 *  - Exactly one VideoClip is active (no compositing needed)
 *  - The clip has no modifications (no effects, masks, opacity, transforms, crop)
 *  - Speed == 1.0 with no speed ramp
 *  - Source resolution matches export resolution
 *  - Source codec matches export codec
 *  - Source FPS matches export FPS
 *  - BlendMode is Normal (0)
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

// Forward declarations
class Timeline;
struct EncoderConfig;
enum class EncoderCodec : uint8_t;

/// Info for a single passthrough frame — which source file and frame to copy.
struct PassthroughFrame
{
    std::string mediaPath;     ///< Source file path
    int64_t     sourceFrame;   ///< Source frame number to read (0-based)
};

/// Result of smart render analysis for an export range.
struct SmartRenderPlan
{
    /// Map from export frame index (0-based) to passthrough info.
    /// Frames NOT in this map must be composited + re-encoded.
    std::unordered_map<int64_t, PassthroughFrame> passthroughFrames;

    /// Summary counts for logging.
    int64_t totalFrames{0};
    int64_t passthroughCount{0};
    int64_t reEncodeCount{0};
};

/// Analyze a timeline + export config and produce a smart render plan.
[[nodiscard]] SmartRenderPlan analyzeSmartRender(
    const Timeline& timeline,
    const EncoderConfig& encoderConfig,
    uint32_t outputWidth,
    uint32_t outputHeight,
    int64_t startFrame,
    int64_t endFrame);

/// Map EncoderCodec enum to the low-level codec name used in source detection.
/// e.g. EncoderCodec::H264 → "h264", H265 → "hevc", AV1 → "av1"
[[nodiscard]] const char* encoderCodecToSourceName(EncoderCodec codec) noexcept;

} // namespace rt
