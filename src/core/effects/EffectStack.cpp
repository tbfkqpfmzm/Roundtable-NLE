/*
 * EffectStack.cpp â€” implements EffectStack + Effect base class.
 *
 * Step 22: Effects System
 */

#include "effects/EffectStack.h"
#include "effects/Effect.h"
#include "effects/ColorCorrect.h"
#include "effects/Blur.h"
#include "effects/Sharpen.h"
#include "effects/Glow.h"
#include "effects/ChromaKey.h"
#include "effects/Transform2D.h"
#include "effects/Vignette.h"
#include "effects/LUT.h"
#include "effects/Letterbox.h"
#include "effects/Ots.h"
#include "effects/Flip.h"
#include "effects/ColorGrading.h"
#include "effects/FillChannel.h"

#include <algorithm>

namespace rt {

// =============================================================================
//  Effect base — static ID counter + implementation
// =============================================================================

uint64_t Effect::s_nextId = 1;

Effect::Effect(EffectType type)
    : m_type(type)
    , m_id(s_nextId++)
{
}

Effect::~Effect() = default;

EffectParam* Effect::findParam(const std::string& n)
{
    for (auto& p : m_params)
        if (p.name == n) return &p;
    return nullptr;
}

const EffectParam* Effect::findParam(const std::string& n) const
{
    for (auto& p : m_params)
        if (p.name == n) return &p;
    return nullptr;
}

std::vector<float> Effect::evalAllParams(int64_t time) const
{
    std::vector<float> vals;
    vals.reserve(m_params.size());
    for (auto& p : m_params)
        vals.push_back(p.track.evaluate(time));
    return vals;
}

void Effect::addParam(const std::string& name, float defaultValue,
                      float minVal, float maxVal)
{
    EffectParam p;
    p.name   = name;
    p.track  = KeyframeTrack<float>(defaultValue);
    p.minVal = minVal;
    p.maxVal = maxVal;
    m_params.push_back(std::move(p));
}

// ---- Factory ----------------------------------------------------------------

std::unique_ptr<Effect> createEffect(EffectType type)
{
    switch (type) {
    case EffectType::ColorCorrect: return std::make_unique<ColorCorrect>();
    case EffectType::Blur:         return std::make_unique<Blur>();
    case EffectType::Sharpen:      return std::make_unique<Sharpen>();
    case EffectType::Glow:         return std::make_unique<Glow>();
    case EffectType::ChromaKey:    return std::make_unique<ChromaKey>();
    case EffectType::Transform2D:  return std::make_unique<Transform2D>();
    case EffectType::Vignette:      return std::make_unique<Vignette>();
    case EffectType::LUT:            return std::make_unique<LUT>();
    case EffectType::Letterbox:      return std::make_unique<Letterbox>();
    case EffectType::ColorGrading:   return std::make_unique<ColorGrading>();
    case EffectType::LumetriColor:   return std::make_unique<ColorGrading>();
    case EffectType::OtsLeft:        return std::make_unique<Ots>(EffectType::OtsLeft);
    case EffectType::OtsRight:       return std::make_unique<Ots>(EffectType::OtsRight);
    case EffectType::FlipHorizontal: return std::make_unique<Flip>(EffectType::FlipHorizontal);
    case EffectType::FlipVertical:   return std::make_unique<Flip>(EffectType::FlipVertical);
    case EffectType::FillLeftWithRight:  return std::make_unique<FillLeftWithRight>();
    case EffectType::FillRightWithLeft:  return std::make_unique<FillRightWithLeft>();
    default: return nullptr;
    }
}

// =============================================================================
//  EffectStack
// =============================================================================

EffectStack::EffectStack() = default;
EffectStack::~EffectStack() = default;

void EffectStack::addEffect(std::unique_ptr<Effect> effect)
{
    m_effects.push_back(std::move(effect));
}

void EffectStack::insertEffect(size_t index, std::unique_ptr<Effect> effect)
{
    if (index >= m_effects.size())
        m_effects.push_back(std::move(effect));
    else
        m_effects.insert(m_effects.begin() + static_cast<ptrdiff_t>(index), std::move(effect));
}

std::unique_ptr<Effect> EffectStack::removeEffect(size_t index)
{
    if (index >= m_effects.size()) return nullptr;
    auto ptr = std::move(m_effects[index]);
    m_effects.erase(m_effects.begin() + static_cast<ptrdiff_t>(index));
    return ptr;
}

void EffectStack::moveEffect(size_t fromIndex, size_t toIndex)
{
    if (fromIndex >= m_effects.size() || toIndex >= m_effects.size()) return;
    if (fromIndex == toIndex) return;

    auto effect = std::move(m_effects[fromIndex]);
    m_effects.erase(m_effects.begin() + static_cast<ptrdiff_t>(fromIndex));
    m_effects.insert(
        m_effects.begin() + static_cast<ptrdiff_t>(std::min(toIndex, m_effects.size())),
        std::move(effect));
}

const Effect* EffectStack::effectById(uint64_t id) const
{
    for (auto& e : m_effects)
        if (e->id() == id) return e.get();
    return nullptr;
}

Effect* EffectStack::effectById(uint64_t id)
{
    for (auto& e : m_effects)
        if (e->id() == id) return e.get();
    return nullptr;
}

size_t EffectStack::indexOf(uint64_t effectId) const
{
    for (size_t i = 0; i < m_effects.size(); ++i)
        if (m_effects[i]->id() == effectId) return i;
    return m_effects.size();
}

bool EffectStack::hasActiveEffects() const
{
    for (auto& e : m_effects)
        if (e->isEnabled()) return true;
    return false;
}

std::vector<EffectStack::EffectSnapshot> EffectStack::evaluate(int64_t time) const
{
    std::vector<EffectSnapshot> result;
    for (auto& e : m_effects) {
        if (!e->isEnabled()) continue;
        // ColorGrading has 20 params but GPU needs 16 â€” use special packing
        // ColorGrading has 31 params + section flags â€” use special packing too
        if (e->effectType() == EffectType::ColorGrading) {
            auto* lumetri = static_cast<const ColorGrading*>(e.get());
            result.push_back({e->effectType(), lumetri->evalGpuParams(time)});

        } else {
            result.push_back({e->effectType(), e->evalAllParams(time)});
        }
    }
    return result;
}

std::unique_ptr<EffectStack> EffectStack::clone() const
{
    auto copy = std::make_unique<EffectStack>();
    for (auto& e : m_effects)
        copy->addEffect(e->clone());
    return copy;
}

} // namespace rt
