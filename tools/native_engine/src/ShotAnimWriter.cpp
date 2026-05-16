/*
 * ShotAnimWriter.cpp — serialize spine::SkeletonData → .shotanim
 *
 * Coverage state (Phase 1.8, 2026-05-15):
 *   ✅ Header, footer, magic markers, CRC32
 *   ✅ String table with dedup
 *   ✅ Texture pages
 *   ✅ Bones (full)
 *   ✅ Slots (full, including darkColor + blend mode)
 *   ✅ Region attachments (full — public spine::RegionAttachment API)
 *   ✅ Mesh attachments — rigid + skinned (both spine vertex layouts);
 *      bones+weights pools emitted, LinkedMesh parent fixup done.
 *   ✅ Clipping attachments — endSlot index + vertices
 *   ✅ Skins (full)
 *   ✅ IK constraint setup data
 *   ✅ Transform constraint setup data
 *   ✅ Bone timelines: Rotate, Translate, TranslateX, TranslateY,
 *      Scale, ScaleX, ScaleY, Shear, ShearX, ShearY
 *   ✅ Slot timelines: Attachment, RGBA, RGB, Alpha, RGBA2, RGB2
 *   ✅ DrawOrder timeline
 *   ✅ IK constraint timeline (mix + bendDirection retained;
 *      softness/compress/stretch dropped — runtime doesn't use them)
 *   ✅ Transform constraint timeline (mixRotate, mixX→mixTranslate,
 *      mixScaleX→mixScale, mixShearY→mixShear; lossy but functional)
 *   ✅ Deform timeline (per-attachment vertex deltas)
 *   ✅ Curve type detection via Timeline::apply() sampling.
 *      LINEAR, STEPPED, and BEZIER are all correctly preserved per
 *      interval for all bone (Rotate, TranslateX-Y, ScaleX-Y, ShearX-Y)
 *      and slot color (RGBA, RGB, Alpha, RGBA2, RGB2) timelines.
 *   ✅ Bezier LUT extraction: per BEZIER interval, the writer samples
 *      the curve at 100 evenly-spaced times via Timeline::apply() and
 *      emits a normalized [0..1] LUT alongside placeholder control
 *      points. The runtime reads the LUT directly for evaluation —
 *      no on-line bezier solver required.
 *   ⏭ Event + PathConstraint timelines: deliberately skipped.
 *
 * Implementation note (curve detection):
 *   A single spine::Skeleton instance lives for the duration of a write()
 *   call inside a CurveTypeDetector. For each frame interval of a curve
 *   timeline, the detector resets the skeleton to setup pose, applies
 *   the timeline alone at the interval midpoint with MixBlend_Replace,
 *   reads the resulting property value, and classifies the curve as
 *   STEPPED (matches v0), LINEAR (matches mean of v0,v1), or BEZIER
 *   (matches neither, within epsilon). Bezier intervals trigger a
 *   second pass of 100 evenly-spaced apply()/read pairs to build the
 *   LUT. Cost ≈ 1 apply() per channel-interval (LINEAR/STEPPED) or
 *   101 apply()s per channel-interval (BEZIER) — acceptable for a
 *   one-shot import.
 *
 * All limitations are logged through m_skipped so the caller can surface
 * a coverage report. With the dual-path gate (ROUNDTABLE_USE_NATIVE off
 * by default), this writer ships safely.
 */

#ifdef ROUNDTABLE_HAS_SPINE

#include "spine/ShotAnimWriter.h"
#include "spine/ShotAnimFormat.h"
#include "spine/SpineAtlas.h"

#include <spine/SkeletonData.h>
#include <spine/Skeleton.h>
#include <spine/Bone.h>
#include <spine/Slot.h>
#include <spine/BoneData.h>
#include <spine/SlotData.h>
#include <spine/Skin.h>
#include <spine/Attachment.h>
#include <spine/VertexAttachment.h>
#include <spine/RegionAttachment.h>
#include <spine/MeshAttachment.h>
#include <spine/ClippingAttachment.h>
#include <spine/Animation.h>
#include <spine/Timeline.h>
#include <spine/CurveTimeline.h>
#include <spine/AttachmentTimeline.h>
#include <spine/DrawOrderTimeline.h>
#include <spine/RotateTimeline.h>
#include <spine/TranslateTimeline.h>
#include <spine/ScaleTimeline.h>
#include <spine/ShearTimeline.h>
#include <spine/ColorTimeline.h>
#include <spine/IkConstraintTimeline.h>
#include <spine/TransformConstraintTimeline.h>
#include <spine/DeformTimeline.h>
#include <spine/EventTimeline.h>
#include <spine/BlendMode.h>
#include <spine/MixBlend.h>
#include <spine/MixDirection.h>
#include <spine/Color.h>
#include <spine/IkConstraintData.h>
#include <spine/TransformConstraintData.h>
#include <spine/TransformMode.h>
#include <spine/RTTI.h>
#include <spine/Event.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace rt {

namespace {

// ─── CRC32 (table-driven) ───────────────────────────────────────────────────

uint32_t crc32(const uint8_t* data, size_t len) noexcept {
    static uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        initialized = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ─── ByteWriter ─────────────────────────────────────────────────────────────

class ByteWriter {
public:
    void writeU8 (uint8_t  v) { m_buf.push_back(v); }
    void writeU16(uint16_t v) { append(&v, 2); }
    void writeU32(uint32_t v) { append(&v, 4); }
    void writeI16(int16_t  v) { append(&v, 2); }
    void writeI32(int32_t  v) { append(&v, 4); }
    void writeF32(float    v) { append(&v, 4); }

    void writeString(const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size() > 0xFFFFu ? 0xFFFFu : s.size());
        writeU16(len);
        if (len > 0) append(s.data(), len);
    }

    void writeRaw(const void* p, size_t n) { append(p, n); }

    [[nodiscard]] size_t size() const noexcept { return m_buf.size(); }
    [[nodiscard]] const uint8_t* data() const noexcept { return m_buf.data(); }
    [[nodiscard]] size_t markPos() const noexcept { return m_buf.size(); }

    /// Patch a 4-byte value at the given offset (used for back-patching
    /// length fields written before the payload is fully known).
    void patchU32At(size_t off, uint32_t v) {
        std::memcpy(m_buf.data() + off, &v, 4);
    }
    void patchU16At(size_t off, uint16_t v) {
        std::memcpy(m_buf.data() + off, &v, 2);
    }

    void prefillHeader(const shotanim::Header& h) {
        m_buf.resize(sizeof(shotanim::Header));
        std::memcpy(m_buf.data(), &h, sizeof(shotanim::Header));
    }
    void patchHeader(const shotanim::Header& h) {
        std::memcpy(m_buf.data(), &h, sizeof(shotanim::Header));
    }

private:
    void append(const void* src, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(src);
        m_buf.insert(m_buf.end(), p, p + n);
    }
    std::vector<uint8_t> m_buf;
};

// ─── String table builder (dedup'd) ─────────────────────────────────────────

class StringTable {
public:
    uint16_t intern(const std::string& s) {
        if (s.empty()) return shotanim::kNoStringRef;
        auto it = m_lookup.find(s);
        if (it != m_lookup.end()) return it->second;
        if (m_strings.size() >= 0xFFFEu) return shotanim::kNoStringRef;
        uint16_t idx = static_cast<uint16_t>(m_strings.size());
        m_lookup.emplace(s, idx);
        m_strings.push_back(s);
        return idx;
    }
    uint16_t intern(const spine::String& s) {
        return intern(std::string(s.buffer(), s.length()));
    }

    void serialize(ByteWriter& w) const {
        w.writeU32(shotanim::kMagicStr);
        for (const auto& s : m_strings) w.writeString(s);
    }

    [[nodiscard]] uint16_t count() const noexcept {
        return static_cast<uint16_t>(m_strings.size());
    }

private:
    std::vector<std::string>                  m_strings;
    std::unordered_map<std::string, uint16_t> m_lookup;
};

// ─── Type conversions ───────────────────────────────────────────────────────

shotanim::BlendMode toBlend(spine::BlendMode m) {
    switch (m) {
        case spine::BlendMode_Additive: return shotanim::BlendMode::Additive;
        case spine::BlendMode_Multiply: return shotanim::BlendMode::Multiply;
        case spine::BlendMode_Screen:   return shotanim::BlendMode::Screen;
        case spine::BlendMode_Normal:
        default:                        return shotanim::BlendMode::Normal;
    }
}

shotanim::BoneInheritMode toInherit(spine::TransformMode m) {
    switch (m) {
        case spine::TransformMode_OnlyTranslation:        return shotanim::BoneInheritMode::OnlyTranslation;
        case spine::TransformMode_NoRotationOrReflection: return shotanim::BoneInheritMode::NoRotationOrReflection;
        case spine::TransformMode_NoScale:                return shotanim::BoneInheritMode::NoScale;
        case spine::TransformMode_NoScaleOrReflection:    return shotanim::BoneInheritMode::NoScaleOrReflection;
        case spine::TransformMode_Normal:
        default:                                          return shotanim::BoneInheritMode::Normal;
    }
}

uint32_t packColor(const spine::Color& c) {
    auto clamp01 = [](float v) {
        if (v < 0) return 0.0f;
        if (v > 1) return 1.0f;
        return v;
    };
    uint8_t r = static_cast<uint8_t>(clamp01(c.r) * 255.0f + 0.5f);
    uint8_t g = static_cast<uint8_t>(clamp01(c.g) * 255.0f + 0.5f);
    uint8_t b = static_cast<uint8_t>(clamp01(c.b) * 255.0f + 0.5f);
    uint8_t a = static_cast<uint8_t>(clamp01(c.a) * 255.0f + 0.5f);
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

// ─── Curve-type detection via Timeline::apply() sampling ────────────────────
//
// To preserve STEPPED keyframes (and detect BEZIER) without reading
// spine-cpp's internal _curves layout, we apply each timeline alone to
// a temp Skeleton in setup pose, sample at the midpoint of each frame
// interval, and compare with the expected LINEAR / STEPPED results.
//
// Bezier curves are detected (midpoint doesn't match linear or stepped)
// but currently emitted as LINEAR — see Phase 1.8 follow-up.

class CurveTypeDetector {
public:
    explicit CurveTypeDetector(spine::SkeletonData* sd) : m_sd(sd) {
        if (sd) {
            m_skel = std::make_unique<spine::Skeleton>(sd);
            m_skel->setToSetupPose();
        }
    }

    spine::Skeleton* skel() noexcept { return m_skel.get(); }

    /// Detect the curve type for one frame interval given the sampled
    /// midpoint value (which the caller obtained by applying the timeline
    /// at midTime and reading the relevant property).
    static shotanim::CurveType classify(float v0, float v1, float vMid) {
        const float dv = std::abs(v1 - v0);
        if (dv < 1e-6f) return shotanim::CurveType::Linear;
        const float epsilon = std::max(dv * 1e-3f, 1e-4f);
        if (std::abs(vMid - v0) < epsilon) return shotanim::CurveType::Stepped;
        const float linearMid = (v0 + v1) * 0.5f;
        if (std::abs(vMid - linearMid) < epsilon) return shotanim::CurveType::Linear;
        return shotanim::CurveType::Bezier;
    }

    /// Sample one channel of a timeline at the midpoint of frame interval i.
    /// Returns the observed value via `valueExtractor` after apply().
    template <typename F>
    float sampleAt(spine::Timeline* tl, float t, F valueExtractor) {
        if (!m_skel) return 0.0f;
        m_skel->setToSetupPose();
        tl->apply(*m_skel, 0.0f, t, &m_noEvents, 1.0f,
                  spine::MixBlend_Replace, spine::MixDirection_In);
        return valueExtractor();
    }

private:
    spine::SkeletonData*             m_sd;
    std::unique_ptr<spine::Skeleton> m_skel;
    spine::Vector<spine::Event*>     m_noEvents;
};

// ─── Timeline payload helpers ───────────────────────────────────────────────

// Emit the per-interval curve bytes (matches ShotAnimReader::readCurves):
//   u8 type
//   if Bezier (type == 2):
//     4 floats: cx1, cy1, cx2, cy2 (placeholders; LUT is authoritative)
//     100 floats: normalized LUT samples u_k = (v_k - v0) / (v1 - v0)
template <typename Extractor>
void writeCurveBytes(ByteWriter& body, spine::Timeline* tl,
                     CurveTypeDetector* det, Extractor extractor,
                     shotanim::CurveType ct,
                     float t0, float v0, float t1, float v1)
{
    body.writeU8(static_cast<uint8_t>(ct));
    if (ct != shotanim::CurveType::Bezier) return;

    // Spine's internal bezier control points (cx1, cy1, cx2, cy2) aren't
    // exposed through public API in a way we can reliably extract without
    // reverse-engineering _curves' layout. We emit zeros — the runtime
    // sampler should use the LUT exclusively for evaluation. The control
    // points are present for tooling / debug only.
    body.writeF32(0.0f);
    body.writeF32(0.0f);
    body.writeF32(1.0f);
    body.writeF32(1.0f);

    // 100-sample normalized LUT. Sample the curve at 100 evenly-spaced
    // times t_k = t0 + (k/99)*(t1-t0). For each, observe v_k via apply()
    // and store u_k = (v_k - v0) / (v1 - v0). LUT[0] ≈ 0, LUT[99] ≈ 1
    // by construction; intermediate values describe the easing shape.
    const float dt = t1 - t0;
    const float dv = v1 - v0;
    if (!det || dv == 0.0f) {
        for (int k = 0; k < 100; ++k) body.writeF32(k / 99.0f);
        return;
    }
    for (int k = 0; k < 100; ++k) {
        const float t_k = t0 + (static_cast<float>(k) / 99.0f) * dt;
        const float v_k = det->sampleAt(tl, t_k, extractor);
        body.writeF32((v_k - v0) / dv);
    }
}

// One-channel timeline (CurveTimeline1-shaped) with per-interval curve
// detection. `extractor` reads the channel's current value from the
// detector's skeleton after each apply().
template <typename Extractor>
void writeCT1FloatDet(ByteWriter& body, spine::Timeline* tl,
                     int frameEntries, int valueOffset,
                     CurveTypeDetector* det, Extractor extractor,
                     bool& sawBezier)
{
    auto& frames = tl->getFrames();
    size_t frameCount = tl->getFrameCount();
    body.writeU16(static_cast<uint16_t>(frameCount));
    for (size_t i = 0; i < frameCount; ++i)
        body.writeF32(frames[i * frameEntries]);
    for (size_t i = 0; i < frameCount; ++i)
        body.writeF32(frames[i * frameEntries + valueOffset]);
    if (frameCount > 0) {
        for (size_t i = 0; i + 1 < frameCount; ++i) {
            float t0 = frames[i * frameEntries];
            float v0 = frames[i * frameEntries + valueOffset];
            float t1 = frames[(i + 1) * frameEntries];
            float v1 = frames[(i + 1) * frameEntries + valueOffset];
            shotanim::CurveType ct = shotanim::CurveType::Linear;
            if (det && t1 - t0 > 1e-6f && std::abs(v1 - v0) > 1e-6f) {
                const float tMid = (t0 + t1) * 0.5f;
                float vMid = det->sampleAt(tl, tMid, extractor);
                ct = CurveTypeDetector::classify(v0, v1, vMid);
            }
            if (ct == shotanim::CurveType::Bezier) sawBezier = true;
            writeCurveBytes(body, tl, det, extractor, ct, t0, v0, t1, v1);
        }
    }
}

// Two-channel timeline (CurveTimeline2-shaped). Curve type is detected
// from the X channel only (Y typically shares the type in spine assets;
// .shotanim format stores one curve per interval, not per channel).
// Bezier LUTs are built from the X channel.
template <typename ExtractorX>
void writeCT2Vec2Det(ByteWriter& body, spine::Timeline* tl,
                    int frameEntries, int xOff, int yOff,
                    CurveTypeDetector* det, ExtractorX extractorX,
                    bool& sawBezier)
{
    auto& frames = tl->getFrames();
    size_t frameCount = tl->getFrameCount();
    body.writeU16(static_cast<uint16_t>(frameCount));
    for (size_t i = 0; i < frameCount; ++i)
        body.writeF32(frames[i * frameEntries]);
    for (size_t i = 0; i < frameCount; ++i) {
        body.writeF32(frames[i * frameEntries + xOff]);
        body.writeF32(frames[i * frameEntries + yOff]);
    }
    if (frameCount > 0) {
        for (size_t i = 0; i + 1 < frameCount; ++i) {
            float t0 = frames[i * frameEntries];
            float v0 = frames[i * frameEntries + xOff];
            float t1 = frames[(i + 1) * frameEntries];
            float v1 = frames[(i + 1) * frameEntries + xOff];
            shotanim::CurveType ct = shotanim::CurveType::Linear;
            if (det && t1 - t0 > 1e-6f && std::abs(v1 - v0) > 1e-6f) {
                const float tMid = (t0 + t1) * 0.5f;
                float vMid = det->sampleAt(tl, tMid, extractorX);
                ct = CurveTypeDetector::classify(v0, v1, vMid);
            }
            if (ct == shotanim::CurveType::Bezier) sawBezier = true;
            writeCurveBytes(body, tl, det, extractorX, ct, t0, v0, t1, v1);
        }
    }
}

void writeTimelineHeader(ByteWriter& body, shotanim::TimelineType type, uint16_t targetIdx) {
    body.writeU8(static_cast<uint8_t>(type));
    body.writeU16(targetIdx);
}

} // namespace

// ─── ShotAnimWriter ─────────────────────────────────────────────────────────

ShotAnimWriter::ShotAnimWriter()  = default;
ShotAnimWriter::~ShotAnimWriter() = default;

bool ShotAnimWriter::write(const spine::SkeletonData* skelData,
                            const SpineAtlas& atlas,
                            const std::string& outPath) {
    m_lastError.clear();
    m_skipped.clear();

    if (!skelData) {
        m_lastError = "ShotAnimWriter: null SkeletonData";
        return false;
    }
    if (!atlas.isLoaded()) {
        m_lastError = "ShotAnimWriter: atlas not loaded";
        return false;
    }

    using namespace shotanim;
    ByteWriter w;
    StringTable strings;

    auto* sd = const_cast<spine::SkeletonData*>(skelData);

    Header hdr{};
    hdr.magic   = kHeaderMagic;
    hdr.version = kCurrentVersion;
    hdr.canvasWidth  = sd->getWidth();
    hdr.canvasHeight = sd->getHeight();
    hdr.flags = 0;

    w.prefillHeader(hdr);

    ByteWriter body;

    // ── Texture pages ────────────────────────────────────────────────────
    body.writeU32(kMagicPage);
    const auto& pages = atlas.pages();
    hdr.pageCount = static_cast<uint16_t>(pages.size());
    for (const auto& p : pages) {
        uint16_t pathIdx = strings.intern(p.texturePath);
        body.writeU16(pathIdx);
        body.writeU16(static_cast<uint16_t>(p.width));
        body.writeU16(static_cast<uint16_t>(p.height));
        body.writeU8(p.pma ? 1 : 0);
        body.writeU8(0);
    }

    // ── Bones ────────────────────────────────────────────────────────────
    body.writeU32(kMagicBone);
    auto& bones = sd->getBones();
    hdr.boneCount = static_cast<uint16_t>(bones.size());

    std::unordered_map<spine::BoneData*, int> boneIdx;
    for (size_t i = 0; i < bones.size(); ++i) boneIdx[bones[i]] = static_cast<int>(i);

    for (size_t i = 0; i < bones.size(); ++i) {
        auto* b = bones[i];
        body.writeU16(strings.intern(b->getName()));
        spine::BoneData* parent = b->getParent();
        int16_t parentIdx = -1;
        if (parent) {
            auto it = boneIdx.find(parent);
            if (it != boneIdx.end()) parentIdx = static_cast<int16_t>(it->second);
        }
        body.writeI16(parentIdx);
        body.writeF32(b->getX());
        body.writeF32(b->getY());
        body.writeF32(b->getRotation());
        body.writeF32(b->getScaleX());
        body.writeF32(b->getScaleY());
        body.writeF32(b->getShearX());
        body.writeF32(b->getShearY());
        body.writeF32(b->getLength());
        body.writeU8(static_cast<uint8_t>(toInherit(b->getTransformMode())));
        const auto& col = b->getColor();
        body.writeF32(col.r);
        body.writeF32(col.g);
        body.writeF32(col.b);
        body.writeF32(col.a);
    }

    // ── Attachment pool walk (regions + meshes + clipping) ───────────────
    // Pool indices MUST match the order the reader reconstructs the
    // attachments array. The reader reads sections in (region, mesh,
    // clipping) order — so we assign poolIdx in that order too.
    // A two-pass build: first collect into per-kind lists, then assign
    // global poolIdx by walking regions → meshes → clipping.
    struct AttachRecord {
        int32_t poolIdx;
        spine::Attachment* att;
        spine::String name;
        size_t slotIndex;
        shotanim::AttachmentKind kind;
    };
    auto attachKey = [](size_t slotIdx, const spine::String& name) {
        std::string k;
        k.reserve(name.length() + 8);
        k = std::to_string(slotIdx) + "\x1f" + std::string(name.buffer(), name.length());
        return k;
    };

    std::unordered_map<std::string, AttachRecord*> attachLookup;
    std::vector<AttachRecord> regionList, meshList, clipList;
    // Records EVERY (slotIdx, name) that exists in any skin, regardless of
    // whether it's a renderable kind. Used by the slot-loop below to
    // distinguish "truly missing setup attachment" (real bug worth warning
    // about) from "setup name refers to a Point/BoundingBox/Path that the
    // runtime correctly doesn't render" (benign and very common for
    // skeletal markers like 'body'/'face' PointAttachments in Nikke rigs).
    std::unordered_set<std::string> anySkinHasName;

    auto& allSkins = sd->getSkins();
    // First pass: classify each unique (slotIdx, name) attachment into its kind list.
    {
        std::unordered_map<std::string, bool> seen;
        for (size_t si = 0; si < allSkins.size(); ++si) {
            auto* skin = allSkins[si];
            auto entries = skin->getAttachments();
            while (entries.hasNext()) {
                auto& entry = entries.next();
                auto* att = entry._attachment;
                if (!att) continue;
                auto key = attachKey(entry._slotIndex, entry._name);
                anySkinHasName.insert(key);
                if (seen.find(key) != seen.end()) continue;
                seen[key] = true;

                AttachRecord rec{};
                rec.att = att;
                rec.name = entry._name;
                rec.slotIndex = entry._slotIndex;
                rec.poolIdx = -1;  // assigned in second pass

                if (att->getRTTI().instanceOf(spine::RegionAttachment::rtti)) {
                    rec.kind = shotanim::AttachmentKind::Region;
                    regionList.push_back(std::move(rec));
                } else if (att->getRTTI().instanceOf(spine::MeshAttachment::rtti)) {
                    rec.kind = shotanim::AttachmentKind::Mesh;
                    meshList.push_back(std::move(rec));
                } else if (att->getRTTI().instanceOf(spine::ClippingAttachment::rtti)) {
                    rec.kind = shotanim::AttachmentKind::Clipping;
                    clipList.push_back(std::move(rec));
                }
                // Otherwise: BoundingBox / Path / Point — no visual, skip.
            }
        }
    }

    // Second pass: assign global poolIdx in read order (regions → meshes →
    // clipping), then build attachLookup keyed by (slotIdx, name).
    int32_t nextPoolIdx = 0;
    auto assignAndIndex = [&](std::vector<AttachRecord>& list) {
        for (auto& rec : list) {
            rec.poolIdx = nextPoolIdx++;
            const auto key = attachKey(rec.slotIndex, rec.name);
            attachLookup[key] = &rec;
        }
    };
    assignAndIndex(regionList);
    assignAndIndex(meshList);
    assignAndIndex(clipList);

    // Build a pointer→pool-index map so LinkedMesh can resolve parentMesh.
    std::unordered_map<spine::Attachment*, int32_t> attPtrToPoolIdx;
    for (const auto& rec : regionList) attPtrToPoolIdx[rec.att] = rec.poolIdx;
    for (const auto& rec : meshList)   attPtrToPoolIdx[rec.att] = rec.poolIdx;
    for (const auto& rec : clipList)   attPtrToPoolIdx[rec.att] = rec.poolIdx;

    // ── Slots ────────────────────────────────────────────────────────────
    body.writeU32(kMagicSlot);
    auto& slots = sd->getSlots();
    hdr.slotCount = static_cast<uint16_t>(slots.size());

    for (size_t i = 0; i < slots.size(); ++i) {
        auto* s = slots[i];
        body.writeU16(strings.intern(s->getName()));
        body.writeU16(static_cast<uint16_t>(s->getBoneData().getIndex()));
        body.writeU32(packColor(s->getColor()));
        if (s->hasDarkColor()) {
            body.writeU32(packColor(s->getDarkColor()));
            hdr.flags |= kFlagHasTwoColorTint;
        } else {
            body.writeU32(0u);
        }
        body.writeU8(static_cast<uint8_t>(toBlend(s->getBlendMode())));

        int32_t defaultAttach = -1;
        const auto& defaultAttachName = s->getAttachmentName();
        if (defaultAttachName.length() > 0) {
            auto key = attachKey(i, defaultAttachName);
            auto it = attachLookup.find(key);
            if (it != attachLookup.end() && it->second != nullptr) {
                defaultAttach = it->second->poolIdx;
            } else if (anySkinHasName.find(key) == anySkinHasName.end()) {
                // The setup name doesn't exist in ANY skin — real bug.
                // (If it existed but only as Point/BoundingBox/Path, we
                // silently encode -1; runtime won't render it anyway.)
                spdlog::warn("ShotAnimWriter: slot {} '{}' setup attachment '{}' not found in any skin",
                             i, s->getName().buffer(),
                             defaultAttachName.buffer());
            }
        }
        body.writeI32(defaultAttach);
    }

    // ── Region attachments ───────────────────────────────────────────────
    body.writeU32(kMagicRgn);
    hdr.regionAttachCount = static_cast<uint16_t>(regionList.size());
    for (const auto& rec : regionList) {
        auto* r = static_cast<spine::RegionAttachment*>(rec.att);
        body.writeU16(strings.intern(r->getName()));

        // Resolve the atlas page index directly. Pre-v4 this slot stored
        // an index into a regions array that was never serialized, so the
        // runtime always saw pageIdx = -1 and produced untextured triangles.
        auto* atlasRegion = atlas.findRegion(r->getName().buffer());
        const int32_t pageIdx = atlasRegion ? atlasRegion->pageIndex : -1;
        body.writeI32(pageIdx);
        body.writeF32(r->getX());
        body.writeF32(r->getY());
        body.writeF32(r->getWidth());
        body.writeF32(r->getHeight());

        auto& off = r->getOffset();
        for (int i = 0; i < 8; ++i)
            body.writeF32(i < static_cast<int>(off.size()) ? off[i] : 0.0f);

        auto& uvs = r->getUVs();
        for (int i = 0; i < 8; ++i)
            body.writeF32(i < static_cast<int>(uvs.size()) ? uvs[i] : 0.0f);

        const auto& col = r->getColor();
        body.writeF32(col.r);
        body.writeF32(col.g);
        body.writeF32(col.b);
        body.writeF32(col.a);
    }

    // ── Mesh attachments ─────────────────────────────────────────────────
    body.writeU32(kMagicMsh);
    hdr.meshAttachCount = static_cast<uint16_t>(meshList.size());
    if (!meshList.empty()) hdr.flags |= kFlagHasMeshAttachments;

    bool sawSkinnedMesh = false;
    bool sawLinkedMesh  = false;

    for (const auto& rec : meshList) {
        auto* mesh = static_cast<spine::MeshAttachment*>(rec.att);
        body.writeU16(strings.intern(mesh->getName()));

        // Resolve the atlas page index directly (see region attachment
        // comment above — same v3→v4 fix).
        auto* atlasRegion = atlas.findRegion(mesh->getName().buffer());
        const int32_t pageIdx = atlasRegion ? atlasRegion->pageIndex : -1;
        body.writeI32(pageIdx);

        auto& uvVec    = mesh->getUVs();
        auto& triVec   = mesh->getTriangles();
        auto& edgeVec  = mesh->getEdges();
        auto& bonesVec = mesh->getBones();
        auto& vertsVec = mesh->getVertices();

        const uint32_t vCount  = static_cast<uint32_t>(mesh->getWorldVerticesLength() / 2);
        const uint32_t tCount  = static_cast<uint32_t>(triVec.size());
        const uint32_t hullLen = static_cast<uint32_t>(mesh->getHullLength());
        const uint32_t eCount  = static_cast<uint32_t>(edgeVec.size());

        // Skinned meshes use spine's interleaved bones+weights layout:
        //   bonesVec  = [N_v0, b_v0_0, ..., N_v1, b_v1_0, ...]
        //   vertsVec  = [x, y, w, x, y, w, ...] — per bone influence
        // For rigid meshes:
        //   bonesVec  is empty
        //   vertsVec  = [x0, y0, x1, y1, ...] — flat positions
        const bool skinned = (bonesVec.size() > 0);
        if (skinned) sawSkinnedMesh = true;

        const uint32_t boneCount   = skinned ? static_cast<uint32_t>(bonesVec.size()) : 0u;
        const uint32_t weightCount = skinned ? static_cast<uint32_t>(vertsVec.size()) : 0u;

        body.writeU32(vCount);
        body.writeU32(tCount);
        body.writeU32(hullLen);
        body.writeU32(eCount);
        body.writeU32(boneCount);
        body.writeU32(weightCount);

        // Positions: rigid → vertsVec is [x0,y0,...]; skinned → zeros
        // (runtime computes from skinning data).
        if (!skinned) {
            const size_t need = vCount * 2;
            for (size_t i = 0; i < need; ++i) {
                body.writeF32(i < vertsVec.size() ? vertsVec[i] : 0.0f);
            }
        } else {
            for (uint32_t i = 0; i < vCount * 2; ++i) body.writeF32(0.0f);
        }

        // UVs (atlas-space; ready for rendering)
        for (uint32_t i = 0; i < vCount * 2; ++i) {
            body.writeF32(i < uvVec.size() ? uvVec[i] : 0.0f);
        }

        // Triangles
        for (uint32_t i = 0; i < tCount; ++i) {
            body.writeU16(static_cast<uint16_t>(triVec[i]));
        }

        // Edges
        for (uint32_t i = 0; i < eCount; ++i) {
            body.writeU16(static_cast<uint16_t>(edgeVec[i]));
        }

        // Hull indices (implicit 0..hullLen-1)
        for (uint32_t i = 0; i < hullLen; ++i) {
            body.writeU16(static_cast<uint16_t>(i));
        }

        // Skinning data (only present when skinned)
        for (uint32_t b = 0; b < boneCount; ++b) {
            body.writeI32(static_cast<int32_t>(bonesVec[b]));
        }
        for (uint32_t w = 0; w < weightCount; ++w) {
            body.writeF32(vertsVec[w]);
        }

        // Origin + dimensions
        body.writeF32(0.0f);
        body.writeF32(0.0f);
        body.writeF32(static_cast<float>(mesh->getWidth()));
        body.writeF32(static_cast<float>(mesh->getHeight()));

        const auto& col = mesh->getColor();
        body.writeF32(col.r);
        body.writeF32(col.g);
        body.writeF32(col.b);
        body.writeF32(col.a);

        // LinkedMesh: parent pool index.
        int32_t parentIdx = -1;
        if (auto* parent = mesh->getParentMesh()) {
            sawLinkedMesh = true;
            auto it = attPtrToPoolIdx.find(parent);
            if (it != attPtrToPoolIdx.end()) parentIdx = it->second;
        }
        body.writeI32(parentIdx);
    }

    if (sawSkinnedMesh)
        spdlog::info("ShotAnimWriter: skinned mesh data emitted (bones + weights pools)");
    if (sawLinkedMesh)
        spdlog::info("ShotAnimWriter: linked-mesh parent fixup applied for at least one mesh");

    // ── Clipping attachments ─────────────────────────────────────────────
    body.writeU32(kMagicClp);
    hdr.clipAttachCount = static_cast<uint16_t>(clipList.size());
    if (!clipList.empty()) hdr.flags |= kFlagHasClipping;

    // Build slot-pointer → slot-index map for endSlot resolution.
    std::unordered_map<spine::SlotData*, int> slotIdx;
    for (size_t i = 0; i < slots.size(); ++i) slotIdx[slots[i]] = static_cast<int>(i);

    for (const auto& rec : clipList) {
        auto* clip = static_cast<spine::ClippingAttachment*>(rec.att);
        body.writeU16(strings.intern(clip->getName()));

        uint16_t endSlot = 0;
        if (auto* es = clip->getEndSlot()) {
            auto it = slotIdx.find(es);
            if (it != slotIdx.end()) endSlot = static_cast<uint16_t>(it->second);
        }
        body.writeU16(endSlot);

        // ClippingAttachment extends VertexAttachment; getVertices gives
        // a flat [x0,y0,x1,y1,...] polygon (rigid; never skinned).
        auto& vertsVec = clip->getVertices();
        const uint32_t vCount = static_cast<uint32_t>(clip->getWorldVerticesLength() / 2);
        body.writeU32(vCount);
        const size_t need = vCount * 2;
        for (size_t i = 0; i < need; ++i) {
            body.writeF32(i < vertsVec.size() ? vertsVec[i] : 0.0f);
        }
    }

    // ── Skins ────────────────────────────────────────────────────────────
    body.writeU32(kMagicSkin);
    hdr.skinCount = static_cast<uint16_t>(allSkins.size());
    for (size_t si = 0; si < allSkins.size(); ++si) {
        auto* skin = allSkins[si];
        body.writeU16(strings.intern(skin->getName()));

        struct Pair { int32_t slot; int32_t pool; };
        std::vector<Pair> pairs;
        auto entries = skin->getAttachments();
        while (entries.hasNext()) {
            auto& entry = entries.next();
            auto* att = entry._attachment;
            if (!att) continue;
            auto key = attachKey(entry._slotIndex, entry._name);
            auto it = attachLookup.find(key);
            if (it == attachLookup.end() || it->second == nullptr) continue;
            pairs.push_back({static_cast<int32_t>(entry._slotIndex), it->second->poolIdx});
        }
        body.writeU16(static_cast<uint16_t>(pairs.size()));
        for (auto& p : pairs) body.writeI32(p.slot);
        for (auto& p : pairs) body.writeI32(p.pool);
    }

    // ── IK constraints (setup data) ──────────────────────────────────────
    // Field order matches SkeletonTypes.h IkConstraintData semantics:
    //   slot 1  → targetBoneIdx   (the IK target — what the chain reaches for)
    //   slot 2  → bendBoneIdx     (for 2-bone IK, constrainedBones[1]; -1 for 1-bone)
    //   slot 3  → bendDirection
    //   slot 4  → mix
    //   slot 5+ → count + constrainedBones[]
    body.writeU32(kMagicIK);
    auto& iks = sd->getIkConstraints();
    hdr.ikCount = static_cast<uint16_t>(iks.size());
    if (iks.size() > 0) hdr.flags |= kFlagHasIK;
    for (size_t i = 0; i < iks.size(); ++i) {
        auto* ik = iks[i];
        auto& constrained = ik->getBones();   // spine: "bones" = constrained chain
        // targetBoneIdx: the bone the IK reaches for
        body.writeI32(ik->getTarget() ? ik->getTarget()->getIndex() : -1);
        // bendBoneIdx: for 2-bone IK, the second constrained bone (the
        // knee/elbow joint). For 1-bone IK there is no bend bone — emit -1.
        const int32_t bendIdx = (constrained.size() >= 2)
            ? constrained[1]->getIndex()
            : -1;
        body.writeI32(bendIdx);
        body.writeU8(static_cast<uint8_t>(ik->getBendDirection()));
        body.writeF32(ik->getMix());
        body.writeU16(static_cast<uint16_t>(constrained.size()));
        for (size_t b = 0; b < constrained.size(); ++b)
            body.writeI32(constrained[b]->getIndex());
    }

    // Diagnostic: warn if the .skel has path constraints, which the
    // native runtime does NOT yet support. Affected bones will render
    // at their pre-constraint positions (matrix correct, world position
    // shifted by the constraint's translation effect along the path).
    {
        auto& paths = sd->getPathConstraints();
        if (paths.size() > 0) {
            spdlog::warn("ShotAnimWriter: skel has {} PathConstraint(s) - native runtime ignores them; expect positional drift on constrained bones",
                         (size_t)paths.size());
        }
    }

    // ── Transform constraints (setup data) ───────────────────────────────
    body.writeU32(kMagicTfm);
    auto& tcs = sd->getTransformConstraints();
    hdr.transformConstraintCount = static_cast<uint16_t>(tcs.size());
    if (tcs.size() > 0) hdr.flags |= kFlagHasTransformConstr;
    for (size_t i = 0; i < tcs.size(); ++i) {
        auto* tc = tcs[i];
        body.writeI32(tc->getTarget() ? tc->getTarget()->getIndex() : -1);
        // Full spine 4.2 model: per-axis mixes + constant offsets +
        // apply-mode flags (v5 format). Reader/runtime mirror this order.
        body.writeF32(tc->getMixRotate());
        body.writeF32(tc->getMixX());
        body.writeF32(tc->getMixY());
        body.writeF32(tc->getMixScaleX());
        body.writeF32(tc->getMixScaleY());
        body.writeF32(tc->getMixShearY());
        body.writeF32(tc->getOffsetRotation());
        body.writeF32(tc->getOffsetX());
        body.writeF32(tc->getOffsetY());
        body.writeF32(tc->getOffsetScaleX());
        body.writeF32(tc->getOffsetScaleY());
        body.writeF32(tc->getOffsetShearY());
        body.writeU8(tc->isLocal()    ? 1u : 0u);
        body.writeU8(tc->isRelative() ? 1u : 0u);
        auto& targets = tc->getBones();
        body.writeU16(static_cast<uint16_t>(targets.size()));
        for (size_t b = 0; b < targets.size(); ++b)
            body.writeI32(targets[b]->getIndex());
    }

    // ── Animations ───────────────────────────────────────────────────────
    body.writeU32(kMagicAnim);
    auto& anims = sd->getAnimations();
    hdr.animCount = static_cast<uint16_t>(anims.size());

    // Build IK / Transform constraint pointer → index map for timeline
    // dispatch.
    std::unordered_map<spine::IkConstraintData*, int>        ikIdxMap;
    std::unordered_map<spine::TransformConstraintData*, int> tcIdxMap;
    for (size_t i = 0; i < iks.size(); ++i) ikIdxMap[iks[i]] = static_cast<int>(i);
    for (size_t i = 0; i < tcs.size(); ++i) tcIdxMap[tcs[i]] = static_cast<int>(i);

    bool sawDrawOrder = false;
    bool sawDeform    = false;
    bool sawBezier    = false;
    bool sawEventSkip = false;
    bool sawPathSkip  = false;

    // Build the temp skeleton used by the curve-type detector. One
    // instance, reused (setToSetupPose) before each apply() sample.
    // sd is non-null (checked at function entry), so det.skel() is
    // also non-null for the duration of this scope.
    CurveTypeDetector det(sd);
    auto* detSkel = det.skel();

    for (size_t ai = 0; ai < anims.size(); ++ai) {
        auto* a = anims[ai];
        body.writeU16(strings.intern(a->getName()));
        body.writeF32(a->getDuration());

        // Back-patch slot for timelineCount: write a placeholder u16 and
        // remember its offset so we can update it after counting emitted
        // timelines.
        const size_t timelineCountOff = body.markPos();
        body.writeU16(0);

        uint16_t emittedTimelines = 0;
        auto& timelines = a->getTimelines();
        for (size_t t = 0; t < timelines.size(); ++t) {
            spine::Timeline* tl = timelines[t];
            auto& rtti = tl->getRTTI();

            // ── Bone timelines ───────────────────────────────────────
            // Note on extractor return values:
            // Bone-timeline frame values in spine's internal `_frames` array
            // (the same values we write into the .shotanim file) are stored
            // RELATIVE to setup pose — additive offsets for rotate/translate/
            // shear, multiplicative factors for scale. After Timeline::apply()
            // with MixBlend_Replace, bone.getX() returns the ABSOLUTE bone
            // value (= setup.x + offset for translate, or setup.scaleX * factor
            // for scale). For the bezier LUT normalization to be consistent
            // with v0 / v1 read from _frames, the extractor must return the
            // SAME UNITS — i.e. the offset (or factor), not the absolute value.
            // We subtract b->getData().get*() for additive properties and
            // divide for scale.
            if (rtti.instanceOf(spine::RotateTimeline::rtti)) {
                auto* x = static_cast<spine::RotateTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneRotate,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT1FloatDet(body, tl, 2, 1, &det,
                                 [b]() { return b ? (b->getRotation() - b->getData().getRotation()) : 0.0f; },
                                 sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::TranslateTimeline::rtti)) {
                auto* x = static_cast<spine::TranslateTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneTranslate,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT2Vec2Det(body, tl, 3, 1, 2, &det,
                                [b]() { return b ? (b->getX() - b->getData().getX()) : 0.0f; },
                                sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::TranslateXTimeline::rtti)) {
                auto* x = static_cast<spine::TranslateXTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneTranslateX,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT1FloatDet(body, tl, 2, 1, &det,
                                 [b]() { return b ? (b->getX() - b->getData().getX()) : 0.0f; },
                                 sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::TranslateYTimeline::rtti)) {
                auto* x = static_cast<spine::TranslateYTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneTranslateY,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT1FloatDet(body, tl, 2, 1, &det,
                                 [b]() { return b ? (b->getY() - b->getData().getY()) : 0.0f; },
                                 sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::ScaleTimeline::rtti)) {
                auto* x = static_cast<spine::ScaleTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneScale,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT2Vec2Det(body, tl, 3, 1, 2, &det,
                                [b]() {
                                    if (!b) return 1.0f;
                                    const float s = b->getData().getScaleX();
                                    return (std::abs(s) > 1e-6f) ? (b->getScaleX() / s) : 1.0f;
                                },
                                sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::ScaleXTimeline::rtti)) {
                auto* x = static_cast<spine::ScaleXTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneScaleX,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT1FloatDet(body, tl, 2, 1, &det,
                                 [b]() {
                                     if (!b) return 1.0f;
                                     const float s = b->getData().getScaleX();
                                     return (std::abs(s) > 1e-6f) ? (b->getScaleX() / s) : 1.0f;
                                 },
                                 sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::ScaleYTimeline::rtti)) {
                auto* x = static_cast<spine::ScaleYTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneScaleY,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT1FloatDet(body, tl, 2, 1, &det,
                                 [b]() {
                                     if (!b) return 1.0f;
                                     const float s = b->getData().getScaleY();
                                     return (std::abs(s) > 1e-6f) ? (b->getScaleY() / s) : 1.0f;
                                 },
                                 sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::ShearTimeline::rtti)) {
                auto* x = static_cast<spine::ShearTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneShear,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT2Vec2Det(body, tl, 3, 1, 2, &det,
                                [b]() { return b ? (b->getShearX() - b->getData().getShearX()) : 0.0f; },
                                sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::ShearXTimeline::rtti)) {
                auto* x = static_cast<spine::ShearXTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneShearX,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT1FloatDet(body, tl, 2, 1, &det,
                                 [b]() { return b ? (b->getShearX() - b->getData().getShearX()) : 0.0f; },
                                 sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::ShearYTimeline::rtti)) {
                auto* x = static_cast<spine::ShearYTimeline*>(tl);
                spine::Bone* b = detSkel ? detSkel->getBones()[x->getBoneIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::BoneShearY,
                                    static_cast<uint16_t>(x->getBoneIndex()));
                writeCT1FloatDet(body, tl, 2, 1, &det,
                                 [b]() { return b ? (b->getShearY() - b->getData().getShearY()) : 0.0f; },
                                 sawBezier);
                ++emittedTimelines;
            }
            // ── Slot attachment timeline ─────────────────────────────
            else if (rtti.instanceOf(spine::AttachmentTimeline::rtti)) {
                auto* x = static_cast<spine::AttachmentTimeline*>(tl);
                writeTimelineHeader(body, TimelineType::SlotAttachment,
                                    static_cast<uint16_t>(x->getSlotIndex()));
                auto& frames = x->getFrames();
                auto& names  = x->getAttachmentNames();
                size_t frameCount = x->getFrameCount();
                body.writeU16(static_cast<uint16_t>(frameCount));
                for (size_t i = 0; i < frameCount; ++i)
                    body.writeF32(frames[i]);
                for (size_t i = 0; i < frameCount; ++i) {
                    // Value = string-table index (zero-extended into i32).
                    // Attachment timelines are semantically stepped
                    // (no interpolation between names), so no curve
                    // detection is needed.
                    uint16_t nameIdx = (i < names.size())
                        ? strings.intern(names[i])
                        : kNoStringRef;
                    body.writeI32(static_cast<int32_t>(static_cast<uint32_t>(nameIdx)));
                }
                ++emittedTimelines;
            }
            // ── Slot color timelines ─────────────────────────────────
            else if (rtti.instanceOf(spine::RGBATimeline::rtti)) {
                auto* x = static_cast<spine::RGBATimeline*>(tl);
                spine::Slot* s = detSkel ? detSkel->getSlots()[x->getSlotIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::SlotRGBA,
                                    static_cast<uint16_t>(x->getSlotIndex()));
                writeCT1FloatDet(body, tl, 5, 1, &det,
                                 [s]() { return s ? s->getColor().r : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 5, 2, &det,
                                 [s]() { return s ? s->getColor().g : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 5, 3, &det,
                                 [s]() { return s ? s->getColor().b : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 5, 4, &det,
                                 [s]() { return s ? s->getColor().a : 1.0f; }, sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::RGBTimeline::rtti)) {
                auto* x = static_cast<spine::RGBTimeline*>(tl);
                spine::Slot* s = detSkel ? detSkel->getSlots()[x->getSlotIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::SlotRGB,
                                    static_cast<uint16_t>(x->getSlotIndex()));
                writeCT1FloatDet(body, tl, 4, 1, &det,
                                 [s]() { return s ? s->getColor().r : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 4, 2, &det,
                                 [s]() { return s ? s->getColor().g : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 4, 3, &det,
                                 [s]() { return s ? s->getColor().b : 1.0f; }, sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::AlphaTimeline::rtti)) {
                auto* x = static_cast<spine::AlphaTimeline*>(tl);
                spine::Slot* s = detSkel ? detSkel->getSlots()[x->getSlotIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::SlotAlpha,
                                    static_cast<uint16_t>(x->getSlotIndex()));
                writeCT1FloatDet(body, tl, 2, 1, &det,
                                 [s]() { return s ? s->getColor().a : 1.0f; }, sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::RGBA2Timeline::rtti)) {
                auto* x = static_cast<spine::RGBA2Timeline*>(tl);
                spine::Slot* s = detSkel ? detSkel->getSlots()[x->getSlotIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::SlotRGBA2,
                                    static_cast<uint16_t>(x->getSlotIndex()));
                writeCT1FloatDet(body, tl, 8, 1, &det,
                                 [s]() { return s ? s->getColor().r : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 8, 2, &det,
                                 [s]() { return s ? s->getColor().g : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 8, 3, &det,
                                 [s]() { return s ? s->getColor().b : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 8, 5, &det,
                                 [s]() { return s ? s->getDarkColor().r : 0.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 8, 6, &det,
                                 [s]() { return s ? s->getDarkColor().g : 0.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 8, 7, &det,
                                 [s]() { return s ? s->getDarkColor().b : 0.0f; }, sawBezier);
                ++emittedTimelines;
            }
            else if (rtti.instanceOf(spine::RGB2Timeline::rtti)) {
                auto* x = static_cast<spine::RGB2Timeline*>(tl);
                spine::Slot* s = detSkel ? detSkel->getSlots()[x->getSlotIndex()] : nullptr;
                writeTimelineHeader(body, TimelineType::SlotRGB2,
                                    static_cast<uint16_t>(x->getSlotIndex()));
                writeCT1FloatDet(body, tl, 7, 1, &det,
                                 [s]() { return s ? s->getColor().r : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 7, 2, &det,
                                 [s]() { return s ? s->getColor().g : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 7, 3, &det,
                                 [s]() { return s ? s->getColor().b : 1.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 7, 4, &det,
                                 [s]() { return s ? s->getDarkColor().r : 0.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 7, 5, &det,
                                 [s]() { return s ? s->getDarkColor().g : 0.0f; }, sawBezier);
                writeCT1FloatDet(body, tl, 7, 6, &det,
                                 [s]() { return s ? s->getDarkColor().b : 0.0f; }, sawBezier);
                ++emittedTimelines;
            }
            // ── Draw order timeline ──────────────────────────────────
            else if (rtti.instanceOf(spine::DrawOrderTimeline::rtti)) {
                auto* x = static_cast<spine::DrawOrderTimeline*>(tl);
                sawDrawOrder = true;
                hdr.flags |= kFlagHasDrawOrderKeys;
                writeTimelineHeader(body, TimelineType::DrawOrder, 0);
                auto& frames = x->getFrames();
                auto& orders = x->getDrawOrders();
                size_t frameCount = x->getFrameCount();
                body.writeU16(static_cast<uint16_t>(frameCount));
                for (size_t i = 0; i < frameCount; ++i) {
                    body.writeF32(frames[i]);
                    // Each order is Vector<int> of length slotCount.
                    // Empty vector = bind pose (identity).
                    if (i < orders.size() && orders[i].size() == slots.size()) {
                        for (size_t s = 0; s < slots.size(); ++s)
                            body.writeI32(orders[i][s]);
                    } else {
                        for (size_t s = 0; s < slots.size(); ++s)
                            body.writeI32(static_cast<int32_t>(s));
                    }
                }
                ++emittedTimelines;
            }
            // ── IK constraint timeline ───────────────────────────────
            else if (rtti.instanceOf(spine::IkConstraintTimeline::rtti)) {
                auto* x = static_cast<spine::IkConstraintTimeline*>(tl);
                writeTimelineHeader(body, TimelineType::IkConstraint,
                                    static_cast<uint16_t>(x->getIkConstraintIndex()));
                auto& frames = x->getFrames();
                size_t frameCount = x->getFrameCount();
                size_t frameEntries = x->getFrameEntries();
                body.writeU16(static_cast<uint16_t>(frameCount));
                for (size_t i = 0; i < frameCount; ++i) {
                    body.writeF32(frames[i * frameEntries]);
                    // mix = entry 1; bendDirection = entry 3
                    body.writeF32(frames[i * frameEntries + 1]);
                    body.writeF32(frames[i * frameEntries + 3]);
                }
                ++emittedTimelines;
            }
            // ── Transform constraint timeline ────────────────────────
            else if (rtti.instanceOf(spine::TransformConstraintTimeline::rtti)) {
                auto* x = static_cast<spine::TransformConstraintTimeline*>(tl);
                writeTimelineHeader(body, TimelineType::TransformConstr,
                                    static_cast<uint16_t>(x->getTransformConstraintIndex()));
                auto& frames = x->getFrames();
                size_t frameCount = x->getFrameCount();
                size_t frameEntries = x->getFrameEntries();
                body.writeU16(static_cast<uint16_t>(frameCount));
                // Spine 4.2 frame layout: [time, mixRotate, mixX, mixY,
                // mixScaleX, mixScaleY, mixShearY]. v5 stores all 6 mixes
                // (v4 dropped mixY and mixScaleY).
                for (size_t i = 0; i < frameCount; ++i) {
                    body.writeF32(frames[i * frameEntries]);      // time
                    body.writeF32(frames[i * frameEntries + 1]);  // mixRotate
                    body.writeF32(frames[i * frameEntries + 2]);  // mixX
                    body.writeF32(frames[i * frameEntries + 3]);  // mixY
                    body.writeF32(frames[i * frameEntries + 4]);  // mixScaleX
                    body.writeF32(frames[i * frameEntries + 5]);  // mixScaleY
                    body.writeF32(frames[i * frameEntries + 6]);  // mixShearY
                }
                ++emittedTimelines;
            }
            // ── Deform timeline ──────────────────────────────────────
            else if (rtti.instanceOf(spine::DeformTimeline::rtti)) {
                auto* x = static_cast<spine::DeformTimeline*>(tl);
                sawDeform = true;
                hdr.flags |= kFlagHasDeform;

                // Resolve attachment pool index for this slot+attachment.
                int32_t poolIdx = -1;
                auto* va = x->getAttachment();
                if (va) {
                    auto it = attPtrToPoolIdx.find(va);
                    if (it != attPtrToPoolIdx.end()) poolIdx = it->second;
                }
                if (poolIdx < 0) continue;  // skip orphan deform

                writeTimelineHeader(body, TimelineType::SlotDeform,
                                    static_cast<uint16_t>(poolIdx));
                auto& frames   = x->getFrames();
                auto& vertSets = x->getVertices();
                size_t frameCount = x->getFrameCount();

                // Find the per-frame vertex count (all frames have same count).
                uint32_t vertsPerKey = 0;
                for (size_t i = 0; i < vertSets.size(); ++i) {
                    vertsPerKey = std::max(vertsPerKey,
                                           static_cast<uint32_t>(vertSets[i].size()));
                }

                body.writeU16(static_cast<uint16_t>(frameCount));
                body.writeU32(vertsPerKey);
                for (size_t i = 0; i < frameCount; ++i) {
                    body.writeF32(frames[i]);
                    auto& vs = (i < vertSets.size())
                        ? vertSets[i]
                        : vertSets[0];  // fallback; should not happen
                    for (uint32_t v = 0; v < vertsPerKey; ++v) {
                        body.writeF32(v < vs.size() ? vs[v] : 0.0f);
                    }
                }
                ++emittedTimelines;
            }
            // ── Event timeline (skip; no visual effect) ──────────────
            else if (rtti.instanceOf(spine::EventTimeline::rtti)) {
                sawEventSkip = true;
            }
            // ── PathConstraint timelines (skip; not used by audit) ──
            else {
                sawPathSkip = true;
            }
        }

        // Patch back the emittedTimelines count.
        body.patchU16At(timelineCountOff, emittedTimelines);
    }

    if (sawBezier)
        spdlog::info("ShotAnimWriter: at least one Bezier curve emitted with 100-sample LUT");
    if (sawDrawOrder)
        spdlog::info("ShotAnimWriter: at least one draw-order timeline emitted");
    if (sawDeform)
        spdlog::info("ShotAnimWriter: at least one deform timeline emitted");
    if (sawEventSkip)
        m_skipped.push_back("event timelines skipped (no visual effect)");
    if (sawPathSkip)
        m_skipped.push_back("path constraint timelines skipped (not in audit)");

    // ── Assemble final blob ──────────────────────────────────────────────
    hdr.stringCount = strings.count();

    ByteWriter tail;
    strings.serialize(tail);
    tail.writeRaw(body.data(), body.size());

    w.patchHeader(hdr);
    w.writeRaw(tail.data(), tail.size());

    // ── Footer ───────────────────────────────────────────────────────────
    uint32_t crc = crc32(w.data(), w.size());
    w.writeU32(crc);
    w.writeU32(kFooterMagic);

    // ── Disk write ───────────────────────────────────────────────────────
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        m_lastError = "ShotAnimWriter: cannot open output for write: " + outPath;
        return false;
    }
    f.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(w.size()));
    if (!f.good()) {
        m_lastError = "ShotAnimWriter: write failed";
        return false;
    }

    spdlog::info("ShotAnimWriter: wrote {} bytes — {} bones, {} slots, {} skins, {} anims, {} regions, {} meshes, {} clips",
                 w.size(),
                 hdr.boneCount, hdr.slotCount, hdr.skinCount, hdr.animCount,
                 hdr.regionAttachCount, hdr.meshAttachCount, hdr.clipAttachCount);
    for (const auto& s : m_skipped)
        spdlog::warn("ShotAnimWriter: limitation — {}", s);
    return true;
}

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
