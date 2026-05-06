/*
 * Glow — Bloom/glow effect.
 *
 * Blurs highlights and adds them back to the image.
 * Parameters: threshold, intensity, radius, blendMode.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Glow : public Effect
{
public:
    Glow();
    ~Glow() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        GlowThreshold = 0,  // 0–1.0 luminance threshold
        Intensity,           // 0–5.0
        GlowRadius,          // 1–50 pixels
        ParamCount
    };
};

} // namespace rt
