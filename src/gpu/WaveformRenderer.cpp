/*
 * WaveformRenderer.cpp — GPU waveform rendering + CPU layout utilities.
 * Step 19: GPU Waveform Renderer
 */

#include "WaveformRenderer.h"

#include <algorithm>
#include <cmath>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

WaveformRenderer::WaveformRenderer()  = default;
WaveformRenderer::~WaveformRenderer() = default;

// ═════════════════════════════════════════════════════════════════════════════
// CPU-side peak layout
// ═════════════════════════════════════════════════════════════════════════════

std::vector<WaveformColumn> WaveformRenderer::layoutPeaks(
    const WaveformPeak* peaks,
    size_t peakCount,
    float startX,
    float pixelsPerPeak,
    float centerY,
    float halfHeight)
{
    std::vector<WaveformColumn> columns;
    columns.reserve(peakCount);

    for (size_t i = 0; i < peakCount; ++i) {
        const auto& pk = peaks[i];
        float x = startX + static_cast<float>(i) * pixelsPerPeak;

        // Map [-1, 1] peak values to screen Y
        // minVal is typically negative (below center), maxVal positive (above center)
        float yMin = centerY - pk.minVal * halfHeight;  // minVal < 0 → below center
        float yMax = centerY - pk.maxVal * halfHeight;  // maxVal > 0 → above center

        // Amplitude = max absolute value (for gradient coloring)
        float amplitude = std::max(std::abs(pk.minVal), std::abs(pk.maxVal));
        amplitude = std::min(amplitude, 1.0f);

        columns.push_back({x, yMin, yMax, amplitude});
    }

    return columns;
}

std::vector<WaveformVertex> WaveformRenderer::peaksToVertices(
    const WaveformPeak* peaks,
    size_t peakCount,
    float startX,
    float pixelsPerPeak,
    float centerY,
    float halfHeight,
    float channel)
{
    std::vector<WaveformVertex> vertices;
    vertices.reserve(peakCount * 2);

    for (size_t i = 0; i < peakCount; ++i) {
        const auto& pk = peaks[i];
        float x = startX + static_cast<float>(i) * pixelsPerPeak;
        float amplitude = std::max(std::abs(pk.minVal), std::abs(pk.maxVal));
        amplitude = std::min(amplitude, 1.0f);

        // One vertex at min, one at max (line segment)
        float yMin = centerY - pk.minVal * halfHeight;
        float yMax = centerY - pk.maxVal * halfHeight;

        vertices.push_back({x, yMin, amplitude, channel});
        vertices.push_back({x, yMax, amplitude, channel});
    }

    return vertices;
}

// ═════════════════════════════════════════════════════════════════════════════
// Color utilities
// ═════════════════════════════════════════════════════════════════════════════

glm::vec4 WaveformRenderer::gradientColor(float amplitude, const GradientColors& colors)
{
    amplitude = std::clamp(amplitude, 0.0f, 1.0f);

    if (amplitude < 0.5f) {
        // Interpolate low → mid
        float t = amplitude * 2.0f;
        return glm::mix(colors.low, colors.mid, t);
    } else {
        // Interpolate mid → high
        float t = (amplitude - 0.5f) * 2.0f;
        return glm::mix(colors.mid, colors.high, t);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Mip level selection
// ═════════════════════════════════════════════════════════════════════════════

uint32_t WaveformRenderer::idealSamplesPerPixel(
    int64_t /*totalFrames*/,
    int64_t visibleFrames,
    int widthPixels)
{
    if (widthPixels <= 0 || visibleFrames <= 0)
        return 256;

    auto spp = static_cast<uint32_t>(visibleFrames / widthPixels);
    return std::max(spp, 1u);
}

} // namespace rt

