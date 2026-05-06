/*
 * Timeline.cpp — Timeline data model implementation.
 * Step 3: Core Data Model
 */

#include "timeline/Timeline.h"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace rt {

// ── Construction ────────────────────────────────────────────────────────────

Timeline::Timeline()  = default;
Timeline::~Timeline() = default;

// ── Track management ────────────────────────────────────────────────────────

Track* Timeline::addVideoTrack(const std::string& name)
{
    std::string trackName = name;
    if (trackName.empty()) {
        // Count existing video tracks to get the next number
        int videoCount = 0;
        for (const auto& t : m_tracks)
            if (t->type() == TrackType::Video) ++videoCount;
        trackName = "V" + std::to_string(videoCount + 1);
    }
    // Insert before the first audio track so video tracks always stay
    // above audio tracks (Premiere Pro convention).
    size_t insertIdx = m_tracks.size();
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i]->type() == TrackType::Audio) {
            insertIdx = i;
            break;
        }
    }
    auto track = std::make_unique<Track>(TrackType::Video, trackName);
    auto* ptr = track.get();
    m_tracks.insert(m_tracks.begin() + static_cast<ptrdiff_t>(insertIdx),
                    std::move(track));
    notifyTrackAdded(insertIdx);
    spdlog::debug("Added video track '{}' at index {}", trackName, insertIdx);
    return ptr;
}

Track* Timeline::addAudioTrack(const std::string& name)
{
    spdlog::info("[Timeline] ENTER addAudioTrack: name='{}'", name);
    std::string trackName = name;
    if (trackName.empty()) {
        // Count existing audio tracks to get the next number
        int audioCount = 0;
        for (const auto& t : m_tracks)
            if (t->type() == TrackType::Audio) ++audioCount;
        trackName = "A" + std::to_string(audioCount + 1);
    }
    m_tracks.push_back(std::make_unique<Track>(TrackType::Audio, trackName));
    size_t idx = m_tracks.size() - 1;
    spdlog::info("[Timeline] addAudioTrack: created Track ptr={} name='{}' idx={}", (void*)m_tracks.back().get(), trackName, idx);
    notifyTrackAdded(idx);
    spdlog::debug("Added audio track '{}' at index {}", trackName, idx);
    return m_tracks.back().get();
}

Track* Timeline::addDividerTrack(size_t insertIndex)
{
    auto track = std::make_unique<Track>(TrackType::Video, "");
    track->setDivider(true);
    track->setHeight(20.0f); // 1/4 of default 80
    track->setTargeted(false);
    track->setSyncLocked(false);
    if (insertIndex > m_tracks.size()) insertIndex = m_tracks.size();
    auto* ptr = track.get();
    m_tracks.insert(m_tracks.begin() + static_cast<ptrdiff_t>(insertIndex),
                    std::move(track));
    notifyTrackAdded(insertIndex);
    spdlog::debug("Added divider track at index {}", insertIndex);
    return ptr;
}

void Timeline::removeTrack(size_t index)
{
    if (index >= m_tracks.size()) return;
    m_tracks.erase(m_tracks.begin() + static_cast<ptrdiff_t>(index));
    notifyTrackRemoved(index);
}

Track* Timeline::insertTrack(size_t index, std::unique_ptr<Track> track)
{
    if (!track) return nullptr;
    if (index > m_tracks.size()) index = m_tracks.size();
    auto* ptr = track.get();
    m_tracks.insert(m_tracks.begin() + static_cast<ptrdiff_t>(index), std::move(track));
    notifyTrackAdded(index);
    return ptr;
}

std::unique_ptr<Track> Timeline::takeTrack(size_t index)
{
    if (index >= m_tracks.size()) return nullptr;
    auto track = std::move(m_tracks[index]);
    m_tracks.erase(m_tracks.begin() + static_cast<ptrdiff_t>(index));
    notifyTrackRemoved(index);
    return track;
}

void Timeline::moveTrack(size_t from, size_t to)
{
    if (from >= m_tracks.size() || to >= m_tracks.size() || from == to) return;

    auto track = std::move(m_tracks[from]);
    m_tracks.erase(m_tracks.begin() + static_cast<ptrdiff_t>(from));
    m_tracks.insert(m_tracks.begin() + static_cast<ptrdiff_t>(to), std::move(track));

    for (auto* obs : m_observers)
        obs->onTrackMoved(from, to);
}

void Timeline::sortTracksByType()
{
    // Stable-partition: video tracks first, audio tracks second.
    // Dividers are treated as video (sort with whichever section they
    // currently sit in) — kept stable so they don't jump.
    std::stable_partition(m_tracks.begin(), m_tracks.end(),
        [](const std::unique_ptr<Track>& t) {
            return t->isDivider() || t->type() == TrackType::Video;
        });
}

size_t Timeline::trackCount() const noexcept
{
    return m_tracks.size();
}

Track* Timeline::track(size_t index) noexcept
{
    return index < m_tracks.size() ? m_tracks[index].get() : nullptr;
}

const Track* Timeline::track(size_t index) const noexcept
{
    return index < m_tracks.size() ? m_tracks[index].get() : nullptr;
}

// ── Marker management ───────────────────────────────────────────────────────

void Timeline::addMarker(TimeTick time, const std::string& label, uint32_t color)
{
    Marker m;
    m.time  = time;
    m.label = label;
    m.color = color;

    // Insert sorted by time
    auto it = std::lower_bound(m_markers.begin(), m_markers.end(), m);
    m_markers.insert(it, m);

    for (auto* obs : m_observers)
        obs->onMarkerChanged();
}

void Timeline::removeMarker(size_t index)
{
    if (index >= m_markers.size()) return;
    m_markers.erase(m_markers.begin() + static_cast<ptrdiff_t>(index));

    for (auto* obs : m_observers)
        obs->onMarkerChanged();
}

const std::vector<Marker>& Timeline::markers() const noexcept
{
    return m_markers;
}

// ── Playback state ──────────────────────────────────────────────────────────

TimeTick Timeline::playheadPosition() const noexcept
{
    return m_playhead;
}

void Timeline::setPlayheadPosition(TimeTick pos) noexcept
{
    if (m_playhead != pos)
    {
        m_playhead = pos;
        for (auto* obs : m_observers)
            obs->onPlayheadChanged(m_playhead);
    }
}

TimeTick Timeline::inPoint() const noexcept  { return m_inPoint; }
TimeTick Timeline::outPoint() const noexcept { return m_outPoint; }

void Timeline::setInPoint(TimeTick t) noexcept
{
    m_inPoint = t;
    for (auto* obs : m_observers)
        obs->onInOutChanged();
}

void Timeline::setOutPoint(TimeTick t) noexcept
{
    m_outPoint = t;
    for (auto* obs : m_observers)
        obs->onInOutChanged();
}

void Timeline::clearInOutPoints() noexcept
{
    m_inPoint  = -1;
    m_outPoint = -1;
    for (auto* obs : m_observers)
        obs->onInOutChanged();
}

TimeTick Timeline::duration() const noexcept
{
    TimeTick maxDur = 0;
    for (const auto& t : m_tracks)
    {
        TimeTick d = t->duration();
        if (d > maxDur) maxDur = d;
    }
    return maxDur;
}

// ── Observer ────────────────────────────────────────────────────────────────

void Timeline::addObserver(TimelineObserver* obs)
{
    if (obs && std::find(m_observers.begin(), m_observers.end(), obs) == m_observers.end())
        m_observers.push_back(obs);
}

void Timeline::removeObserver(TimelineObserver* obs)
{
    m_observers.erase(
        std::remove(m_observers.begin(), m_observers.end(), obs),
        m_observers.end()
    );
}

// ── Serialization ───────────────────────────────────────────────────────────

const std::string& Timeline::name() const noexcept { return m_name; }
void Timeline::setName(const std::string& name) { m_name = name; }

// ── Notifications ───────────────────────────────────────────────────────────

void Timeline::notifyTrackAdded(size_t index)
{
    for (auto* obs : m_observers)
        obs->onTrackAdded(index);
}

void Timeline::notifyTrackRemoved(size_t index)
{
    for (auto* obs : m_observers)
        obs->onTrackRemoved(index);
}

void Timeline::notifyStructureChanged()
{
    for (auto* obs : m_observers)
        obs->onTimelineStructureChanged();
}

} // namespace rt

