/*
 * KeyframeCmds.cpp — Keyframe command implementations.
 * Step 4: Command System
 */

#include "command/commands/KeyframeCmds.h"

#include <algorithm>

namespace rt {

// ── AddKeyframeCommand ──────────────────────────────────────────────────────

AddKeyframeCommand::AddKeyframeCommand(KeyframeTrack<float>* track, int64_t time,
                                       float value, InterpMode interp)
    : m_track(track)
    , m_time(time)
    , m_value(value)
    , m_interp(interp)
{
}

void AddKeyframeCommand::execute()
{
    // Check if there's an existing keyframe at this time (for undo)
    const auto& kfs = m_track->keyframes();
    auto it = std::find_if(kfs.begin(), kfs.end(),
        [this](const Keyframe<float>& kf) { return kf.time == m_time; });

    if (it != kfs.end())
    {
        m_hadExisting = true;
        m_oldKeyframe = *it;
    }
    else
    {
        m_hadExisting = false;
    }

    m_track->addKeyframe(m_time, m_value, m_interp);
}

void AddKeyframeCommand::undo()
{
    if (m_hadExisting)
    {
        // Restore the full old keyframe (including bezier handles)
        m_track->restoreKeyframe(m_oldKeyframe);
    }
    else
    {
        // Remove the keyframe we added
        m_track->removeKeyframeAtTime(m_time);
    }
}

std::string AddKeyframeCommand::description() const
{
    return "Add Keyframe";
}

// ── RemoveKeyframeCommand ───────────────────────────────────────────────────

RemoveKeyframeCommand::RemoveKeyframeCommand(KeyframeTrack<float>* track, int64_t time)
    : m_track(track)
    , m_time(time)
{
}

void RemoveKeyframeCommand::execute()
{
    // Save the keyframe before removing
    const auto& kfs = m_track->keyframes();
    auto it = std::find_if(kfs.begin(), kfs.end(),
        [this](const Keyframe<float>& kf) { return kf.time == m_time; });

    if (it != kfs.end())
    {
        m_savedKeyframe = *it;
        m_track->removeKeyframeAtTime(m_time);
        m_removed = true;
    }
}

void RemoveKeyframeCommand::undo()
{
    if (m_removed)
    {
        // Restore full keyframe including bezier handles
        m_track->restoreKeyframe(m_savedKeyframe);
    }
}

std::string RemoveKeyframeCommand::description() const
{
    return "Remove Keyframe";
}

// ── MoveKeyframeCommand ─────────────────────────────────────────────────────

MoveKeyframeCommand::MoveKeyframeCommand(KeyframeTrack<float>* track,
                                         int64_t oldTime, int64_t newTime, float newValue)
    : m_track(track)
    , m_oldTime(oldTime)
    , m_newTime(newTime)
    , m_newValue(newValue)
{
}

void MoveKeyframeCommand::execute()
{
    // Save full old keyframe state (including bezier handles)
    const auto& kfs = m_track->keyframes();
    auto it = std::find_if(kfs.begin(), kfs.end(),
        [this](const Keyframe<float>& kf) { return kf.time == m_oldTime; });

    if (it != kfs.end())
        m_savedKeyframe = *it;

    // Remove at old time, add at new time
    m_track->removeKeyframeAtTime(m_oldTime);
    m_track->addKeyframe(m_newTime, m_newValue, m_savedKeyframe.interp);
}

void MoveKeyframeCommand::undo()
{
    m_track->removeKeyframeAtTime(m_newTime);
    m_track->restoreKeyframe(m_savedKeyframe);
}

std::string MoveKeyframeCommand::description() const
{
    return "Move Keyframe";
}

bool MoveKeyframeCommand::mergeWith(const Command& next)
{
    auto* moveCmd = dynamic_cast<const MoveKeyframeCommand*>(&next);
    if (!moveCmd || moveCmd->m_track != m_track) return false;

    // Only merge if the next move starts where we ended
    if (moveCmd->m_oldTime != m_newTime) return false;

    m_newTime  = moveCmd->m_newTime;
    m_newValue = moveCmd->m_newValue;
    return true;
}

} // namespace rt
