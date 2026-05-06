/*
 * HistoryPanel.cpp — Visual undo/redo history list implementation.
 * Step 28
 */

#include "panels/project/HistoryPanel.h"
#include "command/CommandStack.h"
#include "Theme.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

HistoryPanel::HistoryPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
}

void HistoryPanel::buildUi()
{
    const auto& m = Theme::metrics();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(m.spacingSm, m.spacingSm, m.spacingSm, m.spacingSm);
    layout->setSpacing(m.spacingXs);

    // ── Toolbar row ─────────────────────────────────────────────────────
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->addStretch();

    m_clearBtn = new QPushButton("Clear", this);
    m_clearBtn->setToolTip("Clear all undo/redo history");
    m_clearBtn->setFixedWidth(60);
    m_clearBtn->setEnabled(false);
    header->addWidget(m_clearBtn);

    layout->addLayout(header);

    // ── History list ────────────────────────────────────────────────────
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    layout->addWidget(m_list, 1);

    // ── Empty state ─────────────────────────────────────────────────────
    m_emptyLabel = new QLabel(tr("No history yet.\nActions you perform will appear here."), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setObjectName("EmptyStateLabel");
    m_emptyLabel->setVisible(true);
    layout->addWidget(m_emptyLabel, 1);
    m_list->setVisible(false);

    setLayout(layout);

    // ── Connections ─────────────────────────────────────────────────────
    connect(m_list, &QListWidget::itemClicked,
            this, &HistoryPanel::onItemClicked);
    connect(m_clearBtn, &QPushButton::clicked,
            this, &HistoryPanel::onClearClicked);
}

// ═════════════════════════════════════════════════════════════════════════════
// Dependency injection
// ═════════════════════════════════════════════════════════════════════════════

void HistoryPanel::setCommandStack(CommandStack* stack)
{
    m_stack = stack;

    if (m_stack) {
        // Register callback so we refresh whenever history changes
        m_stack->setChangeCallback([this]() {
            refresh();
        });
        refresh();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Refresh — rebuild the list from CommandStack snapshot
// ═════════════════════════════════════════════════════════════════════════════

void HistoryPanel::refresh()
{
    if (!m_list) return;

    // Block signals to avoid triggering onItemClicked during rebuild
    m_list->blockSignals(true);
    m_list->clear();

    if (!m_stack) {
        m_clearBtn->setEnabled(false);
        m_list->blockSignals(false);
        return;
    }

    auto snapshot = m_stack->historySnapshot();

    // Colors from theme
    const auto& tc = Theme::colors();
    QColor executedColor = tc.textPrimary;
    QColor currentColor  = tc.accent;
    QColor redoColor     = tc.textDisabled;
    QColor currentBgColor = tc.accentSubtle;

    // ── "Initial State" pseudo-entry (index 0 = undo everything) ────────
    auto* initialItem = new QListWidgetItem(
        snapshot.currentIndex == 0 ? QString::fromUtf8("► Initial State")
                                   : QString::fromUtf8("◆ Initial State"));
    initialItem->setData(Qt::UserRole, 0);  // Jump-to index

    if (snapshot.currentIndex == 0) {
        initialItem->setForeground(currentColor);
        initialItem->setBackground(currentBgColor);
    } else {
        initialItem->setForeground(executedColor);
    }
    m_list->addItem(initialItem);

    // ── Command entries ─────────────────────────────────────────────────
    for (size_t i = 0; i < snapshot.descriptions.size(); ++i) {
        bool isExecuted = (i < snapshot.currentIndex);
        bool isCurrent  = (i + 1 == snapshot.currentIndex);

        QString prefix;
        if (isCurrent) {
            prefix = QString::fromUtf8("► ");
        } else if (isExecuted) {
            prefix = QString::fromUtf8("◆ ");
        } else {
            prefix = QString::fromUtf8("○ ");
        }

        QString text = prefix + QString::fromStdString(snapshot.descriptions[i]);
        auto* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, static_cast<int>(i + 1));  // Jump-to index (1-based, 0 = initial state)

        if (isCurrent) {
            item->setForeground(currentColor);
            item->setBackground(currentBgColor);
        } else if (isExecuted) {
            item->setForeground(executedColor);
        } else {
            item->setForeground(redoColor);
        }

        m_list->addItem(item);
    }

    // ── Highlight current row ───────────────────────────────────────────
    // currentIndex maps to list row: row = currentIndex (0 = initial state)
    int currentRow = static_cast<int>(snapshot.currentIndex);
    if (currentRow >= 0 && currentRow < m_list->count()) {
        m_list->setCurrentRow(currentRow);
        m_list->scrollToItem(m_list->item(currentRow));
    }

    // Enable clear button if there's any history
    m_clearBtn->setEnabled(!snapshot.descriptions.empty());

    // Update empty state + status bar
    bool empty = snapshot.descriptions.empty();
    m_list->setVisible(!empty);
    if (m_emptyLabel) m_emptyLabel->setVisible(empty);
    if (m_statusLabel) {
        int count = static_cast<int>(snapshot.descriptions.size());
        m_statusLabel->setText(count == 0 ? tr("0 actions")
            : (count == 1 ? tr("1 action")
                          : tr("%1 actions").arg(count)));
    }

    m_list->blockSignals(false);
}

// ═════════════════════════════════════════════════════════════════════════════
// Slots
// ═════════════════════════════════════════════════════════════════════════════

void HistoryPanel::onItemClicked(QListWidgetItem* item)
{
    if (!item || !m_stack) return;

    int targetIndex = item->data(Qt::UserRole).toInt();

    spdlog::trace("HistoryPanel: jump to index {}", targetIndex);

    // Block signals to prevent recursive refresh during multi-undo/redo
    // The final notifyChange from jumpToIndex will trigger refresh via callback
    m_stack->jumpToIndex(static_cast<size_t>(targetIndex));

    emit historyJumped(targetIndex);
}

void HistoryPanel::onClearClicked()
{
    if (!m_stack) return;

    spdlog::trace("HistoryPanel: clear all history");
    m_stack->clear();
    // refresh() will be called via the change callback
}

// ═════════════════════════════════════════════════════════════════════════════
// Accessors
// ═════════════════════════════════════════════════════════════════════════════

int HistoryPanel::currentRow() const
{
    return m_list ? m_list->currentRow() : -1;
}

int HistoryPanel::itemCount() const
{
    return m_list ? m_list->count() : 0;
}

} // namespace rt
