/*
 * ChromaKey.cpp — Ultra Key chroma keyer implementation.
 */

#include "effects/ChromaKey.h"

namespace rt {

ChromaKey::ChromaKey()
    : Effect(EffectType::ChromaKey)
{
    // Key color (RGB) — green screen default
    addParam("Key Color R",     0.0f,    0.0f, 1.0f);
    addParam("Key Color G",     1.0f,    0.0f, 1.0f);
    addParam("Key Color B",     0.0f,    0.0f, 1.0f);

    // Output / preset
    addParam("Output",          0.0f,    0.0f, 5.0f);   // 0=Composite, 1=Alpha Matte,
                                                         // 2=Color, 3=Original,
                                                         // 4=Removed Color, 5=Spill Map
    addParam("Setting",         0.0f,    0.0f, 3.0f);   // 0=Default, 1=Relaxed, 2=Aggressive, 3=Custom

    // Matte Generation
    addParam("Transparency",   45.0f,    0.0f, 100.0f);
    addParam("Highlight",      10.0f,    0.0f, 100.0f);
    addParam("Shadow",         50.0f,    0.0f, 100.0f);
    addParam("Tolerance",      50.0f,    0.0f, 100.0f);
    addParam("Pedestal",       10.0f,    0.0f, 100.0f);

    // Matte Cleanup
    addParam("Choke",           0.0f,    0.0f, 100.0f);
    addParam("Soften",          0.0f,    0.0f, 100.0f);
    addParam("Contrast",        0.0f,    0.0f, 100.0f);
    addParam("Mid Point",      50.0f,    0.0f, 100.0f);

    // Spill Suppression
    addParam("Desaturate",     25.0f,    0.0f, 50.0f);
    addParam("Range",          50.0f,    0.0f, 100.0f);
    addParam("Spill",          50.0f,    0.0f, 100.0f);
    addParam("Luma",           50.0f,    0.0f, 100.0f);

    // Foreground Color Correction
    addParam("Saturation",    100.0f,    0.0f, 200.0f);
    addParam("Hue",             0.0f, -180.0f, 180.0f);
    addParam("Luminance",     100.0f,    0.0f, 200.0f);
}

std::unique_ptr<Effect> ChromaKey::clone() const
{
    auto copy = std::make_unique<ChromaKey>();
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

void ChromaKey::applyPreset(int preset)
{
    // Preset values: Tolerance, Shadow, Highlight, Spill, Pedestal
    switch (preset) {
    case 0: // Default
        m_params[Transparency].track.setDefaultValue(45.0f);
        m_params[Highlight].track.setDefaultValue(10.0f);
        m_params[Shadow].track.setDefaultValue(50.0f);
        m_params[Tolerance].track.setDefaultValue(50.0f);
        m_params[Pedestal].track.setDefaultValue(10.0f);
        m_params[Spill].track.setDefaultValue(50.0f);
        break;
    case 1: // Relaxed
        m_params[Transparency].track.setDefaultValue(45.0f);
        m_params[Highlight].track.setDefaultValue(5.0f);
        m_params[Shadow].track.setDefaultValue(40.0f);
        m_params[Tolerance].track.setDefaultValue(30.0f);
        m_params[Pedestal].track.setDefaultValue(5.0f);
        m_params[Spill].track.setDefaultValue(30.0f);
        break;
    case 2: // Aggressive
        m_params[Transparency].track.setDefaultValue(45.0f);
        m_params[Highlight].track.setDefaultValue(20.0f);
        m_params[Shadow].track.setDefaultValue(70.0f);
        m_params[Tolerance].track.setDefaultValue(80.0f);
        m_params[Pedestal].track.setDefaultValue(20.0f);
        m_params[Spill].track.setDefaultValue(75.0f);
        break;
    default: // Custom — no changes
        break;
    }
}

} // namespace rt
