/**
 * query_spine_bounds — standalone tool to print the bounding box of
 * each character's skeleton in Spine world units.
 *
 * Usage: query_spine_bounds.exe
 * (run from the workspace root so "assets/characters/" is reachable)
 */

#include "core/spine/SpineEngine.h"
#include "core/spine/SpineAnimation.h"
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace rt;

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

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "======================================================================\n";
    std::cout << "  Spine Character Bounds Report\n";
    std::cout << "======================================================================\n\n";

    for (const auto& charName : characters) {
        // Only load Default stance
        auto paths = SpineEngine::resolvePaths(assetsDir, charName, "default",
                                                CharacterStance::Default);
        if (!paths.valid) {
            std::cout << charName << ": [no skeleton found]\n";
            continue;
        }

        SpineEngine engine;
        if (!engine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
            std::cout << charName << ": [failed to load]\n";
            continue;
        }

        // Setup pose bounds
        float sx, sy, sw, sh;
        engine.getBounds(sx, sy, sw, sh);

        std::cout << "── " << charName << " ──────────────────────────────────────\n";
        std::cout << "  Skel: " << paths.skelPath << "\n";
        std::cout << "  Setup pose bounds: x=" << sx << " y=" << sy
                  << " w=" << sw << " h=" << sh << "\n";

        // List animations and get bounds for each
        auto anims = engine.animation().listAnimations();
        std::cout << "  Animations (" << anims.size() << "):\n";

        float maxW = sw, maxH = sh;

        for (const auto& anim : anims) {
            // Evaluate at multiple points through the animation to find max bounds
            float animMaxW = 0, animMaxH = 0;
            float animMinX = 1e9f, animMinY = 1e9f;
            float animMaxX = -1e9f, animMaxY = -1e9f;

            int samples = std::max(10, static_cast<int>(anim.duration * 30));
            for (int s = 0; s <= samples; ++s) {
                float t = (anim.duration > 0)
                    ? (static_cast<float>(s) / samples) * anim.duration
                    : 0.0f;
                engine.evaluateAtTime(t, t);
                float bx, by, bw, bh;
                engine.getBounds(bx, by, bw, bh);
                if (bw > animMaxW) animMaxW = bw;
                if (bh > animMaxH) animMaxH = bh;
                if (bx < animMinX) animMinX = bx;
                if (by < animMinY) animMinY = by;
                if (bx + bw > animMaxX) animMaxX = bx + bw;
                if (by + bh > animMaxY) animMaxY = by + bh;
            }

            float envelopeW = animMaxX - animMinX;
            float envelopeH = animMaxY - animMinY;

            std::cout << "    " << std::setw(25) << std::left << anim.name
                      << " dur=" << std::setw(5) << anim.duration << "s"
                      << "  maxFrame=" << animMaxW << "x" << animMaxH
                      << "  envelope=" << envelopeW << "x" << envelopeH
                      << "\n";

            if (envelopeW > maxW) maxW = envelopeW;
            if (envelopeH > maxH) maxH = envelopeH;
        }

        std::cout << "  *** Max envelope across all anims: "
                  << maxW << " x " << maxH << " (Spine units)\n\n";
    }

    std::cout << "======================================================================\n";
    std::cout << "  Notes:\n";
    std::cout << "  - 'Setup pose bounds' = skeleton at rest\n";
    std::cout << "  - 'maxFrame' = largest single-frame bounding box during animation\n";
    std::cout << "  - 'envelope' = total area covered across all frames of animation\n";
    std::cout << "  - Pre-rendered video should use the max envelope dimensions\n";
    std::cout << "======================================================================\n";

    return 0;
}
