/*
 * shotanim_parity — side-by-side comparison of SpineEngine (spine-cpp)
 * vs NativeSpineEngine (.shotanim) output for already-converted
 * characters. Catches regressions / divergences between the two paths
 * before flipping the default runtime.
 *
 * Build: requires both ROUNDTABLE_HAS_SPINE and
 *        ROUNDTABLE_HAS_NATIVE_SHOTANIM. CMake adds the target
 *        automatically when both flags are on.
 *
 * Usage:
 *   shotanim_parity <assets-root>
 *     — Walks every `<root>/<character>/<outfit>/` and runs parity
 *       on any pair that has BOTH skeleton.{skel,atlas} and a
 *       character.shotanim. Prints one summary row per (char,outfit).
 *
 *   shotanim_parity <outfit-dir>
 *     — Runs parity on a single outfit directory. Prints a detailed
 *       per-animation report.
 *
 * What it measures, per animation, at t ∈ {0, 0.5·duration}:
 *   batchCount    spine vs native
 *   vertCount     spine vs native
 *   idxCount      spine vs native
 *   AABB (boundsX, boundsY, boundsW, boundsH) — Euclidean delta
 *   position checksum — sum of (x + y) across all emitted vertices
 *
 * Counts that differ flag a bug; AABB or checksum deltas above
 * epsilon flag a positional drift. (This is a coarse first-pass
 * check; exact pixel parity needs framebuffer comparison.)
 */

#if defined(ROUNDTABLE_HAS_SPINE) && defined(ROUNDTABLE_HAS_NATIVE_SHOTANIM)

#include "spine/SpineEngine.h"
#include "spine/SpineAnimation.h"
#include "spine/NativeSpineEngine.h"
#include "timeline/SpineClip.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ─── Stats ───────────────────────────────────────────────────────────────────

struct FrameStats {
    int   batchCount = 0;
    int   vertCount  = 0;
    int   idxCount   = 0;
    float boundsX = 0, boundsY = 0, boundsW = 0, boundsH = 0;
    double posChecksum = 0;  // sum of (x + y) across all vertices
};

FrameStats statsFor(const rt::SpineRenderData& rd) {
    FrameStats s;
    s.batchCount = static_cast<int>(rd.batches.size());
    s.boundsX = rd.boundsX;
    s.boundsY = rd.boundsY;
    s.boundsW = rd.boundsW;
    s.boundsH = rd.boundsH;
    for (const auto& b : rd.batches) {
        s.vertCount += static_cast<int>(b.vertices.size());
        s.idxCount  += static_cast<int>(b.indices.size());
        for (const auto& v : b.vertices) {
            s.posChecksum += static_cast<double>(v.x);
            s.posChecksum += static_cast<double>(v.y);
        }
    }
    return s;
}

float boundsDeltaMag(const FrameStats& a, const FrameStats& b) {
    const float dx = a.boundsX - b.boundsX;
    const float dy = a.boundsY - b.boundsY;
    const float dw = a.boundsW - b.boundsW;
    const float dh = a.boundsH - b.boundsH;
    return std::sqrt(dx*dx + dy*dy + dw*dw + dh*dh);
}

// ─── Parity for a single outfit directory ────────────────────────────────────

struct OutfitResult {
    std::string character;
    std::string outfit;
    int totalAnims    = 0;
    int batchMismatch = 0;
    int vertMismatch  = 0;
    float maxBoundsDelta = 0;
    double maxChecksumDelta = 0;
    bool spineLoadOk = false;
    bool nativeLoadOk = false;
    std::string error;
};

void runOneOutfit(const fs::path& outfitDir, OutfitResult& res, bool verbose) {
    res.character = outfitDir.parent_path().filename().string();
    res.outfit    = outfitDir.filename().string();

    // Resolve paths (mirroring SpineEngine::resolvePaths behavior — flat,
    // no stance subdir for this comparator's purpose).
    fs::path skelPath, atlasPath, shotPath;
    for (auto& entry : fs::directory_iterator(outfitDir)) {
        const auto ext = entry.path().extension().string();
        if (ext == ".skel" && skelPath.empty())     skelPath  = entry.path();
        if (ext == ".atlas" && atlasPath.empty())   atlasPath = entry.path();
        if (ext == ".shotanim" && shotPath.empty()) shotPath  = entry.path();
    }
    if (skelPath.empty() || atlasPath.empty() || shotPath.empty()) {
        res.error = "missing .skel / .atlas / .shotanim";
        return;
    }

    // Load both engines from the SAME asset.
    rt::SpineEngine spineEngine;
    rt::NativeSpineEngine nativeEngine;

    res.spineLoadOk  = spineEngine.loadSkeleton(skelPath.string(), atlasPath.string());
    res.nativeLoadOk = nativeEngine.loadSkeleton(shotPath.string());
    if (!res.spineLoadOk || !res.nativeLoadOk) {
        res.error = (res.spineLoadOk ? "" : "spine-cpp load failed; ")
                  + std::string(res.nativeLoadOk ? "" : "native load failed");
        return;
    }

    // Compare available animation lists. Use spine-cpp's listing as the
    // source of truth; if native is missing some, report them.
    auto spineAnims = spineEngine.animation().listAnimations();
    auto nativeAnims = nativeEngine.listAnimations();
    res.totalAnims = static_cast<int>(spineAnims.size());

    // Build a set of native names for fast lookup.
    std::vector<std::string> nativeNames(nativeAnims.begin(), nativeAnims.end());

    bool bonesDumped = false;

    for (const auto& ai : spineAnims) {
        const std::string& name = ai.name;
        const float dur = (ai.duration > 0) ? ai.duration : 1.0f;
        const bool nativeHasAnim = std::find(nativeNames.begin(), nativeNames.end(), name)
                                   != nativeNames.end();
        if (!nativeHasAnim) {
            if (verbose) std::printf("  [%s] animation missing in native\n", name.c_str());
            continue;
        }

        spineEngine.animation().setBodyAnimation(name, false);
        nativeEngine.setBodyAnimation(name, false);

        const float samples[] = { 0.0f, dur * 0.5f };
        for (float t : samples) {
            spineEngine.evaluateAtTime(t);
            nativeEngine.evaluateAtTime(t);

            // Bone-by-bone divergence dump — once per outfit, first anim @ t=0.
            // Bones resolve parent-before-child, so the FIRST divergent bone
            // (by index) is the root cause; everything after compounds it.
            if (verbose && !bonesDumped && t == 0.0f) {
                bonesDumped = true;
                auto sb = spineEngine.debugBoneWorldSpineCpp();
                auto nb = nativeEngine.debugBoneWorld();
                std::printf("  ── Bone world transform diff (anim '%s' @ t=0) ──\n",
                            name.c_str());
                std::printf("     spine bones=%zu  native bones=%zu\n",
                            sb.size(), nb.size());
                const size_t n = std::min(sb.size(), nb.size());
                // Rank bones by max(posΔ_X, posΔ_Y, matΔ*100). Print top 15
                // by magnitude so we see the worst bones, not just the first.
                struct BoneDiff { size_t i; float md, pdx, pdy, score; };
                std::vector<BoneDiff> diffs;
                for (size_t bi = 0; bi < n; ++bi) {
                    const auto& S = sb[bi];
                    const auto& N = nb[bi];
                    float md = 0;
                    for (int k = 0; k < 4; ++k)
                        md = std::max(md, std::fabs(S[k] - N[k]));
                    const float pdx = std::fabs(S[4] - N[4]);
                    const float pdy = std::fabs(S[5] - N[5]);
                    const float score = std::max({pdx, pdy, md * 100.0f});
                    if (md > 0.01f || pdx > 0.5f || pdy > 0.5f)
                        diffs.push_back({bi, md, pdx, pdy, score});
                }
                std::sort(diffs.begin(), diffs.end(),
                          [](const BoneDiff& a, const BoneDiff& b) {
                              return a.score > b.score;
                          });
                std::printf("     %zu of %zu bones diverge; top 15 by magnitude:\n",
                            diffs.size(), n);
                const size_t showN = std::min<size_t>(15, diffs.size());
                for (size_t k = 0; k < showN; ++k) {
                    const auto& D = diffs[k];
                    const auto& S = sb[D.i];
                    const auto& N = nb[D.i];
                    std::printf("     bone %3zu  matΔ=%.4f  posΔ=(%.2f, %.2f)\n",
                                D.i, D.md, D.pdx, D.pdy);
                    std::printf("        spine  a=%.4f b=%.4f c=%.4f d=%.4f wx=%.2f wy=%.2f\n",
                                S[0], S[1], S[2], S[3], S[4], S[5]);
                    std::printf("        native a=%.4f b=%.4f c=%.4f d=%.4f wx=%.2f wy=%.2f\n",
                                N[0], N[1], N[2], N[3], N[4], N[5]);
                }
                if (diffs.empty())
                    std::printf("     (all %zu bones within tolerance)\n", n);

                // ── Slot attachment diff at the same instant ──────────
                auto sa = spineEngine.debugSlotAttachmentsSpineCpp();
                auto na = nativeEngine.debugSlotAttachments();
                const rt::SkeletonPkg* pkg = nativeEngine.package();
                std::printf("  ── Slot attachment diff (anim '%s' @ t=0) ──\n",
                            name.c_str());
                std::printf("     spine slots=%zu  native slots=%zu\n",
                            sa.size(), na.size());
                int mismatches = 0;
                const size_t sn = std::min(sa.size(), na.size());
                for (size_t si = 0; si < sn; ++si) {
                    std::string nativeName;
                    if (pkg && na[si] >= 0
                        && na[si] < static_cast<int32_t>(pkg->attachments.size())) {
                        const int32_t ni = pkg->attachments[na[si]].attachNameIdx;
                        if (ni >= 0 && ni < static_cast<int32_t>(pkg->strings.size()))
                            nativeName = pkg->strings[ni];
                    }
                    if (nativeName != sa[si]) {
                        if (mismatches < 25) {
                            std::printf("     slot %3zu  spine='%s'  native='%s'\n",
                                        si,
                                        sa[si].empty() ? "(none)" : sa[si].c_str(),
                                        nativeName.empty() ? "(none)" : nativeName.c_str());
                        }
                        ++mismatches;
                    }
                }
                std::printf("     %d slot attachment mismatch(es)\n", mismatches);
            }

            // IMPORTANT: extractMeshesSpineCpp() — NOT extractMeshes().
            // The latter dispatches to the parallel native runtime that
            // SpineEngine::loadSkeleton spins up internally, which would
            // make this a native-vs-native (always-zero-delta) comparison.
            rt::SpineRenderData sd = spineEngine.extractMeshesSpineCpp();
            rt::SpineRenderData nd = nativeEngine.extractMeshes();

            const auto ss = statsFor(sd);
            const auto ns = statsFor(nd);

            if (ss.batchCount != ns.batchCount) ++res.batchMismatch;
            if (ss.vertCount  != ns.vertCount)  ++res.vertMismatch;
            const float bd = boundsDeltaMag(ss, ns);
            if (bd > res.maxBoundsDelta) res.maxBoundsDelta = bd;
            const double cd = std::abs(ss.posChecksum - ns.posChecksum);
            if (cd > res.maxChecksumDelta) res.maxChecksumDelta = cd;

            if (verbose) {
                std::printf("  [%-24s @ t=%6.3f]  batches %3d vs %3d   verts %5d vs %5d   "
                            "AABB Δ=%9.2f   posSum Δ=%.2f\n",
                            name.c_str(), t,
                            ss.batchCount, ns.batchCount,
                            ss.vertCount, ns.vertCount,
                            bd, cd);
                std::printf("        spine  bounds  x=%9.2f y=%9.2f w=%9.2f h=%9.2f\n",
                            ss.boundsX, ss.boundsY, ss.boundsW, ss.boundsH);
                std::printf("        native bounds  x=%9.2f y=%9.2f w=%9.2f h=%9.2f\n",
                            ns.boundsX, ns.boundsY, ns.boundsW, ns.boundsH);
            }
        }
    }
}

// ─── Driver ──────────────────────────────────────────────────────────────────

void printHeader() {
    std::printf("%-24s %-12s %5s %6s %6s %12s %14s\n",
                "character", "outfit", "anims", "batchΔ", "vertΔ",
                "maxBoundsΔ", "maxPosSumΔ");
    std::printf("%-24s %-12s %5s %6s %6s %12s %14s\n",
                "------------------------", "------------", "-----",
                "------", "------", "------------", "--------------");
}

void printRow(const OutfitResult& r) {
    if (!r.error.empty()) {
        std::printf("%-24s %-12s  -- skipped: %s\n",
                    r.character.c_str(), r.outfit.c_str(), r.error.c_str());
        return;
    }
    std::printf("%-24s %-12s %5d %6d %6d %12.2f %14.2f\n",
                r.character.c_str(), r.outfit.c_str(),
                r.totalAnims, r.batchMismatch, r.vertMismatch,
                r.maxBoundsDelta, r.maxChecksumDelta);
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::warn);  // quiet info logs in tool

    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: shotanim_parity <assets-root>\n"
            "       shotanim_parity <outfit-dir>\n"
            "\n"
            "  assets-root: e.g. assets/characters\n"
            "  outfit-dir:  e.g. assets/characters/Rapi/default\n");
        return 1;
    }

    const fs::path target = argv[1];
    if (!fs::exists(target) || !fs::is_directory(target)) {
        std::fprintf(stderr, "Not a directory: %s\n", target.string().c_str());
        return 2;
    }

    // Heuristic: if `target` contains a .skel directly, treat as outfit dir.
    bool isOutfitDir = false;
    for (auto& entry : fs::directory_iterator(target)) {
        if (entry.path().extension() == ".skel") { isOutfitDir = true; break; }
    }

    if (isOutfitDir) {
        std::printf("Parity check: %s\n", target.string().c_str());
        OutfitResult r;
        runOneOutfit(target, r, /*verbose=*/true);
        std::printf("\n");
        printHeader();
        printRow(r);
        return r.error.empty() ? 0 : 3;
    }

    // Walk <root>/<character>/<outfit>/ structure
    printHeader();
    int totalChecked = 0;
    int totalIssues  = 0;
    for (auto& charEntry : fs::directory_iterator(target)) {
        if (!fs::is_directory(charEntry)) continue;
        for (auto& outfitEntry : fs::directory_iterator(charEntry.path())) {
            if (!fs::is_directory(outfitEntry)) continue;
            OutfitResult r;
            runOneOutfit(outfitEntry.path(), r, /*verbose=*/false);
            printRow(r);
            if (r.error.empty()) {
                ++totalChecked;
                if (r.batchMismatch > 0 || r.vertMismatch > 0) ++totalIssues;
            }
        }
    }
    std::printf("\nChecked %d outfit(s); %d with batch/vert mismatch.\n",
                totalChecked, totalIssues);
    return (totalIssues == 0) ? 0 : 4;
}

#else  // !ROUNDTABLE_HAS_SPINE || !ROUNDTABLE_HAS_NATIVE_SHOTANIM

#include <cstdio>
int main() {
    std::fprintf(stderr,
        "shotanim_parity was built without ROUNDTABLE_HAS_SPINE or "
        "ROUNDTABLE_HAS_NATIVE_SHOTANIM. Reconfigure CMake with both "
        "options ON and rebuild.\n");
    return 1;
}

#endif
