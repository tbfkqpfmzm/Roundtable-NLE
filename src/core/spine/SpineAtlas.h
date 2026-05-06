/*
 * SpineAtlas — loads and manages Spine atlas files.
 *
 * Wraps spine-cpp's Atlas class with a dummy TextureLoader so that
 * the skeleton data can be fully loaded at the core layer without
 * any GPU dependency.  The actual Vulkan texture upload happens in
 * the gpu/ SpineRenderer (Step 9).
 *
 * Each SpineAtlas owns:
 *   - The spine::Atlas (parsed .atlas text)
 *   - Texture page metadata (filename, dimensions, pma flag)
 *   - Lookup from region name → atlas region
 */

#pragma once

#ifdef ROUNDTABLE_HAS_SPINE

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// Forward-declare spine types so callers don't need spine headers
namespace spine {
    class Atlas;
    class AtlasPage;
    class AtlasRegion;
    class TextureLoader;
}

namespace rt {

/// Information about a single texture page in the atlas
struct AtlasPageInfo
{
    std::string texturePath;   ///< Relative path to the PNG file
    int         width  = 0;
    int         height = 0;
    bool        pma    = false; ///< Premultiplied alpha
};

/// A single named region within an atlas page
struct AtlasRegionInfo
{
    std::string name;
    int pageIndex = 0;      
    int x = 0, y = 0;
    int width = 0, height = 0;
    int originalWidth = 0, originalHeight = 0;
    int offsetX = 0, offsetY = 0;
    bool rotate = false;
};

class SpineAtlas
{
public:
    SpineAtlas();
    ~SpineAtlas();

    // Non-copyable, movable
    SpineAtlas(const SpineAtlas&) = delete;
    SpineAtlas& operator=(const SpineAtlas&) = delete;
    SpineAtlas(SpineAtlas&&) noexcept;
    SpineAtlas& operator=(SpineAtlas&&) noexcept;

    /// Load atlas from a .atlas file path
    /// @param atlasPath  Absolute path to the .atlas file
    /// @return true on success
    bool load(const std::string& atlasPath);

    /// Load atlas from raw text data
    /// @param data     Atlas text content
    /// @param length   Length in bytes
    /// @param dir      Directory containing the atlas (for resolving texture paths)
    /// @return true on success
    bool loadFromMemory(const char* data, int length, const std::string& dir);

    /// @return true if atlas is loaded
    [[nodiscard]] bool isLoaded() const noexcept { return m_atlas != nullptr; }

    /// Get the underlying spine::Atlas (for SkeletonBinary loading)
    [[nodiscard]] spine::Atlas* getSpineAtlas() const noexcept { return m_atlas.get(); }

    /// Get texture page info
    [[nodiscard]] const std::vector<AtlasPageInfo>& pages() const noexcept { return m_pages; }

    /// Get all regions
    [[nodiscard]] const std::vector<AtlasRegionInfo>& regions() const noexcept { return m_regions; }

    /// Find a region by name (linear search — cache the result)
    [[nodiscard]] const AtlasRegionInfo* findRegion(const std::string& name) const;

    /// The directory path the atlas was loaded from
    [[nodiscard]] const std::string& directory() const noexcept { return m_directory; }

private:
    void extractMetadata();

    // NOTE: m_textureLoader must be declared BEFORE m_atlas so that
    // m_atlas is destroyed first (Atlas destructor calls textureLoader->unload).
    std::unique_ptr<spine::TextureLoader>            m_textureLoader;
    struct SpineAtlasDeleter { void operator()(spine::Atlas* p) const; };
    std::unique_ptr<spine::Atlas, SpineAtlasDeleter> m_atlas;

    std::vector<AtlasPageInfo>    m_pages;
    std::vector<AtlasRegionInfo>  m_regions;
    std::string                   m_directory;
};

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
