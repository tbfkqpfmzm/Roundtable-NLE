/*
 * NativeSpineEngine.cpp — runtime evaluator for .shotanim files.
 *
 * Phase 2.9 (2026-05-15) — clipping + skinned deform landed:
 *   ✅ Loading via ShotAnimReader
 *   ✅ Skin selection, animation lookup by name, time tracking
 *   ✅ Keyframe sampling (Linear, Stepped, Bezier via 100-sample LUT)
 *   ✅ Bone pose composition (rest + animation overlay; translate/shear
 *      additive, rotate additive, scale multiplicative)
 *   ✅ World hierarchy resolve (parent-before-child) with ALL FIVE
 *      bone inherit modes (Normal, OnlyTranslation,
 *      NoRotationOrReflection, NoScale, NoScaleOrReflection).
 *   ✅ Slot state: default-skin attachment, animated RGBA color
 *   ✅ Draw order timeline (stepped sampling)
 *   ✅ Region attachment vertex transform → batched render data
 *   ✅ Rigid (non-skinned) mesh attachment vertex transform
 *   ✅ Skinned mesh attachments — vertices computed from interleaved
 *      bones+weights pools (spine's standard skinning math).
 *   ✅ Attachment-swap timelines — value (string-table index) is
 *      resolved through a per-skin (slotIdx, nameStringIdx) → poolIdx
 *      lookup built at load time. Falls back to default skin (idx 0)
 *      if the current skin doesn't define the attachment.
 *   ✅ Talk-track overlay — when isTalking() is true, the talk track's
 *      bone timelines are additively blended on top of the body track.
 *   ✅ Transform constraints — target bone's world matrix and
 *      translation are mixed into each constrained bone via the
 *      per-component mixRotate/mixTranslate factors. (mixScale and
 *      mixShear are absorbed into the matrix mix; a proper component
 *      decomposition is a Phase 2.8 refinement.)
 *   ✅ Deform (FFD) timelines on RIGID AND SKINNED meshes. Rigid:
 *      delta[v*2..v*2+1] adds to local (x, y) per vertex. Skinned:
 *      delta[infl*2..infl*2+1] adds to bone-local (x, y) per influence,
 *      BEFORE the bone transform; weights are not deformed.
 *   ✅ IK constraints (1-bone and 2-bone). 1-bone rotates the bone to
 *      face the target's world position. 2-bone uses the law of
 *      cosines on bone lengths to solve for upper/forearm-style chains.
 *      Animated mix from clip.ikTracks (lerped between keys, stepped
 *      bend direction). Chain bones are re-resolved via resolveBone
 *      after rotation so descendants see the updated transform.
 *   ✅ Clipping attachments. Sutherland-Hodgman polygon clipping
 *      against the active clip mask. The clip polygon is built in
 *      world space using the clip slot's bone matrix, normalized to
 *      CCW winding, and stays active across all subsequent slots
 *      until the draw-order walk hits the clip's endSlotIdx. Each
 *      triangle of a clipped attachment is sent through SH and the
 *      resulting polygon is fan-triangulated. Vertex attributes
 *      (x, y, u, v, r, g, b, a) are linearly interpolated at every
 *      clip-edge intersection so UVs and colors stay correct on
 *      cut edges. Assumes convex clip polygons (standard spine usage).
 *   ✅ AABB from emitted world vertices
 *
 * Still deferred:
 *   ⚠  Path constraints: not solved (audit said unused).
 *   ⚠  Two-color tint (darkColor): not applied; requires a SpineVertex
 *      format extension and shader change.
 *   ⚠  IK on chains longer than 2 bones: only 1-bone and 2-bone are
 *      handled. Longer chains fall through to the 2-bone branch using
 *      the first two constrained bones.
 *   ⚠  Transform constraint component decomposition: still uses the
 *      Phase 2.7 simplified matrix-mix. Independent mixScale / mixShear
 *      would need full decomposition.
 *   ⚠  Non-convex clip polygons: would need a different algorithm
 *      (Weiler-Atherton). In practice, spine clip masks are convex.
 */

#ifdef ROUNDTABLE_HAS_NATIVE_SHOTANIM

#include "spine/NativeSpineEngine.h"
#include "spine/ShotAnimReader.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace rt {

namespace {

// ─── Keyframe sampling ──────────────────────────────────────────────────────

/// Map u∈[0,1] through a bezier LUT (100 samples; LUT[0]≈0, LUT[99]≈1).
/// Returns the "eased" position along the curve from v0 to v1.
inline float sampleBezierLUT(const rt::BezierLUT& lut, float u) {
    if (u <= 0.0f) return lut.samples[0];
    if (u >= 1.0f) return lut.samples[99];
    const float scaled = u * 99.0f;
    const int i0 = static_cast<int>(scaled);
    const int i1 = (i0 < 99) ? (i0 + 1) : 99;
    const float frac = scaled - static_cast<float>(i0);
    return lut.samples[i0] * (1.0f - frac) + lut.samples[i1] * frac;
}

/// Apply a keyframe interval's curve to the linear parameter u∈[0,1].
inline float applyCurve(const rt::KeyTimeline<float>& tl, size_t intervalIdx, float u) {
    if (intervalIdx >= tl.curves.size()) return u;
    const auto& c = tl.curves[intervalIdx];
    switch (c.style) {
        case rt::CurveStyle::Stepped: return 0.0f;
        case rt::CurveStyle::Bezier:  return sampleBezierLUT(c.lut, u);
        case rt::CurveStyle::Linear:
        default:                       return u;
    }
}
inline float applyCurve(const rt::KeyTimeline<rt::Vec2>& tl, size_t intervalIdx, float u) {
    if (intervalIdx >= tl.curves.size()) return u;
    const auto& c = tl.curves[intervalIdx];
    switch (c.style) {
        case rt::CurveStyle::Stepped: return 0.0f;
        case rt::CurveStyle::Bezier:  return sampleBezierLUT(c.lut, u);
        case rt::CurveStyle::Linear:
        default:                       return u;
    }
}

float sampleFloat(const rt::KeyTimeline<float>& tl, float time) {
    if (tl.times.empty()) return 0.0f;
    if (time <= tl.times.front()) return tl.values.front();
    if (time >= tl.times.back())  return tl.values.back();

    // Find the interval [lo, hi] where tl.times[lo] <= time < tl.times[hi].
    auto it = std::upper_bound(tl.times.begin(), tl.times.end(), time);
    const size_t hi = static_cast<size_t>(it - tl.times.begin());
    const size_t lo = hi - 1;

    const float t0 = tl.times[lo], t1 = tl.times[hi];
    const float v0 = tl.values[lo], v1 = tl.values[hi];
    const float span = t1 - t0;
    const float u = (span > 0.0f) ? ((time - t0) / span) : 0.0f;
    const float eased = applyCurve(tl, lo, u);
    return v0 + (v1 - v0) * eased;
}

rt::Vec2 sampleVec2(const rt::KeyTimeline<rt::Vec2>& tl, float time) {
    if (tl.times.empty()) return rt::Vec2{};
    if (time <= tl.times.front()) return tl.values.front();
    if (time >= tl.times.back())  return tl.values.back();

    auto it = std::upper_bound(tl.times.begin(), tl.times.end(), time);
    const size_t hi = static_cast<size_t>(it - tl.times.begin());
    const size_t lo = hi - 1;

    const float t0 = tl.times[lo], t1 = tl.times[hi];
    const rt::Vec2 v0 = tl.values[lo], v1 = tl.values[hi];
    const float span = t1 - t0;
    const float u = (span > 0.0f) ? ((time - t0) / span) : 0.0f;
    const float eased = applyCurve(tl, lo, u);
    return rt::Vec2{ v0.x + (v1.x - v0.x) * eased, v0.y + (v1.y - v0.y) * eased };
}

/// Stepped sampling for int32 timelines (attachment swaps).
int32_t sampleI32Stepped(const rt::KeyTimeline<int32_t>& tl, float time) {
    if (tl.times.empty()) return -1;
    if (time < tl.times.front()) return tl.values.front();
    auto it = std::upper_bound(tl.times.begin(), tl.times.end(), time);
    const size_t hi = static_cast<size_t>(it - tl.times.begin());
    return (hi == 0) ? tl.values.front() : tl.values[hi - 1];
}

// ─── Polygon clipping (Sutherland-Hodgman) ──────────────────────────────────
//
// A "ClipVert" carries everything needed to emit a SpineVertex AND survive
// edge intersections (position + UV + color are linearly interpolated at
// each intersection point).
//
// The algorithm assumes the clip polygon is convex and CCW-wound. We
// normalize winding via signed area before clipping; non-convex clip
// polygons would need a different algorithm (Weiler-Atherton). Spine
// clip polygons are typically simple convex shapes (the editor doesn't
// validate convexity, but in practice masks are 4-8 vertex convex hulls).

struct ClipVert {
    float x, y;
    float u, v;
    float r, g, b, a;
};

inline ClipVert lerpClipVert(const ClipVert& a, const ClipVert& b, float t) {
    ClipVert r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    r.r = a.r + (b.r - a.r) * t;
    r.g = a.g + (b.g - a.g) * t;
    r.b = a.b + (b.b - a.b) * t;
    r.a = a.a + (b.a - a.a) * t;
    return r;
}

/// Ensure `poly` (flat [x,y,x,y,...] array) is wound CCW. Reverses in
/// place if it's CW. Used so the half-plane "inside = left of edge"
/// test is consistent regardless of how spine emitted the polygon.
inline void ensureCCW(std::vector<float>& poly) {
    const size_t n = poly.size() / 2;
    if (n < 3) return;
    float signedArea = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        signedArea += (poly[j * 2 + 0] - poly[i * 2 + 0])
                    * (poly[j * 2 + 1] + poly[i * 2 + 1]);
    }
    // signedArea > 0 = CW in screen coords (where y is down); flip.
    if (signedArea > 0.0f) {
        for (size_t i = 0; i < n / 2; ++i) {
            const size_t j = n - 1 - i;
            std::swap(poly[i * 2 + 0], poly[j * 2 + 0]);
            std::swap(poly[i * 2 + 1], poly[j * 2 + 1]);
        }
    }
}

/// Clip a polygon (`in`) against a convex polygon (`clipPoly`, CCW, flat
/// [x,y,...]). Output written to `out`. `scratch` is a working buffer
/// (ping-pong between passes). Both `out` and `scratch` are cleared.
void sutherlandHodgman(const std::vector<ClipVert>& in,
                       const std::vector<float>& clipPoly,
                       std::vector<ClipVert>& out,
                       std::vector<ClipVert>& scratch) {
    out = in;
    const size_t cN = clipPoly.size() / 2;
    if (cN < 3) return;

    for (size_t ci = 0; ci < cN; ++ci) {
        const float ax = clipPoly[ci * 2 + 0];
        const float ay = clipPoly[ci * 2 + 1];
        const size_t ni = (ci + 1) % cN;
        const float bx = clipPoly[ni * 2 + 0];
        const float by = clipPoly[ni * 2 + 1];
        const float ex = bx - ax;
        const float ey = by - ay;

        // Inside half-plane = point lies to the left of edge a→b
        // (signed 2D cross product positive).
        auto isInside = [&](float px, float py) -> bool {
            return ex * (py - ay) - ey * (px - ax) >= 0.0f;
        };
        // Parametric t along S→E where it crosses edge a-b.
        auto intersectT = [&](float sx, float sy, float ex_seg, float ey_seg) -> float {
            const float dx_seg = ex_seg - sx;
            const float dy_seg = ey_seg - sy;
            const float den = ex * dy_seg - ey * dx_seg;
            if (std::abs(den) < 1e-10f) return 0.5f;  // parallel — degenerate
            float t = (ex * (sy - ay) - ey * (sx - ax)) / -den;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            return t;
        };

        scratch.swap(out);
        out.clear();
        if (scratch.empty()) break;

        ClipVert S = scratch.back();
        bool sInside = isInside(S.x, S.y);
        for (const auto& E : scratch) {
            const bool eInside = isInside(E.x, E.y);
            if (eInside) {
                if (!sInside) {
                    const float t = intersectT(S.x, S.y, E.x, E.y);
                    out.push_back(lerpClipVert(S, E, t));
                }
                out.push_back(E);
            } else if (sInside) {
                const float t = intersectT(S.x, S.y, E.x, E.y);
                out.push_back(lerpClipVert(S, E, t));
            }
            S = E;
            sInside = eInside;
        }
    }
}

/// Sample a deform track at time t, linearly interpolating the per-vertex
/// delta arrays. Writes the interpolated deltas into `out`. Returns false
/// if the track has no keys (out is left untouched).
bool sampleDeform(const rt::DeformTrack& trk, float t, std::vector<float>& out) {
    if (trk.keys.empty()) return false;
    // Setup-fallback: spine's DeformTimeline applies NO deform (i.e. the
    // mesh's setup vertices) when t < frames[0]. Return false so the
    // caller skips the deform pass instead of stamping the first key's
    // deltas — those deltas would shove the mesh into its t=firstKey pose
    // even at t=0.
    if (t < trk.keys.front().time) return false;
    if (trk.keys.size() == 1) {
        out = trk.keys.front().verts;
        return true;
    }
    if (t >= trk.keys.back().time) {
        out = trk.keys.back().verts;
        return true;
    }
    // Find interval [lo, hi] with trk.keys[lo].time <= t < trk.keys[hi].time
    size_t hi = 1;
    while (hi < trk.keys.size() && trk.keys[hi].time <= t) ++hi;
    if (hi >= trk.keys.size()) hi = trk.keys.size() - 1;
    const size_t lo = hi - 1;
    const float t0 = trk.keys[lo].time;
    const float t1 = trk.keys[hi].time;
    const float u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;

    const auto& v0 = trk.keys[lo].verts;
    const auto& v1 = trk.keys[hi].verts;
    const size_t n = std::min(v0.size(), v1.size());
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = v0[i] + (v1[i] - v0[i]) * u;
    }
    return true;
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline uint8_t toU8(float v) { return static_cast<uint8_t>(clamp01(v) * 255.0f + 0.5f); }

// ─── Wrap / clamp time to animation duration ────────────────────────────────
float wrapTime(float t, float duration, bool loop) {
    if (duration <= 0.0f) return 0.0f;
    if (loop) {
        t = std::fmod(t, duration);
        if (t < 0.0f) t += duration;
        return t;
    }
    if (t < 0.0f) return 0.0f;
    if (t > duration) return duration;
    return t;
}

} // namespace

// ─── Construction ──────────────────────────────────────────────────────────

NativeSpineEngine::NativeSpineEngine()  = default;
NativeSpineEngine::~NativeSpineEngine() = default;
NativeSpineEngine::NativeSpineEngine(NativeSpineEngine&&) noexcept = default;
NativeSpineEngine& NativeSpineEngine::operator=(NativeSpineEngine&&) noexcept = default;

// ─── Loading ────────────────────────────────────────────────────────────────

bool NativeSpineEngine::loadSkeleton(const std::string& shotanimPath) {
    if (m_loadedPath == shotanimPath && m_pkg) return true;

    auto pkg = std::make_unique<rt::SkeletonPkg>();
    ShotAnimReader reader;
    if (!reader.readFile(shotanimPath, *pkg)) {
        spdlog::error("NativeSpineEngine: load failed — {}", reader.lastError());
        return false;
    }
    m_pkg = std::move(pkg);
    m_loadedPath = shotanimPath;
    m_currentBodyAnim = -1;
    m_currentTalkAnim = -1;
    m_currentSkin     = m_pkg->defaultSkinIdx;
    m_bodyTime = 0.0f;
    m_talkTime = 0.0f;
    m_frameValid = false;

    // Pre-allocate per-frame buffers.
    m_bonesWorld.assign(m_pkg->bones.size(), BoneWorld{});
    m_slotState.assign(m_pkg->slots.size(),  SlotState{});
    m_drawOrder.resize(m_pkg->slots.size());

    // Build per-skin attachment lookup: (slotIdx, nameStringIdx) → poolIdx.
    // Used by AttachmentTimeline resolution at runtime. Each skin has its
    // own table; resolution at runtime falls back to default skin (idx 0)
    // if the active skin doesn't define an entry.
    m_skinAttachLookup.clear();
    m_skinAttachLookup.resize(m_pkg->skins.size());
    for (size_t si = 0; si < m_pkg->skins.size(); ++si) {
        const auto& skin = m_pkg->skins[si];
        auto& table = m_skinAttachLookup[si];
        const size_t pairs = std::min(skin.slotIndices.size(),
                                      skin.attachmentIndices.size());
        for (size_t k = 0; k < pairs; ++k) {
            const int32_t slotIdx = skin.slotIndices[k];
            const int32_t poolIdx = skin.attachmentIndices[k];
            if (slotIdx < 0 || poolIdx < 0
                || poolIdx >= static_cast<int32_t>(m_pkg->attachments.size())) continue;
            const int32_t nameIdx = m_pkg->attachments[poolIdx].attachNameIdx;
            if (nameIdx < 0) continue;
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(slotIdx)) << 32)
                                | static_cast<uint32_t>(nameIdx);
            table.emplace(key, poolIdx);
        }
    }

    return true;
}

bool NativeSpineEngine::loadFromClip(const SpineClip& clip, const std::string& assetsDir) {
    fs::path base = fs::path(assetsDir) / "characters" / clip.characterName() / clip.outfit();
    if (clip.stance() == CharacterStance::Aim)        base = base / "aim";
    else if (clip.stance() == CharacterStance::Cover) base = base / "cover";

    if (!fs::exists(base)) {
        spdlog::warn("NativeSpineEngine: directory not found: {}", base.string());
        return false;
    }

    fs::path shotPath = base / "character.shotanim";
    if (!fs::exists(shotPath)) {
        for (auto& e : fs::directory_iterator(base)) {
            if (e.path().extension() == ".shotanim") { shotPath = e.path(); break; }
        }
    }
    if (!fs::exists(shotPath)) {
        spdlog::warn("NativeSpineEngine: no .shotanim in {}", base.string());
        return false;
    }

    if (!loadSkeleton(shotPath.string())) return false;

    if (!clip.animationName().empty()) {
        setBodyAnimation(clip.animationName(), clip.isLooping());
    }
    setTalking(clip.isTalking());
    return true;
}

// ─── Queries ────────────────────────────────────────────────────────────────

float NativeSpineEngine::skeletonWidth()  const { return m_pkg ? m_pkg->width  : 0.0f; }
float NativeSpineEngine::skeletonHeight() const { return m_pkg ? m_pkg->height : 0.0f; }

std::vector<std::string> NativeSpineEngine::listSkins() const {
    std::vector<std::string> out;
    if (!m_pkg) return out;
    out.reserve(m_pkg->skins.size());
    for (const auto& s : m_pkg->skins) out.push_back(s.name);
    return out;
}

std::vector<std::string> NativeSpineEngine::listAnimations() const {
    std::vector<std::string> out;
    if (!m_pkg) return out;
    out.reserve(m_pkg->animations.size());
    for (const auto& a : m_pkg->animations) out.push_back(a.name);
    return out;
}

bool NativeSpineEngine::setSkin(const std::string& skinName) {
    if (!m_pkg) return false;
    for (size_t i = 0; i < m_pkg->skins.size(); ++i) {
        if (m_pkg->skins[i].name == skinName) {
            m_currentSkin = static_cast<int32_t>(i);
            m_frameValid = false;
            return true;
        }
    }
    spdlog::warn("NativeSpineEngine: skin '{}' not found", skinName);
    return false;
}

bool NativeSpineEngine::setBodyAnimation(const std::string& name, bool loop) {
    if (!m_pkg) return false;
    for (size_t i = 0; i < m_pkg->animations.size(); ++i) {
        if (m_pkg->animations[i].name == name) {
            m_currentBodyAnim = static_cast<int32_t>(i);
            m_bodyLoop = loop;
            m_bodyTime = 0.0f;
            m_frameValid = false;
            return true;
        }
    }
    spdlog::warn("NativeSpineEngine: animation '{}' not found", name);
    return false;
}

bool NativeSpineEngine::setTalkAnimation(const std::string& name, bool loop) {
    if (!m_pkg) return false;
    for (size_t i = 0; i < m_pkg->animations.size(); ++i) {
        if (m_pkg->animations[i].name == name) {
            m_currentTalkAnim = static_cast<int32_t>(i);
            m_talkLoop = loop;
            m_talkTime = 0.0f;
            return true;
        }
    }
    return false;
}

void NativeSpineEngine::setPosition(float x, float y)    { m_posX = x; m_posY = y; m_frameValid = false; }
void NativeSpineEngine::setScale   (float sx, float sy)  { m_scaleX = sx; m_scaleY = sy; m_frameValid = false; }
void NativeSpineEngine::setTalking (bool t)              { m_talking = t; }

std::vector<std::array<float, 6>> NativeSpineEngine::debugBoneWorld() const {
    std::vector<std::array<float, 6>> out;
    out.reserve(m_bonesWorld.size());
    for (const auto& b : m_bonesWorld)
        out.push_back({b.a, b.b, b.c, b.d, b.worldX, b.worldY});
    return out;
}

std::vector<int32_t> NativeSpineEngine::debugSlotAttachments() const {
    std::vector<int32_t> out;
    out.reserve(m_slotState.size());
    for (const auto& s : m_slotState) out.push_back(s.attachmentIdx);
    return out;
}

// ─── evaluateAtTime — the heart of the runtime ──────────────────────────────

void NativeSpineEngine::evaluateAtTime(float bodyTime, float talkTime) {
    m_bodyTime = bodyTime;
    m_talkTime = talkTime;
    m_frameValid = false;

    if (!m_pkg) return;

    const auto& pkg = *m_pkg;
    const size_t boneCount = pkg.bones.size();
    const size_t slotCount = pkg.slots.size();

    if (m_bonesWorld.size() != boneCount) m_bonesWorld.assign(boneCount, BoneWorld{});
    if (m_slotState.size()  != slotCount) m_slotState.assign(slotCount, SlotState{});
    if (m_drawOrder.size()  != slotCount) m_drawOrder.resize(slotCount);

    // ── 1. Build per-bone local pose (rest + animation overlay) ─────────
    // Stored in parallel arrays so we can compose the 2x2 matrix below.
    std::vector<float> rotL(boneCount), txL(boneCount), tyL(boneCount);
    std::vector<float> sxL(boneCount), syL(boneCount);
    std::vector<float> shxL(boneCount), shyL(boneCount);

    for (size_t i = 0; i < boneCount; ++i) {
        const auto& bd = pkg.bones[i];
        rotL[i] = bd.rotation;
        txL[i]  = bd.x;
        tyL[i]  = bd.y;
        sxL[i]  = bd.scaleX;
        syL[i]  = bd.scaleY;
        shxL[i] = bd.shearX;
        shyL[i] = bd.shearY;
    }

    const rt::AnimClip* bodyClip = nullptr;
    float bodyT = 0.0f;
    if (m_currentBodyAnim >= 0 && m_currentBodyAnim < static_cast<int32_t>(pkg.animations.size())) {
        bodyClip = &pkg.animations[m_currentBodyAnim];
        bodyT = wrapTime(m_bodyTime, bodyClip->duration, m_bodyLoop);
    }

    const rt::AnimClip* talkClip = nullptr;
    float talkT = 0.0f;
    if (m_talking
        && m_currentTalkAnim >= 0
        && m_currentTalkAnim < static_cast<int32_t>(pkg.animations.size())) {
        talkClip = &pkg.animations[m_currentTalkAnim];
        talkT = wrapTime(m_talkTime, talkClip->duration, m_talkLoop);
    }

    // Spine timeline semantics: when `time < firstFrameTime` the timeline
    // contributes its SETUP-POSE value (rotation/translate/scale/shear
    // timelines fall back to bone._data.* — i.e. NO delta). Native stores
    // these as relative deltas, so the equivalent is to SKIP the apply
    // entirely when t is before the first key. Without this gate, the
    // sampler clamped to the first keyframe's value and made every anim
    // start from its first-key pose instead of setup, producing the
    // "wrong limb positions / oversized swords at t=0" rendering.
    auto applyBoneTracks = [&](const rt::AnimClip& clip, float t) {
        const size_t trackCount = std::min(boneCount, clip.boneTracks.size());
        for (size_t i = 0; i < trackCount; ++i) {
            const auto& trk = clip.boneTracks[i];
            if (!trk.rotate.times.empty() && t >= trk.rotate.times.front()) {
                rotL[i] += sampleFloat(trk.rotate, t);
            }
            if (!trk.translate.times.empty() && t >= trk.translate.times.front()) {
                rt::Vec2 v = sampleVec2(trk.translate, t);
                txL[i] += v.x;
                tyL[i] += v.y;
            }
            if (!trk.scale.times.empty() && t >= trk.scale.times.front()) {
                rt::Vec2 v = sampleVec2(trk.scale, t);
                sxL[i] *= v.x;
                syL[i] *= v.y;
            }
            if (!trk.shear.times.empty() && t >= trk.shear.times.front()) {
                rt::Vec2 v = sampleVec2(trk.shear, t);
                shxL[i] += v.x;
                shyL[i] += v.y;
            }
        }
    };

    if (bodyClip) applyBoneTracks(*bodyClip, bodyT);
    // Talk track overlays on top of body track. Only bone timelines are
    // blended — slot color / draw order from the talk track are ignored.
    if (talkClip) applyBoneTracks(*talkClip, talkT);

    // ── 2. Compose local 2x2 matrix and resolve world hierarchy ─────────
    // Parent always at lower index in the array (.shotanim invariant),
    // so a single forward pass suffices. All five bone inherit modes
    // are honored: Normal, OnlyTranslation, NoRotationOrReflection,
    // NoScale, NoScaleOrReflection.
    //
    // Factored into a per-bone helper so that IK constraints (Phase 2.8)
    // can re-resolve individual bones after modifying their local pose
    // without duplicating the matrix-composition / inherit-mode logic.
    constexpr float kDeg2Rad = 0.017453292519943295f;
    auto resolveBone = [&](size_t i) {
        const auto& bd = pkg.bones[i];

        // Local 2x2 from (rotation, scale, shear). This matches spine's
        // Bone::updateWorldTransform exactly:
        //   rotationY = rotation + 90 + shearY
        //   a = cosDeg(rotation + shearX) * scaleX
        //   c = sinDeg(rotation + shearX) * scaleX
        //   b = cosDeg(rotationY)         * scaleY = -sinDeg(rot+shearY)*sy
        //   d = sinDeg(rotationY)         * scaleY =  cosDeg(rot+shearY)*sy
        //
        // The X-axis basis (a, c) uses shearX; the Y-axis basis (b, d)
        // uses shearY. (A previous version swapped these — fine for
        // unsheared bones, but it distorted sheared cape/wing rigs and
        // the error compounded down the hierarchy.)
        const float rotShX = (rotL[i] + shxL[i]) * kDeg2Rad;
        const float rotShY = (rotL[i] + shyL[i]) * kDeg2Rad;
        const float cosX = std::cos(rotShX), sinX = std::sin(rotShX);
        const float cosY = std::cos(rotShY), sinY = std::sin(rotShY);

        const float la =  cosX * sxL[i];
        const float lc =  sinX * sxL[i];
        const float lb = -sinY * syL[i];
        const float ld =  cosY * syL[i];

        if (bd.parentIndex < 0) {
            // Root: world = engine_scale * local, plus engine offset.
            m_bonesWorld[i].a = la * m_scaleX;
            m_bonesWorld[i].b = lb * m_scaleX;
            m_bonesWorld[i].c = lc * m_scaleY;
            m_bonesWorld[i].d = ld * m_scaleY;
            m_bonesWorld[i].worldX = txL[i] * m_scaleX + m_posX;
            m_bonesWorld[i].worldY = tyL[i] * m_scaleY + m_posY;
            return;
        }

        const auto& P = m_bonesWorld[bd.parentIndex];
        // Spine modifies pa..pd locally in NoRotationOrReflection, so copy.
        float pa = P.a, pb = P.b, pc = P.c, pd = P.d;

        // Translation: every mode inherits position through the parent's
        // full transform (computed before the switch, exactly as spine).
        m_bonesWorld[i].worldX = pa * txL[i] + pb * tyL[i] + P.worldX;
        m_bonesWorld[i].worldY = pc * txL[i] + pd * tyL[i] + P.worldY;

        // The following is a verbatim port of spine-cpp
        // Bone::updateWorldTransform's non-root switch (Bone.cpp:120-200),
        // using m_scaleX/m_scaleY where spine uses _skeleton.getScaleX/Y.
        // cosD/sinD take degrees; the NoScale branch needs a radians cos/sin.
        const float rot    = rotL[i];
        const float scaleX = sxL[i];
        const float scaleY = syL[i];
        const float shearX = shxL[i];
        const float shearY = shyL[i];
        constexpr float kRad2Deg = 57.295779513082320876f;
        constexpr float kHalfPi  = 1.57079632679489661923f;
        auto cosD = [&](float deg) { return std::cos(deg * kDeg2Rad); };
        auto sinD = [&](float deg) { return std::sin(deg * kDeg2Rad); };

        float A = 1, B = 0, C = 0, D = 1;
        bool isNormal = false;

        switch (bd.inherit) {
            case rt::BoneInherit::Normal: {
                const float rotationY = rot + 90.0f + shearY;
                const float nla = cosD(rot + shearX) * scaleX;
                const float nlb = cosD(rotationY)     * scaleY;
                const float nlc = sinD(rot + shearX) * scaleX;
                const float nld = sinD(rotationY)     * scaleY;
                A = pa * nla + pb * nlc;
                B = pa * nlb + pb * nld;
                C = pc * nla + pd * nlc;
                D = pc * nlb + pd * nld;
                isNormal = true;  // Normal does NOT get skeleton scale here
                break;
            }
            case rt::BoneInherit::OnlyTranslation: {
                const float rotationY = rot + 90.0f + shearY;
                A = cosD(rot + shearX) * scaleX;
                B = cosD(rotationY)     * scaleY;
                C = sinD(rot + shearX) * scaleX;
                D = sinD(rotationY)     * scaleY;
                break;
            }
            case rt::BoneInherit::NoRotationOrReflection: {
                float s = pa * pa + pc * pc;
                float prx;
                if (s > 0.0001f) {
                    s = std::abs(pa * pd - pb * pc) / s;
                    const float sx = (std::abs(m_scaleX) > 1e-8f) ? m_scaleX : 1.0f;
                    const float sy = (std::abs(m_scaleY) > 1e-8f) ? m_scaleY : 1.0f;
                    pa /= sx;
                    pc /= sy;
                    pb = pc * s;
                    pd = pa * s;
                    prx = std::atan2(pc, pa) * kRad2Deg;
                } else {
                    pa = 0.0f;
                    pc = 0.0f;
                    prx = 90.0f - std::atan2(pd, pb) * kRad2Deg;
                }
                const float rx = rot + shearX - prx;
                const float ry = rot + shearY - prx + 90.0f;
                const float nla = cosD(rx) * scaleX;
                const float nlb = cosD(ry) * scaleY;
                const float nlc = sinD(rx) * scaleX;
                const float nld = sinD(ry) * scaleY;
                A = pa * nla - pb * nlc;
                B = pa * nlb - pb * nld;
                C = pc * nla + pd * nlc;
                D = pc * nlb + pd * nld;
                break;
            }
            case rt::BoneInherit::NoScale:
            case rt::BoneInherit::NoScaleOrReflection: {
                const float cosine = cosD(rot);
                const float sine   = sinD(rot);
                const float sx = (std::abs(m_scaleX) > 1e-8f) ? m_scaleX : 1.0f;
                const float sy = (std::abs(m_scaleY) > 1e-8f) ? m_scaleY : 1.0f;
                float za = (pa * cosine + pb * sine) / sx;
                float zc = (pc * cosine + pd * sine) / sy;
                float s  = std::sqrt(za * za + zc * zc);
                if (s > 0.00001f) s = 1.0f / s;
                za *= s;
                zc *= s;
                s = std::sqrt(za * za + zc * zc);
                if (bd.inherit == rt::BoneInherit::NoScale &&
                    ((pa * pd - pb * pc < 0.0f) !=
                     (((m_scaleX < 0.0f) != (m_scaleY < 0.0f)))))
                    s = -s;
                const float r  = kHalfPi + std::atan2(zc, za);
                const float zb = std::cos(r) * s;
                const float zd = std::sin(r) * s;
                const float nla = cosD(shearX)        * scaleX;
                const float nlb = cosD(90.0f + shearY) * scaleY;
                const float nlc = sinD(shearX)        * scaleX;
                const float nld = sinD(90.0f + shearY) * scaleY;
                A = za * nla + zb * nlc;
                B = za * nlb + zb * nld;
                C = zc * nla + zd * nlc;
                D = zc * nlb + zd * nld;
                break;
            }
        }

        if (!isNormal) {
            // OnlyTranslation / NoRotationOrReflection / NoScale all get
            // skeleton scale applied here (spine Bone.cpp:197-200).
            // Normal inherits it through the parent chain instead.
            A *= m_scaleX;
            B *= m_scaleX;
            C *= m_scaleY;
            D *= m_scaleY;
        }
        m_bonesWorld[i].a = A;
        m_bonesWorld[i].b = B;
        m_bonesWorld[i].c = C;
        m_bonesWorld[i].d = D;
    };

    for (size_t i = 0; i < boneCount; ++i) resolveBone(i);

    // ── 2a. IK constraints ──────────────────────────────────────────────
    // Modify rotL[] for constrained bones so the chain reaches its target,
    // then re-resolve the chain bones via resolveBone so descendants see
    // the new rotation. Mix can be animated via clip.ikTracks (stepped
    // bend direction, lerped mix between keys).
    //
    // DIAGNOSTIC: env var ROUNDTABLE_NATIVE_NO_IK=1 disables IK entirely
    // so we can A/B test whether IK is responsible for visible glitches.
    static const bool sDisableIK = []() {
        const char* env = std::getenv("ROUNDTABLE_NATIVE_NO_IK");
        return env && std::string(env) == "1";
    }();
    static const bool sDisableTC = []() {
        const char* env = std::getenv("ROUNDTABLE_NATIVE_NO_TC");
        return env && std::string(env) == "1";
    }();
    if (!pkg.ikConstraints.empty() && !sDisableIK) {
        constexpr float kRad2Deg = 57.29577951308232f;
        constexpr float kPi      = 3.14159265358979f;
        auto shortestArc = [](float deltaDeg) {
            while (deltaDeg >  180.0f) deltaDeg -= 360.0f;
            while (deltaDeg < -180.0f) deltaDeg += 360.0f;
            return deltaDeg;
        };

        for (size_t ci = 0; ci < pkg.ikConstraints.size(); ++ci) {
            const auto& ik = pkg.ikConstraints[ci];
            if (ik.targetBoneIdx < 0
                || ik.targetBoneIdx >= static_cast<int32_t>(boneCount)) continue;
            if (ik.constrainedBones.empty()) continue;

            // Per-frame mix / bendDir overrides from clip.ikTracks if present.
            float mix    = ik.mix;
            int   bendD  = (ik.bendDirection >= 1) ? 1 : -1;
            if (bodyClip && ci < bodyClip->ikTracks.size()
                && !bodyClip->ikTracks[ci].keys.empty()
                && bodyT >= bodyClip->ikTracks[ci].keys.front().time) {
                // Same setup-fallback semantics as bone tracks: when bodyT
                // is before the first key, spine uses the IK's setup mix
                // (already loaded into mix/bendD above) — we just don't
                // override it.
                const auto& keys = bodyClip->ikTracks[ci].keys;
                if (bodyT >= keys.back().time) {
                    mix   = keys.back().mix;
                    bendD = keys.back().bendPositive >= 0.0f ? 1 : -1;
                } else {
                    size_t hi = 1;
                    while (hi < keys.size() && keys[hi].time <= bodyT) ++hi;
                    if (hi >= keys.size()) hi = keys.size() - 1;
                    const size_t lo = hi - 1;
                    const float u = (keys[hi].time - keys[lo].time > 0.0f)
                        ? (bodyT - keys[lo].time) / (keys[hi].time - keys[lo].time)
                        : 0.0f;
                    mix   = keys[lo].mix + (keys[hi].mix - keys[lo].mix) * u;
                    // bendDirection is stepped (not lerped) in spine.
                    bendD = keys[lo].bendPositive >= 0.0f ? 1 : -1;
                }
            }
            if (mix <= 0.0f) continue;

            const auto& tgt = m_bonesWorld[ik.targetBoneIdx];
            const float tx = tgt.worldX, ty = tgt.worldY;

            if (ik.constrainedBones.size() == 1) {
                // ── 1-bone IK: rotate bone to face target in world space.
                const int32_t bi = ik.constrainedBones[0];
                if (bi < 0 || bi >= static_cast<int32_t>(boneCount)) continue;
                const auto& bd = pkg.bones[bi];
                const auto& bw = m_bonesWorld[bi];

                const float dx = tx - bw.worldX;
                const float dy = ty - bw.worldY;
                if (dx * dx + dy * dy < 1e-10f) continue;

                const float desiredWorldDeg = std::atan2(dy, dx) * kRad2Deg;
                float parentWorldDeg = 0.0f;
                if (bd.parentIndex >= 0) {
                    const auto& pp = m_bonesWorld[bd.parentIndex];
                    parentWorldDeg = std::atan2(pp.c, pp.a) * kRad2Deg;
                }
                const float desiredLocalDeg = desiredWorldDeg - parentWorldDeg;
                const float delta = shortestArc(desiredLocalDeg - rotL[bi]);
                rotL[bi] += delta * mix;

                resolveBone(static_cast<size_t>(bi));
            }
            else {
                // ── 2-bone IK: law of cosines.
                const int32_t b1 = ik.constrainedBones[0];
                const int32_t b2 = ik.constrainedBones[1];
                if (b1 < 0 || b1 >= static_cast<int32_t>(boneCount)) continue;
                if (b2 < 0 || b2 >= static_cast<int32_t>(boneCount)) continue;

                const auto& bd1 = pkg.bones[b1];
                const auto& bd2 = pkg.bones[b2];
                const auto& bw1 = m_bonesWorld[b1];

                // Bone lengths. Use rest-pose lengths from BoneData;
                // animation scale is folded into the matrix already so
                // we don't multiply by scale here (rest length is in
                // bone-local units before any matrix transform).
                const float a = bd1.length;
                const float b = bd2.length;
                if (a < 1e-6f || b < 1e-6f) continue;  // degenerate

                const float dx = tx - bw1.worldX;
                const float dy = ty - bw1.worldY;
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist < 1e-6f) continue;

                // Clamp to reachable range [|a-b|, a+b].
                const float dMin = std::abs(a - b);
                const float dMax = a + b;
                if (dist > dMax) dist = dMax;
                else if (dist < dMin) dist = dMin;

                float cosT1 = (a * a + dist * dist - b * b) / (2.0f * a * dist);
                float cosT2 = (a * a + b * b - dist * dist) / (2.0f * a * b);
                if (cosT1 >  1.0f) cosT1 =  1.0f;
                if (cosT1 < -1.0f) cosT1 = -1.0f;
                if (cosT2 >  1.0f) cosT2 =  1.0f;
                if (cosT2 < -1.0f) cosT2 = -1.0f;

                const float t1 = std::acos(cosT1);
                const float t2 = std::acos(cosT2);
                const float toTgt = std::atan2(dy, dx);
                const float bdS = static_cast<float>(bendD);

                // B1's desired world angle (signed by bend direction)
                const float b1WorldRad = toTgt - bdS * t1;
                // B2's desired local angle relative to B1's frame
                const float b2LocalRad = (kPi - t2) * bdS;

                float parentWorldRad = 0.0f;
                if (bd1.parentIndex >= 0) {
                    const auto& pp = m_bonesWorld[bd1.parentIndex];
                    parentWorldRad = std::atan2(pp.c, pp.a);
                }
                const float b1LocalDegDesired = (b1WorldRad - parentWorldRad) * kRad2Deg;
                const float b2LocalDegDesired = b2LocalRad * kRad2Deg;

                const float dB1 = shortestArc(b1LocalDegDesired - rotL[b1]);
                const float dB2 = shortestArc(b2LocalDegDesired - rotL[b2]);
                rotL[b1] += dB1 * mix;
                rotL[b2] += dB2 * mix;

                resolveBone(static_cast<size_t>(b1));
                resolveBone(static_cast<size_t>(b2));

                (void)bd2;  // bd2 currently only used implicitly via rest length
            }
        }
    }

    // ── 2b. Transform constraints (applied AFTER initial hierarchy) ─────
    // Verbatim port of spine-cpp 4.2 TransformConstraint::update() and its
    // four apply modes (TransformConstraint.cpp:64-378). The earlier
    // implementation lerped constrained bones toward the target's BARE
    // world origin (ignoring offsetX/Y/Rotation and the per-axis mix
    // split) which produced 100-180-unit position errors on constrained
    // sub-chains — exactly the "oversized arms / swords too large"
    // rendering the user reported.
    //
    // DIAGNOSTIC: env var ROUNDTABLE_NATIVE_NO_TC=1 disables this block.
    if (!pkg.transformConstraints.empty() && !sDisableTC) {
        constexpr float kPi     = 3.14159265358979323846f;
        constexpr float kTwoPi  = 6.28318530717958647692f;
        constexpr float kDeg2R  = 0.017453292519943295f;

        for (size_t tci = 0; tci < pkg.transformConstraints.size(); ++tci) {
            const auto& tc = pkg.transformConstraints[tci];
            if (tc.targetBoneIdx < 0
                || tc.targetBoneIdx >= static_cast<int32_t>(boneCount)) continue;

            // ── Per-frame mix override from transformTracks (lerped) ───
            float mixRotate = tc.mixRotate;
            float mixX      = tc.mixX,      mixY      = tc.mixY;
            float mixScaleX = tc.mixScaleX, mixScaleY = tc.mixScaleY;
            float mixShearY = tc.mixShearY;
            if (bodyClip && tci < bodyClip->transformTracks.size()) {
                const auto& keys = bodyClip->transformTracks[tci].keys;
                // Setup-fallback semantics: when bodyT is before the first
                // key, spine uses the constraint's setup mix values (which
                // mixRotate/X/Y/ScaleX/ScaleY/ShearY were already seeded
                // with above). Skip the timeline override in that case.
                if (!keys.empty() && bodyT >= keys.front().time) {
                    size_t idx = 0;
                    while (idx + 1 < keys.size() && keys[idx + 1].time <= bodyT) ++idx;
                    if (idx + 1 < keys.size() && bodyT > keys[idx].time) {
                        const auto& k0 = keys[idx];
                        const auto& k1 = keys[idx + 1];
                        const float u = (bodyT - k0.time) / (k1.time - k0.time);
                        mixRotate = k0.mixRotate + (k1.mixRotate - k0.mixRotate) * u;
                        mixX      = k0.mixX      + (k1.mixX      - k0.mixX)      * u;
                        mixY      = k0.mixY      + (k1.mixY      - k0.mixY)      * u;
                        mixScaleX = k0.mixScaleX + (k1.mixScaleX - k0.mixScaleX) * u;
                        mixScaleY = k0.mixScaleY + (k1.mixScaleY - k0.mixScaleY) * u;
                        mixShearY = k0.mixShearY + (k1.mixShearY - k0.mixShearY) * u;
                    } else {
                        const auto& k = keys[idx];
                        mixRotate = k.mixRotate;
                        mixX      = k.mixX;      mixY      = k.mixY;
                        mixScaleX = k.mixScaleX; mixScaleY = k.mixScaleY;
                        mixShearY = k.mixShearY;
                    }
                }
            }
            if (mixRotate == 0 && mixX == 0 && mixY == 0
                && mixScaleX == 0 && mixScaleY == 0 && mixShearY == 0) continue;

            const bool translate = (mixX != 0.0f) || (mixY != 0.0f);
            const auto& tgtW = m_bonesWorld[tc.targetBoneIdx];
            const float ta = tgtW.a, tb = tgtW.b, tc_ = tgtW.c, td = tgtW.d;
            const float degRadReflect = ((ta * td - tb * tc_) > 0.0f) ? kDeg2R : -kDeg2R;
            const float offsetRotation = tc.offsetRotation * degRadReflect;
            const float offsetShearY   = tc.offsetShearY   * degRadReflect;

            const bool isLocal = (tc.local != 0);
            const bool isRel   = (tc.relative != 0);

            // ── Absolute / Relative WORLD modes ─────────────────────────
            if (!isLocal) {
                for (int32_t bi : tc.constrainedBones) {
                    if (bi < 0 || bi >= static_cast<int32_t>(boneCount)) continue;
                    auto& bone = m_bonesWorld[bi];

                    // Rotation
                    if (mixRotate != 0.0f) {
                        const float a = bone.a, b = bone.b, c = bone.c, d = bone.d;
                        float r;
                        if (isRel) {
                            r = std::atan2(tc_, ta) + offsetRotation;
                        } else {
                            r = std::atan2(tc_, ta) - std::atan2(c, a) + offsetRotation;
                        }
                        if (r >  kPi) r -= kTwoPi;
                        else if (r < -kPi) r += kTwoPi;
                        r *= mixRotate;
                        const float cs = std::cos(r), sn = std::sin(r);
                        bone.a = cs * a - sn * c;
                        bone.b = cs * b - sn * d;
                        bone.c = sn * a + cs * c;
                        bone.d = sn * b + cs * d;
                    }

                    // Translation — target.localToWorld(offsetX, offsetY).
                    if (translate) {
                        const float tx = ta * tc.offsetX + tb * tc.offsetY + tgtW.worldX;
                        const float ty = tc_ * tc.offsetX + td * tc.offsetY + tgtW.worldY;
                        if (isRel) {
                            bone.worldX += tx * mixX;
                            bone.worldY += ty * mixY;
                        } else {
                            bone.worldX += (tx - bone.worldX) * mixX;
                            bone.worldY += (ty - bone.worldY) * mixY;
                        }
                    }

                    // ScaleX
                    if (mixScaleX != 0.0f) {
                        const float s0 = std::sqrt(bone.a * bone.a + bone.c * bone.c);
                        float s;
                        if (isRel) {
                            s = (std::sqrt(ta * ta + tc_ * tc_) - 1.0f + tc.offsetScaleX) * mixScaleX + 1.0f;
                        } else {
                            s = (s0 != 0.0f)
                                ? (s0 + (std::sqrt(ta * ta + tc_ * tc_) - s0 + tc.offsetScaleX) * mixScaleX) / s0
                                : 0.0f;
                        }
                        bone.a *= s;
                        bone.c *= s;
                    }
                    // ScaleY
                    if (mixScaleY != 0.0f) {
                        const float s0 = std::sqrt(bone.b * bone.b + bone.d * bone.d);
                        float s;
                        if (isRel) {
                            s = (std::sqrt(tb * tb + td * td) - 1.0f + tc.offsetScaleY) * mixScaleY + 1.0f;
                        } else {
                            s = (s0 != 0.0f)
                                ? (s0 + (std::sqrt(tb * tb + td * td) - s0 + tc.offsetScaleY) * mixScaleY) / s0
                                : 0.0f;
                        }
                        bone.b *= s;
                        bone.d *= s;
                    }

                    // ShearY
                    if (mixShearY != 0.0f) {
                        const float bb = bone.b, dd = bone.d;
                        const float by = std::atan2(dd, bb);
                        float r;
                        if (isRel) {
                            r = std::atan2(td, tb) - std::atan2(tc_, ta);
                            if (r >  kPi) r -= kTwoPi;
                            else if (r < -kPi) r += kTwoPi;
                            r = std::atan2(dd, bb) + (r - kPi * 0.5f + offsetShearY) * mixShearY;
                        } else {
                            r = std::atan2(td, tb) - std::atan2(tc_, ta)
                                - (by - std::atan2(bone.c, bone.a));
                            if (r >  kPi) r -= kTwoPi;
                            else if (r < -kPi) r += kTwoPi;
                            r = by + (r + offsetShearY) * mixShearY;
                        }
                        const float s = std::sqrt(bb * bb + dd * dd);
                        bone.b = std::cos(r) * s;
                        bone.d = std::sin(r) * s;
                    }
                }
                continue;
            }

            // ── Absolute / Relative LOCAL modes ─────────────────────────
            // Modify the constrained bones' LOCAL pose (rotL/txL/.../shyL)
            // then re-resolve them so descendants see the new transform.
            // Target's local pose comes from rotL/txL/sxL/syL/shyL arrays.
            const int32_t ti = tc.targetBoneIdx;
            const float tRot  = rotL[ti];
            const float tTx   = txL[ti],   tTy  = tyL[ti];
            const float tSx   = sxL[ti],   tSy  = syL[ti];
            const float tShyR = shyL[ti];

            for (int32_t bi : tc.constrainedBones) {
                if (bi < 0 || bi >= static_cast<int32_t>(boneCount)) continue;

                if (mixRotate != 0.0f) {
                    float r;
                    if (isRel) {
                        r = rotL[bi] + (tRot + tc.offsetRotation) * mixRotate;
                    } else {
                        r = rotL[bi] + (tRot - rotL[bi] + tc.offsetRotation) * mixRotate;
                    }
                    rotL[bi] = r;
                }
                if (translate) {
                    if (isRel) {
                        txL[bi] += (tTx + tc.offsetX) * mixX;
                        tyL[bi] += (tTy + tc.offsetY) * mixY;
                    } else {
                        txL[bi] += (tTx - txL[bi] + tc.offsetX) * mixX;
                        tyL[bi] += (tTy - tyL[bi] + tc.offsetY) * mixY;
                    }
                }
                // spine's absolute-local stores RATIO (ts/s) into the local
                // scale — see TransformConstraint.cpp:299-302. This matches
                // bone.updateWorldTransform consuming it as the new _scaleX.
                if (mixScaleX != 0.0f) {
                    if (isRel) {
                        sxL[bi] *= ((tSx - 1.0f + tc.offsetScaleX) * mixScaleX) + 1.0f;
                    } else {
                        const float s0 = sxL[bi];
                        if (s0 != 0.0f) {
                            sxL[bi] = (s0 + (tSx - s0 + tc.offsetScaleX) * mixScaleX) / s0;
                        }
                    }
                }
                if (mixScaleY != 0.0f) {
                    if (isRel) {
                        syL[bi] *= ((tSy - 1.0f + tc.offsetScaleY) * mixScaleY) + 1.0f;
                    } else {
                        const float s0 = syL[bi];
                        if (s0 != 0.0f) {
                            syL[bi] = (s0 + (tSy - s0 + tc.offsetScaleY) * mixScaleY) / s0;
                        }
                    }
                }
                if (mixShearY != 0.0f) {
                    if (isRel) {
                        shyL[bi] += (tShyR + tc.offsetShearY) * mixShearY;
                    } else {
                        shyL[bi] += (tShyR - shyL[bi] + tc.offsetShearY) * mixShearY;
                    }
                }
                resolveBone(static_cast<size_t>(bi));
            }
        }
    }

    // ── 3. Slot state — start with setup pose attachment + color ────────
    for (size_t i = 0; i < slotCount; ++i) {
        const auto& sd = pkg.slots[i];
        m_slotState[i].attachmentIdx = sd.defaultAttachmentIdx;
        // Unpack slot.color (ABGR little-endian → float 0..1)
        const uint32_t c = sd.color;
        m_slotState[i].r = ((c >>  0) & 0xFF) / 255.0f;
        m_slotState[i].g = ((c >>  8) & 0xFF) / 255.0f;
        m_slotState[i].b = ((c >> 16) & 0xFF) / 255.0f;
        m_slotState[i].a = ((c >> 24) & 0xFF) / 255.0f;
    }

    // resolveAttachment — look up a (slot, attachmentName) pair through the
    // current skin's table, with fallback to the default skin (index 0).
    // Returns the global attachment pool index, or -1 if nothing matches.
    auto resolveAttachment = [&](int32_t slotIdx, int32_t nameStringIdx) -> int32_t {
        if (nameStringIdx < 0) return -1;  // attachment cleared
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(slotIdx)) << 32)
                            | static_cast<uint32_t>(nameStringIdx);
        if (m_currentSkin >= 0 && m_currentSkin < static_cast<int32_t>(m_skinAttachLookup.size())) {
            const auto& tbl = m_skinAttachLookup[m_currentSkin];
            auto it = tbl.find(key);
            if (it != tbl.end()) return it->second;
        }
        if (m_currentSkin != 0 && !m_skinAttachLookup.empty()) {
            const auto& tbl = m_skinAttachLookup[0];
            auto it = tbl.find(key);
            if (it != tbl.end()) return it->second;
        }
        return -1;
    };

    // ── 4. Skin override (look up each slot's setup-pose attachment NAME
    //      in the current skin; do NOT iterate skin entries blindly).
    //
    // Spine semantics: each slot has exactly one current attachment per
    // frame, named (e.g.) "body" or "eye_open". When you switch skins,
    // the slot's current name is re-resolved against the new skin's
    // attachment table — picking the skin's version of that name. If
    // the named attachment isn't in the current skin, fall back to the
    // default skin.
    //
    // (The old "iterate every entry and last-wins" approach was wrong
    // because spine skins can hold MULTIPLE attachments per slot,
    // differentiated by name — iterating overwrites the slot with
    // whatever the last entry happened to be in HashMap order.)
    for (size_t i = 0; i < slotCount; ++i) {
        const int32_t setupPoolIdx = pkg.slots[i].defaultAttachmentIdx;
        if (setupPoolIdx < 0
            || setupPoolIdx >= static_cast<int32_t>(pkg.attachments.size())) continue;
        const int32_t setupNameIdx = pkg.attachments[setupPoolIdx].attachNameIdx;
        if (setupNameIdx < 0) continue;
        const int32_t resolved = resolveAttachment(static_cast<int32_t>(i), setupNameIdx);
        if (resolved >= 0) m_slotState[i].attachmentIdx = resolved;
    }

    // ── 5. Apply animation slot timelines ───────────────────────────────
    // Color: directly overwrites slot state.
    // Attachment: timeline stores a string-table index; resolve through
    //   the current skin's lookup (falling back to default skin).

    // Same "skip when before first key" gate as bone tracks. Spine's
    // ColorTimeline / AttachmentTimeline both fall back to the slot's
    // setup-pose color/attachment when time < frames[0]; the slot state
    // has already been populated with setup values by passes 3 & 4 above,
    // so simply NOT applying the timeline yields the correct result.
    if (bodyClip) {
        const size_t trackCount = std::min(slotCount, bodyClip->slotTracks.size());
        for (size_t i = 0; i < trackCount; ++i) {
            const auto& st = bodyClip->slotTracks[i];
            if (!st.colorR.times.empty() && bodyT >= st.colorR.times.front())
                m_slotState[i].r = sampleFloat(st.colorR, bodyT);
            if (!st.colorG.times.empty() && bodyT >= st.colorG.times.front())
                m_slotState[i].g = sampleFloat(st.colorG, bodyT);
            if (!st.colorB.times.empty() && bodyT >= st.colorB.times.front())
                m_slotState[i].b = sampleFloat(st.colorB, bodyT);
            if (!st.colorA.times.empty() && bodyT >= st.colorA.times.front())
                m_slotState[i].a = sampleFloat(st.colorA, bodyT);
            if (!st.attachment.times.empty() && bodyT >= st.attachment.times.front()) {
                const int32_t nameIdx = sampleI32Stepped(st.attachment, bodyT);
                m_slotState[i].attachmentIdx = resolveAttachment(static_cast<int32_t>(i), nameIdx);
            }
        }
    }

    // ── 6. Resolve draw order ───────────────────────────────────────────
    // Default to slot-index order. If a draw-order timeline exists,
    // pick the active key by stepped sampling.
    for (size_t i = 0; i < slotCount; ++i)
        m_drawOrder[i] = static_cast<int32_t>(i);

    // Setup-fallback: when bodyT is before the first draw-order key, spine
    // uses the SETUP draw order (slot-index order, already loaded above).
    if (bodyClip && !bodyClip->drawOrder.keys.empty()
        && bodyT >= bodyClip->drawOrder.keys.front().time) {
        const auto& keys = bodyClip->drawOrder.keys;
        size_t k = 0;
        for (; k + 1 < keys.size(); ++k) {
            if (keys[k + 1].time > bodyT) break;
        }
        const auto& key = keys[k];
        if (key.slotOrder.size() == slotCount) {
            for (size_t i = 0; i < slotCount; ++i) m_drawOrder[i] = key.slotOrder[i];
        }
    }

    // Diagnostic: catch bone transforms that have gone extreme. Helps
    // pinpoint blowups in specific animations/timeframes. Only the first
    // offender is logged per evaluate to avoid spam.
    constexpr float kExtremeBoneCoord = 1e5f;  // ~100k px is well past any real character
    for (size_t i = 0; i < boneCount; ++i) {
        const auto& bw = m_bonesWorld[i];
        if (!std::isfinite(bw.worldX) || !std::isfinite(bw.worldY) ||
            !std::isfinite(bw.a) || !std::isfinite(bw.b) ||
            !std::isfinite(bw.c) || !std::isfinite(bw.d) ||
            std::abs(bw.worldX) > kExtremeBoneCoord ||
            std::abs(bw.worldY) > kExtremeBoneCoord) {
            const auto& bd = pkg.bones[i];
            spdlog::warn("NativeSpineEngine: bone[{}] extreme at bodyT={:.3f}: "
                         "world=({:.2e},{:.2e}) mat=[{:.2e},{:.2e};{:.2e},{:.2e}] "
                         "parent={} local rot={:.2f} t=({:.2f},{:.2f}) s=({:.3f},{:.3f}) "
                         "sh=({:.2f},{:.2f}) inherit={}",
                         i, bodyT, bw.worldX, bw.worldY, bw.a, bw.b, bw.c, bw.d,
                         bd.parentIndex, rotL[i], txL[i], tyL[i], sxL[i], syL[i],
                         shxL[i], shyL[i], static_cast<int>(bd.inherit));
            break;  // one bone per evaluate
        }
    }

    m_frameValid = true;
}

void NativeSpineEngine::update(float dt) {
    if (!m_pkg) return;
    if (m_currentBodyAnim >= 0 && m_currentBodyAnim < static_cast<int32_t>(m_pkg->animations.size())) {
        const float dur = m_pkg->animations[m_currentBodyAnim].duration;
        m_bodyTime += dt;
        if (m_bodyLoop && dur > 0.0f) {
            m_bodyTime = std::fmod(m_bodyTime, dur);
            if (m_bodyTime < 0) m_bodyTime += dur;
        } else if (dur > 0.0f) {
            m_bodyTime = std::min(m_bodyTime, dur);
        }
    }
    if (m_talking && m_currentTalkAnim >= 0 && m_currentTalkAnim < static_cast<int32_t>(m_pkg->animations.size())) {
        const float dur = m_pkg->animations[m_currentTalkAnim].duration;
        m_talkTime += dt;
        if (m_talkLoop && dur > 0.0f) {
            m_talkTime = std::fmod(m_talkTime, dur);
            if (m_talkTime < 0) m_talkTime += dur;
        }
    }
    evaluateAtTime(m_bodyTime, m_talkTime);
}

// ─── extractMeshes — produce SpineRenderData for the current pose ───────────

SpineRenderData NativeSpineEngine::extractMeshes() {
    SpineRenderData out;

    if (!m_pkg) {
        out.boundsW = 0;
        out.boundsH = 0;
        return out;
    }

    // If evaluateAtTime hasn't been called for this state, do it now.
    if (!m_frameValid) evaluateAtTime(m_bodyTime, m_talkTime);

    const auto& pkg = *m_pkg;
    float minX =  FLT_MAX, minY =  FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX;
    bool hasBounds = false;

    // Body-track context for deform sampling. Deform timelines are
    // attached to the body animation only (talk-track deform isn't
    // a thing in practice).
    const rt::AnimClip* bodyClip = nullptr;
    float bodyT = 0.0f;
    if (m_currentBodyAnim >= 0
        && m_currentBodyAnim < static_cast<int32_t>(pkg.animations.size())) {
        bodyClip = &pkg.animations[m_currentBodyAnim];
        bodyT = wrapTime(m_bodyTime, bodyClip->duration, m_bodyLoop);
    }
    std::vector<float> deformBuf;

    SpineRenderBatch* curBatch = nullptr;
    int lastPage = -2;
    SpineBlendMode lastBlend = SpineBlendMode::Normal;

    // Clip state — persists across slots, starting at a Clipping attachment
    // and ending when the draw-order walk reaches its endSlotIdx (exclusive,
    // i.e. the endSlot itself is NOT clipped).
    struct ClipState {
        bool               active = false;
        int32_t            endSlotIdx = -1;
        std::vector<float> polyWorld;   // CCW convex polygon, world space
    } clip;

    // Reusable temp buffers for per-attachment vertex collection and SH clipping.
    std::vector<ClipVert> attachVerts;
    std::vector<uint16_t> triIndices;
    std::vector<ClipVert> clipIn, clipOut, clipScratch;

    auto recordBounds = [&](float wx, float wy) {
        if (!hasBounds) { minX = maxX = wx; minY = maxY = wy; hasBounds = true; }
        else {
            if (wx < minX) minX = wx; if (wx > maxX) maxX = wx;
            if (wy < minY) minY = wy; if (wy > maxY) maxY = wy;
        }
    };
    auto pushSpineVert = [&](SpineRenderBatch& batch, const ClipVert& v) {
        SpineVertex sv;
        sv.x = v.x; sv.y = v.y;
        sv.u = v.u; sv.v = v.v;
        sv.r = v.r; sv.g = v.g; sv.b = v.b; sv.a = v.a;
        batch.vertices.push_back(sv);
        recordBounds(v.x, v.y);
    };

    // Emit a whole attachment (vertices + triangle index list) into curBatch.
    //
    // Fast path (clip OFF): push attachVerts as a single block, then push
    // triangle indices offset by the block's base — preserves vertex
    // sharing and matches spine-cpp's emission shape.
    //
    // Slow path (clip ON): each triangle is sent through Sutherland-Hodgman
    // and fan-triangulated. Clipped triangles can't share vertices because
    // intersections produce new ones, so we emit 3 fresh verts per output
    // triangle.
    auto emitAttachment = [&](const std::vector<ClipVert>& verts,
                              const std::vector<uint16_t>& tris) {
        if (verts.empty() || tris.size() < 3) return;

        if (!clip.active) {
            const uint16_t base = static_cast<uint16_t>(curBatch->vertices.size());
            for (const auto& v : verts) pushSpineVert(*curBatch, v);
            for (size_t t = 0; t + 2 < tris.size(); t += 3) {
                const uint16_t i0 = tris[t + 0];
                const uint16_t i1 = tris[t + 1];
                const uint16_t i2 = tris[t + 2];
                if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;
                curBatch->indices.push_back(static_cast<uint16_t>(base + i0));
                curBatch->indices.push_back(static_cast<uint16_t>(base + i1));
                curBatch->indices.push_back(static_cast<uint16_t>(base + i2));
            }
            return;
        }

        // Clipped path: per-triangle SH + fan triangulation.
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            const uint16_t i0 = tris[t + 0];
            const uint16_t i1 = tris[t + 1];
            const uint16_t i2 = tris[t + 2];
            if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;
            clipIn.clear();
            clipIn.push_back(verts[i0]);
            clipIn.push_back(verts[i1]);
            clipIn.push_back(verts[i2]);
            sutherlandHodgman(clipIn, clip.polyWorld, clipOut, clipScratch);
            const size_t n = clipOut.size();
            if (n < 3) continue;
            for (size_t i = 1; i + 1 < n; ++i) {
                const uint16_t base = static_cast<uint16_t>(curBatch->vertices.size());
                pushSpineVert(*curBatch, clipOut[0]);
                pushSpineVert(*curBatch, clipOut[i]);
                pushSpineVert(*curBatch, clipOut[i + 1]);
                curBatch->indices.push_back(base);
                curBatch->indices.push_back(static_cast<uint16_t>(base + 1));
                curBatch->indices.push_back(static_cast<uint16_t>(base + 2));
            }
        }
    };

    for (size_t orderI = 0; orderI < m_drawOrder.size(); ++orderI) {
        const int32_t slotI = m_drawOrder[orderI];
        if (slotI < 0 || slotI >= static_cast<int32_t>(pkg.slots.size())) continue;

        // Deactivate clipping when we reach the clip's end slot.
        // The endSlot itself is rendered WITHOUT the clip.
        if (clip.active && slotI == clip.endSlotIdx) {
            clip.active = false;
            clip.polyWorld.clear();
        }

        const auto& slot   = pkg.slots[slotI];
        const auto& sstate = m_slotState[slotI];

        // Skip slots with no attachment or fully transparent.
        if (sstate.attachmentIdx < 0
            || sstate.attachmentIdx >= static_cast<int32_t>(pkg.attachments.size())) continue;
        if (sstate.a <= 0.0f) continue;

        const auto& attach = pkg.attachments[sstate.attachmentIdx];

        // Clipping attachment: build its world-space polygon and start
        // clipping subsequent slots. The clip itself doesn't render.
        if (attach.kind == rt::AttachKind::Clipping) {
            if (slot.boneIndex >= 0
                && slot.boneIndex < static_cast<int32_t>(m_bonesWorld.size())) {
                const auto& bClip = m_bonesWorld[slot.boneIndex];
                clip.polyWorld.clear();
                clip.polyWorld.reserve(attach.meshVertCount * 2);
                for (uint32_t i = 0; i < attach.meshVertCount; ++i) {
                    const size_t ip = attach.meshVertOffset + i * 2;
                    if (ip + 1 >= pkg.meshVertices.size()) break;
                    const float lx = pkg.meshVertices[ip + 0];
                    const float ly = pkg.meshVertices[ip + 1];
                    clip.polyWorld.push_back(bClip.a * lx + bClip.b * ly + bClip.worldX);
                    clip.polyWorld.push_back(bClip.c * lx + bClip.d * ly + bClip.worldY);
                }
                if (clip.polyWorld.size() >= 6) {  // need >= 3 vertices
                    ensureCCW(clip.polyWorld);
                    clip.active = true;
                    clip.endSlotIdx = static_cast<int32_t>(attach.clipEndSlotIdx);
                }
            }
            continue;
        }

        // Other non-visual attachment kinds — skip but don't break clipping.
        if (attach.kind == rt::AttachKind::BoundingBox ||
            attach.kind == rt::AttachKind::Path ||
            attach.kind == rt::AttachKind::Point) {
            continue;
        }

        if (slot.boneIndex < 0 || slot.boneIndex >= static_cast<int32_t>(m_bonesWorld.size())) continue;
        const auto& bone = m_bonesWorld[slot.boneIndex];
        const bool skinned = (attach.kind == rt::AttachKind::Mesh
                              || attach.kind == rt::AttachKind::LinkedMesh)
                             && attach.meshBoneCount > 0;

        // Texture page is stored directly on the attachment (v4+).
        // Pre-v4 this used to be an index into a regions array that was
        // never serialized — the loader rejects pre-v4 files now, so by
        // the time we reach here attach.regionIdx is a valid pageIndex
        // (or -1 if the writer couldn't resolve a region for this name).
        const int pageIdx = attach.regionIdx;

        const SpineBlendMode blend = static_cast<SpineBlendMode>(static_cast<uint8_t>(slot.blend));

        // Start a new batch if (page, blend) changed — preserves draw order.
        if (!curBatch || pageIdx != lastPage || blend != lastBlend) {
            out.batches.emplace_back();
            curBatch = &out.batches.back();
            curBatch->texturePageIndex = pageIdx;
            curBatch->blendMode = blend;
            lastPage = pageIdx;
            lastBlend = blend;
        }

        // Combined color: slot tint × attachment tint, premultiplied alpha.
        const float fa = sstate.a * attach.colorA;
        const float fr = sstate.r * attach.colorR * fa;
        const float fg = sstate.g * attach.colorG * fa;
        const float fb = sstate.b * attach.colorB * fa;

        // ── Build attachVerts[] (per-attachment vertex array in ClipVert form)
        attachVerts.clear();
        triIndices.clear();

        if (attach.kind == rt::AttachKind::Region) {
            attachVerts.reserve(4);
            for (int v = 0; v < 4; ++v) {
                const float lx = attach.localVerts[v * 2 + 0];
                const float ly = attach.localVerts[v * 2 + 1];
                attachVerts.push_back(ClipVert{
                    bone.a * lx + bone.b * ly + bone.worldX,
                    bone.c * lx + bone.d * ly + bone.worldY,
                    attach.localUVs[v * 2 + 0],
                    attach.localUVs[v * 2 + 1],
                    fr, fg, fb, fa
                });
            }
            // Quad → 2 triangles
            triIndices = {0, 1, 2, 0, 2, 3};
        }
        else if (attach.kind == rt::AttachKind::Mesh ||
                 attach.kind == rt::AttachKind::LinkedMesh) {
            const uint32_t vc = attach.meshVertCount;
            attachVerts.reserve(vc);

            // Deform timeline lookup (rigid or skinned; per-vertex or per-influence)
            const std::vector<float>* deformDelta = nullptr;
            if (bodyClip && sstate.attachmentIdx >= 0) {
                const auto& dt = bodyClip->deformTracks;
                if (sstate.attachmentIdx < static_cast<int32_t>(dt.size())
                    && !dt[sstate.attachmentIdx].keys.empty()) {
                    if (sampleDeform(dt[sstate.attachmentIdx], bodyT, deformBuf)) {
                        deformDelta = &deformBuf;
                    }
                }
            }

            if (!skinned) {
                // Rigid mesh — bone-local positions in meshVertices[meshVertOffset..]
                // transformed through the slot's single bone matrix.
                for (uint32_t v = 0; v < vc; ++v) {
                    const size_t pi = attach.meshVertOffset + v * 2;
                    const size_t ui = attach.meshUvOffset   + v * 2;
                    if (pi + 1 >= pkg.meshVertices.size()) break;
                    if (ui + 1 >= pkg.meshVertices.size()) break;
                    float lx = pkg.meshVertices[pi + 0];
                    float ly = pkg.meshVertices[pi + 1];
                    if (deformDelta) {
                        const size_t dx = v * 2 + 0;
                        const size_t dy = v * 2 + 1;
                        if (dy < deformDelta->size()) {
                            lx += (*deformDelta)[dx];
                            ly += (*deformDelta)[dy];
                        }
                    }
                    attachVerts.push_back(ClipVert{
                        bone.a * lx + bone.b * ly + bone.worldX,
                        bone.c * lx + bone.d * ly + bone.worldY,
                        pkg.meshVertices[ui + 0],
                        pkg.meshVertices[ui + 1],
                        fr, fg, fb, fa
                    });
                }
            } else {
                // Skinned mesh — accumulate weighted contributions from each
                // influencing bone. Deform deltas apply per-influence here.
                size_t boneCur   = attach.meshBoneOffset;
                size_t weightCur = attach.meshWeightOffset;
                const size_t boneEnd   = boneCur   + attach.meshBoneCount;
                const size_t weightEnd = weightCur + attach.meshWeightCount;
                size_t globalInfluence = 0;

                for (uint32_t v = 0; v < vc; ++v) {
                    if (boneCur >= boneEnd) break;
                    const int32_t influences = pkg.meshBones[boneCur];
                    boneCur++;

                    if (boneCur + influences > boneEnd) break;
                    if (weightCur + influences * 3u > weightEnd) break;

                    float wx = 0.0f, wy = 0.0f;
                    for (int k = 0; k < influences; ++k) {
                        const int32_t bIdx = pkg.meshBones[boneCur + k];
                        float bx = pkg.meshWeights[weightCur + k * 3 + 0];
                        float by = pkg.meshWeights[weightCur + k * 3 + 1];
                        const float bw = pkg.meshWeights[weightCur + k * 3 + 2];

                        if (deformDelta) {
                            const size_t dx = globalInfluence * 2 + 0;
                            const size_t dy = globalInfluence * 2 + 1;
                            if (dy < deformDelta->size()) {
                                bx += (*deformDelta)[dx];
                                by += (*deformDelta)[dy];
                            }
                        }
                        ++globalInfluence;

                        if (bIdx < 0 || bIdx >= static_cast<int32_t>(m_bonesWorld.size())) continue;
                        const auto& bb = m_bonesWorld[bIdx];
                        wx += (bb.a * bx + bb.b * by + bb.worldX) * bw;
                        wy += (bb.c * bx + bb.d * by + bb.worldY) * bw;
                    }

                    const size_t ui = attach.meshUvOffset + v * 2;
                    const float u  = (ui + 1 < pkg.meshVertices.size()) ? pkg.meshVertices[ui + 0] : 0.0f;
                    const float vc_uv = (ui + 1 < pkg.meshVertices.size()) ? pkg.meshVertices[ui + 1] : 0.0f;
                    attachVerts.push_back(ClipVert{ wx, wy, u, vc_uv, fr, fg, fb, fa });

                    boneCur   += influences;
                    weightCur += influences * 3;
                }
            }

            // Collect triangle indices for this mesh.
            triIndices.reserve(attach.meshTriCount);
            for (uint32_t t = 0; t < attach.meshTriCount; ++t) {
                const size_t ti = attach.meshTriOffset + t;
                if (ti >= pkg.meshTriangles.size()) break;
                triIndices.push_back(pkg.meshTriangles[ti]);
            }
        }

        // Emit the whole attachment. emitAttachment chooses fast bulk
        // vertex emission (with index sharing) when clipping is off,
        // and per-triangle SH clipping when it's on.
        emitAttachment(attachVerts, triIndices);
    }

    if (hasBounds) {
        out.boundsX = minX;
        out.boundsY = minY;
        out.boundsW = maxX - minX;
        out.boundsH = maxY - minY;
    } else {
        out.boundsX = m_posX;
        out.boundsY = m_posY;
        out.boundsW = pkg.width  * m_scaleX;
        out.boundsH = pkg.height * m_scaleY;
    }
    return out;
}

void NativeSpineEngine::getBounds(float& x, float& y, float& w, float& h) {
    // If we have an evaluated frame, use the AABB from the rendered pose.
    if (m_frameValid && !m_bonesWorld.empty()) {
        // Quick re-walk through extractMeshes' AABB logic without
        // emitting render data. For Phase 2 simplicity, call extractMeshes()
        // and use its computed bounds. (Cheap on the average character.)
        SpineRenderData rd = extractMeshes();
        x = rd.boundsX;
        y = rd.boundsY;
        w = rd.boundsW;
        h = rd.boundsH;
        return;
    }
    x = m_posX;
    y = m_posY;
    w = (m_pkg ? m_pkg->width  : 0.0f) * m_scaleX;
    h = (m_pkg ? m_pkg->height : 0.0f) * m_scaleY;
}

} // namespace rt

#endif // ROUNDTABLE_HAS_NATIVE_SHOTANIM
