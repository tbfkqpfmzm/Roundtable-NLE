/*
 * Sharpen — Unsharp-mask sharpening effect.
 *
 * Parameters: amount, radius, threshold.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Sharpen : public Effect
{
public:
    Sharpen();
    ~Sharpen() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Amount = 0,    // 0–5.0
        Radius,        // 0.1–10.0
        Threshold,     // 0–1.0
        ParamCount
    };
};

} // namespace rt
