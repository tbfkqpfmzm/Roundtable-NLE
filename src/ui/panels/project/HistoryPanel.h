/*
 * HistoryPanel — Visual undo/redo history list.
 *
 * Step 28: History Panel
 *
 * Shows a scrollable list of all commands in the undo/redo history.
 * The current position is highlighted. Clicking any item jumps to
 * that point in history (undoing or redoing as needed).
 *
 * Layout:
 *   ┌─────────────────────────────────────┐
 *   │  History                    [🗑 Clear]│
 *   ├─────────────────────────────────────┤
 *   │  ◆ Initial State                    │  ← always present
 *   │  ◆ Add Video Clip                   │  ← undo items
 *   │  ◆ Move Clip                        │
 *   │  ► Set Volume to 0.8         ◄──────│  ← current position
 *   │  ○ Delete Clip                      │  ← redo items (dimmed)
 *   │  ○ Add Audio Clip                   │
 *   └─────────────────────────────────────┘
 *
 * Features:
 *   - "Initial State" pseudo-entry at the top (undo everything)
 *   - Executed commands shown with filled bullet (◆)
 *   - Current position has arrow indicator (►) and highlight
 *   - Future (redo) commands shown dimmed with open bullet (○)
 *   - Click to jump to any history point
 *   - Clear button removes all history
 *   - Auto-scrolls to keep current position visible
 *   - Updates live via CommandStack::setChangeCallback
 */

#pragma once

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace rt {

class CommandStack;

class HistoryPanel : public QWidget
{
    Q_OBJECT

public:
    explicit HistoryPanel(QWidget* parent = nullptr);
    ~HistoryPanel() override = default;

    /// Inject the command stack to observe.
    void setCommandStack(CommandStack* stack);

    /// Get the associated command stack.
    [[nodiscard]] CommandStack* commandStack() const noexcept { return m_stack; }

    /// Force a refresh of the history list from the command stack.
    void refresh();

    /// Currently highlighted row (for testing).
    [[nodiscard]] int currentRow() const;

    /// Total number of items in the list (for testing).
    [[nodiscard]] int itemCount() const;

    /// Get the list widget (for testing).
    [[nodiscard]] QListWidget* listWidget() const noexcept { return m_list; }

    /// Get the clear button (for testing).
    [[nodiscard]] QPushButton* clearButton() const noexcept { return m_clearBtn; }

signals:
    /// Emitted when the user clicks to jump to a history position.
    void historyJumped(int index);

private slots:
    void onItemClicked(QListWidgetItem* item);
    void onClearClicked();

private:
    void buildUi();

    CommandStack* m_stack{nullptr};
    QListWidget*  m_list{nullptr};
    QPushButton*  m_clearBtn{nullptr};
    QLabel*       m_emptyLabel{nullptr};
    QLabel*       m_statusLabel{nullptr};
};

} // namespace rt
