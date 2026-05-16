/*
 * ShotAnimReader.cpp — deserialize the .shotanim binary format.
 *
 * The format is a strictly forward-sequential read — no seeking, no
 * back-references except by index. The reader allocates rt::SkeletonPkg
 * vectors up front (sizes are in the header) and fills them.
 *
 * Error handling: any structural anomaly (bad magic, truncated buffer,
 * size mismatch) returns false with a populated lastError(). The caller
 * is responsible for not using the partial SkeletonPkg.
 */

#include "spine/ShotAnimReader.h"
#include "spine/ShotAnimFormat.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace rt {

namespace {

// ─── ByteReader — bounds-checked sequential read over a buffer ──────────────

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) noexcept
        : m_data(data), m_size(size), m_pos(0), m_ok(true) {}

    [[nodiscard]] bool ok()    const noexcept { return m_ok; }
    [[nodiscard]] size_t pos() const noexcept { return m_pos; }
    [[nodiscard]] size_t size() const noexcept { return m_size; }
    [[nodiscard]] size_t remaining() const noexcept { return m_pos >= m_size ? 0 : m_size - m_pos; }

    void fail(const char* why) noexcept {
        if (m_ok) { m_ok = false; m_failReason = why; }
    }
    [[nodiscard]] const char* failReason() const noexcept { return m_failReason; }

    bool needBytes(size_t n) noexcept {
        if (!m_ok) return false;
        if (m_pos + n > m_size) { fail("truncated"); return false; }
        return true;
    }

    uint8_t  readU8()  noexcept { if (!needBytes(1)) return 0; return m_data[m_pos++]; }
    uint16_t readU16() noexcept { uint16_t v=0; readRaw(&v, 2); return v; }
    uint32_t readU32() noexcept { uint32_t v=0; readRaw(&v, 4); return v; }
    int16_t  readI16() noexcept { int16_t v=0;  readRaw(&v, 2); return v; }
    int32_t  readI32() noexcept { int32_t v=0;  readRaw(&v, 4); return v; }
    float    readF32() noexcept { float v=0;    readRaw(&v, 4); return v; }

    void readRaw(void* dst, size_t n) noexcept {
        if (!needBytes(n)) { std::memset(dst, 0, n); return; }
        std::memcpy(dst, m_data + m_pos, n);
        m_pos += n;
    }

    /// Read a length-prefixed UTF-8 string (u16 length + bytes).
    std::string readString() noexcept {
        uint16_t len = readU16();
        if (!m_ok) return {};
        if (!needBytes(len)) return {};
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return s;
    }

    /// Verify the next 4 bytes match `expected` magic; fail with `tag` if not.
    void expectMagic(uint32_t expected, const char* tag) noexcept {
        uint32_t got = readU32();
        if (!m_ok) return;
        if (got != expected) {
            m_ok = false;
            m_failReason = tag;
        }
    }

private:
    const uint8_t* m_data;
    size_t         m_size;
    size_t         m_pos;
    bool           m_ok;
    const char*    m_failReason = "";
};

// ─── Helpers ────────────────────────────────────────────────────────────────

template <typename V>
void readKeyTimelineHeader(ByteReader& r, rt::KeyTimeline<V>& tl, uint16_t& outCount) {
    outCount = r.readU16();
    tl.times.resize(outCount);
    tl.values.resize(outCount);
    tl.curves.clear();
    tl.curves.resize(outCount > 0 ? outCount - 1 : 0);
}

void readCurves(ByteReader& r, std::vector<rt::KeyCurve>& curves) {
    for (auto& c : curves) {
        uint8_t style = r.readU8();
        c.style = static_cast<rt::CurveStyle>(style);
        if (c.style == rt::CurveStyle::Bezier) {
            c.cx1 = r.readF32();
            c.cy1 = r.readF32();
            c.cx2 = r.readF32();
            c.cy2 = r.readF32();
            // LUT is precomputed at write time and serialized inline.
            for (int i = 0; i < 100; ++i) c.lut.samples[i] = r.readF32();
        }
    }
}

void readFloatTimeline(ByteReader& r, rt::KeyTimeline<float>& tl) {
    uint16_t count = 0;
    readKeyTimelineHeader(r, tl, count);
    for (uint16_t i = 0; i < count; ++i) tl.times[i] = r.readF32();
    for (uint16_t i = 0; i < count; ++i) tl.values[i] = r.readF32();
    readCurves(r, tl.curves);
}

void readVec2Timeline(ByteReader& r, rt::KeyTimeline<rt::Vec2>& tl) {
    uint16_t count = 0;
    readKeyTimelineHeader(r, tl, count);
    for (uint16_t i = 0; i < count; ++i) tl.times[i] = r.readF32();
    for (uint16_t i = 0; i < count; ++i) {
        tl.values[i].x = r.readF32();
        tl.values[i].y = r.readF32();
    }
    readCurves(r, tl.curves);
}

void readI32Timeline(ByteReader& r, rt::KeyTimeline<int32_t>& tl) {
    uint16_t count = 0;
    readKeyTimelineHeader(r, tl, count);
    for (uint16_t i = 0; i < count; ++i) tl.times[i] = r.readF32();
    for (uint16_t i = 0; i < count; ++i) tl.values[i] = r.readI32();
    // Attachment timelines are always stepped (no interpolation between names).
    // Curves intentionally not serialized — leave default Linear/Stepped.
    for (auto& c : tl.curves) c.style = rt::CurveStyle::Stepped;
}

} // namespace

// ─── ShotAnimReader ─────────────────────────────────────────────────────────

ShotAnimReader::ShotAnimReader()  = default;
ShotAnimReader::~ShotAnimReader() = default;

bool ShotAnimReader::readFile(const std::string& path, rt::SkeletonPkg& out) {
    m_lastError.clear();
    m_lastVersion = 0;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        std::ostringstream os;
        os << "ShotAnimReader: cannot open '" << path << "'";
        m_lastError = os.str();
        return false;
    }

    std::streamsize size = f.tellg();
    if (size <= static_cast<std::streamsize>(sizeof(shotanim::Header))) {
        m_lastError = "ShotAnimReader: file too small";
        return false;
    }
    f.seekg(0);

    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
        m_lastError = "ShotAnimReader: file read failed";
        return false;
    }

    return readBuffer(buf.data(), buf.size(), out);
}

bool ShotAnimReader::readBuffer(const uint8_t* data, size_t size, rt::SkeletonPkg& out) {
    using namespace shotanim;

    m_lastError.clear();
    m_lastVersion = 0;

    if (size < sizeof(Header)) {
        m_lastError = "buffer smaller than header";
        return false;
    }

    ByteReader r(data, size);

    // ── Header ───────────────────────────────────────────────────────────
    Header hdr{};
    r.readRaw(&hdr, sizeof(Header));
    if (!r.ok()) {
        m_lastError = "header read failed";
        return false;
    }
    if (hdr.magic != kHeaderMagic) {
        m_lastError = "bad magic (not a .shotanim file)";
        return false;
    }
    if (hdr.version != kCurrentVersion) {
        std::ostringstream os;
        os << "unsupported version " << hdr.version
           << " (expected " << kCurrentVersion << ")";
        m_lastError = os.str();
        return false;
    }
    m_lastVersion = hdr.version;

    out = rt::SkeletonPkg{};
    out.width  = hdr.canvasWidth;
    out.height = hdr.canvasHeight;

    // ── String table ─────────────────────────────────────────────────────
    r.expectMagic(kMagicStr, "missing string table magic");
    out.strings.reserve(hdr.stringCount);
    for (uint16_t i = 0; i < hdr.stringCount; ++i) {
        out.strings.push_back(r.readString());
    }

    auto getStr = [&](uint16_t idx) -> std::string {
        if (idx == kNoStringRef) return {};
        if (idx >= out.strings.size()) return {};
        return out.strings[idx];
    };

    // ── Texture pages ────────────────────────────────────────────────────
    r.expectMagic(kMagicPage, "missing page magic");
    out.pages.resize(hdr.pageCount);
    for (auto& p : out.pages) {
        uint16_t pathIdx = r.readU16();
        p.name   = getStr(pathIdx);
        p.width  = r.readU16();
        p.height = r.readU16();
        (void)r.readU8();  // pma flag — not yet wired to AtlasPage; reserved
        (void)r.readU8();  // padding byte
    }

    // ── Bones ────────────────────────────────────────────────────────────
    r.expectMagic(kMagicBone, "missing bone magic");
    out.bones.resize(hdr.boneCount);
    for (auto& b : out.bones) {
        (void)r.readU16();              // nameIdx — debug only, not retained
        int16_t parent = r.readI16();
        b.parentIndex = (parent < 0) ? -1 : parent;
        b.x        = r.readF32();
        b.y        = r.readF32();
        b.rotation = r.readF32();
        b.scaleX   = r.readF32();
        b.scaleY   = r.readF32();
        b.shearX   = r.readF32();
        b.shearY   = r.readF32();
        b.length   = r.readF32();
        b.inherit  = static_cast<rt::BoneInherit>(r.readU8());
        b.colorR   = r.readF32();
        b.colorG   = r.readF32();
        b.colorB   = r.readF32();
        b.colorA   = r.readF32();
    }

    // ── Slots ────────────────────────────────────────────────────────────
    r.expectMagic(kMagicSlot, "missing slot magic");
    out.slots.resize(hdr.slotCount);
    for (auto& s : out.slots) {
        (void)r.readU16();             // slotNameIdx
        s.boneIndex = static_cast<int32_t>(r.readU16());
        s.color     = r.readU32();
        s.darkColor = r.readU32();
        s.blend     = static_cast<rt::SlotBlend>(r.readU8());
        int32_t defAttach = r.readI32();
        s.defaultAttachmentIdx = defAttach;
    }

    // ── Region attachments ───────────────────────────────────────────────
    r.expectMagic(kMagicRgn, "missing region magic");
    const size_t firstRegionIdx = out.attachments.size();
    out.attachments.reserve(firstRegionIdx
                            + hdr.regionAttachCount
                            + hdr.meshAttachCount
                            + hdr.clipAttachCount);
    for (uint16_t i = 0; i < hdr.regionAttachCount; ++i) {
        rt::AttachmentData a{};
        a.kind = rt::AttachKind::Region;
        const uint16_t nameIdx = r.readU16();
        a.attachNameIdx = (nameIdx == shotanim::kNoStringRef)
            ? -1 : static_cast<int32_t>(nameIdx);
        a.regionIdx = static_cast<int32_t>(r.readI32());
        a.originX   = r.readF32();
        a.originY   = r.readF32();
        a.width     = r.readF32();
        a.height    = r.readF32();
        for (int v = 0; v < 8; ++v) a.localVerts[v] = r.readF32();
        for (int v = 0; v < 8; ++v) a.localUVs[v]   = r.readF32();
        a.colorR    = r.readF32();
        a.colorG    = r.readF32();
        a.colorB    = r.readF32();
        a.colorA    = r.readF32();
        out.attachments.push_back(std::move(a));
    }

    // ── Mesh attachments ─────────────────────────────────────────────────
    r.expectMagic(kMagicMsh, "missing mesh magic");
    for (uint16_t i = 0; i < hdr.meshAttachCount; ++i) {
        rt::AttachmentData a{};
        a.kind = rt::AttachKind::Mesh;
        const uint16_t nameIdx = r.readU16();
        a.attachNameIdx = (nameIdx == shotanim::kNoStringRef)
            ? -1 : static_cast<int32_t>(nameIdx);
        a.regionIdx = static_cast<int32_t>(r.readI32());
        uint32_t vCount = r.readU32();
        uint32_t tCount = r.readU32();
        uint32_t hullLen = r.readU32();
        uint32_t edgeCount = r.readU32();
        uint32_t boneCount = r.readU32();
        uint32_t weightCount = r.readU32();

        a.meshVertOffset = static_cast<uint32_t>(out.meshVertices.size());
        a.meshVertCount  = vCount;
        out.meshVertices.resize(out.meshVertices.size() + vCount * 2);
        for (uint32_t v = 0; v < vCount * 2; ++v) {
            out.meshVertices[a.meshVertOffset + v] = r.readF32();
        }

        // UVs share the meshVertices pool — laid out as the next
        // vCount*2 floats right after positions. meshUvOffset records
        // where each attachment's UVs begin so the evaluator can fetch
        // them with a simple index lookup.
        a.meshUvOffset = static_cast<uint32_t>(out.meshVertices.size());
        out.meshVertices.resize(out.meshVertices.size() + vCount * 2);
        for (uint32_t v = 0; v < vCount * 2; ++v) {
            out.meshVertices[a.meshUvOffset + v] = r.readF32();
        }

        a.meshTriOffset = static_cast<uint32_t>(out.meshTriangles.size());
        a.meshTriCount  = tCount;
        out.meshTriangles.resize(out.meshTriangles.size() + tCount);
        for (uint32_t t = 0; t < tCount; ++t) {
            out.meshTriangles[a.meshTriOffset + t] = r.readU16();
        }

        // Edges (mesh hull edges, used for FFD seam handling) — read & skip
        for (uint32_t e = 0; e < edgeCount; ++e) (void)r.readU16();
        a.meshEdgeCount = edgeCount;

        a.meshHullCount = hullLen;
        // Hull is a subset of vertex indices — read & skip; reader does
        // not yet expose it as a separate pool.
        for (uint32_t h = 0; h < hullLen; ++h) (void)r.readU16();

        // Mesh skinning: bone indices + weights pool.
        // Layout per SkeletonTypes.h AttachmentData comments:
        //   meshBones   = [N_v0, b_v0_0, ..., N_v1, b_v1_0, ...]
        //   meshWeights = [x, y, weight, x, y, weight, ...] per influence
        a.meshBoneOffset   = static_cast<uint32_t>(out.meshBones.size());
        a.meshBoneCount    = boneCount;
        for (uint32_t b = 0; b < boneCount; ++b) {
            out.meshBones.push_back(r.readI32());
        }
        a.meshWeightOffset = static_cast<uint32_t>(out.meshWeights.size());
        a.meshWeightCount  = weightCount;
        for (uint32_t w = 0; w < weightCount; ++w) {
            out.meshWeights.push_back(r.readF32());
        }

        a.originX = r.readF32();
        a.originY = r.readF32();
        a.width   = r.readF32();
        a.height  = r.readF32();
        a.colorR  = r.readF32();
        a.colorG  = r.readF32();
        a.colorB  = r.readF32();
        a.colorA  = r.readF32();

        // LinkedMesh? — a parentMeshIdx of -1 means standalone mesh.
        int32_t parentMesh = r.readI32();
        if (parentMesh >= 0) {
            a.kind = rt::AttachKind::LinkedMesh;
            a.parentMeshIdx = parentMesh;
        }

        out.attachments.push_back(std::move(a));
    }

    // ── Clipping attachments ─────────────────────────────────────────────
    r.expectMagic(kMagicClp, "missing clip magic");
    for (uint16_t i = 0; i < hdr.clipAttachCount; ++i) {
        rt::AttachmentData a{};
        a.kind = rt::AttachKind::Clipping;
        const uint16_t nameIdx = r.readU16();
        a.attachNameIdx = (nameIdx == shotanim::kNoStringRef)
            ? -1 : static_cast<int32_t>(nameIdx);
        uint16_t endSlot = r.readU16();
        a.clipEndSlotIdx = endSlot;
        uint32_t vCount = r.readU32();
        a.meshVertOffset = static_cast<uint32_t>(out.meshVertices.size());
        a.meshVertCount  = vCount;
        out.meshVertices.resize(out.meshVertices.size() + vCount * 2);
        for (uint32_t v = 0; v < vCount * 2; ++v) {
            out.meshVertices[a.meshVertOffset + v] = r.readF32();
        }
        out.attachments.push_back(std::move(a));
    }

    // ── Skin table ───────────────────────────────────────────────────────
    r.expectMagic(kMagicSkin, "missing skin magic");
    out.skins.resize(hdr.skinCount);
    for (auto& s : out.skins) {
        uint16_t nameIdx = r.readU16();
        s.name = getStr(nameIdx);
        uint16_t entryCount = r.readU16();
        s.slotIndices.resize(entryCount);
        s.attachmentIndices.resize(entryCount);
        for (uint16_t e = 0; e < entryCount; ++e) s.slotIndices[e]       = r.readI32();
        for (uint16_t e = 0; e < entryCount; ++e) s.attachmentIndices[e] = r.readI32();
    }

    // ── IK constraints ───────────────────────────────────────────────────
    r.expectMagic(kMagicIK, "missing IK magic");
    out.ikConstraints.resize(hdr.ikCount);
    for (auto& ik : out.ikConstraints) {
        ik.targetBoneIdx = r.readI32();
        ik.bendBoneIdx   = r.readI32();
        ik.bendDirection = r.readU8();
        ik.mix           = r.readF32();
        uint16_t boneCt = r.readU16();
        ik.constrainedBones.resize(boneCt);
        for (auto& b : ik.constrainedBones) b = r.readI32();
    }

    // ── Transform constraints ────────────────────────────────────────────
    r.expectMagic(kMagicTfm, "missing transform magic");
    out.transformConstraints.resize(hdr.transformConstraintCount);
    for (auto& tc : out.transformConstraints) {
        tc.targetBoneIdx   = r.readI32();
        // v5 format: per-axis mixes, constant offsets, mode flags.
        tc.mixRotate       = r.readF32();
        tc.mixX            = r.readF32();
        tc.mixY            = r.readF32();
        tc.mixScaleX       = r.readF32();
        tc.mixScaleY       = r.readF32();
        tc.mixShearY       = r.readF32();
        tc.offsetRotation  = r.readF32();
        tc.offsetX         = r.readF32();
        tc.offsetY         = r.readF32();
        tc.offsetScaleX    = r.readF32();
        tc.offsetScaleY    = r.readF32();
        tc.offsetShearY    = r.readF32();
        tc.local           = r.readU8();
        tc.relative        = r.readU8();
        uint16_t boneCt = r.readU16();
        tc.constrainedBones.resize(boneCt);
        for (auto& b : tc.constrainedBones) b = r.readI32();
    }

    // ── Animations ───────────────────────────────────────────────────────
    r.expectMagic(kMagicAnim, "missing anim magic");
    out.animations.resize(hdr.animCount);
    for (auto& clip : out.animations) {
        uint16_t nameIdx = r.readU16();
        clip.name = getStr(nameIdx);
        clip.duration = r.readF32();

        // Per-anim bone/slot tracks are stored sparsely — only animated
        // bones/slots are emitted, indexed by their target index.
        clip.boneTracks.assign(hdr.boneCount, rt::BoneTrackSet{});
        clip.slotTracks.assign(hdr.slotCount, rt::SlotTrackSet{});

        uint16_t timelineCount = r.readU16();
        for (uint16_t t = 0; t < timelineCount; ++t) {
            uint8_t type8 = r.readU8();
            auto type = static_cast<shotanim::TimelineType>(type8);
            uint16_t targetIdx = r.readU16();   // bone/slot/constraint idx

            switch (type) {
                case shotanim::TimelineType::BoneRotate:
                    readFloatTimeline(r, clip.boneTracks[targetIdx].rotate);
                    break;
                case shotanim::TimelineType::BoneTranslate:
                    readVec2Timeline(r, clip.boneTracks[targetIdx].translate);
                    break;
                case shotanim::TimelineType::BoneTranslateX:
                case shotanim::TimelineType::BoneTranslateY: {
                    // single-axis timelines decoded into the Vec2 timeline,
                    // with the other axis preserved via stepped sampling.
                    rt::KeyTimeline<float> tmp;
                    readFloatTimeline(r, tmp);
                    auto& tl = clip.boneTracks[targetIdx].translate;
                    if (tl.times.empty()) {
                        tl.times = tmp.times;
                        tl.values.assign(tmp.times.size(), rt::Vec2{});
                        tl.curves = tmp.curves;
                    }
                    for (size_t i = 0; i < tmp.times.size() && i < tl.values.size(); ++i) {
                        if (type == shotanim::TimelineType::BoneTranslateX)
                            tl.values[i].x = tmp.values[i];
                        else
                            tl.values[i].y = tmp.values[i];
                    }
                    break;
                }
                case shotanim::TimelineType::BoneScale:
                    readVec2Timeline(r, clip.boneTracks[targetIdx].scale);
                    break;
                case shotanim::TimelineType::BoneScaleX:
                case shotanim::TimelineType::BoneScaleY: {
                    rt::KeyTimeline<float> tmp;
                    readFloatTimeline(r, tmp);
                    auto& tl = clip.boneTracks[targetIdx].scale;
                    if (tl.times.empty()) {
                        tl.times = tmp.times;
                        tl.values.assign(tmp.times.size(), rt::Vec2{1.0f, 1.0f});
                        tl.curves = tmp.curves;
                    }
                    for (size_t i = 0; i < tmp.times.size() && i < tl.values.size(); ++i) {
                        if (type == shotanim::TimelineType::BoneScaleX)
                            tl.values[i].x = tmp.values[i];
                        else
                            tl.values[i].y = tmp.values[i];
                    }
                    break;
                }
                case shotanim::TimelineType::BoneShear:
                    readVec2Timeline(r, clip.boneTracks[targetIdx].shear);
                    break;
                case shotanim::TimelineType::BoneShearX:
                case shotanim::TimelineType::BoneShearY: {
                    rt::KeyTimeline<float> tmp;
                    readFloatTimeline(r, tmp);
                    auto& tl = clip.boneTracks[targetIdx].shear;
                    if (tl.times.empty()) {
                        tl.times = tmp.times;
                        tl.values.assign(tmp.times.size(), rt::Vec2{});
                        tl.curves = tmp.curves;
                    }
                    for (size_t i = 0; i < tmp.times.size() && i < tl.values.size(); ++i) {
                        if (type == shotanim::TimelineType::BoneShearX)
                            tl.values[i].x = tmp.values[i];
                        else
                            tl.values[i].y = tmp.values[i];
                    }
                    break;
                }
                case shotanim::TimelineType::SlotAttachment:
                    readI32Timeline(r, clip.slotTracks[targetIdx].attachment);
                    break;
                case shotanim::TimelineType::SlotRGBA: {
                    // 4 channels emitted as 4 parallel float timelines
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorR);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorG);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorB);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorA);
                    break;
                }
                case shotanim::TimelineType::SlotRGB: {
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorR);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorG);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorB);
                    break;
                }
                case shotanim::TimelineType::SlotAlpha:
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorA);
                    break;
                case shotanim::TimelineType::SlotRGBA2: {
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorR);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorG);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorB);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].darkR);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].darkG);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].darkB);
                    break;
                }
                case shotanim::TimelineType::SlotRGB2: {
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorR);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorG);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].colorB);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].darkR);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].darkG);
                    readFloatTimeline(r, clip.slotTracks[targetIdx].darkB);
                    break;
                }
                case shotanim::TimelineType::DrawOrder: {
                    uint16_t keyCount = r.readU16();
                    clip.drawOrder.keys.resize(keyCount);
                    for (auto& key : clip.drawOrder.keys) {
                        key.time = r.readF32();
                        key.slotOrder.resize(hdr.slotCount);
                        for (auto& s : key.slotOrder) s = r.readI32();
                    }
                    break;
                }
                case shotanim::TimelineType::IkConstraint: {
                    if (clip.ikTracks.size() <= targetIdx)
                        clip.ikTracks.resize(targetIdx + 1);
                    auto& trk = clip.ikTracks[targetIdx];
                    uint16_t kc = r.readU16();
                    trk.keys.resize(kc);
                    for (auto& k : trk.keys) {
                        k.time = r.readF32();
                        k.mix  = r.readF32();
                        k.bendPositive = r.readF32();
                    }
                    break;
                }
                case shotanim::TimelineType::TransformConstr: {
                    if (clip.transformTracks.size() <= targetIdx)
                        clip.transformTracks.resize(targetIdx + 1);
                    auto& trk = clip.transformTracks[targetIdx];
                    uint16_t kc = r.readU16();
                    trk.keys.resize(kc);
                    for (auto& k : trk.keys) {
                        k.time      = r.readF32();
                        k.mixRotate = r.readF32();
                        k.mixX      = r.readF32();
                        k.mixY      = r.readF32();
                        k.mixScaleX = r.readF32();
                        k.mixScaleY = r.readF32();
                        k.mixShearY = r.readF32();
                    }
                    break;
                }
                case shotanim::TimelineType::SlotDeform: {
                    // Deform timeline: per-attachment vertex offsets.
                    // targetIdx here is the attachment index.
                    if (clip.deformTracks.size() <= targetIdx)
                        clip.deformTracks.resize(targetIdx + 1);
                    auto& trk = clip.deformTracks[targetIdx];
                    uint16_t kc = r.readU16();
                    uint32_t vertsPerKey = r.readU32();
                    trk.keys.resize(kc);
                    for (auto& k : trk.keys) {
                        k.time = r.readF32();
                        k.verts.resize(vertsPerKey);
                        for (auto& f : k.verts) f = r.readF32();
                    }
                    break;
                }
                case shotanim::TimelineType::Event:
                default:
                    // Skip unknown / unsupported timeline payloads.
                    // Writer emits a u32 byte-length immediately after the
                    // header (targetIdx) so the reader can jump past.
                    {
                        uint32_t skip = r.readU32();
                        for (uint32_t k = 0; k < skip; ++k) (void)r.readU8();
                    }
                    break;
            }
        }
    }

    // ── Footer (CRC + magic) ─────────────────────────────────────────────
    // CRC is informational — we do not currently validate to keep the
    // reader fast; a corrupted file generally fails on a section magic
    // check before reaching here. Verify the trailing magic so we know
    // we consumed the whole payload.
    (void)r.readU32();  // crc32 (informational)
    uint32_t endMagic = r.readU32();

    if (!r.ok()) {
        std::ostringstream os;
        os << "ShotAnimReader: " << r.failReason()
           << " (pos=" << r.pos() << "/" << r.size() << ")";
        m_lastError = os.str();
        return false;
    }

    if (endMagic != kFooterMagic) {
        m_lastError = "ShotAnimReader: missing footer magic";
        return false;
    }

    spdlog::info("ShotAnimReader: loaded — {} bones, {} slots, {} skins, {} anims, {} attachments",
                 out.bones.size(),
                 out.slots.size(),
                 out.skins.size(),
                 out.animations.size(),
                 out.attachments.size());

    return true;
}

} // namespace rt
