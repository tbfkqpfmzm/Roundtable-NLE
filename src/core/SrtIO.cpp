/*
 * SrtIO.cpp — SRT subtitle import / export implementation.
 */

#ifdef _MSC_VER
#  pragma warning(disable : 4996) // sscanf — format strings are fixed/safe
#endif

#include "SrtIO.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  SRT timestamp parsing helpers
// ═════════════════════════════════════════════════════════════════════════════

static int64_t srtTimestampToTicks(const std::string& ts)
{
    // Format: HH:MM:SS,mmm
    int h = 0, m = 0, s = 0, ms = 0;
    if (std::sscanf(ts.c_str(), "%d:%d:%d,%d", &h, &m, &s, &ms) != 4) {
        // Try with dot separator (some SRT files use it)
        std::sscanf(ts.c_str(), "%d:%d:%d.%d", &h, &m, &s, &ms);
    }
    double totalSeconds = h * 3600.0 + m * 60.0 + s + ms / 1000.0;
    return static_cast<int64_t>(totalSeconds * kTicksPerSecond);
}

static std::string ticksToSrtTimestamp(int64_t ticks)
{
    double totalSeconds = static_cast<double>(ticks) / kTicksPerSecond;
    int totalMs = static_cast<int>(totalSeconds * 1000.0 + 0.5);

    int h  = totalMs / 3600000; totalMs %= 3600000;
    int m  = totalMs / 60000;   totalMs %= 60000;
    int s  = totalMs / 1000;
    int ms = totalMs % 1000;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d,%03d", h, m, s, ms);
    return buf;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Parse SRT
// ═════════════════════════════════════════════════════════════════════════════

std::vector<SrtEntry> parseSrt(const std::filesystem::path& path)
{
    std::vector<SrtEntry> entries;

    std::ifstream ifs(path);
    if (!ifs.is_open()) return entries;

    std::string line;
    enum State { ExpectIndex, ExpectTimestamp, ExpectText } state = ExpectIndex;
    SrtEntry current;

    while (std::getline(ifs, line)) {
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        switch (state) {
        case ExpectIndex: {
            if (line.empty()) continue;
            // Try to parse as integer index
            try {
                current.index = std::stoi(line);
                state = ExpectTimestamp;
            } catch (...) {
                // Skip non-numeric lines
            }
            break;
        }
        case ExpectTimestamp: {
            // Parse "HH:MM:SS,mmm --> HH:MM:SS,mmm"
            auto arrowPos = line.find("-->");
            if (arrowPos == std::string::npos) break;

            std::string startStr = line.substr(0, arrowPos);
            std::string endStr   = line.substr(arrowPos + 3);

            // Trim whitespace
            while (!startStr.empty() && startStr.back() == ' ') startStr.pop_back();
            while (!endStr.empty() && endStr.front() == ' ') endStr.erase(0, 1);

            current.startTick = srtTimestampToTicks(startStr);
            current.endTick   = srtTimestampToTicks(endStr);
            current.text.clear();
            state = ExpectText;
            break;
        }
        case ExpectText: {
            if (line.empty()) {
                // Empty line = end of entry
                if (!current.text.empty())
                    entries.push_back(current);
                current = SrtEntry{};
                state = ExpectIndex;
            } else {
                if (!current.text.empty())
                    current.text += '\n';
                current.text += line;
            }
            break;
        }
        }
    }

    // Don't forget the last entry if file doesn't end with blank line
    if (state == ExpectText && !current.text.empty())
        entries.push_back(current);

    return entries;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Import SRT → GraphicClip text clips
// ═════════════════════════════════════════════════════════════════════════════

int importSrt(Timeline& timeline, const std::vector<SrtEntry>& entries)
{
    if (entries.empty()) return 0;

    // Create a new video track for subtitles
    Track* track = timeline.addVideoTrack("Subtitles");
    if (!track) return 0;

    int count = 0;
    for (const auto& entry : entries) {
        auto clip = std::make_unique<GraphicClip>();
        clip->setLabel("Subtitle");
        clip->setTimelineIn(entry.startTick);
        clip->setDuration(entry.endTick - entry.startTick);

        // Add a TextLayer with the subtitle text
        auto* textLayer = clip->addTextLayer(entry.text);
        if (textLayer) {
            textLayer->setFontSize(32.0f);
            textLayer->setAlignment(GTextAlign::Center);
            textLayer->setVAlignment(GTextVAlign::Bottom);
            // White text with default appearance (typically includes a shadow)
        }

        track->addClip(std::move(clip));
        ++count;
    }

    return count;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Export SRT ← timeline text clips
// ═════════════════════════════════════════════════════════════════════════════

int exportSrt(const Timeline& timeline, const std::filesystem::path& path)
{
    // Collect all GraphicClip text clips from video tracks
    struct SubEntry {
        int64_t     start;
        int64_t     end;
        std::string text;
    };
    std::vector<SubEntry> subs;

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* track = timeline.track(ti);
        if (!track || track->type() != TrackType::Video) continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const Clip* clip = track->clip(ci);
            if (!clip || clip->clipType() != ClipType::Graphic) continue;

            const auto* gc = dynamic_cast<const GraphicClip*>(clip);
            if (!gc) continue;

            // Collect text from all TextLayers
            std::string text;
            for (size_t li = 0; li < gc->layerCount(); ++li) {
                const auto* layer = gc->layer(li);
                if (!layer || layer->layerType() != GraphicLayerType::Text) continue;

                const auto* tl = dynamic_cast<const TextLayer*>(layer);
                if (!tl) continue;

                if (!text.empty()) text += '\n';
                text += tl->text();
            }

            if (!text.empty()) {
                subs.push_back({clip->timelineIn(), clip->timelineOut(), text});
            }
        }
    }

    if (subs.empty()) return 0;

    // Sort by start time
    std::sort(subs.begin(), subs.end(),
              [](const SubEntry& a, const SubEntry& b) { return a.start < b.start; });

    // Write SRT file
    std::ofstream ofs(path);
    if (!ofs.is_open()) return 0;

    for (size_t i = 0; i < subs.size(); ++i) {
        ofs << (i + 1) << "\n";
        ofs << ticksToSrtTimestamp(subs[i].start) << " --> "
            << ticksToSrtTimestamp(subs[i].end) << "\n";
        ofs << subs[i].text << "\n\n";
    }

    return static_cast<int>(subs.size());
}

} // namespace rt
