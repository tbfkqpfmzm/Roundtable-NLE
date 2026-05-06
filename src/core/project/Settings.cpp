/*
 * Settings.cpp — Project settings implementation.
 * Step 5: Project Serialization
 */

#include "project/Settings.h"

namespace rt {

bool Settings::operator==(const Settings& o) const noexcept
{
    return m_resolution == o.m_resolution
        && m_frameRate  == o.m_frameRate
        && m_colorSpace == o.m_colorSpace
        && m_audio.sampleRate == o.m_audio.sampleRate
        && m_audio.bitDepth   == o.m_audio.bitDepth
        && m_audio.channels   == o.m_audio.channels
        && m_export.codec        == o.m_export.codec
        && m_export.quality      == o.m_export.quality
        && m_export.audioBitrate == o.m_export.audioBitrate
        && m_export.outputPath   == o.m_export.outputPath;
}

} // namespace rt

