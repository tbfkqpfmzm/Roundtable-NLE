/*
 * CharacterThumbnailCache.h — Persistent on-disk character thumbnail cache.
 *
 * When a character is downloaded, a single frame of the default idle pose
 * is rendered via Spine's software rasterizer and saved as a PNG.
 * makeCharacterThumbnail() checks this cache first, avoiding expensive
 * per-character Spine loads at startup.
 */

#pragma once

#include <string>

class QImage;
class QPixmap;

namespace rt {

/// Directory name for the thumbnail cache (under userDataDir/cache/)
constexpr const char* kCharacterThumbCacheDir = "character_thumbs";

/// Render a single idle frame for the given character and save it to the
/// persistent thumbnail cache.  Uses Spine CPU software rasterization.
/// @param charName   Folder name of the character (e.g. "Crown")
/// @param outfit     Outfit name (e.g. "default")
/// @return true if the thumbnail was rendered and saved successfully
bool renderAndCacheCharacterThumbnail(const std::string& charName,
                                      const std::string& outfit = "default");

/// Get the file path for a cached character thumbnail.
/// Does not check if the file exists.
std::string cachedCharacterThumbnailPath(const std::string& charName);

/// Check if a cached thumbnail exists for the given character.
bool hasCachedCharacterThumbnail(const std::string& charName);

/// Load a cached character thumbnail (cropped close-up with background), scaled to the given size.
/// Returns a null QPixmap if no cached thumbnail exists.
QPixmap loadCachedCharacterThumbnail(const std::string& charName, int sz);

/// Load the full-body cached render (uncropped, transparent background) for the given character.
/// Returns a null QPixmap if no cached full-body render exists.
QPixmap loadCachedCharacterFullBody(const std::string& charName);

/// Load the outfit-specific full-body cached render for the given character + outfit combination.
/// Falls back to the generic full-body render if no outfit-specific one exists.
QPixmap loadCachedCharacterOutfitFullBody(const std::string& charName,
                                           const std::string& outfit);

/// Get the file path for the full-body cached render.
std::string cachedCharacterFullBodyPath(const std::string& charName);

/// Get the file path for an outfit-specific full-body cached render.
std::string cachedCharacterOutfitFullBodyPath(const std::string& charName,
                                                const std::string& outfit);

} // namespace rt
