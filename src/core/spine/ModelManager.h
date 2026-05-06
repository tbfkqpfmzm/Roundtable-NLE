/*
 * ModelManager — scans the assets/characters/ directory tree and builds
 *                a catalog of available Spine characters with their
 *                outfits and stances.
 *
 * Also loads character_metadata.json when available to enrich entries
 * with display names, has_mouth_animation flags, etc.
 *
 * The ModelManager does NOT load skeleton data itself — use SpineEngine
 * for that.  This is a lightweight directory scanner + metadata cache.
 */

#pragma once

#ifdef ROUNDTABLE_HAS_SPINE

#include "timeline/SpineClip.h" // for CharacterStance

#include <atomic>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace rt {

/// A single outfit variant (default, aim, cover)
struct ModelVariant
{
    CharacterStance stance = CharacterStance::Default;
    std::string     skelPath;
    std::string     atlasPath;
    std::string     texturePath;
};

/// An outfit (may contain default/aim/cover stances)
struct ModelOutfit
{
    std::string                name;         ///< e.g., "default", "outfit_01"
    std::string                displayName;  ///< from metadata, or same as name
    std::vector<ModelVariant>  variants;
};

/// A character entry in the model catalog
struct ModelEntry
{
    std::string                id;           ///< e.g., "c260"
    std::string                name;         ///< folder name on disk (sanitized for Windows)
    std::string                displayName;  ///< human-readable name (may contain colons etc.)
    std::string                category;     ///< e.g., "nikke" (from metadata)
    bool                       hasMouthAnim = false;
    std::vector<ModelOutfit>   outfits;
};

class ModelManager
{
public:
    ModelManager();
    ~ModelManager();

    /// Scan the assets directory and build the model catalog.
    /// @param assetsDir  Path to the assets/ directory
    /// @return Number of characters found
    int scan(const std::string& assetsDir);

    /// Scan an additional directory for characters without clearing existing
    /// entries.  Useful for merging installed (read-only) and downloaded
    /// (user-data) character directories.
    /// @param assetsDir  Path to another assets/ directory to scan
    /// @return Number of additional characters found
    int scanAdditional(const std::string& assetsDir);

    /// @return true if catalog has been scanned
    [[nodiscard]] bool isScanned() const noexcept { return m_scanned; }

    /// Get all model entries
    [[nodiscard]] const std::vector<ModelEntry>& entries() const noexcept { return m_entries; }

    /// Find a model by character name (case-insensitive)
    [[nodiscard]] const ModelEntry* findByName(const std::string& name) const;

    /// Find a model by character ID (e.g., "c260")
    [[nodiscard]] const ModelEntry* findById(const std::string& id) const;

    /// List all character names (folder names — use for filesystem operations)
    [[nodiscard]] std::vector<std::string> characterNames() const;

    /// List all character display names (human-readable — use for UI)
    [[nodiscard]] std::vector<std::string> characterDisplayNames() const;

    /// Get the display name for a given folder name
    [[nodiscard]] std::string getDisplayName(const std::string& folderName) const;

    /// Get the folder name for a given display name
    [[nodiscard]] std::string getFolderName(const std::string& displayName) const;

    /// Find the variant (file paths) for a specific character/outfit/stance
    [[nodiscard]] const ModelVariant* findVariant(
        const std::string& characterName,
        const std::string& outfit,
        CharacterStance stance) const;

    /// Get the assets directory this manager was scanned from
    [[nodiscard]] const std::string& assetsDir() const noexcept { return m_assetsDir; }

    /// Get all outfits from metadata for a character (key → display name).
    /// Includes outfits that may not be downloaded yet.
    struct MetadataOutfit {
        std::string key;          ///< e.g., "default", "outfit_01"
        std::string displayName;  ///< e.g., "Rapi", "Crown Naked King"
    };
    [[nodiscard]] std::vector<MetadataOutfit> getMetadataOutfits(const std::string& charName) const;

    /// Get the character ID from metadata for a given character name
    [[nodiscard]] std::string getCharacterId(const std::string& charName) const;

private:
    void scanCharacterDirectory(const std::string& charDir, const std::string& charName);
    void scanOutfitDirectory(const std::string& outfitDir, const std::string& outfitName,
                             ModelEntry& entry);
    void scanStance(const std::string& dir, CharacterStance stance,
                    ModelOutfit& outfit);
    void loadMetadata(const std::string& metadataPath);

    std::vector<ModelEntry> m_entries;
    std::string             m_assetsDir;
    std::atomic<bool>       m_scanned{false};

    // Metadata lookup: character name (lowercase) → metadata
    struct CharMetadata {
        std::string id;
        std::string displayName;
        std::string category;
        bool hasMouthAnimation = false;
        std::unordered_map<std::string, std::string> outfitDisplayNames;
    };
    std::unordered_map<std::string, CharMetadata> m_metadata;      // by display name (lowercase)
    std::unordered_map<std::string, CharMetadata> m_metadataById;  // by charId (e.g. "c940")
    std::unordered_map<std::string, std::string>  m_folderToDisplay; // sanitized folder name → display name

    /// Resolve metadata for a character name, handling disambiguated names like "E.H. (c940)".
    const CharMetadata* findMetadata(const std::string& charName) const;
};

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
