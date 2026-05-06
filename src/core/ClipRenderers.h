#pragma once

#include <cstdint>
#include <memory>

struct CachedFrame;

class TitleClip;
class GraphicClip;

namespace rt {

/// CPU-render a TitleClip to a BGRA CachedFrame using QPainter.
std::shared_ptr<CachedFrame> renderTitleClip(TitleClip* clip, int64_t tick,
                                             uint32_t outW, uint32_t outH);

/// CPU-render a GraphicClip (multi-layer text/shape container) to a BGRA CachedFrame.
/// When refW/refH are non-zero, font sizes and pixel-based metrics are scaled by
/// outW/refW so that text maintains the same visual proportion at reduced resolution.
std::shared_ptr<CachedFrame> renderGraphicClip(GraphicClip* clip, int64_t tick,
                                               uint32_t outW, uint32_t outH,
                                               uint32_t refW = 0, uint32_t refH = 0);

} // namespace rt
