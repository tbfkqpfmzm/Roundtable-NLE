/*
 * AudioSyncScriptHistory.cpp - Script history, filters, and small visual helpers.
 */

#include "panels/audio/AudioSync.h"

#include "ai/ScriptMatcher.h"
#include "widgets/MiniWaveformWidget.h"

#include <spdlog/spdlog.h>

#include "QtHelpers.h"

#include <QFile>
#include <QTextStream>

namespace rt {

void AudioSync::loadScriptHistory()
{
    if (!m_scriptUrlCombo) return;

    QFile file(rt::userDataDir() + QStringLiteral("/script_history.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QStringList urls;
    QTextStream input(&file);
    while (!input.atEnd()) {
        QString line = input.readLine().trimmed();
        if (!line.isEmpty())
            urls.append(line);
    }

    m_scriptUrlCombo->blockSignals(true);
    m_scriptUrlCombo->clear();
    for (const auto& url : urls)
        m_scriptUrlCombo->addItem(url);
    m_scriptUrlCombo->setCurrentText(QString());
    m_scriptUrlCombo->blockSignals(false);

    spdlog::info("AudioSync: Loaded {} script history entries", urls.size());
}

void AudioSync::addToScriptHistory(const QString& url)
{
    if (url.isEmpty()) return;

    QStringList urls;
    QFile readFile(rt::userDataDir() + QStringLiteral("/script_history.txt"));
    if (readFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream input(&readFile);
        while (!input.atEnd()) {
            QString line = input.readLine().trimmed();
            if (!line.isEmpty())
                urls.append(line);
        }
    }

    urls.removeAll(url);
    urls.prepend(url);
    while (urls.size() > 20)
        urls.removeLast();

    QFile writeFile(rt::userDataDir() + QStringLiteral("/script_history.txt"));
    if (writeFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream output(&writeFile);
        for (const auto& entry : urls)
            output << entry << "\n";
    }

    m_scriptUrlCombo->blockSignals(true);
    m_scriptUrlCombo->clear();
    for (const auto& entry : urls)
        m_scriptUrlCombo->addItem(entry);
    m_scriptUrlCombo->setCurrentText(url);
    m_scriptUrlCombo->blockSignals(false);

    spdlog::info("AudioSync: Added URL to script history (total: {})", urls.size());
}

void AudioSync::populateScriptFilter()
{
    if (m_scriptFilterCombo && m_script) {
        m_scriptFilterCombo->blockSignals(true);
        m_scriptFilterCombo->clear();
        m_scriptFilterCombo->addItem("All Characters");
        for (const auto& character : m_script->characters)
            m_scriptFilterCombo->addItem(QString::fromStdString(character));
        m_scriptFilterCombo->setCurrentIndex(0);
        m_scriptFilterCombo->blockSignals(false);
    }

    populateCharacterTabs();
}

void AudioSync::onScriptFilterChanged(int /*index*/)
{
    populateScriptList();
}

void AudioSync::updateSelectedClipHighlight()
{
    updateTransportBar();
}

void AudioSync::updateTransportBar()
{
    if (!m_transportTimeLabel || !m_transportClipLabel) return;

    if (m_playingClipIdx >= 0 && static_cast<size_t>(m_playingClipIdx) < m_clips.size()) {
        const auto& clip = m_clips[static_cast<size_t>(m_playingClipIdx)];
        QString label = clip.character.empty()
            ? QStringLiteral("Clip #%1").arg(m_playingClipIdx + 1)
            : QString::fromStdString(clip.character);
        m_transportClipLabel->setText(label);

        if (auto* waveform = waveformForClip(m_playingClipIdx)) {
            double playhead = waveform->playhead();
            int minutes = static_cast<int>(playhead) / 60;
            double seconds = playhead - minutes * 60;
            m_transportTimeLabel->setText(
                QString("%1:%2").arg(minutes, 2, 10, QChar('0'))
                               .arg(seconds, 5, 'f', 2, QChar('0')));
        }
        if (m_transportPlayBtn)
            m_transportPlayBtn->setText(QStringLiteral("\u23F8"));
    } else {
        m_transportClipLabel->setText(QStringLiteral("No clip playing"));
        m_transportTimeLabel->setText(QStringLiteral("00:00.00"));
        if (m_transportPlayBtn)
            m_transportPlayBtn->setText(QStringLiteral("\u25B6"));
    }
}

QColor AudioSync::characterColor(const std::string& character) const
{
    if (character.empty()) return QColor(160, 160, 160);

    auto it = m_characterColors.find(character);
    if (it != m_characterColors.end()) return it->second;

    static const QColor palette[] = {
        QColor(90, 158, 233),
        QColor(233, 150, 90),
        QColor(130, 200, 130),
        QColor(200, 130, 200),
        QColor(200, 200, 100),
        QColor(130, 200, 200),
        QColor(233, 130, 130),
        QColor(180, 160, 130),
    };
    constexpr size_t paletteSize = sizeof(palette) / sizeof(palette[0]);

    size_t hash = 5381;
    for (char c : character) hash = hash * 33 + static_cast<unsigned char>(c);
    QColor color = palette[hash % paletteSize];

    m_characterColors[character] = color;
    return color;
}

} // namespace rt
