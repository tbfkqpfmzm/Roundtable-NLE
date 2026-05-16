/*
 * ShotAnimConvert.cpp — implementation.
 *
 * Loads spine-cpp's SkeletonBinary against an Atlas, hands the resulting
 * SkeletonData to ShotAnimWriter, and writes .shotanim to disk.
 */

#ifdef ROUNDTABLE_HAS_SPINE

#include "spine/ShotAnimConvert.h"
#include "spine/ShotAnimWriter.h"
#include "spine/SpineAtlas.h"

#include <spine/Atlas.h>
#include <spine/AtlasAttachmentLoader.h>
#include <spine/SkeletonBinary.h>
#include <spine/SkeletonData.h>
#include <spine/SpineString.h>

#include <spdlog/spdlog.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace rt {

bool convertSkelToShotanim(const std::string& skelPath,
                            const std::string& atlasPath,
                            const std::string& outPath,
                            std::string* errorMsg) {
    // 1. Load atlas (wraps spine::Atlas + texture loader).
    SpineAtlas atlas;
    if (!atlas.load(atlasPath)) {
        if (errorMsg) *errorMsg = "atlas load failed: " + atlasPath;
        return false;
    }

    // 2. Parse .skel via spine::SkeletonBinary. The AttachmentLoader is
    //    needed so attachments are linked to atlas regions during parse.
    spine::AtlasAttachmentLoader loader(atlas.getSpineAtlas());
    spine::SkeletonBinary binary(&loader);
    spine::SkeletonData* sd = binary.readSkeletonDataFile(spine::String(skelPath.c_str()));
    if (!sd) {
        if (errorMsg) {
            *errorMsg = "skeleton parse failed: ";
            *errorMsg += binary.getError().buffer();
        }
        return false;
    }

    // 3. Hand off to ShotAnimWriter.
    ShotAnimWriter writer;
    const bool ok = writer.write(sd, atlas, outPath);
    delete sd;

    if (!ok && errorMsg) *errorMsg = writer.lastError();
    return ok;
}

namespace {

/// Try to find a (.skel, .atlas) pair in `dir`. If both exist, run the
/// converter and write `dir/character.shotanim`. Returns true on success.
bool tryConvertDir(const fs::path& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) return false;

    fs::path skelPath, atlasPath;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        const auto ext = entry.path().extension().string();
        if (skelPath.empty() && ext == ".skel")  skelPath  = entry.path();
        if (atlasPath.empty() && ext == ".atlas") atlasPath = entry.path();
    }
    if (skelPath.empty() || atlasPath.empty()) return false;

    const fs::path outPath = dir / "character.shotanim";
    std::string err;
    const bool ok = convertSkelToShotanim(skelPath.string(),
                                           atlasPath.string(),
                                           outPath.string(),
                                           &err);
    if (ok) {
        spdlog::info("ShotAnimConvert: {} → {}",
                     skelPath.filename().string(), outPath.filename().string());
    } else {
        spdlog::warn("ShotAnimConvert: failed for {}: {}",
                     skelPath.string(), err);
    }
    return ok;
}

} // namespace

int convertCharacterDirectory(const std::string& charOutfitDir) {
    const fs::path root(charOutfitDir);
    if (!fs::exists(root) || !fs::is_directory(root)) {
        spdlog::warn("ShotAnimConvert: directory not found: {}", charOutfitDir);
        return 0;
    }

    int count = 0;
    if (tryConvertDir(root)) ++count;

    // Stance subdirectories (per SpineEngine::resolvePaths convention)
    for (const char* stance : {"aim", "cover"}) {
        const fs::path sub = root / stance;
        if (fs::exists(sub) && tryConvertDir(sub)) ++count;
    }
    return count;
}

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
