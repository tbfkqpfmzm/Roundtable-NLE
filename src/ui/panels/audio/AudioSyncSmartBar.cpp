/*
 * AudioSyncSmartBar.cpp - Workflow state and smart-bar label helpers.
 */

#include "panels/audio/AudioSync.h"

#include "Theme.h"
#include "ai/ScriptMatcher.h"

#include <unordered_set>

namespace rt {

void AudioSync::updateWorkflowState()
{
    updateSmartBar();

    if (m_audioGroup)      m_audioGroup->setEnabled(m_scriptLoaded);
    if (m_transcribeGroup) m_transcribeGroup->setEnabled(m_audioImported);
    if (m_syncGroup)       m_syncGroup->setEnabled(m_transcriptionDone && m_scriptLoaded);
    if (m_reviewGroup)     m_reviewGroup->setEnabled(m_syncDone || m_transcriptionDone);
    bool exportReady = m_syncDone && missingDefaultShots().isEmpty();
    if (m_exportBtn)       m_exportBtn->setEnabled(exportReady);
    if (m_confirmBtn)      m_confirmBtn->setEnabled(m_syncDone);
    if (m_confirmAllBtn)   m_confirmAllBtn->setEnabled(m_syncDone);

    if (m_syncActionBtn)       m_syncActionBtn->setEnabled(m_transcriptionDone && m_scriptLoaded);
    if (m_confirmAllActionBtn) m_confirmAllActionBtn->setEnabled(m_syncDone);
    if (m_clearActionBtn)      m_clearActionBtn->setEnabled(m_syncDone);
    if (m_exportActionBtn)     m_exportActionBtn->setEnabled(exportReady);

    if (m_statusLabel) {
        int confirmed = 0;
        int total = 0;
        if (m_script) {
            total = static_cast<int>(m_script->lines.size());
            std::unordered_set<int> confirmedLines;
            for (const auto& clip : m_clips) {
                if (clip.scriptLineNumber > 0 && clip.matchState == 2)
                    confirmedLines.insert(clip.scriptLineNumber);
            }
            confirmed = static_cast<int>(confirmedLines.size());
        }
        m_statusLabel->setText(
            total > 0 ? QString("%1 / %2 confirmed").arg(confirmed).arg(total) : QString());
    }
}

void AudioSync::updateSmartBar()
{
    if (!m_smartBarLabel || !m_smartBarIcon) return;

    int confirmed = 0;
    int tentative = 0;
    int total = 0;
    if (m_script) {
        total = static_cast<int>(m_script->lines.size());
        std::unordered_set<int> confirmedLines;
        std::unordered_set<int> tentativeLines;
        for (const auto& clip : m_clips) {
            if (clip.scriptLineNumber > 0) {
                if (clip.matchState == 2) confirmedLines.insert(clip.scriptLineNumber);
                else if (clip.matchState == 1) tentativeLines.insert(clip.scriptLineNumber);
            }
        }
        confirmed = static_cast<int>(confirmedLines.size());
        tentative = static_cast<int>(tentativeLines.size());
    }

    if (!m_scriptLoaded) {
        m_smartBarLabel->setText("Load a script to begin");
        m_smartBarIcon->setStyleSheet(QString("QLabel { color: %1; font-size: 14px; border: none; }")
            .arg(Theme::hex(Theme::colors().textDisabled)));
    } else if (!m_audioImported) {
        m_smartBarLabel->setText(QString("Script: %1 lines \u2014 Import audio next").arg(total));
        m_smartBarIcon->setStyleSheet(QString("QLabel { color: %1; font-size: 14px; border: none; }")
            .arg(Theme::hex(Theme::colors().accent)));
    } else if (!m_transcriptionDone) {
        m_smartBarLabel->setText(QString("Script: %1 lines | %2 audio file(s) \u2014 Ready to transcribe")
            .arg(total).arg(m_audioPaths.size()));
        m_smartBarIcon->setStyleSheet(QString("QLabel { color: %1; font-size: 14px; border: none; }")
            .arg(Theme::hex(Theme::colors().warning)));
    } else if (confirmed + tentative == 0) {
        m_smartBarLabel->setText(QString("%1 clips | %2 lines \u2014 Run Auto-Sync")
            .arg(m_clips.size()).arg(total));
        m_smartBarIcon->setStyleSheet(QString("QLabel { color: %1; font-size: 14px; border: none; }")
            .arg(Theme::hex(Theme::colors().warning)));
    } else if (confirmed == total) {
        // Check if all characters have default shots configured
        QStringList missingDefault = missingDefaultShots();
        if (!missingDefault.isEmpty()) {
            m_smartBarLabel->setText(
                QStringLiteral("\u2713 All %1 lines confirmed \u2014 "
                               "Export unavailable: set default shot(s) for %2 in Compose")
                    .arg(total)
                    .arg(missingDefault.join(QStringLiteral(", "))));
            m_smartBarIcon->setStyleSheet(QString("QLabel { color: %1; font-size: 14px; border: none; }")
                .arg(Theme::hex(Theme::colors().warning)));
        } else {
            m_smartBarLabel->setText(QStringLiteral("\u2713 All %1 lines confirmed \u2014 Ready to export")
                .arg(total));
            m_smartBarIcon->setStyleSheet(QString("QLabel { color: %1; font-size: 14px; border: none; }")
                .arg(Theme::hex(Theme::colors().success)));
        }
    } else {
        m_smartBarLabel->setText(QString("%1/%2 confirmed, %3 tentative, %4 unmatched")
            .arg(confirmed).arg(total).arg(tentative).arg(total - confirmed - tentative));
        m_smartBarIcon->setStyleSheet(QString("QLabel { color: %1; font-size: 14px; border: none; }")
            .arg(Theme::hex(Theme::colors().warning)));
    }

    if (m_smartAutoSyncBtn) m_smartAutoSyncBtn->setVisible(m_transcriptionDone && m_scriptLoaded);
    if (m_smartConfirmAllBtn) m_smartConfirmAllBtn->setVisible(tentative > 0);
    if (m_smartImportBtn) m_smartImportBtn->setVisible(m_scriptLoaded);
}

} // namespace rt