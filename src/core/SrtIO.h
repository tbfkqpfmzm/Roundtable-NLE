/*
 * SrtIO — Import / export SRT subtitle files.
 *
 * Import: parses SRT entries and creates GraphicClip text clips on a video track.
 * Export: scans timeline for GraphicClip text clips and writes SRT entries.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rt {

class Timeline;

struct SrtEntry
{
    int         index{0};
    int64_t     startTick{0};
    int64_t     endTick{0};
    std::string text;
};

/// Parse an SRT file into subtitle entries.
std::vector<SrtEntry> parseSrt(const std::filesystem::path& path);

/// Import SRT entries onto a new video track as GraphicClip text clips.
/// Returns the number of clips created.
int importSrt(Timeline& timeline, const std::vector<SrtEntry>& entries);

/// Export subtitle text clips from the timeline to an SRT file.
/// Scans all video tracks for GraphicClip clips that contain TextLayers.
/// Returns the number of entries written.
int exportSrt(const Timeline& timeline, const std::filesystem::path& path);

} // namespace rt
