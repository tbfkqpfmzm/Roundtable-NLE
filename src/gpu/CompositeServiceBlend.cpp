/*
 * CompositeServiceBlend.cpp - CPU pixel blending helpers.
 * Extracted from CompositeServiceFrame.cpp.
 *
 * Contains blitLayerWithTransform (affine transform + alpha blend)
 * and rasterizeMasks (CPU mask rasterizer for opacity masks).
 */

#include "CompositeServiceBlend.h"
#include "timeline/OpacityMask.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rt {

/// Helper: alpha-blend a source BGRA layer onto a destination BGRA buffer
/// with affine transform (position, scale, rotation).
/// Source is fill-to-output (covers entire output, may crop edges) first,
/// then clip transforms applied.
void blitLayerWithTransform(
    uint8_t* dst, uint32_t dstW, uint32_t dstH,
    const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
    float opacity,
    float posXPx, float posYPx,
    float scX, float scY,
    float rotDeg,
    float cropL, float cropR,
    float cropT, float cropB,
    bool containFit)
{
    if (opacity < 0.001f) return;

    // Guard against zero / degenerate scale that would cause division-by-zero
    // or astronomically large bounding boxes.
    if (std::abs(scX) < 0.001f || std::abs(scY) < 0.001f) return;

    // Cover/contain fit: scale source to fill (cover) or fit within (contain)
    // the output.  Resolution-independent — same visual at any output size.
    const float scaleToFitW = static_cast<float>(dstW) / static_cast<float>(srcW);
    const float scaleToFitH = static_cast<float>(dstH) / static_cast<float>(srcH);
    const float fitScale = containFit
        ? std::min(scaleToFitW, scaleToFitH)
        : std::max(scaleToFitW, scaleToFitH);

    const float fittedW = srcW * fitScale;
    const float fittedH = srcH * fitScale;
    const float baseOffX = (dstW - fittedW) * 0.5f;
    const float baseOffY = (dstH - fittedH) * 0.5f;

    // Combined transform center is output center
    const float cx = dstW * 0.5f;
    const float cy = dstH * 0.5f;

    // 3) Rotation matrix
    const float radians = rotDeg * 3.14159265358979f / 180.0f;
    const float cosR = std::cos(radians);
    const float sinR = std::sin(radians);

    // Fast path: identity transform, same dimensions — direct blit
    //    Avoid per-pixel inverse-transform computation entirely.
    const bool hasCrop = cropL > 0.01f || cropR > 0.01f || cropT > 0.01f || cropB > 0.01f;
    const bool noTransform = !hasCrop &&
                             std::abs(posXPx) < 0.5f && std::abs(posYPx) < 0.5f &&
                             std::abs(scX - 1.0f) < 0.001f && std::abs(scY - 1.0f) < 0.001f &&
                             std::abs(rotDeg) < 0.01f;
    if (noTransform && srcW == dstW && srcH == dstH && opacity >= 0.999f) {
        // Source matches output: opaque copy (skip alpha blend for bg layer)
        for (uint32_t y = 0; y < dstH; ++y) {
            const uint8_t* sp = src + y * srcStride;
            uint8_t* dp = dst + y * dstW * 4;
            std::memcpy(dp, sp, static_cast<size_t>(dstW) * 4);
        }
        return;
    }

    // Precompute fixed-point opacity: 0-256 range for fast integer blend.
    // 256 means "fully opaque" and lets us use (x >> 8) instead of (x / 255).
    const uint32_t opac256 = static_cast<uint32_t>(opacity * 256.0f + 0.5f);

    // Fast integer "source-over" alpha blend helper (inline lambda).
    // Uses fixed-point 0-255 math with no floating-point or division.
    //   sa256 = source alpha * opacity, in 0-256 range
    //   Blends BGRA in-place: dst = src*sa + dst*(255-sa'), approximated via
    //   the exact integer divide-by-255: (v + 1 + (v >> 8)) >> 8.
    auto blendPixel = [](uint8_t* dp, const uint8_t* sp, uint32_t sa256) {
        // sa = effective source alpha in 0-255
        uint32_t sa = (static_cast<uint32_t>(sp[3]) * sa256) >> 8;
        if (sa == 0) return;
        if (sa >= 255) {
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
            return;
        }
        uint32_t invA = 255u - sa;
        // Exact divide-by-255 for each channel: ((a*sa + b*invA) + 1 + (tmp>>8)) >> 8
        auto div255 = [](uint32_t v) -> uint8_t {
            return static_cast<uint8_t>((v + 1 + (v >> 8)) >> 8);
        };
        dp[0] = div255(sp[0] * sa + dp[0] * invA);
        dp[1] = div255(sp[1] * sa + dp[1] * invA);
        dp[2] = div255(sp[2] * sa + dp[2] * invA);
        // Output alpha: sa + da*(1-sa/255) — sa + da*invA/255
        uint32_t outA = sa + ((dp[3] * invA + 127) / 255);
        dp[3] = static_cast<uint8_t>(std::min(outA, 255u));
    };

    if (noTransform && srcW == dstW && srcH == dstH) {
        // Same dimensions, no transform, but opacity < 1
        for (uint32_t i = 0; i < dstW * dstH; ++i) {
            const uint8_t* sp = src + i * 4;
            uint8_t* dp = dst + i * 4;
            if (sp[3] == 0) continue;
            blendPixel(dp, sp, opac256);
        }
        return;
    }

    // Bounding-box clipping
    //    Instead of iterating ALL output pixels, compute the output-space
    //    AABB of the source rectangle and iterate only those pixels.
    // Forward transform: fitSpace -> scale -> rotate -> position
    auto forwardXY = [&](float fitX, float fitY, float& outX, float& outY) {
        float rx = (fitX - cx + baseOffX) * scX;
        float ry = (fitY - cy + baseOffY) * scY;
        outX = rx * cosR - ry * sinR + cx + posXPx;
        outY = rx * sinR + ry * cosR + cy + posYPx;
    };

    // Transform the 4 corners of the fitted source rectangle
    float ox0, oy0, ox1, oy1, ox2, oy2, ox3, oy3;
    forwardXY(0.0f,    0.0f,    ox0, oy0);  // top-left
    forwardXY(fittedW, 0.0f,    ox1, oy1);  // top-right
    forwardXY(0.0f,    fittedH, ox2, oy2);  // bottom-left
    forwardXY(fittedW, fittedH, ox3, oy3);  // bottom-right

    float aabbMinX = std::min({ox0, ox1, ox2, ox3});
    float aabbMaxX = std::max({ox0, ox1, ox2, ox3});
    float aabbMinY = std::min({oy0, oy1, oy2, oy3});
    float aabbMaxY = std::max({oy0, oy1, oy2, oy3});

    // Apply crop: narrow the AABB by the crop percentages.
    // Crop values are 0-100 representing percentage of the layer to cut off.
    if (cropL > 0.01f || cropR > 0.01f || cropT > 0.01f || cropB > 0.01f) {
        float aabbW = aabbMaxX - aabbMinX;
        float aabbH = aabbMaxY - aabbMinY;
        aabbMinX += aabbW * (cropL / 100.0f);
        aabbMaxX -= aabbW * (cropR / 100.0f);
        aabbMinY += aabbH * (cropT / 100.0f);
        aabbMaxY -= aabbH * (cropB / 100.0f);
        if (aabbMinX >= aabbMaxX || aabbMinY >= aabbMaxY) return;
    }

    // Clamp to output bounds
    uint32_t startX = static_cast<uint32_t>(std::max(0, static_cast<int>(std::floor(aabbMinX))));
    uint32_t startY = static_cast<uint32_t>(std::max(0, static_cast<int>(std::floor(aabbMinY))));
    uint32_t endX   = static_cast<uint32_t>(std::min(static_cast<int>(dstW),
                                                      static_cast<int>(std::ceil(aabbMaxX)) + 1));
    uint32_t endY   = static_cast<uint32_t>(std::min(static_cast<int>(dstH),
                                                      static_cast<int>(std::ceil(aabbMaxY)) + 1));

    if (startX >= endX || startY >= endY) return;

    // 4) For each output pixel IN THE BOUNDING BOX, compute inverse
    //    transform to find source pixel.
    for (uint32_t dy = startY; dy < endY; ++dy) {
        for (uint32_t dx = startX; dx < endX; ++dx) {
            // Output pixel relative to center + position offset
            float px = static_cast<float>(dx) - cx - posXPx;
            float py = static_cast<float>(dy) - cy - posYPx;

            // Inverse rotation
            float rx = px * cosR + py * sinR;
            float ry = -px * sinR + py * cosR;

            // Inverse scale
            rx /= scX;
            ry /= scY;

            // Back to output pixel space (relative to fit origin)
            float fitX = rx + cx - baseOffX;
            float fitY = ry + cy - baseOffY;

            // Map from fitted space to source pixel
            float sx = fitX / fitScale;
            float sy = fitY / fitScale;

            // Nearest-neighbor sampling
            int isx = static_cast<int>(sx);
            int isy = static_cast<int>(sy);

            if (isx < 0 || isx >= static_cast<int>(srcW) ||
                isy < 0 || isy >= static_cast<int>(srcH))
                continue;

            const uint8_t* sp = src + isy * srcStride + isx * 4;
            uint8_t* dp = dst + (dy * dstW + dx) * 4;

            // Skip fully transparent source pixels
            if (sp[3] == 0) continue;

            // Integer alpha blending (source-over)
            blendPixel(dp, sp, opac256);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  CPU mask rasterizer — generates an RGBA texture where white = opaque,
//  black = transparent. Each mask shape is rasterized, then combined
//  with multiply (intersection of all masks on a clip).
// ═══════════════════════════════════════════════════════════════════════════
std::vector<uint8_t> rasterizeMasks(const std::vector<OpacityMask>& masks,
                                            uint32_t w, uint32_t h)
{
    // Start with all-white (fully opaque)
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 255);

    for (const auto& mask : masks) {
        // Per-mask alpha buffer (white = inside mask)
        std::vector<float> maskAlpha(static_cast<size_t>(w) * h, 0.0f);

        const float cx = mask.centerX * static_cast<float>(w);
        const float cy = mask.centerY * static_cast<float>(h);
        const float hw = mask.width   * static_cast<float>(w) * 0.5f;
        const float hh = mask.height  * static_cast<float>(h) * 0.5f;
        const float rotRad = mask.rotation * 3.14159265f / 180.0f;
        const float cosR = std::cos(-rotRad);
        const float sinR = std::sin(-rotRad);
        const float expW = mask.expansion;
        const float expH = mask.expansion;

        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                float px = static_cast<float>(x) + 0.5f - cx;
                float py = static_cast<float>(y) + 0.5f - cy;
                // Rotate into mask-local space
                float lx = px * cosR - py * sinR;
                float ly = px * sinR + py * cosR;

                float alpha = 0.0f;
                if (mask.shape == rt::MaskShape::Ellipse) {
                    float eHW = hw + expW;
                    float eHH = hh + expH;
                    if (eHW > 0.0f && eHH > 0.0f) {
                        float d = (lx * lx) / (eHW * eHW) + (ly * ly) / (eHH * eHH);
                        if (d <= 1.0f)
                            alpha = 1.0f;
                        else if (mask.feather > 0.0f) {
                            float dist = (std::sqrt(d) - 1.0f) * std::min(eHW, eHH);
                            alpha = std::max(0.0f, 1.0f - dist / mask.feather);
                        }
                    }
                } else if (mask.shape == rt::MaskShape::Rectangle) {
                    float rHW = hw + expW;
                    float rHH = hh + expH;
                    if (rHW > 0.0f && rHH > 0.0f) {
                        float dx = std::abs(lx) - rHW;
                        float dy = std::abs(ly) - rHH;
                        if (dx <= 0.0f && dy <= 0.0f)
                            alpha = 1.0f;
                        else if (mask.feather > 0.0f) {
                            float dist = std::max(dx, dy);
                            if (dist < mask.feather)
                                alpha = std::max(0.0f, 1.0f - dist / mask.feather);
                        }
                    }
                } else if (mask.shape == rt::MaskShape::FreeDrawBezier && mask.vertices.size() >= 3) {
                    // Point-in-polygon using ray casting on bezier approximation
                    float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                    float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
                    int crossings = 0;
                    size_t n = mask.vertices.size();
                    for (size_t i = 0; i < n; ++i) {
                        size_t j = (i + 1) % n;
                        float y0 = mask.vertices[i].y;
                        float y1 = mask.vertices[j].y;
                        float x0 = mask.vertices[i].x;
                        float x1 = mask.vertices[j].x;
                        if ((y0 <= ny && y1 > ny) || (y1 <= ny && y0 > ny)) {
                            float t = (ny - y0) / (y1 - y0);
                            if (nx < x0 + t * (x1 - x0))
                                crossings++;
                        }
                    }
                    alpha = (crossings % 2 == 1) ? 1.0f : 0.0f;
                }

                if (mask.inverted)
                    alpha = 1.0f - alpha;
                alpha *= mask.maskOpacity;
                maskAlpha[static_cast<size_t>(y) * w + x] = alpha;
            }
        }

        // Multiply into the accumulated pixel buffer (all channels)
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                size_t idx = (static_cast<size_t>(y) * w + x) * 4;
                float a = maskAlpha[static_cast<size_t>(y) * w + x];
                uint8_t ai = static_cast<uint8_t>(a * 255.0f);
                pixels[idx + 0] = static_cast<uint8_t>(pixels[idx + 0] * a);
                pixels[idx + 1] = static_cast<uint8_t>(pixels[idx + 1] * a);
                pixels[idx + 2] = static_cast<uint8_t>(pixels[idx + 2] * a);
                pixels[idx + 3] = static_cast<uint8_t>(std::min(255, static_cast<int>(pixels[idx + 3]) * ai / 255));
            }
        }
    }

    return pixels;
}

} // namespace rt
