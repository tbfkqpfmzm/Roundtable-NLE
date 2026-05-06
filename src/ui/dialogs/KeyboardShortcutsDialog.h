/*
 * KeyboardShortcutsDialog — Dialog for viewing and customizing keyboard shortcuts.
 */

#pragma once

#include <QDialog>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

namespace rt {

class ShortcutManager;

class KeyboardShortcutsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit KeyboardShortcutsDialog(ShortcutManager& manager, QWidget* parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void buildUI();
    void populateTree();
    void filterTree(const QString& text);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void finishCapture(const QKeySequence& key);
    void cancelCapture();
    void resetSelected();
    void resetAll();

    ShortcutManager& m_manager;

    QLineEdit*   m_searchEdit{nullptr};
    QTreeWidget* m_tree{nullptr};
    QPushButton* m_resetBtn{nullptr};
    QPushButton* m_resetAllBtn{nullptr};

    // Key capture state
    QTreeWidgetItem* m_capturingItem{nullptr};
    bool             m_capturing{false};
};

} // namespace rt
