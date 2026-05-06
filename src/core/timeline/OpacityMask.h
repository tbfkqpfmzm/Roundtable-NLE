/*
 * OpacityMask — Premiere Pro–style opacity mask data model.
 *
 * Each clip can have zero or more masks. Each mask is either an ellipse,
 * rectangle, or free-draw bezier shape. The mask alpha is multiplied
 * with the clip opacity during compositing.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rt {

/// Mask shape type
enum class MaskShape : uint8_t
{
    Ellipse   = 0,
    Rectangle = 1,
    FreeDrawBezier = 2
};

/// A single bezier control point for free-draw masks.
struct MaskVertex
{
    float x{0.0f};      ///< Position X (normalized 0–1 of frame)
    float y{0.0f};      ///< Position Y (normalized 0–1 of frame)
    float inTanX{0.0f}; ///< Incoming tangent handle X
    float inTanY{0.0f}; ///< Incoming tangent handle Y
    float outTanX{0.0f};///< Outgoing tangent handle X
    float outTanY{0.0f};///< Outgoing tangent handle Y
};

/// An opacity mask on a clip.
struct OpacityMask
{
    MaskShape shape{MaskShape::Ellipse};

    /// Center position (normalized 0–1 of frame). Default = center.
    float centerX{0.5f};
    float centerY{0.5f};

    /// Size (normalized 0–1 of frame).
    float width{0.5f};
    float height{0.5f};

    /// Rotation in degrees.
    float rotation{0.0f};

    /// Feather amount in pixels (blurred edge).
    float feather{0.0f};

    /// Expansion: positive grows the mask, negative shrinks it (pixels).
    float expansion{0.0f};

    /// Mask opacity (0–1). Multiplied with the mask shape alpha.
    float maskOpacity{1.0f};

    /// When true, the mask is inverted (transparent inside, opaque outside).
    bool inverted{false};

    /// Bezier vertices for FreeDrawBezier shape.
    std::vector<MaskVertex> vertices;

    /// Display name for the UI (e.g. "Mask 1", "Mask 2").
    std::string name;
};

} // namespace rt
