/*
 * ColorCorrect.cpp — Color correction effect implementation.
 * Step 22: Effects System
 */

#include "effects/ColorCorrect.h"

namespace rt {

ColorCorrect::ColorCorrect()
    : Effect(EffectType::ColorCorrect)
{
    addParam("Brightness",  0.0f, -1.0f, 1.0f);
    addParam("Contrast",    1.0f,  0.0f, 3.0f);
    addParam("Saturation",  1.0f,  0.0f, 3.0f);
    addParam("Hue",         0.0f, -180.0f, 180.0f);
    addParam("Temperature", 0.0f, -1.0f, 1.0f);
    addParam("Tint",        0.0f, -1.0f, 1.0f);
    addParam("Gamma",       1.0f,  0.1f, 5.0f);
    addParam("Gain",        1.0f,  0.0f, 5.0f);
    addParam("Lift",        0.0f, -1.0f, 1.0f);
}

std::unique_ptr<Effect> ColorCorrect::clone() const
{
    auto copy = std::make_unique<ColorCorrect>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

} // namespace rt
