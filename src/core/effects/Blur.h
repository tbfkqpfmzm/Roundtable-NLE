/*
 * Blur — Gaussian blur effect.
 *
 * Single-parameter Gaussian blur: only Radius is exposed. Sigma is
 * derived inside the shader as radius / 3.0 (the standard "3-sigma"
 * rule that keeps the kernel weight curve well-shaped for any radius).
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Blur : public Effect
{
public:
    Blur();
    ~Blur() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Radius = 0,   // 0–100 pixels
        ParamCount
    };
};

} // namespace rt
