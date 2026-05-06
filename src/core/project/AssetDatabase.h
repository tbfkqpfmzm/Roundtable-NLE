/*
 * AssetDatabase — manages all referenced assets in a project.
 *
 * Tracks characters, backgrounds, audio files, and video files.
 * Uses SQLite for persistent asset metadata and quick lookup.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace rt {

/// Asset type discriminator
enum class AssetType : uint8_t
{
    Character,      // Spine character folder
    Background,     // Background image
    Audio,          // Audio file (WAV, FLAC, MP3)
    Video,          // Video file
    Image,          // Static image overlay
    Font,           // Font file for titles
    ImageSequence   // Numbered image sequence (e.g. frame_0001.png - frame_0100.png)
};

/// Alpha interpretation mode
enum class AlphaInterpretation : uint8_t
{
    Auto,           // Detect from file metadata
    Ignore,         // Treat as opaque
    Straight,       // Unassociated alpha
    Premultiplied   // Pre-multiplied alpha
};

/// Per-asset footage interpretation overrides
struct FootageInterpretation
{
    double               overrideFps{0.0};          // 0 = use file's native FPS
    AlphaInterpretation  alpha{AlphaInterpretation::Auto};
    double               pixelAspectRatio{1.0};     // 1.0 = square pixels
};

/// Metadata for a single asset
struct AssetEntry
{
    uint64_t               id{0};
    AssetType              type{AssetType::Image};
    std::string            name;
    std::filesystem::path  path;         // Relative to assets/ directory
    std::filesystem::path  absolutePath; // Resolved at load time
    uint64_t               fileSize{0};
    std::string            hash;         // SHA-256 for integrity checking
    // Image sequence fields (only used when type == ImageSequence)
    std::string            sequencePattern;  // FFmpeg pattern, e.g. "frame_%04d.png"
    int64_t                sequenceStart{0}; // First frame number
    int64_t                sequenceCount{0}; // Total frames in sequence
    // Footage interpretation overrides
    FootageInterpretation  interpretation;
};

/// Character-specific asset info
struct CharacterAsset
{
    std::string                name;      // e.g., "Modernia"
    std::vector<std::string>   outfits;   // e.g., "default", "outfit_01", "outfit_02"
    std::vector<std::string>   stances;   // e.g., "idle", "aim", "cover"
    std::filesystem::path      basePath;  // e.g., "characters/Modernia"
};

class AssetDatabase
{
public:
    AssetDatabase();
    ~AssetDatabase();

    /// Scan a directory for assets and add them to the database
    void scanDirectory(const std::filesystem::path& dir);

    /// Detect and import image sequences in a directory.
    /// Groups numbered image files (e.g. frame_0001.png) into single ImageSequence assets.
    void scanImageSequences(const std::filesystem::path& dir);

    /// Scan the characters directory specifically
    void scanCharacters(const std::filesystem::path& charDir);

    /// Look up an asset by ID
    [[nodiscard]] const AssetEntry* findById(uint64_t id) const;
    [[nodiscard]] AssetEntry* findById(uint64_t id);

    /// Look up an asset by path (relative or absolute)
    [[nodiscard]] AssetEntry* findByPath(const std::filesystem::path& p);

    /// Look up assets by type
    [[nodiscard]] std::vector<const AssetEntry*> findByType(AssetType type) const;

    /// Look up a character by name
    [[nodiscard]] const CharacterAsset* findCharacter(const std::string& name) const;

    /// Get all known characters
    [[nodiscard]] const std::vector<CharacterAsset>& characters() const noexcept;

    /// Add/remove individual assets
    uint64_t addAsset(AssetEntry entry);
    void     removeAsset(uint64_t id);

    /// Check all assets for offline (missing) files.
    /// Returns list of asset IDs whose absolutePath no longer exists.
    [[nodiscard]] std::vector<uint64_t> findOfflineAssets() const;

    /// Attempt to relink an offline asset to a new file path.
    /// Returns true if the file at newPath exists.
    bool relinkAsset(uint64_t id, const std::filesystem::path& newPath);

    /// Total count
    [[nodiscard]] size_t assetCount() const noexcept;

private:
    std::vector<AssetEntry>                            m_assets;
    std::vector<CharacterAsset>                        m_characters;
    std::unordered_map<uint64_t, size_t>               m_idIndex;   // id → m_assets index
    uint64_t                                           m_nextId{1};
};

} // namespace rt
