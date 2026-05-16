/*
 * NativeSpineEngine — runtime evaluator for .shotanim files.
 *
 * Mirrors the public SpineEngine API exactly so callers (SpineRenderer,
 * SpinePrerenderer, etc.) can be re-pointed at this engine via a CMake
 * gate without source-level changes.
 *
 * Pipeline:
 *   .shotanim  ──► ShotAnimReader  ──► rt::SkeletonPkg
 *                                          │
 *                                          ▼
 *                                  NativeSpineEngine (this class)
 *                                          │
 *                                          ▼
 *                                  rt::SpineRenderData (existing struct)
 *                                          │
 *                                          ▼
 *                                  SpineRenderer (unchanged)
 *
 * This engine has ZERO spine-cpp dependency. It is built independently
 * of ROUNDTABLE_HAS_SPINE and gated by ROUNDTABLE_HAS_NATIVE_SHOTANIM.
 *
 * STATUS (Phase 2, 2026-05-15): per-frame evaluator implemented for the
 * common case — bone hierarchy, region attachments, rigid (non-skinned)
 * meshes, slot colors, draw-order timelines, Linear/Stepped/Bezier curve
 * sampling.
 *
 * Phase 2.5+ deferred features (engine still loads files that use these,
 * but doesn't render or apply them yet):
 *   - Skinned mesh attachments (meshBoneCount > 0): not drawn.
 *   - Clipping attachments: clip-mask state is not enforced; clipped
 *     slots draw as if unclipped.
 *   - IK / Transform / Path constraints: setup data is loaded but
 *     constraint solving is not run, so animations that REQUIRE
 *     constraints to look right will look off.
 *   - Deform (FFD) timelines: vertex offsets are not applied.
 *   - Bone inherit modes other than Normal: treated as Normal.
 *   - Two-color tint (slot.darkColor): only main color is applied.
 *   - Talk animation overlay: bodyTime path is fully evaluated;
 *     talkTime path is stored but its timelines aren't blended in yet.
 */

#pragma once

#ifdef ROUNDTABLE_HAS_NATIVE_SHOTANIM

#include "skeleton/SkeletonTypes.h"
#include "timeline/SpineClip.h"

// Render-data structs (SpineRenderData et al.) are canonically defined
// in SpineEngine.h. When spine-cpp is part of the build we pick them up
// from there; when it's not, we declare local copies in this header.
#ifdef ROUNDTABLE_HAS_SPINE
#  include "spine/SpineEngine.h"
#endif

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace rt {

#ifndef ROUNDTABLE_HAS_SPINE

enum class SpineBlendMode : uint8_t {
    Normal, Additive, Multiply, Screen,
};

struct SpineVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

struct SpineRenderBatch {
    int                       texturePageIndex = -1;
    SpineBlendMode            blendMode = SpineBlendMode::Normal;
    std::vector<SpineVertex>  vertices;
    std::vector<uint16_t>     indices;
};

struct SpineRenderData {
    std::vector<SpineRenderBatch> batches;
    float boundsX = 0, boundsY = 0, boundsW = 0, boundsH = 0;
};

#endif // !ROUNDTABLE_HAS_SPINE

/// Native runtime equivalent of SpineEngine, consuming .shotanim files
/// (rt::SkeletonPkg) instead of spine-cpp's SkeletonData.
class NativeSpineEngine {
public:
    NativeSpineEngine();
    ~NativeSpineEngine();

    NativeSpineEngine(const NativeSpineEngine&) = delete;
    NativeSpineEngine& operator=(const NativeSpineEngine&) = delete;
    NativeSpineEngine(NativeSpineEngine&&) noexcept;
    NativeSpineEngine& operator=(NativeSpineEngine&&) noexcept;

    /// Load skeleton from a .shotanim file on disk.
    /// Sibling .png texture pages are resolved relative to the file's
    /// directory using the page paths embedded in the .shotanim header.
    bool loadSkeleton(const std::string& shotanimPath);

    /// Load skeleton from a SpineClip + asset base directory.
    /// Looks for character.shotanim in the resolved stance directory.
    bool loadFromClip(const SpineClip& clip, const std::string& assetsDir);

    [[nodiscard]] bool isLoaded() const noexcept { return m_pkg && !m_pkg->bones.empty(); }
    [[nodiscard]] const std::string& loadedPath() const noexcept { return m_loadedPath; }

    /// Skeleton dimensions from the .shotanim canvas
    [[nodiscard]] float skeletonWidth() const;
    [[nodiscard]] float skeletonHeight() const;

    /// Active skin (by name from skin table)
    bool setSkin(const std::string& skinName);
    [[nodiscard]] std::vector<std::string> listSkins() const;

    /// Set the current animation by name. Returns false if not found.
    bool setBodyAnimation(const std::string& name, bool loop);
    bool setTalkAnimation(const std::string& name, bool loop);

    /// Available animations (for character UI)
    [[nodiscard]] std::vector<std::string> listAnimations() const;

    /// Skeleton position offset (added to all bone world transforms)
    void setPosition(float x, float y);
    void setScale(float sx, float sy);

    /// Evaluate the pose at the specified time. No state carried frame-to-frame.
    void evaluateAtTime(float bodyTime, float talkTime = 0.0f);

    /// Advance both tracks by dt and evaluate.
    void update(float dt);

    /// Talking flag (gates whether talkTime contributes to evaluation).
    void setTalking(bool t);
    [[nodiscard]] bool isTalking() const noexcept { return m_talking; }

    /// Extract all render meshes for the current pose. Result batches
    /// are grouped by texture page + blend mode.
    [[nodiscard]] SpineRenderData extractMeshes();

    /// Axis-aligned bounding box for the current pose.
    void getBounds(float& x, float& y, float& w, float& h);

    /// Direct access to the loaded skeleton package (for diagnostics).
    [[nodiscard]] const rt::SkeletonPkg* package() const noexcept { return m_pkg.get(); }

    /// Per-bone world transform after the last evaluateAtTime(), one row
    /// per bone in package bone order: {a, b, c, d, worldX, worldY}.
    /// For the parity harness's bone-by-bone divergence diff.
    [[nodiscard]] std::vector<std::array<float, 6>> debugBoneWorld() const;

    /// Per-slot active attachment after the last evaluateAtTime().
    /// Index = slot index; value = attachment pool index in package
    /// (-1 = no attachment / invisible). For the parity harness's
    /// slot-by-slot attachment divergence diff.
    [[nodiscard]] std::vector<int32_t> debugSlotAttachments() const;

private:
    // ─── Per-frame state (transient; rebuilt by evaluateAtTime) ─────────
    // World transform per bone: 2x2 affine matrix [a,b;c,d] + (worldX, worldY).
    // A vertex (lx, ly) on this bone maps to world coords:
    //   wx = a*lx + b*ly + worldX
    //   wy = c*lx + d*ly + worldY
    struct BoneWorld {
        float a = 1.0f, b = 0.0f;
        float c = 0.0f, d = 1.0f;
        float worldX = 0.0f, worldY = 0.0f;
    };
    struct SlotState {
        int32_t  attachmentIdx = -1;          ///< -1 = no attachment
        float    r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;  ///< unclamped 0..1
    };

    // Reusable per-frame buffers (pre-allocated at load time;
    // resized in evaluateAtTime, kept across frames to avoid heap thrash).
    std::vector<BoneWorld>  m_bonesWorld;
    std::vector<SlotState>  m_slotState;
    std::vector<int32_t>    m_drawOrder;
    bool                    m_frameValid = false;

    // Per-skin attachment lookup, built at load time. Resolves an
    // AttachmentTimeline value (which carries a string-table index) to
    // a global attachment pool index, using the current skin's mapping.
    // Index 0 = default skin. Outer vector is parallel to m_pkg->skins;
    // inner map keys are (uint64_t)slotIdx << 32 | nameStringIdx.
    std::vector<std::unordered_map<uint64_t, int32_t>>  m_skinAttachLookup;

    std::unique_ptr<rt::SkeletonPkg>  m_pkg;
    std::string                       m_loadedPath;

    int32_t  m_currentBodyAnim = -1;   ///< index into m_pkg->animations
    int32_t  m_currentTalkAnim = -1;
    int32_t  m_currentSkin     = 0;    ///< default skin
    float    m_bodyTime        = 0.0f;
    float    m_talkTime        = 0.0f;
    bool     m_bodyLoop        = true;
    bool     m_talkLoop        = true;
    bool     m_talking         = false;
    float    m_posX            = 0.0f;
    float    m_posY            = 0.0f;
    float    m_scaleX          = 1.0f;
    float    m_scaleY          = 1.0f;
};

} // namespace rt

#endif // ROUNDTABLE_HAS_NATIVE_SHOTANIM
