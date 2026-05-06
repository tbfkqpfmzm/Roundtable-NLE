/*
 * ColorGrading.cpp Ã¢â‚¬â€ Unified color grading effect implementation.
 */

#include "effects/ColorGrading.h"

namespace rt {

ColorGrading::ColorGrading()
 : Effect(EffectType::ColorGrading)
{
 // Initialize curves to identity (linear ramp)
 resetCurveLUTs();
 // Ã¢â€â‚¬Ã¢â€â‚¬ Basic Correction Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 addParam("Temperature", 0.0f, -100.0f, 100.0f);
 addParam("Tint", 0.0f, -100.0f, 100.0f);
 addParam("Exposure", 0.0f, -5.0f, 5.0f);
 addParam("Contrast", 0.0f, -100.0f, 100.0f);
 addParam("Highlights", 0.0f, -100.0f, 100.0f);
 addParam("Shadows", 0.0f, -100.0f, 100.0f);
 addParam("Whites", 0.0f, -100.0f, 100.0f);
 addParam("Blacks", 0.0f, -100.0f, 100.0f);
 addParam("Saturation", 100.0f, 0.0f, 200.0f);

 // Ã¢â€â‚¬Ã¢â€â‚¬ Creative Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 addParam("Faded Film", 0.0f, 0.0f, 100.0f);
 addParam("Sharpen", 0.0f, 0.0f, 100.0f);
 addParam("Vibrance", 0.0f, -100.0f, 100.0f);
 addParam("Creative Sat",100.0f, 0.0f, 200.0f);

 // Ã¢â€â‚¬Ã¢â€â‚¬ Color Wheels (lift/gamma/gain per tonal range) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 addParam("Shadow R", 0.0f, -1.0f, 1.0f);
 addParam("Shadow G", 0.0f, -1.0f, 1.0f);
 addParam("Shadow B", 0.0f, -1.0f, 1.0f);
 addParam("Midtone R", 0.0f, -1.0f, 1.0f);
 addParam("Midtone G", 0.0f, -1.0f, 1.0f);
 addParam("Midtone B", 0.0f, -1.0f, 1.0f);
 addParam("Highlight R", 0.0f, -1.0f, 1.0f);
 addParam("Highlight G", 0.0f, -1.0f, 1.0f);
 addParam("Highlight B", 0.0f, -1.0f, 1.0f);
 addParam("Shadow Master", 0.0f, -1.0f, 1.0f);
 addParam("Midtone Master",0.0f, -1.0f, 1.0f);
 addParam("Highlight Master",0.0f,-1.0f, 1.0f);

 // Ã¢â€â‚¬Ã¢â€â‚¬ Vignette Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 addParam("Vignette Amount", 0.0f, -5.0f, 5.0f);
 addParam("Vignette Midpoint", 50.0f, 0.0f, 100.0f);
 addParam("Vignette Roundness",50.0f, 0.0f, 100.0f);
 addParam("Vignette Feather", 50.0f, 0.0f, 100.0f);
}

std::unique_ptr<Effect> ColorGrading::clone() const
{
 auto copy = std::make_unique<ColorGrading>();
 copy->m_enabled = m_enabled;
 copy->m_basicEnabled = m_basicEnabled;
 copy->m_creativeEnabled = m_creativeEnabled;
 copy->m_wheelsEnabled = m_wheelsEnabled;
 copy->m_curvesEnabled = m_curvesEnabled;
 copy->m_hslEnabled = m_hslEnabled;
 copy->m_vignetteEnabled = m_vignetteEnabled;
 copy->m_curveLuts = m_curveLuts;
 copy->m_curvesIdentity = m_curvesIdentity;
 copy->m_hslParams = m_hslParams;
 for (size_t i = 0; i < m_params.size(); ++i)
 copy->m_params[i].track = m_params[i].track;
 return copy;
}

void ColorGrading::resetCurveLUTs()
{
 for (auto& lut : m_curveLuts) {
 for (size_t i = 0; i < 256; ++i)
 lut[i] = static_cast<float>(i) / 255.0f;
 }
 m_curvesIdentity = true;
}

std::vector<float> ColorGrading::evalAllParamsWithFlags(int64_t time) const
{
 // Evaluate all normal parameters
 auto vals = evalAllParams(time);
 // Append section-enable flags as floats (0.0 = off, 1.0 = on)
 vals.push_back(m_basicEnabled ? 1.0f : 0.0f);
 vals.push_back(m_creativeEnabled ? 1.0f : 0.0f);
 vals.push_back(m_wheelsEnabled ? 1.0f : 0.0f);
 vals.push_back(m_vignetteEnabled ? 1.0f : 0.0f);
 return vals;
}

std::vector<float> ColorGrading::evalGpuParams(int64_t time) const
{
 // Repack effect params into 28 floats for GPU push constants.
 // Shader layout:
 // [0-8] Basic (Temperature through Saturation)
 // [9] FadedFilm, [10] Vibrance, [11] CreativeSat
 // [12-15] Vignette
 // [16-18] Shadow R/G/B, [19-21] Midtone R/G/B, [22-24] Highlight R/G/B
 // [25] Shadow Master, [26] Midtone Master, [27] Highlight Master
 auto all = evalAllParams(time);
 std::vector<float> gpu(28, 0.0f);

 // [0-8] Basic Correction (Temperature through Saturation)
 for (size_t i = 0; i <= Saturation && i < all.size(); ++i)
 gpu[i] = all[i];

 // [9] Faded Film, [10] Vibrance, [11] Creative Sat
 if (FadedFilm < all.size()) gpu[9] = all[FadedFilm];
 if (Vibrance < all.size()) gpu[10] = all[Vibrance];
 if (CreativeSat < all.size()) gpu[11] = all[CreativeSat];

 // [12-15] Vignette
 if (VignetteAmount < all.size()) gpu[12] = all[VignetteAmount];
 if (VignetteMidpoint < all.size()) gpu[13] = all[VignetteMidpoint];
 if (VignetteRoundness < all.size()) gpu[14] = all[VignetteRoundness];
 if (VignetteFeather < all.size()) gpu[15] = all[VignetteFeather];

 // [16-18] Shadow R/G/B
 if (ShadowR < all.size()) gpu[16] = all[ShadowR];
 if (ShadowG < all.size()) gpu[17] = all[ShadowG];
 if (ShadowB < all.size()) gpu[18] = all[ShadowB];

 // [19-21] Midtone R/G/B
 if (MidtoneR < all.size()) gpu[19] = all[MidtoneR];
 if (MidtoneG < all.size()) gpu[20] = all[MidtoneG];
 if (MidtoneB < all.size()) gpu[21] = all[MidtoneB];

 // [22-24] Highlight R/G/B
 if (HighlightR < all.size()) gpu[22] = all[HighlightR];
 if (HighlightG < all.size()) gpu[23] = all[HighlightG];
 if (HighlightB < all.size()) gpu[24] = all[HighlightB];

 // [25-27] Master offsets
 if (ShadowMaster < all.size()) gpu[25] = all[ShadowMaster];
 if (MidtoneMaster < all.size()) gpu[26] = all[MidtoneMaster];
 if (HighlightMaster < all.size()) gpu[27] = all[HighlightMaster];

 return gpu;
}

} // namespace rt
