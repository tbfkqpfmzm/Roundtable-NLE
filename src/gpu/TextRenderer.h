/*
 * TextRenderer — GPU-accelerated SDF text rendering via Vulkan.
 *
 * Step 20: GPU Text Renderer
 *
 * Font loading via stb_truetype → Signed Distance Field glyph atlas →
 * Vulkan pipeline for crisp text at any size.
 *
 * Used for: timecode display, clip labels, title clips, markers, UI overlays.
 *
 * Features:
 *   - SDF glyph atlas (renders crisp at any zoom)
 *   - Per-glyph color via vertex attribute
 *   - Outline + shadow via push constants
 *   - Text measurement and layout (left / center / right alignment)
 *   - Word wrapping at maxWidth
 *   - Shelf-based atlas packer
 *   - Manual glyph insertion for testing (no font file required)
 *
 * Lifecycle (GPU mode):
 *   1. Create TextRenderer
 *   2. Load fonts into atlas: atlas().addFont(data, size, fontSize)
 *   3. Call init() with Vulkan resources
 *   4. Each frame: render(cmd, layout, pushConstants)
 *   5. shutdown()
 *
 * CPU mode (for tests / Qt widgets):
 *   - Use layoutText() / measureText() / layoutToVertices()
 *   - Render with QPainter or export to vertex buffer
 */

#pragma once

#include "timeline/TitleClip.h"     // TextAlign, TextVAlign

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt {

// ── Glyph info in the atlas ─────────────────────────────────────────────────

struct GlyphInfo
{
    uint32_t codepoint{0};

    // Atlas coordinates (pixels, top-left of glyph rect in atlas)
    float atlasX{0}, atlasY{0};
    float atlasW{0}, atlasH{0};

    // Bearing: offset from pen position to top-left of glyph bitmap
    float bearingX{0}, bearingY{0};

    // Horizontal advance to next glyph
    float advance{0};

    // SDF padding around glyph in atlas (pixels)
    float sdfPadding{4.0f};

    // Whether this is a valid (rendered) glyph
    bool valid{false};
};

// ── Font metrics ────────────────────────────────────────────────────────────

struct FontMetrics
{
    float ascender{0};      ///< Distance from baseline to top of tallest glyph
    float descender{0};     ///< Distance from baseline to lowest descender (negative)
    float lineHeight{0};    ///< Full line height (ascender - descender + gap)
    float emSize{0};        ///< Font size in pixels
};

// ── Positioned glyph (output of layout) ─────────────────────────────────────

struct PositionedGlyph
{
    GlyphInfo glyph;
    float x{0};             ///< Screen X position (pen position + bearing)
    float y{0};             ///< Screen Y position
    float scale{1.0f};      ///< Scale from atlas size to desired size
};

// ── Text layout result ──────────────────────────────────────────────────────

struct TextLayout
{
    std::vector<PositionedGlyph> glyphs;
    float width{0};         ///< Total width of laid-out text
    float height{0};        ///< Total height of laid-out text
    float baseline{0};      ///< Y offset to first baseline from top
    int   lineCount{1};     ///< Number of lines
};

// ── Text vertex (matches text.vert) ─────────────────────────────────────────

struct TextVertex
{
    float x, y;             ///< Screen-space position
    float u, v;             ///< Atlas UV coordinates [0,1]
    uint32_t color;         ///< RGBA packed color
};

// ── Push constants (matches text.vert/frag) ─────────────────────────────────

struct TextPushConstants
{
    glm::mat4 mvp{1.0f};                                    // 64 bytes
    glm::vec4 outlineColor{0.0f, 0.0f, 0.0f, 1.0f};        // 16 bytes
    float outlineWidth{0.0f};                                //  4 bytes
    float smoothing{1.0f / 16.0f};                           //  4 bytes
    float shadowOffsetX{0.0f};                               //  4 bytes
    float shadowOffsetY{0.0f};                               //  4 bytes
    glm::vec4 shadowColor{0.0f, 0.0f, 0.0f, 0.5f};          // 16 bytes
};                                                           // Total: 112

static_assert(sizeof(TextPushConstants) <= 128,
              "Push constants must fit in 128 bytes (Vulkan minimum guarantee)");

// ── Text layout options ─────────────────────────────────────────────────────

using FontId = uint32_t;

struct TextLayoutOptions
{
    FontId    fontId{0};
    float     fontSize{48.0f};
    TextAlign  hAlign{TextAlign::Left};
    TextVAlign vAlign{TextVAlign::Top};
    float     maxWidth{0.0f};       ///< 0 = no wrap
    float     tracking{0.0f};       ///< Extra letter spacing (pixels)
    float     lineSpacing{1.2f};    ///< Line height multiplier
};

// ── Glyph atlas configuration ───────────────────────────────────────────────

struct GlyphAtlasConfig
{
    uint32_t atlasWidth{1024};
    uint32_t atlasHeight{1024};
    uint32_t sdfPadding{4};
    uint32_t sdfOnEdge{128};        ///< SDF value at the glyph edge
    float    sdfPixelDist{32.0f};   ///< Pixel distance per SDF unit
};

// ── Render statistics ───────────────────────────────────────────────────────

struct TextRenderStats
{
    uint32_t vertexCount{0};
    uint32_t glyphCount{0};
    uint32_t drawCalls{0};
    float    gpuTimeMs{0.0f};
};

// ═════════════════════════════════════════════════════════════════════════════
// GlyphAtlas — manages the SDF texture atlas
// ═════════════════════════════════════════════════════════════════════════════

class GlyphAtlas
{
public:
    GlyphAtlas();
    ~GlyphAtlas();

    // Non-copyable, movable
    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;
    GlyphAtlas(GlyphAtlas&&) noexcept;
    GlyphAtlas& operator=(GlyphAtlas&&) noexcept;

    // ── Configuration ───────────────────────────────────────────────────

    void setConfig(const GlyphAtlasConfig& config);
    [[nodiscard]] const GlyphAtlasConfig& config() const;

    // ── Font loading ────────────────────────────────────────────────────

    /// Add a font from TTF/OTF data in memory. Returns font ID.
    FontId addFont(const uint8_t* ttfData, size_t dataSize, float fontSize);

    /// Add a font from a file path. Returns font ID.
    FontId addFontFromFile(const std::string& path, float fontSize);

    /// Number of loaded fonts.
    [[nodiscard]] size_t fontCount() const;

    // ── Glyph query ─────────────────────────────────────────────────────

    /// Get glyph info for a codepoint. Generates SDF on first access.
    /// Returns a fallback glyph if the codepoint is not in the font.
    [[nodiscard]] const GlyphInfo& getGlyph(FontId font, uint32_t codepoint);

    /// Get font metrics.
    [[nodiscard]] FontMetrics getMetrics(FontId font) const;

    // ── Atlas texture ───────────────────────────────────────────────────

    [[nodiscard]] uint32_t atlasWidth() const;
    [[nodiscard]] uint32_t atlasHeight() const;

    /// Single-channel atlas pixel data (8-bit SDF values).
    [[nodiscard]] const uint8_t* atlasPixels() const;

    /// True if atlas was modified since last clearDirty().
    [[nodiscard]] bool isDirty() const;
    void clearDirty();

    // ── Manual glyph management (for testing, no font file needed) ──────

    /// Insert a glyph manually (bypasses font loading / SDF generation).
    void addManualGlyph(FontId font, uint32_t codepoint, const GlyphInfo& info);

    /// Set font metrics manually.
    void setManualMetrics(FontId font, const FontMetrics& metrics);

    /// Ensure a font ID exists (creates an empty slot if needed).
    FontId ensureFont(FontId font);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ═════════════════════════════════════════════════════════════════════════════
// TextRenderer — text layout + GPU rendering
// ═════════════════════════════════════════════════════════════════════════════

class TextRenderer
{
public:
    TextRenderer();
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // ── Atlas access ────────────────────────────────────────────────────

    [[nodiscard]] GlyphAtlas& atlas() noexcept;
    [[nodiscard]] const GlyphAtlas& atlas() const noexcept;

    // ── CPU text layout ─────────────────────────────────────────────────

    /// Lay out text into positioned glyphs.
    [[nodiscard]] static TextLayout layoutText(
        const std::string& text,
        GlyphAtlas& atlas,
        const TextLayoutOptions& options);

    /// Measure text dimensions without full layout.
    [[nodiscard]] static glm::vec2 measureText(
        const std::string& text,
        GlyphAtlas& atlas,
        const TextLayoutOptions& options);

    /// Convert a text layout into GPU vertices (6 per glyph, 2 triangles).
    [[nodiscard]] static std::vector<TextVertex> layoutToVertices(
        const TextLayout& layout,
        const GlyphAtlas& atlas,
        uint32_t color,
        float originX = 0.0f,
        float originY = 0.0f);

    // ── Color utilities ─────────────────────────────────────────────────

    /// Pack RGBA bytes into a uint32_t.
    [[nodiscard]] static uint32_t packColor(
        uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

    /// Pack RGBA floats [0,1] into a uint32_t.
    [[nodiscard]] static uint32_t packColorF(
        float r, float g, float b, float a = 1.0f);

    /// Unpack a uint32_t into RGBA floats [0,1].
    static void unpackColor(uint32_t packed,
                            float& r, float& g, float& b, float& a);

    // ── GPU lifecycle ───────────────────────────────────────────────────
    // (Requires Vulkan — not used in unit tests)

    // bool init(Device& device, Allocator& allocator, CommandPool& cmdPool,
    //           VkQueue graphicsQueue);
    // void shutdown();
    // [[nodiscard]] bool isInitialized() const noexcept;
    // void uploadAtlas();   // Upload modified atlas to GPU texture
    // void render(VkCommandBuffer cmd, const TextLayout& layout,
    //             const TextPushConstants& pc, uint32_t color);
    // [[nodiscard]] const TextRenderStats& stats() const noexcept;

private:
    GlyphAtlas m_atlas;
};

} // namespace rt
