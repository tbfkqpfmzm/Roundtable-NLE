/*
 * migrate_shotanim — one-shot migration tool. Walks an assets root and
 * runs the .skel → .shotanim converter on every character/outfit dir
 * that doesn't yet have a .shotanim file (or whose .shotanim predates
 * the current format version).
 *
 * Phase 3 wired auto-conversion into the character DOWNLOAD flow, so
 * any character downloaded after that point is migrated automatically.
 * This tool exists for characters already on disk before the migration
 * landed, or after format version bumps that invalidate v1 files.
 *
 * Usage:
 *   migrate_shotanim <assets-root>
 *
 *   <assets-root> e.g. assets/characters
 *
 * For each <root>/<character>/<outfit>/ found, runs
 * rt::convertCharacterDirectory which itself recurses into aim/ and
 * cover/ stance subdirs.
 */

#ifdef ROUNDTABLE_HAS_SPINE

#include "spine/ShotAnimConvert.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: migrate_shotanim <assets-root>\n"
            "  Walks <root>/<character>/<outfit>/ and converts .skel\n"
            "  to .shotanim. Handles aim/ and cover/ stances recursively.\n"
            "  Existing .shotanim files are overwritten.\n");
        return 1;
    }

    spdlog::set_level(spdlog::level::warn);  // quiet info logs

    const fs::path root = argv[1];
    if (!fs::exists(root) || !fs::is_directory(root)) {
        std::fprintf(stderr, "Not a directory: %s\n", root.string().c_str());
        return 2;
    }

    int outfitsConverted = 0;
    int outfitsSkipped   = 0;
    int stancesConverted = 0;

    for (auto& charEntry : fs::directory_iterator(root)) {
        if (!fs::is_directory(charEntry)) continue;
        for (auto& outfitEntry : fs::directory_iterator(charEntry.path())) {
            if (!fs::is_directory(outfitEntry)) continue;
            const std::string outfitDir = outfitEntry.path().string();
            const int n = rt::convertCharacterDirectory(outfitDir);
            if (n > 0) {
                std::printf("  %s — converted %d stance(s)\n", outfitDir.c_str(), n);
                ++outfitsConverted;
                stancesConverted += n;
            } else {
                std::printf("  %s — skipped (no .skel/.atlas pair)\n", outfitDir.c_str());
                ++outfitsSkipped;
            }
        }
    }
    std::printf("\nMigration complete: %d outfit(s) converted (%d stance(s) total), %d skipped.\n",
                outfitsConverted, stancesConverted, outfitsSkipped);
    return 0;
}

#else  // !ROUNDTABLE_HAS_SPINE

#include <cstdio>
int main() {
    std::fprintf(stderr,
        "migrate_shotanim was built without ROUNDTABLE_HAS_SPINE. "
        "spine-cpp is required for .skel parsing during migration.\n");
    return 1;
}

#endif
