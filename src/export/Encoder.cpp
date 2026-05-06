/*
 * Encoder.cpp — Base encoder utilities and factory.
 */

#include "Encoder.h"
#include "formats/H264Encoder.h"
#include "formats/H265Encoder.h"
#include "formats/AV1Encoder.h"
#include "formats/ProResEncoder.h"
#include "formats/DNxHREncoder.h"
#include "formats/ImageSequence.h"

#include <spdlog/spdlog.h>

namespace rt {

const char* encoderCodecName(EncoderCodec codec) noexcept
{
    switch (codec) {
        case EncoderCodec::H264:          return "H.264";
        case EncoderCodec::H265:          return "H.265";
        case EncoderCodec::AV1:           return "AV1";
        case EncoderCodec::ProRes:        return "ProRes";
        case EncoderCodec::DNxHR:          return "DNxHR";
        case EncoderCodec::ImageSequence: return "Image Sequence";
        default:                          return "Unknown";
    }
}

std::unique_ptr<Encoder> Encoder::create(EncoderCodec codec, HardwareAccel hwAccel)
{
    spdlog::info("Encoder: Creating {} encoder (hw={})",
                 encoderCodecName(codec), static_cast<int>(hwAccel));

    switch (codec) {
        case EncoderCodec::H264:
            return std::make_unique<H264Encoder>();
        case EncoderCodec::H265:
            return std::make_unique<H265Encoder>();
        case EncoderCodec::AV1:
            return std::make_unique<AV1Encoder>();
        case EncoderCodec::ProRes:
            return std::make_unique<ProResEncoder>();
        case EncoderCodec::DNxHR:
            return std::make_unique<DNxHREncoder>();
        case EncoderCodec::ImageSequence:
            return std::make_unique<ImageSequence>();
        default:
            spdlog::error("Encoder: Unknown codec {}", static_cast<int>(codec));
            return nullptr;
    }
}

} // namespace rt
