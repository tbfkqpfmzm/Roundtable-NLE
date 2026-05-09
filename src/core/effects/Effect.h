/*
 * Effect — base class for all GPU-accelerated effects.
 *
 * Each effect has:
 *  - A unique type ID and human-readable name
 *  - A set of keyframeable parameters (KeyframeTrack<float>)
 *  - Enable/disable toggle
 *  - Serialization support via parameter map
 *
 * Effects are processed by EffectProcessor (GPU compute shaders).
 * The CPU-side Effect objects define parameters; the GPU side executes them.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"

namespace rt {

// ── Effect type identifiers ─────────────────────────────────────────────────

enum class EffectType : uint8_t
{
    ColorCorrect,
    Blur,
    Sharpen,
    Glow,
    ChromaKey,
    Transform2D,
    Vignette,
    LUT,
    Letterbox,
    ColorGrading,
    LumetriColor,
    OtsLeft,
    OtsRight,
    // Audio effects
    FillLeftWithRight,
    FillRightWithLeft,
    Count
};

/// Human-readable name for an effect type
inline const char* effectTypeName(EffectType t) noexcept
{
    switch (t) {
    case EffectType::ColorCorrect: return "Color Correct";
    case EffectType::Blur:         return "Gaussian Blur";
    case EffectType::Sharpen:      return "Sharpen";
    case EffectType::Glow:         return "Glow";
    case EffectType::ChromaKey:    return "Ultra Key";
    case EffectType::Transform2D:  return "Transform 2D";
    case EffectType::Vignette:     return "Vignette";
    case EffectType::LUT:          return "LUT";
    case EffectType::Letterbox:    return "Letterbox";
    case EffectType::ColorGrading: return "Color Grading";
    case EffectType::LumetriColor: return "Color Grading (legacy)";
    case EffectType::OtsLeft:      return "OTS LEFT";
    case EffectType::OtsRight:     return "OTS RIGHT";
    case EffectType::FillLeftWithRight: return "Fill Left with Right";
    case EffectType::FillRightWithLeft: return "Fill Right with Left";
    default:                       return "Unknown";
    }
}

/// Returns true for audio-only effects (no GPU shader needed)
inline bool isAudioEffect(EffectType t) noexcept
{
    return t == EffectType::FillLeftWithRight
        || t == EffectType::FillRightWithLeft;
}

// ── Parameter descriptor ────────────────────────────────────────────────────

struct EffectParam
{
    std::string          name;
    KeyframeTrack<float> track;
    float                minVal{0.0f};
    float                maxVal{1.0f};
};

// ── Effect base class ───────────────────────────────────────────────────────

class Effect
{
public:
    explicit Effect(EffectType type);
    virtual ~Effect();

    // Non-copyable, movable
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;
    Effect(Effect&&) noexcept = default;
    Effect& operator=(Effect&&) noexcept = default;

    // ── Identity ────────────────────────────────────────────────────────
    [[nodiscard]] EffectType          effectType() const noexcept { return m_type; }
    [[nodiscard]] const char*         name()       const noexcept { return effectTypeName(m_type); }
    [[nodiscard]] uint64_t            id()         const noexcept { return m_id; }

    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }
    void setEnabled(bool v) noexcept { m_enabled = v; }

    // ── Parameters ──────────────────────────────────────────────────────
    [[nodiscard]] size_t                         paramCount() const noexcept { return m_params.size(); }
    [[nodiscard]] const EffectParam&             param(size_t i) const { return m_params[i]; }
    [[nodiscard]] EffectParam&                   param(size_t i) { return m_params[i]; }
    [[nodiscard]] const std::vector<EffectParam>& params() const noexcept { return m_params; }

    /// Find parameter by name. Returns nullptr if not found.
    [[nodiscard]] EffectParam*       findParam(const std::string& name);
    [[nodiscard]] const EffectParam* findParam(const std::string& name) const;

    /// Evaluate parameter at time (convenience)
    [[nodiscard]] float evalParam(size_t i, int64_t time) const
    {
        return m_params[i].track.evaluate(time);
    }

    /// Evaluate all parameters at a given time into a flat float array.
    /// Returns a vector of values in parameter order — used by GPU dispatch.
    [[nodiscard]] std::vector<float> evalAllParams(int64_t time) const;

    // ── Clone ───────────────────────────────────────────────────────────
    [[nodiscard]] virtual std::unique_ptr<Effect> clone() const = 0;

protected:
    /// Derived classes call this in their constructor to register parameters
    void addParam(const std::string& name, float defaultValue,
                  float minVal = 0.0f, float maxVal = 1.0f);

    EffectType m_type;
    uint64_t   m_id;
    bool       m_enabled{true};

    std::vector<EffectParam> m_params;

    static uint64_t s_nextId;
};

// ── Factory ─────────────────────────────────────────────────────────────────

/// Create an effect instance by type.
[[nodiscard]] std::unique_ptr<Effect> createEffect(EffectType type);

} // namespace rt
