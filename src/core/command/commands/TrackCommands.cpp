/*
 * TrackCommands.cpp — Track command implementations.
 * Step 4: Command System
 */

#include "command/commands/TrackCommands.h"
#include "timeline/Timeline.h"

namespace rt {

// ── AddTrackCommand ─────────────────────────────────────────────────────────

AddTrackCommand::AddTrackCommand(Timeline* timeline, TrackType type, const std::string& name)
    : m_timeline(timeline)
    , m_type(type)
    , m_name(name)
{
}

void AddTrackCommand::execute()
{
    if (m_track)
    {
        // Re-executing (redo): insert the saved track at the saved index
        m_trackPtr = m_timeline->insertTrack(m_index, std::move(m_track));
        m_track = nullptr;
    }
    else
    {
        // First execution: create a new track
        if (m_type == TrackType::Video)
            m_trackPtr = m_timeline->addVideoTrack(m_name);
        else
            m_trackPtr = m_timeline->addAudioTrack(m_name);

        m_index = m_timeline->trackCount() - 1;
    }
}

void AddTrackCommand::undo()
{
    m_track = m_timeline->takeTrack(m_index);
    m_trackPtr = nullptr;
}

std::string AddTrackCommand::description() const
{
    return m_type == TrackType::Video ? "Add Video Track" : "Add Audio Track";
}

// ── RemoveTrackCommand ──────────────────────────────────────────────────────

RemoveTrackCommand::RemoveTrackCommand(Timeline* timeline, size_t index)
    : m_timeline(timeline)
    , m_index(index)
{
}

void RemoveTrackCommand::execute()
{
    m_track = m_timeline->takeTrack(m_index);
}

void RemoveTrackCommand::undo()
{
    if (m_track)
    {
        m_timeline->insertTrack(m_index, std::move(m_track));
        m_track = nullptr;
    }
}

std::string RemoveTrackCommand::description() const
{
    return "Remove Track";
}

// ── MoveTrackCommand ────────────────────────────────────────────────────────

MoveTrackCommand::MoveTrackCommand(Timeline* timeline, size_t from, size_t to)
    : m_timeline(timeline)
    , m_from(from)
    , m_to(to)
{
}

void MoveTrackCommand::execute()
{
    m_timeline->moveTrack(m_from, m_to);
}

void MoveTrackCommand::undo()
{
    m_timeline->moveTrack(m_to, m_from);
}

std::string MoveTrackCommand::description() const
{
    return "Move Track";
}

} // namespace rt

