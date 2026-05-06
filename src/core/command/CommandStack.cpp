/*
 * CommandStack.cpp — Undo/redo history implementation.
 * Step 4: Command System
 */

#include "command/CommandStack.h"

#include <spdlog/spdlog.h>

namespace rt {

CommandStack::CommandStack(size_t maxDepth)
    : m_maxDepth(maxDepth)
{
}

CommandStack::~CommandStack() = default;

void CommandStack::execute(std::unique_ptr<Command> cmd)
{
    if (!cmd) return;

    // Always execute first (apply the change)
    cmd->execute();

    // Try to merge with the most recent command (absorb into existing)
    if (!m_undoStack.empty() && m_undoStack.back()->typeId() >= 0
        && m_undoStack.back()->typeId() == cmd->typeId()
        && m_undoStack.back()->mergeWith(*cmd))
    {
        spdlog::trace("Merged command: {}", m_undoStack.back()->description());
        notifyChange();
        return;
    }

    m_undoStack.push_back(std::move(cmd));

    // Executing a new command clears the redo stack (branch is abandoned)
    m_redoStack.clear();

    trimToMaxDepth();
    notifyChange();

    spdlog::trace("Executed command: {} (undo depth: {})",
                  m_undoStack.back()->description(), m_undoStack.size());
}

void CommandStack::pushWithoutExecute(std::unique_ptr<Command> cmd)
{
    if (!cmd) return;

    // Push onto undo stack without calling cmd->execute().
    // The caller is responsible for having already applied the change.
    m_undoStack.push_back(std::move(cmd));
    m_redoStack.clear();
    trimToMaxDepth();
    notifyChange();

    spdlog::trace("Pushed (no-exec) command: {} (undo depth: {})",
                  m_undoStack.back()->description(), m_undoStack.size());
}

bool CommandStack::undo()
{
    if (m_undoStack.empty()) return false;

    auto cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();

    cmd->undo();
    spdlog::trace("Undid command: {}", cmd->description());

    m_redoStack.push_back(std::move(cmd));
    notifyChange();
    return true;
}

bool CommandStack::redo()
{
    if (m_redoStack.empty()) return false;

    auto cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();

    cmd->execute();
    spdlog::trace("Redid command: {}", cmd->description());

    m_undoStack.push_back(std::move(cmd));
    notifyChange();
    return true;
}

bool CommandStack::canUndo() const noexcept
{
    return !m_undoStack.empty();
}

bool CommandStack::canRedo() const noexcept
{
    return !m_redoStack.empty();
}

std::string CommandStack::undoDescription() const
{
    return m_undoStack.empty() ? "" : m_undoStack.back()->description();
}

std::string CommandStack::redoDescription() const
{
    return m_redoStack.empty() ? "" : m_redoStack.back()->description();
}

size_t CommandStack::undoCount() const noexcept
{
    return m_undoStack.size();
}

size_t CommandStack::redoCount() const noexcept
{
    return m_redoStack.size();
}

void CommandStack::clear()
{
    m_undoStack.clear();
    m_redoStack.clear();
    notifyChange();
}

CommandStack::HistorySnapshot CommandStack::historySnapshot() const
{
    HistorySnapshot snap;
    snap.descriptions.reserve(m_undoStack.size() + m_redoStack.size());

    // Undo stack: oldest first
    for (auto& cmd : m_undoStack)
        snap.descriptions.push_back(cmd->description());

    snap.currentIndex = m_undoStack.size();

    // Redo stack: newest first (redo stack is LIFO, so reverse order)
    for (auto it = m_redoStack.rbegin(); it != m_redoStack.rend(); ++it)
        snap.descriptions.push_back((*it)->description());

    return snap;
}

bool CommandStack::jumpToIndex(size_t index)
{
    size_t current = m_undoStack.size();
    size_t total = m_undoStack.size() + m_redoStack.size();

    if (index > total) return false;

    if (index < current) {
        // Undo (current - index) times
        size_t count = current - index;
        for (size_t i = 0; i < count; ++i) {
            if (!undo()) return false;
        }
    } else if (index > current) {
        // Redo (index - current) times
        size_t count = index - current;
        for (size_t i = 0; i < count; ++i) {
            if (!redo()) return false;
        }
    }

    return true;
}

void CommandStack::trimToMaxDepth()
{
    while (m_undoStack.size() > m_maxDepth)
    {
        m_undoStack.pop_front(); // Drop oldest commands
    }
}

void CommandStack::notifyChange()
{
    if (m_callback) m_callback();
}

} // namespace rt

