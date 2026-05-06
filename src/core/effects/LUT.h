/*
 * LUT — 3D Look-Up Table color grading effect.
 *
 * Loads .cube (Resolve/Adobe) LUT files and applies them via compute shader.
 * Supports intensity parameter for blending between original and graded.
 */

#pragma once

#include "effects/Effect.h"

#include <array>
#include <string>
#include <vector>

namespace rt {

class LUT : public Effect
{
public:
    LUT();
    ~LUT() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Intensity = 0,  // 0–1 (blend between original and LUT-graded)
        ParamCount
    };

    // ── LUT data ────────────────────────────────────────────────────────

    /// Load a .cube LUT file from disk.
    bool loadCubeFile(const std::string& path);

    /// Get the 3D LUT data (size^3 × 3 floats, row-major, R varies fastest).
    [[nodiscard]] const std::vector<float>& lutData() const noexcept { return m_lutData; }
    [[nodiscard]] int lutSize() const noexcept { return m_lutSize; }
    [[nodiscard]] const std::string& lutPath() const noexcept { return m_lutPath; }

    [[nodiscard]] bool hasLUT() const noexcept { return m_lutSize > 0 && !m_lutData.empty(); }

private:
    std::vector<float> m_lutData;    // size^3 × 3 floats (RGB)
    int                m_lutSize{0}; // dimension (e.g. 33 for 33x33x33)
    std::string        m_lutPath;
};

} // namespace rt
