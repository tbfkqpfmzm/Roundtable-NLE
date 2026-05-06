/*
 * AudioSyncUnmatchedCardSections.cpp - Unmatched script-card controls for AudioSync.
 * Split from AudioSyncCards.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "Theme.h"
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <filesystem>
#include <utility>
namespace rt {
void AudioSync::addUnmatchedScriptCardSections(QVBoxLayout* leftLayout,
                                               QVBoxLayout* rightLayout,
                                               int scriptLineNumber,
                                               const std::string& character,
                                               const std::string& dialogue)
{
    const auto& _tc = Theme::colors();
    const auto _radM = QString::number(Theme::metrics().radiusMd);
    const auto _rad  = QString::number(Theme::metrics().radiusSm);
    // ├óΓÇ¥Γé¼├óΓÇ¥Γé¼ Unmatched: left (text) | right (manual match) ├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼
                auto* noAudioLabel = new QLabel("No audio assigned");
                noAudioLabel->setAlignment(Qt::AlignCenter);
                noAudioLabel->setStyleSheet(
                    QString("QLabel { color: %1; font-size: 13px; font-style: italic; border: none; "
                    "padding: 20px; }").arg(Theme::hex(_tc.textDisabled)));
                leftLayout->addWidget(noAudioLabel, 1);
                int lineNum = scriptLineNumber;
                // Audio file selector ├óΓé¼ΓÇ¥ lets the user pick which imported
                // audio file this line should match against.
                if (m_audioPaths.size() > 1) {
                    auto* audioFileCombo = new QComboBox;
                    audioFileCombo->setFixedHeight(24);
                    audioFileCombo->setStyleSheet(
                        QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
                        "padding: 2px 6px; border-radius: %4px; font-size: 12px; }").arg(
                            Theme::hex(_tc.inputBg), Theme::hex(_tc.textPrimary),
                            Theme::hex(_tc.inputBorder), _rad));
                    audioFileCombo->addItem("Audio file: auto");
                    // Build sorted list of (filename, full path) pairs
                    std::vector<std::pair<QString, QString>> sortedAudio;
                    sortedAudio.reserve(m_audioPaths.size());
                    for (const auto& ap : m_audioPaths) {
                        QString fname = QString::fromStdString(
                            std::filesystem::path(ap).filename().string());
                        sortedAudio.emplace_back(fname, QString::fromStdString(ap));
                    }
                    std::sort(sortedAudio.begin(), sortedAudio.end(),
                        [](const auto& a, const auto& b) {
                            return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
                        });
                    for (const auto& [fname, fullPath] : sortedAudio)
                        audioFileCombo->addItem(fname, fullPath);
                    // Pre-select if already assigned
                    auto ait = m_lineAudioFile.find(lineNum);
                    if (ait != m_lineAudioFile.end()) {
                        for (int ci = 1; ci < audioFileCombo->count(); ++ci) {
                            if (audioFileCombo->itemData(ci).toString().toStdString() == ait->second) {
                                audioFileCombo->setCurrentIndex(ci);
                                break;
                            }
                        }
                    }
                    connect(audioFileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                            this, [this, lineNum](int idx) {
                        if (idx <= 0) {
                            m_lineAudioFile.erase(lineNum);
                        } else {
                            auto* combo = qobject_cast<QComboBox*>(sender());
                            if (combo) {
                                m_lineAudioFile[lineNum] = combo->itemData(idx).toString().toStdString();
                            }
                        }
                    });
                    rightLayout->addWidget(audioFileCombo);
                }
                auto* manualBtn = new QPushButton("MANUAL");
                manualBtn->setFixedHeight(40);
                manualBtn->setToolTip("Open manual match dialog");
                manualBtn->setCursor(Qt::PointingHandCursor);
                manualBtn->setStyleSheet(
                    QString("QPushButton { background: %1; color: %2; font-weight: bold; "
                    "font-size: 13px; border: 2px solid %3; border-radius: %4px; }"
                    "QPushButton:hover { background: %3; }").arg(
                        Theme::hex(_tc.accentDim), Theme::hex(_tc.textBright),
                        Theme::hex(_tc.accent), _radM));
                connect(manualBtn, &QPushButton::clicked, this, [this, lineNum]() {
                    openManualMatch(lineNum);
                });
                rightLayout->addWidget(manualBtn);
                rightLayout->addStretch();
                // Assign from orphan dropdown
                auto* assignCombo = new QComboBox;
                assignCombo->setFixedHeight(24);
                assignCombo->setStyleSheet(
                    QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
                    "padding: 2px 6px; border-radius: %4px; font-size: 12px; }").arg(
                        Theme::hex(_tc.inputBg), Theme::hex(_tc.textPrimary),
                        Theme::hex(_tc.inputBorder), _rad));
                assignCombo->addItem("Assign clip...");
                for (size_t i = 0; i < m_clips.size(); ++i) {
                    if (m_clips[i].scriptLineNumber < 0) {
                        const auto& oc = m_clips[i];
                        QString label = QString("Clip #%1 (%2) %3s")
                            .arg(i + 1)
                            .arg(oc.character.empty() ? "?" : QString::fromStdString(oc.character))
                            .arg(oc.end - oc.start, 0, 'f', 1);
                        assignCombo->addItem(label, QVariant(static_cast<int>(i)));
                    }
                }
                std::string lineChar = character;
                std::string lineDial = dialogue;
                connect(assignCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this, lineNum, lineChar, lineDial](int idx) {
                    if (idx <= 0) return;
                    auto* combo = qobject_cast<QComboBox*>(sender());
                    if (!combo) return;
                    int clipIdx = combo->itemData(idx).toInt();
                    if (clipIdx >= 0 && static_cast<size_t>(clipIdx) < m_clips.size()) {
                        m_clips[static_cast<size_t>(clipIdx)].scriptLineNumber = lineNum;
                        m_clips[static_cast<size_t>(clipIdx)].character = lineChar;
                        m_clips[static_cast<size_t>(clipIdx)].scriptSegment = lineChar + ": " + lineDial;
                        m_clips[static_cast<size_t>(clipIdx)].matchState = 1;
                        m_clips[static_cast<size_t>(clipIdx)].confidence = 1.0f;
                        // Defer rebuild so the combo isn't deleted during its own signal
                        QTimer::singleShot(0, this, [this]() { populateCards(); });
                    }
                });
                rightLayout->addWidget(assignCombo);
}
} // namespace rt