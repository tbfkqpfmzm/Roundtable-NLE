/*
 * CompoundCommand.cpp — Atomic group of sub-commands.
 * Step 4: Command System
 */

#include "command/CompoundCommand.h"

namespace rt {

CompoundCommand::CompoundCommand(std::string description)
    : m_description(std::move(description))
{
}

CompoundCommand::~CompoundCommand() = default;

void CompoundCommand::addCommand(std::unique_ptr<Command> cmd)
{
    if (cmd)
        m_commands.push_back(std::move(cmd));
}

void CompoundCommand::addExecuted(std::unique_ptr<Command> cmd)
{
    if (cmd) {
        m_commands.push_back(std::move(cmd));
        ++m_executedCount;
    }
}

void CompoundCommand::execute()
{
    // Execute sub-commands in order, skipping those already executed via addExecuted()
    for (size_t i = m_executedCount; i < m_commands.size(); ++i)
        m_commands[i]->execute();
    m_executedCount = 0; // On re-execute (redo), execute all
}

void CompoundCommand::undo()
{
    // Undo sub-commands in reverse order
    for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
        (*it)->undo();
}

std::string CompoundCommand::description() const
{
    return m_description;
}

} // namespace rt

