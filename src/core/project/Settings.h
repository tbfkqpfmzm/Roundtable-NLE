/*
 * Settings — project-level settings for rendering and export.
 *
 * Covers resolution, frame rate, color space, audio format, and export
 * parameters. These are stored as part of the project file.
 */

#pragma once

#include <cstdint>
#include <string>

namespace rt {

/// Color space for the project
enum class ColorSpace : uint8_t
{
    sRGB,       // Default for web/SDR content
    Rec709,     // Broadcast HD
    Rec2020,    // HDR / UHD
    LinearSRGB  // For compositing
};

/// Audio format for the project
struct AudioFormat
{
    uint32_t sampleRate{48000};
    uint32_t bitDepth{16};
    uint32_t channels{2};
};

/// Video resolution
struct Resolution
{
    uint32_t width{1920};
    uint32_t height{1080};

    bool operator==(const Resolution& o) const noexcept
    {
        return width == o.width && height == o.height;
    }
    bool operator!=(const Resolution& o) const noexcept { return !(*this == o); }
};

/// Export encoder settings
struct ExportSettings
{
    std::string codec{"h264_nvenc"};
    uint32_t    quality{23};       // CRF / QP
    uint32_t    audioBitrate{192}; // kbps
    std::string outputPath;
};

/// Complete project settings
class Settings
{
public:
    Settings() = default;
    ~Settings() = default;

    // ── Resolution ──────────────────────────────────────────────────────
    [[nodiscard]] const Resolution& resolution() const noexcept { return m_resolution; }
    void setResolution(const Resolution& r) noexcept { m_resolution = r; }
    void setResolution(uint32_t w, uint32_t h) noexcept { m_resolution = {w, h}; }

    // ── Frame rate ──────────────────────────────────────────────────────
    [[nodiscard]] double frameRate() const noexcept { return m_frameRate; }
    void setFrameRate(double fps) noexcept { m_frameRate = fps; }

    /// Ticks per frame at current frame rate (48000 / fps)
    [[nodiscard]] int64_t ticksPerFrame() const noexcept
    {
        return static_cast<int64_t>(48000.0 / m_frameRate);
    }

    // ── Color space ─────────────────────────────────────────────────────
    [[nodiscard]] ColorSpace colorSpace() const noexcept { return m_colorSpace; }
    void setColorSpace(ColorSpace cs) noexcept { m_colorSpace = cs; }

    // ── Audio ───────────────────────────────────────────────────────────
    [[nodiscard]] const AudioFormat& audioFormat() const noexcept { return m_audio; }
    void setAudioFormat(const AudioFormat& fmt) noexcept { m_audio = fmt; }

    [[nodiscard]] uint32_t sampleRate() const noexcept { return m_audio.sampleRate; }
    [[nodiscard]] uint32_t audioBitDepth() const noexcept { return m_audio.bitDepth; }
    [[nodiscard]] uint32_t audioChannels() const noexcept { return m_audio.channels; }

    // ── Export ──────────────────────────────────────────────────────────
    [[nodiscard]] const ExportSettings& exportSettings() const noexcept { return m_export; }
    [[nodiscard]] ExportSettings& exportSettings() noexcept { return m_export; }
    void setExportSettings(const ExportSettings& es) noexcept { m_export = es; }

    // ── Comparison ──────────────────────────────────────────────────────
    [[nodiscard]] bool operator==(const Settings& o) const noexcept;
    [[nodiscard]] bool operator!=(const Settings& o) const noexcept { return !(*this == o); }

private:
    Resolution     m_resolution{1920, 1080};
    double         m_frameRate{30.0};
    ColorSpace     m_colorSpace{ColorSpace::sRGB};
    AudioFormat    m_audio{48000, 16, 2};
    ExportSettings m_export;
};

} // namespace rt
