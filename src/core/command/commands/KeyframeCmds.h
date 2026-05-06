/*
 * KeyframeCmds — undo/redo commands for keyframe operations.
 *
 * These work on KeyframeTrack<float> since all animated properties in the
 * NLE use float tracks (opacity, position, scale, rotation, volume, etc.).
 */

#pragma once

#include <cstdint>
#include <string>

#include "command/Command.h"
#include "command/commands/ClipCommands.h"    // CommandTypeId
#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"

namespace rt {

// ─────────────────────────────────────────────────────────────────────────────
// AddKeyframeCommand — adds or replaces a keyframe on a float track
// ─────────────────────────────────────────────────────────────────────────────
class AddKeyframeCommand : public Command
{
public:
    AddKeyframeCommand(KeyframeTrack<float>* track, int64_t time, float value,
                       InterpMode interp = InterpMode::Linear);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::AddKeyframe); }

private:
    KeyframeTrack<float>* m_track;
    int64_t               m_time;
    float                 m_value;
    InterpMode            m_interp;

    bool                  m_hadExisting{false};
    Keyframe<float>       m_oldKeyframe; // Saved if we replaced an existing one
};

// ─────────────────────────────────────────────────────────────────────────────
// RemoveKeyframeCommand — removes a keyframe at a specific time
// ─────────────────────────────────────────────────────────────────────────────
class RemoveKeyframeCommand : public Command
{
public:
    RemoveKeyframeCommand(KeyframeTrack<float>* track, int64_t time);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::RemoveKeyframe); }

private:
    KeyframeTrack<float>* m_track;
    int64_t               m_time;
    bool                  m_removed{false};
    Keyframe<float>       m_savedKeyframe;
};

// ─────────────────────────────────────────────────────────────────────────────
// MoveKeyframeCommand — moves a keyframe to a new time and/or value
// ─────────────────────────────────────────────────────────────────────────────
class MoveKeyframeCommand : public Command
{
public:
    MoveKeyframeCommand(KeyframeTrack<float>* track,
                        int64_t oldTime, int64_t newTime, float newValue);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override { return static_cast<int>(CommandTypeId::MoveKeyframe); }
    bool mergeWith(const Command& next) override;

private:
    KeyframeTrack<float>* m_track;
    int64_t               m_oldTime;
    int64_t               m_newTime;
    float                 m_newValue;
    Keyframe<float>       m_savedKeyframe; // Full keyframe at m_oldTime (saved on first execute)
};

} // namespace rt
