/*
 * ShotAnimConvert — one-shot .skel + .atlas → .shotanim conversion.
 *
 * Invoked at character download time (CharacterBrowser → after files
 * arrive). The runtime distinction:
 *   - This helper REQUIRES spine-cpp (uses spine::SkeletonBinary to
 *     parse the .skel). It only compiles under ROUNDTABLE_HAS_SPINE.
 *   - The OUTPUT .shotanim file is consumed by NativeSpineEngine,
 *     which has no spine-cpp dependency.
 *
 * Phase 3 policy: conversion runs at download time, but the original
 * .skel and .atlas are RETAINED on disk. The runtime still uses
 * spine-cpp by default; NativeSpineEngine is opt-in via build flag.
 * Once parity is verified, a future phase will switch the default
 * runtime and remove the redundant .skel files.
 */

#pragma once

#ifdef ROUNDTABLE_HAS_SPINE

#include <string>

namespace rt {

/// Convert a single .skel + .atlas pair to a .shotanim file on disk.
///
/// @param skelPath   Input .skel binary
/// @param atlasPath  Input .atlas text
/// @param outPath    Output .shotanim path (overwrites if exists)
/// @param errorMsg   Optional; populated with a human-readable error on failure
/// @return true on success
bool convertSkelToShotanim(const std::string& skelPath,
                            const std::string& atlasPath,
                            const std::string& outPath,
                            std::string* errorMsg = nullptr);

/// Scan a character/outfit directory and convert every
/// `*.skel` + `*.atlas` pair found in the directory itself and in its
/// `aim/` and `cover/` stance subdirectories.
///
/// For each pair, the output is named `character.shotanim` next to the
/// source files. Existing .shotanim files are overwritten so re-running
/// conversion is safe.
///
/// @param charOutfitDir  e.g. `assets/characters/Rapi/default`
/// @return number of successful conversions (0 on total failure)
int convertCharacterDirectory(const std::string& charOutfitDir);

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
