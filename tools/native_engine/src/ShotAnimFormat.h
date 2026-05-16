/*
 * ShotAnimFormat.h — Binary layout for the .shotanim format.
 *
 * The .shotanim format is a self-contained binary blob holding all
 * skeletal data for one character/outfit/stance combination. It is
 * written by ShotAnimWriter (using spine-cpp's SkeletonData at import
 * time) and read by ShotAnimReader (which has zero spine-cpp dependency).
 *
 * Once written, the file owns all the structural data (bones, slots,
 * skins, attachments, animations). The runtime evaluator never touches
 * spine-cpp.
 *
 * Layout overview (all little-endian, no padding except where noted):
 *
 *   ┌─ Header (64 bytes, fixed)
 *   │  magic / version / counts / flags / canvas size
 *   ├─ String table (variable)
 *   │  All names referenced by index throughout the file
 *   ├─ Texture pages
 *   ├─ Bone hierarchy (parent-before-child order)
 *   ├─ Slot definitions (default skin attachment refs)
 *   ├─ Atlas regions (UV lookup for region/mesh attachments)
 *   ├─ Region attachments (region quads with bone-local geometry)
 *   ├─ Mesh attachments (mesh data + skinning weights)
 *   ├─ Clipping attachments (polygon mask)
 *   ├─ Linked mesh fixups (parentMeshIdx after pool ordering)
 *   ├─ Skin table (additional skin overrides)
 *   ├─ IK / Transform constraint setup data
 *   ├─ Animations (per-anim timeline list)
 *   └─ Footer (CRC32 + "ENDA" magic)
 *
 * Numeric types:
 *   u8/u16/u32  — unsigned ints, little-endian
 *   i16/i32     — signed ints, little-endian
 *   f32         — IEEE 754 single, little-endian
 *   strRef      — u16 index into the file's string table
 *   boneRef     — u16 index into the bone array (0..boneCount-1)
 *   slotRef     — u16 index into the slot array
 *   attachRef   — u32 index into the global attachment pool; 0xFFFFFFFF = none
 *
 * Versioning:
 *   The header carries a 32-bit version. Reader rejects unknown major
 *   versions. Forward-incompatible changes bump the major version.
 *
 * This header is intentionally pure-data — no I/O, no spine-cpp.
 * Both the writer and the reader include this file.
 */

#pragma once

#include <cstdint>

namespace rt {
namespace shotanim {

// ─── Magic numbers ──────────────────────────────────────────────────────────

inline constexpr uint32_t kHeaderMagic = 0x41544853u;   // "SHTA" little-endian
inline constexpr uint32_t kFooterMagic = 0x41444E45u;   // "ENDA" little-endian
//
// Version history:
//   1 — initial format (2026-05-15). IK section had a writer bug where
//       slot 1 stored the first constrained bone (not the target) and
//       slot 2 stored the target (not the bend bone), making the on-disk
//       semantics inconsistent with the in-memory IkConstraintData field
//       names. The runtime never used IK data at v1 so nothing was visibly
//       broken, but v1 files are unsafe to use with the Phase 2.8+ IK
//       solver. Loader rejects them; re-conversion (which is automatic
//       on character (re)download) produces v2.
//   2 — IK section field order matches IkConstraintData semantics:
//       slot 1 = target, slot 2 = bend bone (or -1 for 1-bone IK).
//   3 — Attachment pool indices now match the reader's read order
//       (regions → meshes → clipping). v1/v2 files had skin entries
//       referencing wrong attachments because the writer assigned
//       poolIdx in skin-iteration order while the reader reconstructs
//       the array in kind-grouped order. Surfaced by shotanim_parity:
//       native engine was rendering ~141 verts vs spine-cpp's ~2800
//       for Modernia, because skin entries pointed to region
//       attachments instead of the intended meshes.
//   4 — Region/mesh attachments now store the resolved atlas PAGE
//       index in the regionIdx slot, instead of an index into an
//       AtlasRegion array that was never serialized. v3 and earlier
//       always rendered attachments with pageIdx=-1 ("no texture"),
//       producing invisible-or-glass-effect output at runtime even
//       though the parity tool reported reasonable vert counts.
//   5 — Transform constraints now store the FULL spine 4.2 model:
//       per-axis mixes (mixX/mixY/mixScaleX/mixScaleY/mixShearY),
//       constant offsets (offsetRotation/X/Y/ScaleX/ScaleY/ShearY)
//       and the local/relative apply-mode flags. v4 and earlier only
//       stored a single mixTranslate and ZERO offsets, so the runtime
//       lerped constrained bones toward the target's bare world origin
//       instead of target.localToWorld(offsetX, offsetY). Surfaced by
//       shotanim_parity bone-diff: constrained sub-chains (hair/swords)
//       had correct matrices but world positions off by 100-180 units,
//       producing the "oversized limbs / swords too big" rendering.
//       Transform-constraint timelines now also carry all 6 mix
//       components (was 4: mixRotate/X/ScaleX/ShearY only).
inline constexpr uint32_t kCurrentVersion = 5u;

// ─── Header flags ───────────────────────────────────────────────────────────

inline constexpr uint32_t kFlagHasClipping        = 1u << 0;
inline constexpr uint32_t kFlagHasMeshAttachments = 1u << 1;
inline constexpr uint32_t kFlagHasDrawOrderKeys   = 1u << 2;
inline constexpr uint32_t kFlagHasIK              = 1u << 3;
inline constexpr uint32_t kFlagHasTransformConstr = 1u << 4;
inline constexpr uint32_t kFlagHasTwoColorTint    = 1u << 5;
inline constexpr uint32_t kFlagHasDeform          = 1u << 6;

// ─── Header layout (64 bytes) ───────────────────────────────────────────────

// Layout (offsets):
//   0  magic                      u32
//   4  version                    u32
//   8  boneCount                  u16
//  10  slotCount                  u16
//  12  skinCount                  u16
//  14  animCount                  u16
//  16  pageCount                  u16
//  18  regionAttachCount          u16
//  20  meshAttachCount            u16
//  22  clipAttachCount            u16
//  24  ikCount                    u16
//  26  transformConstraintCount   u16
//  28  stringCount                u16
//  30  _pad0                      u16
//  32  canvasWidth                f32
//  36  canvasHeight               f32
//  40  flags                      u32
//  44  reserved[20]               u8 × 20
//  64  END
struct Header {
    uint32_t magic;
    uint32_t version;
    uint16_t boneCount;
    uint16_t slotCount;
    uint16_t skinCount;
    uint16_t animCount;
    uint16_t pageCount;
    uint16_t regionAttachCount;
    uint16_t meshAttachCount;
    uint16_t clipAttachCount;
    uint16_t ikCount;
    uint16_t transformConstraintCount;
    uint16_t stringCount;
    uint16_t _pad0;
    float    canvasWidth;
    float    canvasHeight;
    uint32_t flags;
    uint8_t  reserved[20];
};
static_assert(sizeof(Header) == 64, "Header must be exactly 64 bytes");

// ─── Sentinel values for optional refs ──────────────────────────────────────

inline constexpr uint16_t kNoStringRef  = 0xFFFFu;
inline constexpr uint16_t kNoBoneRef    = 0xFFFFu;   // root bone in parent slot
inline constexpr uint16_t kNoSlotRef    = 0xFFFFu;
inline constexpr uint32_t kNoAttachRef  = 0xFFFFFFFFu;

// ─── Section magic markers ──────────────────────────────────────────────────
// Each section starts with one of these uint32_t tags so the reader can
// sanity-check the layout. Tags are 4-char ASCII (little-endian encoded).

inline constexpr uint32_t kMagicStr  = 0x52545353u;  // "SSTR" — string table
inline constexpr uint32_t kMagicPage = 0x53504750u;  // "PGPS" — texture pages
inline constexpr uint32_t kMagicBone = 0x53454E42u;  // "BNES" — bones
inline constexpr uint32_t kMagicSlot = 0x53544C53u;  // "SLTS" — slots
inline constexpr uint32_t kMagicRgn  = 0x534E4752u;  // "RGNS" — region attachments
inline constexpr uint32_t kMagicMsh  = 0x5348534Du;  // "MSHS" — mesh attachments
inline constexpr uint32_t kMagicClp  = 0x53504C43u;  // "CLPS" — clipping attachments
inline constexpr uint32_t kMagicSkin = 0x534E4B53u;  // "SKNS" — skin table
inline constexpr uint32_t kMagicIK   = 0x534B4949u;  // "IIKS" — IK constraints
inline constexpr uint32_t kMagicTfm  = 0x534D4654u;  // "TFMS" — transform constraints
inline constexpr uint32_t kMagicAnim = 0x534D4E41u;  // "ANMS" — animations

// ─── Timeline type IDs (per-animation timelines) ────────────────────────────

enum class TimelineType : uint8_t {
    BoneRotate     = 0,
    BoneTranslate  = 1,   // 2-component value
    BoneTranslateX = 2,
    BoneTranslateY = 3,
    BoneScale      = 4,
    BoneScaleX     = 5,
    BoneScaleY     = 6,
    BoneShear      = 7,
    BoneShearX     = 8,
    BoneShearY     = 9,
    SlotAttachment = 10,  // value = stringRef of attachment name (kNoStringRef = none)
    SlotRGBA       = 11,
    SlotRGB        = 12,
    SlotAlpha      = 13,
    SlotRGBA2      = 14,  // two-color
    SlotRGB2       = 15,
    SlotDeform     = 16,
    DrawOrder      = 17,
    IkConstraint   = 18,
    TransformConstr= 19,
    Event          = 20,
    // 21..255 reserved
};

// ─── Curve types (per keyframe interval) ────────────────────────────────────

enum class CurveType : uint8_t {
    Linear  = 0,
    Stepped = 1,
    Bezier  = 2,
};

// ─── Attachment type IDs (in the global attachment pool) ────────────────────

enum class AttachmentKind : uint8_t {
    Region      = 0,
    Mesh        = 1,
    LinkedMesh  = 2,
    Clipping    = 3,
    BoundingBox = 4,  // present but ignored at runtime
    Point       = 5,  // present but ignored at runtime
};

// ─── Blend modes (slot.blend) ───────────────────────────────────────────────

enum class BlendMode : uint8_t {
    Normal   = 0,
    Additive = 1,
    Multiply = 2,
    Screen   = 3,
};

// ─── Bone inherit mode ──────────────────────────────────────────────────────

enum class BoneInheritMode : uint8_t {
    Normal                 = 0,
    OnlyTranslation        = 1,
    NoRotationOrReflection = 2,
    NoScale                = 3,
    NoScaleOrReflection    = 4,
};

} // namespace shotanim
} // namespace rt
