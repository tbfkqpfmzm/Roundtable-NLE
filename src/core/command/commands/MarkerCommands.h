/*
 * MarkerCommands — undo/redo commands for timeline marker operations.
 */

#pragma once

#include <cstdint>
#include <string>

#include "command/Command.h"
#include "timeline/Marker.h"

namespace rt {

class Timeline;

// ── Command type IDs for markers ────────────────────────────────────────────
// Extends CommandTypeId enum range (markers = 500+)
enum class MarkerCommandTypeId : int
{
    AddMarker    = 500,
    RemoveMarker = 501,
};

// ─────────────────────────────────────────────────────────────────────────────
// AddMarkerCommand — adds a marker to the timeline
// ─────────────────────────────────────────────────────────────────────────────
class AddMarkerCommand : public Command
{
public:
    AddMarkerCommand(Timeline* timeline, int64_t time,
                     const std::string& label, uint32_t color = 0xFF4444FF);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override {
        return static_cast<int>(MarkerCommandTypeId::AddMarker);
    }

private:
    Timeline*   m_timeline;
    Marker      m_marker;
};

// ─────────────────────────────────────────────────────────────────────────────
// RemoveMarkerCommand — removes a marker from the timeline by index
// ─────────────────────────────────────────────────────────────────────────────
class RemoveMarkerCommand : public Command
{
public:
    RemoveMarkerCommand(Timeline* timeline, size_t index);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override {
        return static_cast<int>(MarkerCommandTypeId::RemoveMarker);
    }

private:
    Timeline*   m_timeline;
    Marker      m_marker;   // Saved on execute for undo
    size_t      m_index;
};

} // namespace rt
