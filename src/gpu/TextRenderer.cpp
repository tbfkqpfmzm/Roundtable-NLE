/*
 * TextRenderer.cpp — SDF text rendering with stb_truetype.
 * Step 20: GPU Text Renderer
 */

#include "TextRenderer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>

// ── stb_truetype (conditionally available via stb target) ────────────────────
#if __has_include("stb_truetype.h")
#define RT_HAS_STB_TRUETYPE 1
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#else
#define RT_HAS_STB_TRUETYPE 0
#endif

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// GlyphAtlas::Impl
// ═════════════════════════════════════════════════════════════════════════════

struct GlyphAtlas::Impl
{
    struct FontSlot
    {
        std::vector<uint8_t> ttfData;       // Owned copy of TTF file bytes
        float fontSize{48.0f};
        float scale{1.0f};                  // stbtt scale factor
        FontMetrics metrics;
        std::unordered_map<uint32_t, GlyphInfo> glyphCache;
        bool hasStbFont{false};

#if RT_HAS_STB_TRUETYPE
        stbtt_fontinfo stbFont{};
#endif
    };

    std::vector<FontSlot> fonts;
    GlyphAtlasConfig config;

    // Atlas pixel buffer (single-channel 8-bit)
    std::vector<uint8_t> pixels;
    bool dirty{true};

    // Shelf packer state
    uint32_t shelfX{0};
    uint32_t shelfY{0};
    uint32_t shelfHeight{0};

    // Fallback glyph (returned for missing codepoints)
    GlyphInfo fallbackGlyph;

    Impl()
    {
        pixels.resize(config.atlasWidth * config.atlasHeight, 0);
        fallbackGlyph.codepoint = 0xFFFD;
        fallbackGlyph.advance = 16.0f;
    }

    // Allocate a rect in the atlas. Returns (false) if no space.
    bool allocateRect(uint32_t w, uint32_t h, uint32_t& outX, uint32_t& outY)
    {
        if (w > config.atlasWidth) return false;

        // Check if it fits in the current shelf
        if (shelfX + w > config.atlasWidth) {
            // Move to next shelf
            shelfY += shelfHeight;
            shelfX = 0;
            shelfHeight = 0;
        }

        if (shelfY + h > config.atlasHeight) return false;

        outX = shelfX;
        outY = shelfY;
        shelfX += w;
        shelfHeight = std::max(shelfHeight, h);
        dirty = true;
        return true;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// GlyphAtlas construction / move
// ═════════════════════════════════════════════════════════════════════════════

GlyphAtlas::GlyphAtlas()
    : m_impl(std::make_unique<Impl>())
{
}

GlyphAtlas::~GlyphAtlas() = default;

GlyphAtlas::GlyphAtlas(GlyphAtlas&&) noexcept = default;
GlyphAtlas& GlyphAtlas::operator=(GlyphAtlas&&) noexcept = default;

// ═════════════════════════════════════════════════════════════════════════════
// Configuration
// ═════════════════════════════════════════════════════════════════════════════

void GlyphAtlas::setConfig(const GlyphAtlasConfig& cfg)
{
    m_impl->config = cfg;
    m_impl->pixels.resize(cfg.atlasWidth * cfg.atlasHeight, 0);
    m_impl->dirty = true;
}

const GlyphAtlasConfig& GlyphAtlas::config() const
{
    return m_impl->config;
}

// ═════════════════════════════════════════════════════════════════════════════
// Font loading
// ═════════════════════════════════════════════════════════════════════════════

FontId GlyphAtlas::addFont(const uint8_t* ttfData, size_t dataSize, float fontSize)
{
    FontId id = static_cast<FontId>(m_impl->fonts.size());
    auto& slot = m_impl->fonts.emplace_back();
    slot.fontSize = fontSize;

#if RT_HAS_STB_TRUETYPE
    if (ttfData && dataSize > 0) {
        slot.ttfData.assign(ttfData, ttfData + dataSize);

        if (stbtt_InitFont(&slot.stbFont, slot.ttfData.data(), 0)) {
            slot.hasStbFont = true;
            slot.scale = stbtt_ScaleForPixelHeight(&slot.stbFont, fontSize);

            int ascent, descent, lineGap;
            stbtt_GetFontVMetrics(&slot.stbFont, &ascent, &descent, &lineGap);

            slot.metrics.ascender  = ascent * slot.scale;
            slot.metrics.descender = descent * slot.scale;
            slot.metrics.lineHeight = (ascent - descent + lineGap) * slot.scale;
            slot.metrics.emSize    = fontSize;
        } else {
            spdlog::warn("TextRenderer: failed to init font (id={})", id);
        }
    }
#else
    (void)ttfData; (void)dataSize;
#endif

    return id;
}

FontId GlyphAtlas::addFontFromFile(const std::string& path, float fontSize)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::warn("TextRenderer: cannot open font file: {}", path);
        return addFont(nullptr, 0, fontSize);
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    return addFont(data.data(), data.size(), fontSize);
}

size_t GlyphAtlas::fontCount() const
{
    return m_impl->fonts.size();
}

// ═════════════════════════════════════════════════════════════════════════════
// Glyph query
// ═════════════════════════════════════════════════════════════════════════════

const GlyphInfo& GlyphAtlas::getGlyph(FontId font, uint32_t codepoint)
{
    if (font >= static_cast<FontId>(m_impl->fonts.size()))
        return m_impl->fallbackGlyph;

    auto& slot = m_impl->fonts[font];
    auto it = slot.glyphCache.find(codepoint);
    if (it != slot.glyphCache.end())
        return it->second;

    // Try generating with stb_truetype
#if RT_HAS_STB_TRUETYPE
    if (slot.hasStbFont) {
        int glyphIndex = stbtt_FindGlyphIndex(&slot.stbFont,
                                               static_cast<int>(codepoint));
        if (glyphIndex == 0 && codepoint != 0) {
            // Missing glyph — cache the fallback
            slot.glyphCache[codepoint] = m_impl->fallbackGlyph;
            return slot.glyphCache[codepoint];
        }

        GlyphInfo info;
        info.codepoint = codepoint;
        info.sdfPadding = static_cast<float>(m_impl->config.sdfPadding);

        // Get glyph metrics
        int advW, lsb;
        stbtt_GetGlyphHMetrics(&slot.stbFont, glyphIndex, &advW, &lsb);
        info.advance = advW * slot.scale;
        info.bearingX = lsb * slot.scale;

        // Generate SDF bitmap
        int sdfW = 0, sdfH = 0, xoff = 0, yoff = 0;
        uint8_t onEdge = static_cast<uint8_t>(m_impl->config.sdfOnEdge);
        float pixDist = m_impl->config.sdfPixelDist;
        int padding = static_cast<int>(m_impl->config.sdfPadding);

        unsigned char* sdfBitmap = stbtt_GetGlyphSDF(
            &slot.stbFont, slot.scale, glyphIndex,
            padding, onEdge, pixDist,
            &sdfW, &sdfH, &xoff, &yoff);

        if (sdfBitmap && sdfW > 0 && sdfH > 0) {
            uint32_t ax = 0, ay = 0;
            if (m_impl->allocateRect(static_cast<uint32_t>(sdfW),
                                      static_cast<uint32_t>(sdfH), ax, ay)) {
                // Copy SDF to atlas
                for (int row = 0; row < sdfH; ++row) {
                    uint32_t dstOffset = (ay + row) * m_impl->config.atlasWidth + ax;
                    std::memcpy(&m_impl->pixels[dstOffset],
                                &sdfBitmap[row * sdfW],
                                static_cast<size_t>(sdfW));
                }

                info.atlasX = static_cast<float>(ax);
                info.atlasY = static_cast<float>(ay);
                info.atlasW = static_cast<float>(sdfW);
                info.atlasH = static_cast<float>(sdfH);
                info.bearingX = static_cast<float>(xoff);
                info.bearingY = static_cast<float>(-yoff);  // Flip Y
                info.valid = true;
            }
            stbtt_FreeSDF(sdfBitmap, nullptr);
        }

        slot.glyphCache[codepoint] = info;
        return slot.glyphCache[codepoint];
    }
#endif

    // No stb font — return fallback
    slot.glyphCache[codepoint] = m_impl->fallbackGlyph;
    return slot.glyphCache[codepoint];
}

FontMetrics GlyphAtlas::getMetrics(FontId font) const
{
    if (font < static_cast<FontId>(m_impl->fonts.size()))
        return m_impl->fonts[font].metrics;
    return {};
}

// ═════════════════════════════════════════════════════════════════════════════
// Atlas texture
// ═════════════════════════════════════════════════════════════════════════════

uint32_t GlyphAtlas::atlasWidth() const  { return m_impl->config.atlasWidth; }
uint32_t GlyphAtlas::atlasHeight() const { return m_impl->config.atlasHeight; }

const uint8_t* GlyphAtlas::atlasPixels() const
{
    return m_impl->pixels.data();
}

bool GlyphAtlas::isDirty() const  { return m_impl->dirty; }
void GlyphAtlas::clearDirty()     { m_impl->dirty = false; }

// ═════════════════════════════════════════════════════════════════════════════
// Manual glyph management (testing)
// ═════════════════════════════════════════════════════════════════════════════

void GlyphAtlas::addManualGlyph(FontId font, uint32_t codepoint, const GlyphInfo& info)
{
    ensureFont(font);
    m_impl->fonts[font].glyphCache[codepoint] = info;
}

void GlyphAtlas::setManualMetrics(FontId font, const FontMetrics& metrics)
{
    ensureFont(font);
    m_impl->fonts[font].metrics = metrics;
}

FontId GlyphAtlas::ensureFont(FontId font)
{
    while (m_impl->fonts.size() <= font)
        m_impl->fonts.emplace_back();
    return font;
}

// ═════════════════════════════════════════════════════════════════════════════
// TextRenderer
// ═════════════════════════════════════════════════════════════════════════════

TextRenderer::TextRenderer()  = default;
TextRenderer::~TextRenderer() = default;

GlyphAtlas& TextRenderer::atlas() noexcept { return m_atlas; }
const GlyphAtlas& TextRenderer::atlas() const noexcept { return m_atlas; }

// ── Text layout ─────────────────────────────────────────────────────────────

TextLayout TextRenderer::layoutText(
    const std::string& text,
    GlyphAtlas& atlas,
    const TextLayoutOptions& options)
{
    TextLayout layout;
    if (text.empty()) return layout;

    FontMetrics metrics = atlas.getMetrics(options.fontId);
    float scale = (metrics.emSize > 0)
        ? options.fontSize / metrics.emSize
        : 1.0f;

    float lineH = metrics.lineHeight * scale * options.lineSpacing;
    if (lineH <= 0) lineH = options.fontSize * options.lineSpacing;

    layout.baseline = metrics.ascender * scale;

    // Lay out glyphs line by line
    struct Line { size_t startIdx; size_t count; float width; };
    std::vector<Line> lines;
    lines.push_back({0, 0, 0.0f});

    float penX = 0.0f;
    float penY = layout.baseline;

    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];

        // Newline
        if (ch == '\n') {
            lines.back().width = penX;
            penX = 0.0f;
            penY += lineH;
            lines.push_back({layout.glyphs.size(), 0, 0.0f});
            continue;
        }

        uint32_t cp = static_cast<uint32_t>(static_cast<unsigned char>(ch));
        const auto& glyph = atlas.getGlyph(options.fontId, cp);

        float advanceScaled = glyph.advance * scale;

        // Word wrap
        if (options.maxWidth > 0 && penX + advanceScaled > options.maxWidth
            && penX > 0) {
            lines.back().width = penX;
            penX = 0.0f;
            penY += lineH;
            lines.push_back({layout.glyphs.size(), 0, 0.0f});
        }

        PositionedGlyph pg;
        pg.glyph = glyph;
        pg.x = penX + glyph.bearingX * scale;
        pg.y = penY - glyph.bearingY * scale;
        pg.scale = scale;
        layout.glyphs.push_back(pg);

        lines.back().count++;
        penX += advanceScaled + options.tracking;
    }

    // Finalize last line
    lines.back().width = penX;
    layout.lineCount = static_cast<int>(lines.size());

    // Compute total width (max line width)
    for (const auto& line : lines)
        layout.width = std::max(layout.width, line.width);

    layout.height = penY - layout.baseline + lineH;

    // Apply horizontal alignment
    if (options.hAlign != TextAlign::Left) {
        for (const auto& line : lines) {
            float offset = 0.0f;
            if (options.hAlign == TextAlign::Center)
                offset = (layout.width - line.width) * 0.5f;
            else if (options.hAlign == TextAlign::Right)
                offset = layout.width - line.width;

            for (size_t g = line.startIdx; g < line.startIdx + line.count; ++g)
                layout.glyphs[g].x += offset;
        }
    }

    return layout;
}

// ── Text measurement ────────────────────────────────────────────────────────

glm::vec2 TextRenderer::measureText(
    const std::string& text,
    GlyphAtlas& atlas,
    const TextLayoutOptions& options)
{
    auto layout = layoutText(text, atlas, options);
    return {layout.width, layout.height};
}

// ── Vertex generation ───────────────────────────────────────────────────────

std::vector<TextVertex> TextRenderer::layoutToVertices(
    const TextLayout& layout,
    const GlyphAtlas& atlas,
    uint32_t color,
    float originX,
    float originY)
{
    std::vector<TextVertex> vertices;
    vertices.reserve(layout.glyphs.size() * 6);

    float aw = static_cast<float>(atlas.atlasWidth());
    float ah = static_cast<float>(atlas.atlasHeight());
    if (aw <= 0 || ah <= 0) return vertices;

    for (const auto& pg : layout.glyphs) {
        if (!pg.glyph.valid && pg.glyph.atlasW <= 0) continue;

        // Quad corners in screen space
        float x0 = originX + pg.x;
        float y0 = originY + pg.y;
        float x1 = x0 + pg.glyph.atlasW * pg.scale;
        float y1 = y0 + pg.glyph.atlasH * pg.scale;

        // UV in atlas
        float u0 = pg.glyph.atlasX / aw;
        float v0 = pg.glyph.atlasY / ah;
        float u1 = (pg.glyph.atlasX + pg.glyph.atlasW) / aw;
        float v1 = (pg.glyph.atlasY + pg.glyph.atlasH) / ah;

        // Two triangles (CCW winding)
        // Triangle 1: top-left, bottom-left, bottom-right
        vertices.push_back({x0, y0, u0, v0, color});
        vertices.push_back({x0, y1, u0, v1, color});
        vertices.push_back({x1, y1, u1, v1, color});

        // Triangle 2: top-left, bottom-right, top-right
        vertices.push_back({x0, y0, u0, v0, color});
        vertices.push_back({x1, y1, u1, v1, color});
        vertices.push_back({x1, y0, u1, v0, color});
    }

    return vertices;
}

// ── Color utilities ─────────────────────────────────────────────────────────

uint32_t TextRenderer::packColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) <<  8) |
           (static_cast<uint32_t>(r));
}

uint32_t TextRenderer::packColorF(float r, float g, float b, float a)
{
    return packColor(
        static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}

void TextRenderer::unpackColor(uint32_t packed, float& r, float& g, float& b, float& a)
{
    r = static_cast<float>((packed >>  0) & 0xFF) / 255.0f;
    g = static_cast<float>((packed >>  8) & 0xFF) / 255.0f;
    b = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
    a = static_cast<float>((packed >> 24) & 0xFF) / 255.0f;
}

} // namespace rt

