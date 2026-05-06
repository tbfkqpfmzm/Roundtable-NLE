/*
 * Transform2D — 2D transform effect (applied as post-effect).
 *
 * Parameters: position offset, scale, rotation, anchor point.
 * Distinct from clip transform — this is an effect in the stack.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Transform2D : public Effect
{
public:
    Transform2D();
    ~Transform2D() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        OffsetX = 0,   // -2000 to 2000 pixels
        OffsetY,       // -2000 to 2000 pixels
        Scale,         // 0.01 to 10.0
        Rotation,      // -360 to 360 degrees
        AnchorX,       // 0–1 (normalized)
        AnchorY,       // 0–1 (normalized)
        ParamCount
    };
};

} // namespace rt
