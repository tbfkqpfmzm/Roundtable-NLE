/*
 * ClipCommands.cpp — Clip command implementations.
 * Step 4: Command System
 */

#include "command/commands/ClipCommands.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"

namespace rt {

// ── AddClipCommand ──────────────────────────────────────────────────────────

AddClipCommand::AddClipCommand(Track* track, std::unique_ptr<Clip> clip)
    : m_track(track)
    , m_clip(std::move(clip))
    , m_clipId(m_clip ? m_clip->id() : 0)
{
}

void AddClipCommand::execute()
{
    if (m_clip)
    {
        m_track->addClip(std::move(m_clip));
        m_clip = nullptr; // Track now owns it
    }
}

void AddClipCommand::undo()
{
    m_clip = m_track->removeClipById(m_clipId);
}

std::string AddClipCommand::description() const
{
    return "Add Clip";
}

// ── RemoveClipCommand ───────────────────────────────────────────────────────

RemoveClipCommand::RemoveClipCommand(Track* track, uint64_t clipId)
    : m_track(track)
    , m_clipId(clipId)
{
}

void RemoveClipCommand::execute()
{
    m_clip = m_track->removeClipById(m_clipId);
}

void RemoveClipCommand::undo()
{
    if (m_clip)
    {
        m_track->addClip(std::move(m_clip));
        m_clip = nullptr;
    }
}

std::string RemoveClipCommand::description() const
{
    return "Remove Clip";
}

// ── MoveClipCommand ─────────────────────────────────────────────────────────

MoveClipCommand::MoveClipCommand(Track* track, uint64_t clipId, int64_t newPosition)
    : m_track(track)
    , m_clipId(clipId)
    , m_oldPosition(0)
    , m_newPosition(newPosition)
{
}

void MoveClipCommand::execute()
{
    size_t idx = m_track->findClipIndexById(m_clipId);
    if (idx == m_track->clipCount()) return;

    m_oldPosition = m_track->clip(idx)->timelineIn();
    m_track->moveClip(idx, m_newPosition);
}

void MoveClipCommand::undo()
{
    size_t idx = m_track->findClipIndexById(m_clipId);
    if (idx == m_track->clipCount()) return;

    m_track->moveClip(idx, m_oldPosition);
}

std::string MoveClipCommand::description() const
{
    return "Move Clip";
}

bool MoveClipCommand::mergeWith(const Command& next)
{
    auto* moveCmd = dynamic_cast<const MoveClipCommand*>(&next);
    if (!moveCmd) return false;
    if (moveCmd->m_clipId != m_clipId || moveCmd->m_track != m_track) return false;

    // Absorb the new destination, keep our original old position
    m_newPosition = moveCmd->m_newPosition;
    return true;
}

// ── TrimClipCommand ─────────────────────────────────────────────────────────

TrimClipCommand::TrimClipCommand(Track* track, uint64_t clipId,
                                 int64_t newTimelineIn, int64_t newDuration, int64_t newSourceIn)
    : m_track(track)
    , m_clipId(clipId)
    , m_oldTimelineIn(0), m_newTimelineIn(newTimelineIn)
    , m_oldDuration(0),   m_newDuration(newDuration)
    , m_oldSourceIn(0),   m_newSourceIn(newSourceIn)
{
}

void TrimClipCommand::execute()
{
    size_t idx = m_track->findClipIndexById(m_clipId);
    if (idx == m_track->clipCount()) return;

    Clip* c = m_track->clip(idx);
    m_oldTimelineIn = c->timelineIn();
    m_oldDuration   = c->duration();
    m_oldSourceIn   = c->sourceIn();

    c->setTimelineIn(m_newTimelineIn);
    c->setDuration(m_newDuration);
    c->setSourceIn(m_newSourceIn);
}

void TrimClipCommand::undo()
{
    size_t idx = m_track->findClipIndexById(m_clipId);
    if (idx == m_track->clipCount()) return;

    Clip* c = m_track->clip(idx);
    c->setTimelineIn(m_oldTimelineIn);
    c->setDuration(m_oldDuration);
    c->setSourceIn(m_oldSourceIn);
}

std::string TrimClipCommand::description() const
{
    return "Trim Clip";
}

bool TrimClipCommand::mergeWith(const Command& next)
{
    auto* trimCmd = dynamic_cast<const TrimClipCommand*>(&next);
    if (!trimCmd) return false;
    if (trimCmd->m_clipId != m_clipId || trimCmd->m_track != m_track) return false;

    // Absorb new values, keep our original old values
    m_newTimelineIn = trimCmd->m_newTimelineIn;
    m_newDuration   = trimCmd->m_newDuration;
    m_newSourceIn   = trimCmd->m_newSourceIn;
    return true;
}

} // namespace rt

