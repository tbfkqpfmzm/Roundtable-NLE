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
#include <vector>

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

// ── MacroCommand ───────────────────────────────────────────────────────────
// Groups multiple child commands into a single undoable unit.
// Used for multi-clip drags, batch operations, etc.

class MacroCommand : public Command
{
public:
    explicit MacroCommand(std::string description)
        : m_description(std::move(description)) {}

    void addCommand(std::unique_ptr<Command> cmd)
    {
        m_children.push_back(std::move(cmd));
    }

    [[nodiscard]] bool empty() const noexcept { return m_children.empty(); }
    [[nodiscard]] size_t size() const noexcept { return m_children.size(); }

    void execute() override
    {
        for (auto& cmd : m_children)
            if (cmd) cmd->execute();
    }

    void undo() override
    {
        // Undo in reverse order
        for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
            if (*it) (*it)->undo();
    }

    [[nodiscard]] std::string description() const override { return m_description; }

private:
    std::string m_description;
    std::vector<std::unique_ptr<Command>> m_children;
};

} // namespace rt
