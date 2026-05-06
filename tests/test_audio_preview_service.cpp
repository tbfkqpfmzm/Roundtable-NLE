#include "media/AudioPreviewService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace rt {
namespace {

void writeU16(std::ofstream& out, uint16_t value)
{
    out.put(static_cast<char>(value & 0xff));
    out.put(static_cast<char>((value >> 8) & 0xff));
}

void writeU32(std::ofstream& out, uint32_t value)
{
    out.put(static_cast<char>(value & 0xff));
    out.put(static_cast<char>((value >> 8) & 0xff));
    out.put(static_cast<char>((value >> 16) & 0xff));
    out.put(static_cast<char>((value >> 24) & 0xff));
}

std::filesystem::path writeTestWav()
{
    const auto path = std::filesystem::temp_directory_path() / "roundtable_audio_preview_test.wav";
    constexpr uint32_t sampleRate = 48000;
    constexpr uint16_t channels = 1;
    constexpr uint16_t bitsPerSample = 16;
    constexpr uint32_t frameCount = 2048;
    constexpr uint32_t dataBytes = frameCount * channels * (bitsPerSample / 8);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    writeU32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(out, 16);
    writeU16(out, 1);
    writeU16(out, channels);
    writeU32(out, sampleRate);
    writeU32(out, sampleRate * channels * (bitsPerSample / 8));
    writeU16(out, channels * (bitsPerSample / 8));
    writeU16(out, bitsPerSample);
    out.write("data", 4);
    writeU32(out, dataBytes);

    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const int16_t sample = (frame % 64 < 32) ? 12000 : -12000;
        writeU16(out, static_cast<uint16_t>(sample));
    }

    return path;
}

std::filesystem::path writeStereoTestWav()
{
    const auto path = std::filesystem::temp_directory_path() / "roundtable_audio_preview_stereo_test.wav";
    constexpr uint32_t sampleRate = 44100;
    constexpr uint16_t channels = 2;
    constexpr uint16_t bitsPerSample = 16;
    constexpr uint32_t frameCount = 128;
    constexpr uint32_t dataBytes = frameCount * channels * (bitsPerSample / 8);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    writeU32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(out, 16);
    writeU16(out, 1);
    writeU16(out, channels);
    writeU32(out, sampleRate);
    writeU32(out, sampleRate * channels * (bitsPerSample / 8));
    writeU16(out, channels * (bitsPerSample / 8));
    writeU16(out, bitsPerSample);
    out.write("data", 4);
    writeU32(out, dataBytes);

    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        writeU16(out, static_cast<uint16_t>(10000));
        writeU16(out, static_cast<uint16_t>(-5000));
    }

    return path;
}

TEST(AudioPreviewServiceTest, EmptyRequestsReturnInvalidResults)
{
    AudioPreviewService service;

    EXPECT_FALSE(service.loadFullResampled({}).valid());
    EXPECT_FALSE(service.loadRegionResampled({}).valid());
    EXPECT_FALSE(service.buildWaveformEnvelope({}).valid());
}

TEST(AudioPreviewServiceTest, LoadsPreviewBuffersAndWaveformEnvelope)
{
    const auto path = writeTestWav();
    AudioPreviewService service;

    const auto full = service.loadFullResampled({path, 48000});
    ASSERT_TRUE(full.valid());
    EXPECT_EQ(full.channels, 1u);
    EXPECT_EQ(full.sampleRate, 48000u);
    EXPECT_GE(full.samples.size(), 2048u);

    const auto region = service.loadRegionResampled({path, 128, 512, 48000});
    ASSERT_TRUE(region.valid());
    EXPECT_EQ(region.channels, 1u);
    EXPECT_GE(region.samples.size(), 512u);

    const auto waveform = service.buildWaveformEnvelope({path, 48000, 128});
    ASSERT_TRUE(waveform.valid());
    EXPECT_EQ(waveform.channels, 1u);
    EXPECT_EQ(waveform.framesPerPeak, 128);
    EXPECT_FALSE(waveform.peaks.empty());
    EXPECT_TRUE(std::all_of(waveform.peaks.begin(), waveform.peaks.end(), [](float peak) {
        return peak >= 0.0f && peak <= 1.0f;
    }));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(AudioPreviewServiceTest, LoadsNativeMonoBufferForAudioSyncCache)
{
    const auto path = writeStereoTestWav();
    AudioPreviewService service;

    const auto mono = service.loadFullNativeMono({path});
    ASSERT_TRUE(mono.valid());
    EXPECT_EQ(mono.channels, 1u);
    EXPECT_EQ(mono.sampleRate, 44100u);
    ASSERT_EQ(mono.samples.size(), 128u);
    EXPECT_NEAR(mono.samples.front(), 2500.0f / 32768.0f, 0.001f);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace
} // namespace rt
