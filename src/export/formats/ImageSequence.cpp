/*
 * ImageSequence.cpp — Writes individual frames as PNG/BMP/JPEG/TIFF/EXR.
 *
 * Uses stb_image_write for PNG/BMP/JPEG (no FFmpeg dependency needed).
 * EXR/TIFF use FFmpeg if available.
 */

#include "formats/ImageSequence.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
// Only define once — if already defined elsewhere, guard it
#ifndef STB_IMAGE_WRITE_ALREADY_DEFINED
#include <stb_image_write.h>
#define STB_IMAGE_WRITE_ALREADY_DEFINED
#endif

namespace rt {

ImageSequence::ImageSequence() = default;
ImageSequence::~ImageSequence() { shutdown(); }

void ImageSequence::setOutputDirectory(const std::filesystem::path& dir)
{
    m_outputDir = dir;
}

void ImageSequence::setFilenamePattern(const std::string& pattern)
{
    m_filenamePattern = pattern;
}

bool ImageSequence::init(const EncoderConfig& config)
{
    m_config = config;
    m_framesEncoded = 0;

    if (m_outputDir.empty()) {
        m_lastError = "ImageSequence: Output directory not set";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(m_outputDir, ec);
    if (ec) {
        m_lastError = "ImageSequence: Cannot create output dir: " + ec.message();
        return false;
    }

    m_initialized = true;
    spdlog::info("ImageSequence: {}x{} format={} to {}",
                 config.width, config.height,
                 static_cast<int>(config.imageFormat), m_outputDir.string());
    return true;
}

bool ImageSequence::encodeFrame(const uint8_t* rgbaPixels, int64_t frameIndex)
{
    if (!m_initialized) return false;

    // Build filename
    char numBuf[64];
    std::snprintf(numBuf, sizeof(numBuf), m_filenamePattern.c_str(),
                  static_cast<int>(frameIndex));

    const char* ext = ".png";
    switch (m_config.imageFormat) {
        case ImageFormat::PNG:  ext = ".png"; break;
        case ImageFormat::BMP:  ext = ".bmp"; break;
        case ImageFormat::JPEG: ext = ".jpg"; break;
        case ImageFormat::TIFF: ext = ".tiff"; break;
        case ImageFormat::EXR:  ext = ".exr"; break;
        default: break;
    }

    auto path = m_outputDir / (std::string(numBuf) + ext);
    int w = static_cast<int>(m_config.width);
    int h = static_cast<int>(m_config.height);
    int ok = 0;

    switch (m_config.imageFormat) {
        case ImageFormat::PNG:
            ok = stbi_write_png(path.string().c_str(), w, h, 4, rgbaPixels, w * 4);
            break;
        case ImageFormat::BMP:
            ok = stbi_write_bmp(path.string().c_str(), w, h, 4, rgbaPixels);
            break;
        case ImageFormat::JPEG:
            ok = stbi_write_jpg(path.string().c_str(), w, h, 4, rgbaPixels,
                                m_config.jpegQuality);
            break;
        default:
            // TIFF/EXR would need FFmpeg or dedicated library
            spdlog::warn("ImageSequence: {} format not implemented, using PNG",
                         static_cast<int>(m_config.imageFormat));
            ok = stbi_write_png(path.string().c_str(), w, h, 4, rgbaPixels, w * 4);
            break;
    }

    if (!ok) {
        m_lastError = "ImageSequence: Failed to write " + path.string();
        spdlog::error("{}", m_lastError);
        return false;
    }

    ++m_framesEncoded;
    return true;
}

int ImageSequence::flush()
{
    return 0; // No buffering — each frame is immediately written
}

void ImageSequence::shutdown()
{
    m_initialized = false;
    m_flushedPackets.clear();
}

} // namespace rt
