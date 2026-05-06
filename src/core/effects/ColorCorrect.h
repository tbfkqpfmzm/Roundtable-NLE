/*
 * ColorCorrect — color correction effect.
 *
 * Parameters: brightness, contrast, saturation, hue, temperature, tint,
 *             gamma, gain, lift (shadows), midtones, highlights.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class ColorCorrect : public Effect
{
public:
    ColorCorrect();
    ~ColorCorrect() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    // Convenience accessors (indices into m_params)
    enum Param : size_t {
        Brightness = 0,
        Contrast,
        Saturation,
        Hue,
        Temperature,
        Tint,
        Gamma,
        Gain,
        Lift,
        ParamCount
    };
};

} // namespace rt
