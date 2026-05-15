/**
 * spine_feature_audit — Phase 1 diagnostic: catalog every Spine feature
 * actually used by character .skel files.
 *
 * This tool uses spine-cpp ONLY for audit/diagnostic purposes during
 * the PARSER_PLAN migration. It will be deleted after Phase 1.
 *
 * Usage: spine_feature_audit.exe
 * (run from workspace root so "assets/characters/" is reachable)
 */

#include <spine/spine.h>
#include <spine/Atlas.h>
#include <spine/AtlasAttachmentLoader.h>
#include <spine/SkeletonBinary.h>
#include <spine/SkeletonData.h>
#include <spine/Skeleton.h>
#include <spine/Animation.h>
#include <spine/Timeline.h>
#include <spine/CurveTimeline.h>
#include <spine/AttachmentTimeline.h>
#include <spine/DrawOrderTimeline.h>
#include <spine/EventTimeline.h>
#include <spine/IkConstraintTimeline.h>
#include <spine/TransformConstraintTimeline.h>
#include <spine/PathConstraintPositionTimeline.h>
#include <spine/PathConstraintMixTimeline.h>
#include <spine/RegionAttachment.h>
#include <spine/MeshAttachment.h>
#include <spine/ClippingAttachment.h>
#include <spine/BoundingBoxAttachment.h>
#include <spine/PathAttachment.h>
#include <spine/PointAttachment.h>
#include <spine/BoneData.h>
#include <spine/TransformMode.h>
#include <spine/SlotData.h>
#include <spine/Skin.h>
#include <spine/IkConstraintData.h>
#include <spine/TransformConstraintData.h>
#include <spine/PathConstraintData.h>
#include <spine/RTTI.h>

#include <filesystem>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <cstring>

namespace fs = std::filesystem;

// ─── Helper: count curves by type in a CurveTimeline ────────────────────────
struct CurveCounts {
    int linear  = 0;
    int stepped = 0;
    int bezier  = 0;
};

CurveCounts countCurves(spine::Timeline* tl) {
    CurveCounts c;
    auto* ct = dynamic_cast<spine::CurveTimeline*>(tl);
    if (!ct) return c;

    auto& curves = ct->getCurves();
    size_t frameCount = curves.size() > 0 ? curves.size() / 19 : 0;  // bezier = 19 floats
    // Actually, frameCount for curve purposes is better determined from the timeline itself.
    // The curves array: each frame has an entry. LINEAR=0, STEPPED=1, BEZIER=2 (as first float,
    // then 18 more floats for bezier control points).
    // Actually, the spine-cpp CurveTimeline stores curves as a flat float array:
    //   curves[frameIndex * BEZIER_SIZE]  = type (LINEAR, STEPPED, BEZIER)
    //   curves[frameIndex * BEZIER_SIZE + 1..18] = bezier data
    static const int BEZIER_SIZE = 19;
    // We need to know frameCount. The timeline stores frameCount in the base class.
    // But it's protected. Let's just iterate the curves array.
    size_t count = curves.size() / BEZIER_SIZE;
    for (size_t i = 0; i < count; ++i) {
        float type = curves[i * BEZIER_SIZE];
        if (type == 0.0f)      c.linear++;
        else if (type == 1.0f) c.stepped++;
        else                   c.bezier++;
    }
    return c;
}

// ─── Helper: check if a timeline is a specific type ─────────────────────────
bool isTimelineType(spine::Timeline* tl, const char* typeName) {
    return tl->getRTTI().instanceOf(spine::RTTI(typeName));
}

enum class TimelineKind {
    BoneRotate, BoneTranslate, BoneScale, BoneShear,
    SlotAttachment, SlotColor, SlotTwoColor,
    DrawOrder, Event,
    IkConstraint, TransformConstraint,
    PathConstraintMix, PathConstraintPosition,
    Deform,
    Unknown
};

TimelineKind classifyTimeline(spine::Timeline* tl) {
    const char* cn = tl->getRTTI().getClassName();
    std::string name(cn);

    if (name == "RotateTimeline")           return TimelineKind::BoneRotate;
    if (name == "TranslateTimeline")        return TimelineKind::BoneTranslate;
    if (name == "ScaleTimeline")            return TimelineKind::BoneScale;
    if (name == "ShearTimeline")            return TimelineKind::BoneShear;
    if (name == "AttachmentTimeline")       return TimelineKind::SlotAttachment;
    if (name == "RGBATimeline" ||
        name == "RGBTimeline" ||
        name == "AlphaTimeline")            return TimelineKind::SlotColor;
    if (name == "RGBA2Timeline" ||
        name == "RGB2Timeline")             return TimelineKind::SlotTwoColor;
    if (name == "DrawOrderTimeline")        return TimelineKind::DrawOrder;
    if (name == "EventTimeline")            return TimelineKind::Event;
    if (name == "IkConstraintTimeline")     return TimelineKind::IkConstraint;
    if (name == "TransformConstraintTimeline") return TimelineKind::TransformConstraint;
    if (name == "PathConstraintMixTimeline")   return TimelineKind::PathConstraintMix;
    if (name == "PathConstraintPositionTimeline") return TimelineKind::PathConstraintPosition;
    if (name == "DeformTimeline")           return TimelineKind::Deform;
    return TimelineKind::Unknown;
}

const char* timelineKindName(TimelineKind k) {
    switch (k) {
        case TimelineKind::BoneRotate:       return "bone-rotate";
        case TimelineKind::BoneTranslate:    return "bone-translate";
        case TimelineKind::BoneScale:        return "bone-scale";
        case TimelineKind::BoneShear:        return "bone-shear";
        case TimelineKind::SlotAttachment:   return "slot-attachment";
        case TimelineKind::SlotColor:        return "slot-color";
        case TimelineKind::SlotTwoColor:     return "slot-two-color";
        case TimelineKind::DrawOrder:        return "draw-order";
        case TimelineKind::Event:            return "event";
        case TimelineKind::IkConstraint:     return "ik-constraint";
        case TimelineKind::TransformConstraint: return "transform-constraint";
        case TimelineKind::PathConstraintMix:   return "path-constraint-mix";
        case TimelineKind::PathConstraintPosition: return "path-constraint-position";
        case TimelineKind::Deform:           return "deform";
        default:                             return "unknown";
    }
}

// ─── Attachment type name ───────────────────────────────────────────────────
const char* attachmentTypeName(spine::Attachment* a) {
    const char* cn = a->getRTTI().getClassName();
    return cn;
}

// ─── Bone inherit mode names ────────────────────────────────────────────────
const char* inheritModeName(spine::TransformMode mode) {
    switch (mode) {
        case spine::TransformMode_Normal:              return "Normal";
        case spine::TransformMode_OnlyTranslation:     return "OnlyTranslation";
        case spine::TransformMode_NoRotationOrReflection: return "NoRotationOrReflection";
        case spine::TransformMode_NoScale:             return "NoScale";
        case spine::TransformMode_NoScaleOrReflection: return "NoScaleOrReflection";
        default:                                       return "?";
    }
}

// ─── Main audit logic ───────────────────────────────────────────────────────
struct PerAnimStats {
    std::string name;
    float duration = 0;
    std::map<TimelineKind, int> timelineKindCounts;
    CurveCounts curveCounts;
    bool hasDrawOrder = false;
    bool hasEvents    = false;
    bool hasDeform    = false;
    int keyframeCount = 0;
};

struct CharacterAudit {
    std::string name;
    std::string skelPath;
    std::string version;
    int boneCount = 0;
    int slotCount = 0;
    int animCount = 0;
    int skinCount = 0;
    int atlasPageCount = 0;
    int atlasRegionCount = 0;

    // Attachment type counts (across all skins)
    int regionAttachments    = 0;
    int meshAttachments      = 0;
    int linkedMeshAttachments = 0;
    int clippingAttachments  = 0;
    int boundingBoxAttachments = 0;
    int pathAttachments      = 0;
    int pointAttachments     = 0;

    // Mesh aggregate stats
    int maxMeshVertices   = 0;
    int maxMeshTriangles  = 0;
    int totalMeshVertices = 0;

    // Constraint counts
    int ikConstraints          = 0;
    int transformConstraints   = 0;
    int pathConstraints        = 0;

    // Bone inheritance modes
    std::map<int, int> inheritModes;  // TransformMode enum as int key

    // Bones with non-white color
    int coloredBones = 0;

    // Events
    int eventCount = 0;

    // Slots with non-default blend modes
    int additiveSlots = 0;
    int multiplySlots = 0;
    int screenSlots   = 0;

    // Skins beyond default
    std::vector<std::string> extraSkinNames;

    // Per-animation stats
    std::vector<PerAnimStats> animations;

    // Aggregate across all animations
    bool anyBezier     = false;
    bool anyDrawOrder  = false;
    bool anyEvents     = false;
    bool anyDeform     = false;
    bool anyTwoColor   = false;
    bool anyIkTimeline = false;
    bool anyTransformTimeline = false;
    bool anyPathTimeline = false;
};

void auditCharacter(const std::string& charName,
                    const std::string& skelPath,
                    const std::string& atlasPath,
                    CharacterAudit& out)
{
    out.name = charName;
    out.skelPath = skelPath;

    // Load atlas
    auto* atlas = new spine::Atlas(
        spine::String(atlasPath.c_str()),
        nullptr  // no custom texture loader
    );
    if (!atlas || atlas->getPages().size() == 0) {
        std::cerr << "  WARNING: Failed to load atlas: " << atlasPath << "\n";
        delete atlas;
        return;
    }
    out.atlasPageCount = static_cast<int>(atlas->getPages().size());
    out.atlasRegionCount = static_cast<int>(atlas->getRegions().size());

    // Load skeleton binary
    spine::AtlasAttachmentLoader attachmentLoader(atlas);
    spine::SkeletonBinary binary(&attachmentLoader);
    binary.setScale(1.0f);

    auto* skelData = binary.readSkeletonDataFile(spine::String(skelPath.c_str()));
    if (!skelData) {
        std::cerr << "  WARNING: Failed to load skeleton: " << skelPath
                  << " — " << binary.getError().buffer() << "\n";
        delete atlas;
        return;
    }

    out.version    = skelData->getVersion().buffer();
    out.boneCount  = static_cast<int>(skelData->getBones().size());
    out.slotCount  = static_cast<int>(skelData->getSlots().size());
    out.animCount  = static_cast<int>(skelData->getAnimations().size());
    out.skinCount  = static_cast<int>(skelData->getSkins().size());
    out.eventCount = static_cast<int>(skelData->getEvents().size());

    // ── Bone inheritance modes ──────────────────────────────────────────
    for (size_t i = 0; i < skelData->getBones().size(); ++i) {
        auto* bone = skelData->getBones()[i];
        out.inheritModes[static_cast<int>(bone->getTransformMode())]++;

        // Check for non-white bone color
        float r = bone->getColor().r;
        float g = bone->getColor().g;
        float b = bone->getColor().b;
        float a = bone->getColor().a;
        if (r != 1.0f || g != 1.0f || b != 1.0f || a != 1.0f) {
            out.coloredBones++;
        }
    }

    // ── Slot blend modes ────────────────────────────────────────────────
    for (size_t i = 0; i < skelData->getSlots().size(); ++i) {
        auto* slot = skelData->getSlots()[i];
        switch (slot->getBlendMode()) {
            case spine::BlendMode_Additive: out.additiveSlots++; break;
            case spine::BlendMode_Multiply: out.multiplySlots++; break;
            case spine::BlendMode_Screen:   out.screenSlots++;   break;
            default: break;
        }
    }

    // ── Attachments (across ALL skins) ──────────────────────────────────
    auto countAttachmentsInSkin = [&](spine::Skin* skin) {
        if (!skin) return;
        auto entries = skin->getAttachments();
        while (entries.hasNext()) {
            auto& entry = entries.next();
            if (!entry._attachment) continue;
            const char* cn = entry._attachment->getRTTI().getClassName();
            std::string typeName(cn);

            if (typeName == "RegionAttachment")       out.regionAttachments++;
            else if (typeName == "MeshAttachment")    out.meshAttachments++;
            else if (typeName == "LinkedMeshAttachment") out.linkedMeshAttachments++;
            else if (typeName == "ClippingAttachment") out.clippingAttachments++;
            else if (typeName == "BoundingBoxAttachment") out.boundingBoxAttachments++;
            else if (typeName == "PathAttachment")     out.pathAttachments++;
            else if (typeName == "PointAttachment")    out.pointAttachments++;

            // Mesh stats
            if (typeName == "MeshAttachment") {
                auto* mesh = static_cast<spine::MeshAttachment*>(entry._attachment);
                int vertCount = static_cast<int>(mesh->getWorldVerticesLength() / 2);
                int triCount  = static_cast<int>(mesh->getTriangles().size());
                if (vertCount > out.maxMeshVertices)  out.maxMeshVertices = vertCount;
                if (triCount  > out.maxMeshTriangles) out.maxMeshTriangles = triCount;
                out.totalMeshVertices += vertCount;
            }
        }
    };

    // Default skin
    countAttachmentsInSkin(skelData->getDefaultSkin());

    // Extra skins
    for (size_t i = 0; i < skelData->getSkins().size(); ++i) {
        auto* skin = skelData->getSkins()[i];
        if (skin == skelData->getDefaultSkin()) continue;
        out.extraSkinNames.push_back(skin->getName().buffer());
        countAttachmentsInSkin(skin);
    }

    // ── Constraints ─────────────────────────────────────────────────────
    out.ikConstraints        = static_cast<int>(skelData->getIkConstraints().size());
    out.transformConstraints = static_cast<int>(skelData->getTransformConstraints().size());
    out.pathConstraints      = static_cast<int>(skelData->getPathConstraints().size());

    // ── Animations ──────────────────────────────────────────────────────
    for (size_t ai = 0; ai < skelData->getAnimations().size(); ++ai) {
        auto* anim = skelData->getAnimations()[ai];
        PerAnimStats pas;
        pas.name     = anim->getName().buffer();
        pas.duration = anim->getDuration();

        auto& timelines = anim->getTimelines();
        for (size_t ti = 0; ti < timelines.size(); ++ti) {
            auto* tl = timelines[ti];
            TimelineKind kind = classifyTimeline(tl);
            pas.timelineKindCounts[kind]++;

            // Count curves
            CurveCounts cc = countCurves(tl);
            pas.curveCounts.linear  += cc.linear;
            pas.curveCounts.stepped += cc.stepped;
            pas.curveCounts.bezier  += cc.bezier;

            if (kind == TimelineKind::DrawOrder) pas.hasDrawOrder = true;
            if (kind == TimelineKind::Event)     pas.hasEvents    = true;
            if (kind == TimelineKind::Deform)    pas.hasDeform    = true;
        }

        // Aggregate keyframe count from frameCount accessible via RTTI
        pas.keyframeCount = static_cast<int>(timelines.size());

        out.animations.push_back(pas);

        // Aggregate flags
        if (pas.curveCounts.bezier > 0) out.anyBezier = true;
        if (pas.hasDrawOrder)           out.anyDrawOrder = true;
        if (pas.hasEvents)              out.anyEvents    = true;
        if (pas.hasDeform)              out.anyDeform    = true;

        auto itTc = pas.timelineKindCounts.find(TimelineKind::SlotTwoColor);
        if (itTc != pas.timelineKindCounts.end() && itTc->second > 0)
            out.anyTwoColor = true;

        auto itIk = pas.timelineKindCounts.find(TimelineKind::IkConstraint);
        if (itIk != pas.timelineKindCounts.end() && itIk->second > 0)
            out.anyIkTimeline = true;

        auto itTf = pas.timelineKindCounts.find(TimelineKind::TransformConstraint);
        if (itTf != pas.timelineKindCounts.end() && itTf->second > 0)
            out.anyTransformTimeline = true;

        auto itPm = pas.timelineKindCounts.find(TimelineKind::PathConstraintMix);
        auto itPp = pas.timelineKindCounts.find(TimelineKind::PathConstraintPosition);
        if ((itPm != pas.timelineKindCounts.end() && itPm->second > 0) ||
            (itPp != pas.timelineKindCounts.end() && itPp->second > 0))
            out.anyPathTimeline = true;
    }

    delete skelData;
    delete atlas;
}

// ─── Print per-character audit ──────────────────────────────────────────────
void printAudit(const CharacterAudit& a) {
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  " << a.name << "\n";
    std::cout << "  Spine version: " << a.version << "\n";
    std::cout << "══════════════════════════════════════════════════════════════\n\n";

    // ── Skeleton basics
    std::cout << "  ── Skeleton ──────────────────────────────────────────────\n";
    std::cout << "  Bones: " << a.boneCount << "  Slots: " << a.slotCount
              << "  Skins: " << a.skinCount << "  Animations: " << a.animCount << "\n";
    std::cout << "  Atlas: " << a.atlasPageCount << " page(s), "
              << a.atlasRegionCount << " regions\n";

    // ── Attachment breakdown
    std::cout << "\n  ── Attachments ───────────────────────────────────────────\n";
    int totalAtt = a.regionAttachments + a.meshAttachments + a.linkedMeshAttachments
                 + a.clippingAttachments + a.boundingBoxAttachments
                 + a.pathAttachments + a.pointAttachments;
    std::cout << "  Total: " << totalAtt << "\n";
    if (a.regionAttachments    > 0) std::cout << "    Region:      " << a.regionAttachments << "\n";
    if (a.meshAttachments      > 0) std::cout << "    Mesh:        " << a.meshAttachments
                                              << "  (max verts=" << a.maxMeshVertices
                                              << ", max tris=" << a.maxMeshTriangles << ")\n";
    if (a.linkedMeshAttachments > 0) std::cout << "    LinkedMesh:  " << a.linkedMeshAttachments << "\n";
    if (a.clippingAttachments  > 0) std::cout << "    Clipping:    " << a.clippingAttachments << " ⚠\n";
    if (a.boundingBoxAttachments>0) std::cout << "    BoundingBox: " << a.boundingBoxAttachments << "\n";
    if (a.pathAttachments      > 0) std::cout << "    Path:        " << a.pathAttachments << "\n";
    if (a.pointAttachments     > 0) std::cout << "    Point:       " << a.pointAttachments << "\n";

    // ── Constraints
    if (a.ikConstraints > 0 || a.transformConstraints > 0 || a.pathConstraints > 0) {
        std::cout << "\n  ── Constraints ───────────────────────────────────────────\n";
        if (a.ikConstraints        > 0) std::cout << "    IK:                " << a.ikConstraints << " ⚠\n";
        if (a.transformConstraints > 0) std::cout << "    Transform:         " << a.transformConstraints << " ⚠\n";
        if (a.pathConstraints      > 0) std::cout << "    Path:              " << a.pathConstraints << " ⚠\n";
    }

    // ── Bone inheritance
    bool hasNonNormalInherit = false;
    for (auto it = a.inheritModes.begin(); it != a.inheritModes.end(); ++it) {
        if (it->first != spine::TransformMode_Normal && it->second > 0) hasNonNormalInherit = true;
    }
    if (hasNonNormalInherit) {
        std::cout << "\n  ── Bone Inheritance ──────────────────────────────────────\n";
        for (auto it = a.inheritModes.begin(); it != a.inheritModes.end(); ++it) {
            std::cout << "    " << inheritModeName(static_cast<spine::TransformMode>(it->first)) << ": " << it->second << "\n";
        }
    }
    if (a.coloredBones > 0) {
        std::cout << "    Colored bones: " << a.coloredBones << "\n";
    }

    // ── Blend modes
    if (a.additiveSlots > 0 || a.multiplySlots > 0 || a.screenSlots > 0) {
        std::cout << "\n  ── Slot Blend Modes ──────────────────────────────────────\n";
        if (a.additiveSlots > 0) std::cout << "    Additive: " << a.additiveSlots << "\n";
        if (a.multiplySlots > 0) std::cout << "    Multiply: " << a.multiplySlots << "\n";
        if (a.screenSlots   > 0) std::cout << "    Screen:   " << a.screenSlots << "\n";
    }

    // ── Events
    if (a.eventCount > 0) {
        std::cout << "\n  ── Events ────────────────────────────────────────────────\n";
        std::cout << "    " << a.eventCount << " events defined ⚠\n";
    }

    // ── Extra skins
    if (!a.extraSkinNames.empty()) {
        std::cout << "\n  ── Extra Skins ───────────────────────────────────────────\n";
        for (auto& sn : a.extraSkinNames) {
            std::cout << "    \"" << sn << "\"\n";
        }
    }

    // ── Per-animation feature flags
    std::cout << "\n  ── Animation Feature Summary ─────────────────────────────\n";
    int totalLinear = 0, totalStepped = 0, totalBezier = 0;
    for (auto& pas : a.animations) {
        totalLinear  += pas.curveCounts.linear;
        totalStepped += pas.curveCounts.stepped;
        totalBezier  += pas.curveCounts.bezier;
    }
    std::cout << "  Keyframe curves: " << totalLinear << " linear, "
              << totalStepped << " stepped, " << totalBezier << " bezier";
    if (totalBezier > 0) std::cout << " ⚠";
    std::cout << "\n";

    std::cout << "  Flags:";
    if (a.anyBezier)     std::cout << " BEZIER";
    if (a.anyDrawOrder)  std::cout << " DRAW_ORDER";
    if (a.anyTwoColor)   std::cout << " TWO_COLOR";
    if (a.anyDeform)     std::cout << " DEFORM";
    if (a.anyEvents)     std::cout << " EVENTS";
    if (a.anyIkTimeline) std::cout << " IK_TIMELINE";
    if (a.anyTransformTimeline) std::cout << " TRANSFORM_TIMELINE";
    if (a.anyPathTimeline) std::cout << " PATH_TIMELINE";
    if (!a.anyBezier && !a.anyDrawOrder && !a.anyTwoColor && !a.anyDeform &&
        !a.anyEvents && !a.anyIkTimeline && !a.anyTransformTimeline && !a.anyPathTimeline)
        std::cout << " (none — simple only)";
    std::cout << "\n";

    // ── Detailed per-animation timeline breakdown
    std::cout << "\n  ── Per-Animation Detail ──────────────────────────────────\n";
    for (auto& pas : a.animations) {
        std::cout << "    " << std::setw(20) << std::left << pas.name
                  << " dur=" << std::setw(5) << pas.duration << "s"
                  << "  timelines=" << std::setw(3) << pas.timelineKindCounts.size();
        // Show timeline kind breakdown
        bool first = true;
        for (auto it = pas.timelineKindCounts.begin(); it != pas.timelineKindCounts.end(); ++it) {
            if (!first) std::cout << ",";
            std::cout << " " << timelineKindName(it->first) << ":" << it->second;
            first = false;
        }
        if (pas.hasDrawOrder) std::cout << " [DRAW_ORDER]";
        if (pas.hasEvents)    std::cout << " [EVENTS]";
        if (pas.hasDeform)    std::cout << " [DEFORM]";
        if (pas.curveCounts.bezier  > 0) std::cout << " [bezier:" << pas.curveCounts.bezier << "]";
        if (pas.curveCounts.stepped > 0) std::cout << " [stepped:" << pas.curveCounts.stepped << "]";
        std::cout << "\n";
    }

    std::cout << "\n";
}

// ─── Summary table ──────────────────────────────────────────────────────────
void printSummary(const std::vector<CharacterAudit>& audits) {
    std::cout << "\n";
    std::cout << "══════════════════════════════════════════════════════════════════════\n";
    std::cout << "  PHASE 1 AUDIT — FEATURE SUMMARY MATRIX\n";
    std::cout << "══════════════════════════════════════════════════════════════════════\n\n";

    // Header
    std::cout << std::left
              << std::setw(22) << "Character"
              << std::setw(10) << "Version"
              << std::setw(6)  << "Bones"
              << std::setw(6)  << "Slots"
              << std::setw(6)  << "Anims"
              << std::setw(6)  << "Skins"
              << "Mesh Clip Bezier DrawOrd 2Color Deform Events IKTim TFTime PathTim Inherit\n";
    std::cout << std::string(130, '-') << "\n";

    for (auto& a : audits) {
        std::cout << std::left
                  << std::setw(22) << a.name.substr(0, 21)
                  << std::setw(10) << a.version
                  << std::setw(6)  << a.boneCount
                  << std::setw(6)  << a.slotCount
                  << std::setw(6)  << a.animCount
                  << std::setw(6)  << a.skinCount;

        auto check = [](bool v) { return v ? "  ✓  " : "  -  "; };
        std::cout << (a.meshAttachments > 0 ? " ✓ " : " - ")
                  << (a.clippingAttachments > 0 ? " ✓ " : " - ")
                  << check(a.anyBezier)
                  << check(a.anyDrawOrder)
                  << check(a.anyTwoColor)
                  << check(a.anyDeform)
                  << check(a.anyEvents || a.eventCount > 0)
                  << check(a.anyIkTimeline)
                  << check(a.anyTransformTimeline)
                  << check(a.anyPathTimeline);

        // Inherit mode summary
        bool hasInherit = false;
        for (auto it = a.inheritModes.begin(); it != a.inheritModes.end(); ++it) {
            if (it->first != spine::TransformMode_Normal && it->second > 0) { hasInherit = true; break; }
        }
        std::cout << (hasInherit ? " ✓" : " -");

        std::cout << "\n";
    }

    // ── Totals row ──────────────────────────────────────────────────────
    std::cout << std::string(130, '-') << "\n";
    int totalBones = 0, totalSlots = 0, totalAnims = 0;
    int withMesh = 0, withClip = 0, withBezier = 0, withDrawOrder = 0;
    int withTwoColor = 0, withDeform = 0, withEvents = 0;
    int withIkTimeline = 0, withTfTimeline = 0, withPathTimeline = 0;
    int withInherit = 0;

    for (auto& a : audits) {
        totalBones += a.boneCount;
        totalSlots += a.slotCount;
        totalAnims += a.animCount;
        if (a.meshAttachments > 0)      withMesh++;
        if (a.clippingAttachments > 0)  withClip++;
        if (a.anyBezier)                withBezier++;
        if (a.anyDrawOrder)             withDrawOrder++;
        if (a.anyTwoColor)              withTwoColor++;
        if (a.anyDeform)                withDeform++;
        if (a.anyEvents || a.eventCount > 0) withEvents++;
        if (a.anyIkTimeline)            withIkTimeline++;
        if (a.anyTransformTimeline)     withTfTimeline++;
        if (a.anyPathTimeline)          withPathTimeline++;
        for (auto it = a.inheritModes.begin(); it != a.inheritModes.end(); ++it) {
            if (it->first != spine::TransformMode_Normal && it->second > 0) { withInherit++; break; }
        }
    }

    std::cout << std::left
              << std::setw(22) << "TOTALS (" + std::to_string(audits.size()) + " chars)"
              << std::setw(10) << ""
              << std::setw(6)  << totalBones
              << std::setw(6)  << totalSlots
              << std::setw(6)  << totalAnims
              << std::setw(6)  << "";
    std::cout << " " << withMesh << "/" << audits.size()
              << " " << withClip << "/" << audits.size()
              << " " << withBezier << "/" << audits.size()
              << " " << withDrawOrder << "/" << audits.size()
              << " " << withTwoColor << "/" << audits.size()
              << " " << withDeform << "/" << audits.size()
              << " " << withEvents << "/" << audits.size()
              << " " << withIkTimeline << "/" << audits.size()
              << " " << withTfTimeline << "/" << audits.size()
              << "  " << withPathTimeline << "/" << audits.size()
              << "  " << withInherit << "/" << audits.size()
              << "\n";

    // ── MVP recommendation ──────────────────────────────────────────────
    std::cout << "\n══════════════════════════════════════════════════════════════════════\n";
    std::cout << "  MVP SCOPE RECOMMENDATION\n";
    std::cout << "══════════════════════════════════════════════════════════════════════\n\n";

    std::cout << "  REQUIRED (used by ≥1 character):\n";
    std::cout << "    ✅ Region attachments     (all characters)\n";
    if (withMesh > 0)
        std::cout << "    ✅ Mesh attachments       (" << withMesh << " characters)\n";
    if (withClip > 0)
        std::cout << "    ✅ Clipping attachments   (" << withClip << " characters)\n";
    if (withBezier > 0)
        std::cout << "    ✅ Bezier curves          (" << withBezier << " characters)\n";
    if (withDrawOrder > 0)
        std::cout << "    ✅ Draw order keyframes   (" << withDrawOrder << " characters)\n";
    if (withTwoColor > 0)
        std::cout << "    ✅ Two-color (dark tint)  (" << withTwoColor << " characters)\n";
    if (withDeform > 0)
        std::cout << "    ✅ Deform (mesh warp)     (" << withDeform << " characters)\n";
    if (withIkTimeline > 0)
        std::cout << "    ✅ IK constraint timelines(" << withIkTimeline << " characters)\n";

    std::cout << "\n  DEFERRABLE (not used by any character):\n";
    if (withEvents == 0)
        std::cout << "    ⏸️  Event timelines        (0 characters — skip)\n";
    if (withTfTimeline == 0)
        std::cout << "    ⏸️  Transform constraints   (0 characters — skip)\n";
    if (withPathTimeline == 0)
        std::cout << "    ⏸️  Path constraints        (0 characters — skip)\n";
    if (withInherit == 0)
        std::cout << "    ⏸️  Non-normal bone inherit (0 characters — skip)\n";

    // Check for constraint DATA (setup pose) vs constraint TIMELINES
    int withIkData = 0, withTfData = 0, withPathData = 0;
    for (auto& a : audits) {
        if (a.ikConstraints > 0)        withIkData++;
        if (a.transformConstraints > 0) withTfData++;
        if (a.pathConstraints > 0)      withPathData++;
    }
    if (withIkData > 0)
        std::cout << "\n  NOTE: " << withIkData << " character(s) have IK constraint DATA\n"
                  << "        (setup pose), but no IK animation timelines were found.\n"
                  << "        IK setup data may need to be stored for correct pose.\n";
    if (withTfData > 0)
        std::cout << "\n  NOTE: " << withTfData << " character(s) have Transform constraint DATA\n"
                  << "        (setup pose), but no transform animation timelines.\n";
    if (withPathData > 0)
        std::cout << "\n  NOTE: " << withPathData << " character(s) have Path constraint DATA\n"
                  << "        (setup pose), but no path animation timelines.\n";
}

// ─── Main ───────────────────────────────────────────────────────────────────
int main()
{
    const std::string assetsDir = "assets";
    const std::string charsDir  = assetsDir + "/characters";

    if (!fs::exists(charsDir)) {
        std::cerr << "ERROR: '" << charsDir << "' not found. Run from workspace root.\n";
        return 1;
    }

    // Discover all characters
    std::vector<std::string> characters;
    for (auto& entry : fs::directory_iterator(charsDir)) {
        if (entry.is_directory())
            characters.push_back(entry.path().filename().string());
    }
    std::sort(characters.begin(), characters.end());

    std::vector<CharacterAudit> audits;

    for (const auto& charName : characters) {
        // Find the default stance .skel and .atlas
        fs::path baseDir = fs::path(charsDir) / charName / "default";

        if (!fs::exists(baseDir)) {
            std::cerr << charName << ": no default/ directory, skipping\n";
            continue;
        }

        std::string skelPath, atlasPath;
        for (auto& entry : fs::directory_iterator(baseDir)) {
            auto ext = entry.path().extension().string();
            if (ext == ".skel" && skelPath.empty()) {
                skelPath = entry.path().string();
            } else if (ext == ".atlas" && atlasPath.empty()) {
                atlasPath = entry.path().string();
            }
        }

        if (skelPath.empty() || atlasPath.empty()) {
            std::cerr << charName << ": missing .skel or .atlas, skipping\n";
            continue;
        }

        CharacterAudit audit;
        auditCharacter(charName, skelPath, atlasPath, audit);
        audits.push_back(audit);
    }

    // Print per-character audits
    for (auto& a : audits) {
        printAudit(a);
    }

    // Print summary matrix
    printSummary(audits);

    return 0;
}
