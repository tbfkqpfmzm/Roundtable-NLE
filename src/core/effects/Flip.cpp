/*
 * Flip.cpp — horizontal / vertical mirror effect.
 *
 * The axis is fixed by the effect type and exposed to the GPU shader as
 * the single (hidden) "Axis" param: 0 = horizontal, 1 = vertical.
 */

#include "effects/Flip.h"

namespace rt {

Flip::Flip(EffectType type)
    : Effect(type)
{
    // Axis is driven by the effect type, not user-edited.  Registered as
    // a param so the generic GPU dispatch path forwards it to the shader.
    const float axis = (type == EffectType::FlipVertical) ? 1.0f : 0.0f;
    addParam("Axis", axis, 0.0f, 1.0f);
}

std::unique_ptr<Effect> Flip::clone() const
{
    auto copy = std::make_unique<Flip>(m_type);
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

} // namespace rt
