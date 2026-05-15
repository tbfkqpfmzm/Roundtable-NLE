/*
 * SkeletonTypes.h — Clean-room skeletal animation data structures.
 *
 * These structures hold IMPORTED data from .skel/.atlas files.
 * They are the intermediate representation between the binary parser
 * (SkelBinaryReader) and the runtime evaluator (SkeletonEvaluator).
 *
 * All types in namespace rt. No "Spine" in any identifier.
 * No virtual functions. No dynamic polymorphism.
 * Contiguous arrays for cache efficiency.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rt {

// ─── Atlas types ────────────────────────────────────────────────────────────

/// A single texture page described in the .atlas file
struct AtlasPage {
    std::string name;       ///< e.g. "c260_00.png"
    uint16_t width  = 0;
    uint16_t height = 0;
};

/// A rectangular region within an atlas page, with precomputed UVs
struct AtlasRegion {
    int32_t  pageIndex = -1;
    float    u0 = 0, v0 = 0;   ///< top-left UV (normalized 0..1)
    float    u1 = 1, v1 = 1;   ///< bottom-right UV
    uint16_t pixelWidth  = 0;
    uint16_t pixelHeight = 0;
    int16_t  offsetX = 0;      ///< trim offset from original
    int16_t  offsetY = 0;
    bool     rotated = false;
};

// ─── Bone hierarchy ─────────────────────────────────────────────────────────

/// How a bone inherits its parent's transform
enum class BoneInherit : uint8_t {
    Normal              = 0,
    OnlyTranslation     = 1,
    NoRotationOrReflection = 2,
    NoScale             = 3,
    NoScaleOrReflection = 4
};

/// Static data for one bone in the skeleton
struct BoneData {
    int32_t    parentIndex = -1;   ///< -1 = root bone
    float      x = 0, y = 0;      ///< local position
    float      rotation = 0;       ///< local rotation (degrees)
    float      scaleX = 1, scaleY = 1;
    float      shearX = 0, shearY = 0;
    float      length = 0;
    BoneInherit inherit = BoneInherit::Normal;
    float      colorR = 1, colorG = 1, colorB = 1, colorA = 1;
};

// ─── Slots ──────────────────────────────────────────────────────────────────

/// Blend mode for draw batches
enum class SlotBlend : uint8_t {
    Normal   = 0,
    Additive = 1,
    Multiply = 2,
    Screen   = 3
};

/// Static data for one slot (drawable attachment point)
struct SlotData {
    int32_t   boneIndex = -1;
    int32_t   defaultAttachmentIdx = -1;  ///< -1 = no attachment
    uint32_t  color = 0xFFFFFFFFu;        ///< RGBA packed (ABGR in memory)
    uint32_t  darkColor = 0x000000FFu;    ///< RGBA dark tint (two-color)
    SlotBlend blend = SlotBlend::Normal;
};

// ─── Attachments ────────────────────────────────────────────────────────────

enum class AttachKind : uint8_t {
    Region      = 0,
    BoundingBox = 1,
    Mesh        = 2,
    LinkedMesh  = 3,
    Path        = 4,
    Point       = 5,
    Clipping    = 6
};

/// Generic attachment data. Region attachments store their vertices inline.
/// Mesh attachments reference pool arrays in the parent RtSkelPkg.
struct AttachmentData {
    AttachKind kind = AttachKind::Region;
    int32_t    regionIdx = -1;       ///< index into AtlasRegion array
    int32_t    parentMeshIdx = -1;   ///< for LinkedMesh

    // Region attachments: 4 corners of the quad (8 floats: x0,y0,x1,y1,x2,y2,x3,y3)
    float      localVerts[8] = {};

    // UVs for the 4 corners (matching vertex order)
    float      localUVs[8] = {};

    // Mesh attachments: ranges into RtSkelPkg pool arrays
    uint32_t   meshVertOffset  = 0;
    uint32_t   meshVertCount   = 0;
    uint32_t   meshTriOffset   = 0;
    uint32_t   meshTriCount    = 0;
    uint32_t   meshBoneOffset  = 0;   ///< into meshBones[]
    uint32_t   meshWeightOffset = 0;  ///< into meshWeights[]

    // Mesh hull / edges (for FFD / deform)
    uint32_t   meshEdgeCount   = 0;
    uint32_t   meshHullCount   = 0;

    // Clipping
    uint32_t   clipEndSlotIdx  = 0;   ///< which slot ends the clip region

    // Common
    float      originX = 0, originY = 0;
    float      width   = 0, height  = 0;
    float      colorR  = 1, colorG  = 1, colorB = 1, colorA = 1;
};

// ─── Skins ──────────────────────────────────────────────────────────────────

/// A skin maps (slotIndex → attachmentIndex) pairs.
/// Pairs are sorted by slotIndex for binary-search lookup at runtime.
struct SkinData {
    std::string          name;
    std::vector<int32_t> slotIndices;       ///< sorted ascending
    std::vector<int32_t> attachmentIndices; ///< parallel to slotIndices
};

// ─── Keyframes & Timelines ──────────────────────────────────────────────────

enum class CurveStyle : uint8_t {
    Linear  = 0,
    Stepped = 1,
    Bezier  = 2
};

/// Precomputed bezier lookup table: 100 samples mapping t→eased_value.
/// Computed at import time via Newton iteration; sampled at runtime via linear LUT lookup.
struct BezierLUT {
    float samples[100] = {};
};

/// Per-keyframe curve descriptor
struct KeyCurve {
    CurveStyle style = CurveStyle::Linear;
    BezierLUT  lut;             ///< precomputed for bezier, unused for linear/stepped
    float      cx1=0, cy1=0;   ///< bezier control points (raw, for debugging)
    float      cx2=0, cy2=0;
};

/// Timeline = sorted keyframe times + values + per-keyframe curves.
/// Template on value type V (float for rotate, Vector2 for translate/scale/shear).
template <typename V>
struct KeyTimeline {
    std::vector<float>    times;   ///< sorted ascending
    std::vector<V>        values;  ///< one per keyframe
    std::vector<KeyCurve> curves;  ///< one per keyframe interval (size = times.size()-1 or 0)
};

/// 2D vector for translate/scale/shear keyframes
struct Vec2 { float x = 0, y = 0; };

// ─── Animation clips ────────────────────────────────────────────────────────

/// All keyframe data for a single bone property across one animation
struct BoneTrackSet {
    KeyTimeline<float> rotate;
    KeyTimeline<Vec2>  translate;
    KeyTimeline<Vec2>  scale;
    KeyTimeline<Vec2>  shear;
};

/// Keyframe data for one slot across one animation
struct SlotTrackSet {
    KeyTimeline<int32_t>  attachment;  ///< value = index into AttachmentData[]
    KeyTimeline<float>    colorR;
    KeyTimeline<float>    colorG;
    KeyTimeline<float>    colorB;
    KeyTimeline<float>    colorA;
    KeyTimeline<float>    darkR;       ///< two-color dark tint
    KeyTimeline<float>    darkG;
    KeyTimeline<float>    darkB;
    KeyTimeline<float>    darkA;
};

/// Draw order keyframe: at a given time, slots are drawn in a specific order
struct DrawOrderKey {
    float                   time = 0;
    std::vector<int32_t>    slotOrder;  ///< permutation of [0..slotCount-1]
};

struct DrawOrderTrack {
    std::vector<DrawOrderKey> keys;     ///< sorted by time
};

/// Deform keyframes: per-vertex offsets for a mesh attachment
struct DeformKey {
    float                time = 0;
    std::vector<float>   verts;   ///< vertex position offsets, length = meshVertCount*2
};

struct DeformTrack {
    std::vector<DeformKey> keys;    ///< sorted by time
};

/// One named animation clip (idle, angry, talk_start, etc.)
struct AnimClip {
    std::string                   name;
    float                         duration = 0;

    std::vector<BoneTrackSet>     boneTracks;    ///< indexed by boneIndex
    std::vector<SlotTrackSet>     slotTracks;    ///< indexed by slotIndex
    DrawOrderTrack                drawOrder;
    std::vector<DeformTrack>      deformTracks;  ///< indexed by attachmentIndex

    // IK constraint animation (if any)
    struct IkKey {
        float time = 0;
        float mix = 0;
        float bendPositive = 0;
    };
    struct IkTrack {
        std::vector<IkKey> keys;
    };
    std::vector<IkTrack>          ikTracks;       ///< indexed by ikConstraintIndex

    // Transform constraint animation (if any)
    struct TransformKey {
        float time = 0;
        float mixRotate=0, mixTranslate=0, mixScale=0, mixShear=0;
    };
    struct TransformTrack {
        std::vector<TransformKey> keys;
    };
    std::vector<TransformTrack>   transformTracks; ///< indexed by transformConstraintIndex
};

// ─── Constraints (setup pose data) ──────────────────────────────────────────

struct IkConstraintData {
    int32_t  targetBoneIdx = -1;
    int32_t  bendBoneIdx   = -1;
    uint8_t  bendDirection = 1;    ///< 1 or -1
    float    mix = 1;
    std::vector<int32_t> constrainedBones;  ///< bone indices affected by this IK
};

struct TransformConstraintData {
    int32_t  targetBoneIdx = -1;
    float    mixRotate = 0, mixTranslate = 0, mixScale = 0, mixShear = 0;
    std::vector<int32_t> constrainedBones;
};

// ─── Events ─────────────────────────────────────────────────────────────────

struct EventData {
    std::string name;
    int32_t     intValue = 0;
    float       floatValue = 0;
    std::string stringValue;
};

// ─── Top-level skeleton package ─────────────────────────────────────────────

/// All imported data for one character/outfit/stance combination.
/// Built by SkelBinaryReader + AtlasParser.
/// Consumed by SkeletonEvaluator at runtime.
struct SkeletonPkg {
    // Atlas
    std::vector<AtlasPage>        pages;
    std::vector<AtlasRegion>      regions;

    // Skeleton
    std::vector<BoneData>         bones;
    std::vector<SlotData>         slots;

    // Attachments (all types pooled here)
    std::vector<AttachmentData>   attachments;

    // Mesh pools (referenced by AttachmentData offsets)
    std::vector<float>            meshVertices;   ///< all mesh vertex positions
    std::vector<uint16_t>         meshTriangles;  ///< all mesh triangle indices
    std::vector<int32_t>          meshBones;      ///< per-vertex bone indices (skinning)
    std::vector<float>            meshWeights;    ///< per-vertex bone weights

    // Skins
    std::vector<SkinData>         skins;
    int32_t                       defaultSkinIdx = 0;

    // Constraints
    std::vector<IkConstraintData>        ikConstraints;
    std::vector<TransformConstraintData> transformConstraints;

    // Events
    std::vector<EventData>        events;

    // Animations
    std::vector<AnimClip>         animations;

    // String table (deduplicated names for bones, slots, attachments, skins, anims)
    std::vector<std::string>      strings;

    // Metadata
    std::string                   version;       ///< e.g. "4.1.20"
    float                         width  = 0;    ///< skeleton canvas width
    float                         height = 0;    ///< skeleton canvas height
};

} // namespace rt
