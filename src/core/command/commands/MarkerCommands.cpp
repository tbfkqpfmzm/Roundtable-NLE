/*
 * MarkerCommands — implementation of marker undo/redo commands.
 */

#include "command/commands/MarkerCommands.h"
#include "timeline/Timeline.h"

#include <algorithm>

namespace rt {

// ─── AddMarkerCommand ────────────────────────────────────────────────────────

AddMarkerCommand::AddMarkerCommand(Timeline* timeline, int64_t time,
                                   const std::string& label, uint32_t color)
    : m_timeline(timeline)
{
    m_marker.time  = time;
    m_marker.label = label;
    m_marker.color = color;
}

void AddMarkerCommand::execute()
{
    m_timeline->addMarker(m_marker.time, m_marker.label, m_marker.color);
}

void AddMarkerCommand::undo()
{
    // Find the marker we added by matching time
    const auto& markers = m_timeline->markers();
    for (size_t i = 0; i < markers.size(); ++i) {
        if (markers[i].time == m_marker.time && markers[i].label == m_marker.label) {
            m_timeline->removeMarker(i);
            return;
        }
    }
}

std::string AddMarkerCommand::description() const
{
    return "Add Marker";
}

// ─── RemoveMarkerCommand ─────────────────────────────────────────────────────

RemoveMarkerCommand::RemoveMarkerCommand(Timeline* timeline, size_t index)
    : m_timeline(timeline)
    , m_index(index)
{
}

void RemoveMarkerCommand::execute()
{
    // Save marker data before removing
    const auto& markers = m_timeline->markers();
    if (m_index < markers.size()) {
        m_marker = markers[m_index];
    }
    m_timeline->removeMarker(m_index);
}

void RemoveMarkerCommand::undo()
{
    // Re-add the saved marker
    m_timeline->addMarker(m_marker.time, m_marker.label, m_marker.color);
}

std::string RemoveMarkerCommand::description() const
{
    return "Remove Marker";
}

} // namespace rt
