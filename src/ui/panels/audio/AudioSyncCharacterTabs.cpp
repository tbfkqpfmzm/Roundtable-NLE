/*
 * AudioSyncCharacterTabs.cpp - Character filter and tab refresh helpers.
 */

#include "panels/audio/AudioSync.h"

#include "Theme.h"
#include "ai/ScriptMatcher.h"

#include <QColor>
#include <QFrame>
#include <QListWidgetItem>
#include <QSize>
#include <QTabBar>

namespace rt {

void AudioSync::populateCharacterTabs()
{
    if (!m_script) return;

    if (m_charFilterList) {
        QString currentText;
        if (m_charFilterList->currentItem())
            currentText = m_charFilterList->currentItem()->text();
        if (currentText.isEmpty()) currentText = "ALL";

        m_charFilterList->blockSignals(true);
        m_charFilterList->clear();
        m_charFilterList->addItem("ALL");
        for (const auto& character : m_script->characters) {
            m_charFilterList->addItem(QString::fromStdString(character));
        }

        auto* sepItem = new QListWidgetItem(m_charFilterList);
        sepItem->setFlags(Qt::NoItemFlags);
        sepItem->setSizeHint(QSize(0, 28));
        auto* sepWidget = new QFrame;
        sepWidget->setFixedHeight(2);
        sepWidget->setStyleSheet(
            QString("QFrame { background: %1; margin: 8px 8px; }")
                .arg(Theme::hex(Theme::colors().border)));
        m_charFilterList->setItemWidget(sepItem, sepWidget);

        auto* unmatchedItem = new QListWidgetItem("UNMATCHED");
        unmatchedItem->setForeground(Qt::white);
        unmatchedItem->setBackground(QColor(0xCC, 0x33, 0x33));
        unmatchedItem->setData(Qt::UserRole, QStringLiteral("unmatched"));
        m_charFilterList->addItem(unmatchedItem);

        bool restored = false;
        for (int i = 0; i < m_charFilterList->count(); ++i) {
            if (m_charFilterList->item(i)->text() == currentText) {
                m_charFilterList->setCurrentRow(i);
                restored = true;
                break;
            }
        }
        if (!restored)
            m_charFilterList->setCurrentRow(0);
        m_charFilterList->blockSignals(false);
    }

    if (m_charTabBar) {
        while (m_charTabBar->count() > 1)
            m_charTabBar->removeTab(1);
        for (const auto& character : m_script->characters)
            m_charTabBar->addTab(QString::fromStdString(character));
        m_charTabBar->setCurrentIndex(0);
    }
}

} // namespace rt