/*
 * Transform2D.cpp — 2D transform effect implementation.
 * Step 22: Effects System
 */

#include "effects/Transform2D.h"

namespace rt {

Transform2D::Transform2D()
    : Effect(EffectType::Transform2D)
{
    addParam("Offset X",  0.0f, -2000.0f, 2000.0f);
    addParam("Offset Y",  0.0f, -2000.0f, 2000.0f);
    addParam("Scale",     1.0f,     0.01f,   10.0f);
    addParam("Rotation",  0.0f,  -360.0f,  360.0f);
    addParam("Anchor X",  0.5f,     0.0f,    1.0f);
    addParam("Anchor Y",  0.5f,     0.0f,    1.0f);
}

std::unique_ptr<Effect> Transform2D::clone() const
{
    auto copy = std::make_unique<Transform2D>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

} // namespace rt
