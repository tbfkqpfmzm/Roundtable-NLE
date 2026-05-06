/*
 * Command — base class for the undo/redo system.
 *
 * Every user action that modifies the project goes through a Command.
 * Commands store only the delta needed to undo, not a full copy of state.
 * This keeps memory usage to <1KB per undo level instead of megabytes.
 */

#pragma once

#include <memory>
#include <string>

namespace rt {

class Command
{
public:
    virtual ~Command() = default;

    /// Execute the command (first time or redo)
    virtual void execute() = 0;

    /// Undo the command — must perfectly reverse execute()
    virtual void undo() = 0;

    /// Human-readable description for the undo history panel
    [[nodiscard]] virtual std::string description() const = 0;

    /// If true, this command can be merged with the next command of the same type.
    /// Used for continuous operations like dragging (many small moves → one undo step).
    [[nodiscard]] virtual bool mergeWith(const Command& /*next*/) { return false; }

    /// Unique type ID for merge checking
    [[nodiscard]] virtual int typeId() const { return -1; }
};

} // namespace rt
