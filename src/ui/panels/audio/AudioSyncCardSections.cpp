/*
 * AudioSyncCardSections.cpp - Matched script-card audio/control sections for AudioSync.
 * Split from AudioSyncCards.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "media/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"
#include "Theme.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <utility>
namespace rt {
void AudioSync::addMatchedClipCardSections(QVBoxLayout* leftLayout,
                                           QVBoxLayout* rightLayout,
                                           MiniWaveformWidget*& waveform,
                                           size_t clipIdx,
                                           int matchState,
                                           float confidence,
                                           const std::vector<ScriptLineDisplay>& displayLines)
{
    const auto& _tc = Theme::colors();
    const auto _radM = QString::number(Theme::metrics().radiusMd);
    const auto _rad  = QString::number(Theme::metrics().radiusSm);
    const auto& clip = m_clips[clipIdx];
                // Confidence bar
                if (confidence > 0.001f) {
                    auto* confBar = new QProgressBar;
                    confBar->setFixedHeight(4);
                    confBar->setRange(0, 100);
                    confBar->setValue(static_cast<int>(confidence * 100.0f));
                    confBar->setTextVisible(false);
                    QString barColor = (confidence >= 0.8f) ? Theme::hex(_tc.success)
                                     : (confidence >= 0.5f) ? Theme::hex(_tc.warning)
                                                             : Theme::hex(_tc.error);
                    confBar->setStyleSheet(
                        QString("QProgressBar { background: %2; border: none; border-radius: 2px; }"
                                "QProgressBar::chunk { background: %1; border-radius: 2px; }")
                            .arg(barColor, Theme::hex(_tc.inputBg)));
                    confBar->setToolTip(QString("Match confidence: %1%")
                        .arg(static_cast<int>(confidence * 100.0f)));
                    leftLayout->addWidget(confBar);
                }
                // Waveform
                waveform = new MiniWaveformWidget;
                auto sampleIt = m_audioSamples.find(clip.sourceFile);
                if (sampleIt != m_audioSamples.end()) {
                    waveform->setAudioShared(&sampleIt->second.samples, sampleIt->second.sampleRate,
                                       clip.start, clip.end);
                }
                connectMatchedClipWaveform(waveform, clipIdx);
                if (!clip.deletedRegions.empty())
                    waveform->setDeletedRegions(clip.deletedRegions);
                leftLayout->addWidget(waveform);
                // ├óΓÇ¥Γé¼├óΓÇ¥Γé¼ Right-panel controls ├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼├óΓÇ¥Γé¼
                if (matchState == 2) {
                    // Already confirmed ├óΓé¼ΓÇ¥ show status label instead of button
                    auto* confirmedLabel = new QLabel(QStringLiteral("\u2713  CONFIRMED"));
                    confirmedLabel->setFixedHeight(36);
                    confirmedLabel->setAlignment(Qt::AlignCenter);
                    confirmedLabel->setStyleSheet(
                        QString("QLabel { background: %1; color: %2; font-weight: bold; "
                        "font-size: 13px; border: 2px solid %3; border-radius: %4px; }")
                            .arg(Theme::hex(_tc.success), Theme::hex(_tc.textBright),
                                 Theme::hex(_tc.success), _radM));
                    rightLayout->addWidget(confirmedLabel);
                } else {
                    auto* confirmBtn = new QPushButton(QStringLiteral("\u2713  CONFIRM"));
                    confirmBtn->setFixedHeight(36);
                    confirmBtn->setToolTip("Confirm match");
                    confirmBtn->setCursor(Qt::PointingHandCursor);
                    confirmBtn->setStyleSheet(
                        QString("QPushButton { background: %1; color: %2; font-weight: bold; "
                        "font-size: 13px; border: 2px solid %3; border-radius: %4px; }"
                        "QPushButton:hover { background: %5; }").arg(
                            Theme::hex(_tc.successBtnBg), Theme::hex(_tc.textBright),
                            Theme::hex(_tc.success), _radM, Theme::hex(_tc.successBtnHover)));
                    connect(confirmBtn, &QPushButton::clicked, this, [this, clipIdx]() {
                        if (clipIdx < m_clips.size()) {
                            m_clips[clipIdx].matchState = 2;
                            populateLeftList();
                            updateCardMatchStyle(clipIdx);
                            updateSmartBar();
                        }
                    });
                    rightLayout->addWidget(confirmBtn);
                }
                auto* smallBtnRow = new QHBoxLayout;
                smallBtnRow->setSpacing(3);
                auto makeSmallBtn = [&_tc, &_rad](const QString& text, const QString& tip, const QString& bg) {
                    auto* btn = new QPushButton(text);
                    btn->setFixedHeight(26);
                    btn->setToolTip(tip);
                    btn->setCursor(Qt::PointingHandCursor);
                    btn->setStyleSheet(
                        QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                                "border-radius: %4px; font-size: 12px; padding: 2px 6px; }"
                                "QPushButton:hover { border-color: %5; }")
                            .arg(bg, Theme::hex(_tc.textSecondary), Theme::hex(_tc.controlBorder), _rad, Theme::hex(_tc.accent)));
                    return btn;
                };
                auto* playBtn = makeSmallBtn(QStringLiteral("\u25B6"), "Play clip", Theme::hex(_tc.successBg));
                auto* trimBtn = makeSmallBtn(QStringLiteral("\u26A1"), "Auto-trim", Theme::hex(_tc.surface3));
                connect(playBtn, &QPushButton::clicked, this, [this, clipIdx]() {
                    m_selectedClipIdx = static_cast<int>(clipIdx);
                    togglePlayClip(clipIdx);
                });
                connect(trimBtn, &QPushButton::clicked, this, [this, clipIdx]() {
                    autoTrimClip(clipIdx);
                });
                smallBtnRow->addWidget(playBtn);
                smallBtnRow->addWidget(trimBtn);
                rightLayout->addLayout(smallBtnRow);
                auto* rejectBtn = new QPushButton(QStringLiteral("\u2715  Reject"));
                rejectBtn->setFixedHeight(26);
                rejectBtn->setToolTip("Reject match");
                rejectBtn->setCursor(Qt::PointingHandCursor);
                rejectBtn->setStyleSheet(
                    QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                    "border-radius: %4px; font-size: 12px; padding: 2px 6px; }"
                    "QPushButton:hover { border-color: %5; }").arg(
                        Theme::hex(_tc.dangerBg), Theme::hex(_tc.textSecondary),
                        Theme::hex(_tc.controlBorder), _rad, Theme::hex(_tc.error)));
                connect(rejectBtn, &QPushButton::clicked, this, [this, clipIdx]() {
                    if (clipIdx < m_clips.size()) {
                        m_clips[clipIdx].matchState = 0;
                        m_clips[clipIdx].scriptLineNumber = -1;
                        m_clips[clipIdx].scriptSegment.clear();
                        m_clips[clipIdx].confidence = 0.0f;
                    }
                    // Defer rebuild so the clicked button isn't deleted during its own signal
                    QTimer::singleShot(0, this, [this]() { populateCards(); });
                });
                rightLayout->addWidget(rejectBtn);
                rightLayout->addStretch();
                // Reassign dropdown
                auto* lineCombo = new QComboBox;
                lineCombo->setFixedHeight(24);
                lineCombo->setStyleSheet(
                    QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
                    "padding: 2px 6px; border-radius: %4px; font-size: 12px; }").arg(
                        Theme::hex(_tc.inputBg), Theme::hex(_tc.textPrimary),
                        Theme::hex(_tc.inputBorder), _rad));
                lineCombo->addItem("Reassign...");
                for (size_t j = 0; j < displayLines.size(); ++j) {
                    const auto& dl = displayLines[j];
                    QString entry = QString("%1. %2: %3")
                        .arg(dl.displayNum)
                        .arg(QString::fromStdString(dl.character))
                        .arg(QString::fromStdString(dl.dialogue).left(20));
                    lineCombo->addItem(entry);
                }
                lineCombo->setCurrentIndex(0);
                auto displayLinesCopy = displayLines;
                connect(lineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this, clipIdx, displayLinesCopy](int comboIdx) {
                    if (clipIdx >= m_clips.size() || comboIdx <= 0) return;
                    size_t lineIdx = static_cast<size_t>(comboIdx - 1);
                    if (lineIdx < displayLinesCopy.size()) {
                        const auto& dl = displayLinesCopy[lineIdx];
                        m_clips[clipIdx].scriptLineNumber = dl.lineNumber;
                        m_clips[clipIdx].character = dl.character;
                        m_clips[clipIdx].scriptSegment = dl.character + ": " + dl.dialogue;
                        m_clips[clipIdx].matchState = 1;
                        m_clips[clipIdx].confidence = 1.0f;
                    }
                    // Defer rebuild so the combo isn't deleted during its own signal
                    QTimer::singleShot(0, this, [this]() { populateCards(); });
                });
                rightLayout->addWidget(lineCombo);
}
} // namespace rt