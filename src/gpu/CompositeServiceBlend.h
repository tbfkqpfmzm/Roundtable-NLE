/*
 * CompositeServiceBlend.h - CPU pixel blending declarations.
 * Extracted from CompositeServiceFrame.cpp.
 */
#pragma once

#include "timeline/OpacityMask.h"

#include <cstdint>
#include <vector>

namespace rt {

/// Alpha-blend a source BGRA layer onto a destination BGRA buffer
/// with affine transform (position, scale, rotation).
/// Source is fill-to-output (covers entire output, may crop edges) first,
/// then clip transforms applied.
void blitLayerWithTransform(
    uint8_t* dst, uint32_t dstW, uint32_t dstH,
    const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
    float opacity,
    float posXPx, float posYPx,     // position offset in output pixels
    float scX, float scY,           // scale multiplier (1.0 = normal)
    float rotDeg,                   // rotation in degrees
    float cropL = 0.0f, float cropR = 0.0f,  // crop percentages (0-100)
    float cropT = 0.0f, float cropB = 0.0f,
    bool containFit = false);       // true = contain fit, false = cover fit

/// CPU mask rasterizer — generates an RGBA texture where white = opaque,
/// black = transparent. Each mask shape is rasterized, then combined
/// with multiply (intersection of all masks on a clip).
std::vector<uint8_t> rasterizeMasks(const std::vector<OpacityMask>& masks,
                                     uint32_t w, uint32_t h);

} // namespace rt
