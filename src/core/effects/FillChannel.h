/*
 * FillChannel — Audio channel fill effects.
 *
 * FillLeftWithRight: replaces the left channel with a copy of the right.
 * FillRightWithLeft: replaces the right channel with a copy of the left.
 *
 * No parameters — these are simple stereo channel operations.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class FillLeftWithRight : public Effect
{
public:
    FillLeftWithRight();
    ~FillLeftWithRight() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;
};

class FillRightWithLeft : public Effect
{
public:
    FillRightWithLeft();
    ~FillRightWithLeft() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;
};

} // namespace rt
