/*
 * AudioSyncTranscribeFileList.cpp - Transcription file list widgets.
 */

#include "panels/audio/AudioSync.h"

#include "Theme.h"
#include "media/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"

#include <spdlog/spdlog.h>

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace rt {

void AudioSync::refreshTranscribeFileList()
{
    if (!m_transcribeFileList) return;

    spdlog::debug("AudioSync: Clearing transcribe file list");
    m_transcribeFileList->blockSignals(true);
    m_transcribeFileList->clear();
    m_transcribeFileList->blockSignals(false);
    m_transcribeWaveforms.clear();
    m_transcribePlayBtns.clear();
    m_transcribeTimeLabels.clear();

    const auto& colors = Theme::colors();
    const auto radius = QString::number(Theme::metrics().radiusSm);

    if (m_allTranscriptionResults.size() < m_audioPaths.size())
        m_allTranscriptionResults.resize(m_audioPaths.size());

    for (size_t i = 0; i < m_audioPaths.size(); ++i) {
        QFileInfo fileInfo(QString::fromStdString(m_audioPaths[i]));
        bool done = !m_allTranscriptionResults[i].segments.empty();
        QString icon = done ? QStringLiteral("\u2713") : QStringLiteral("\u25CB");
        QString status = done ? "Transcribed" : "Pending";
        QColor foreground = done ? colors.success : colors.textTertiary;

        auto* widget = new QWidget;
        widget->setStyleSheet("background: transparent; border: none;");
        auto* layout = new QVBoxLayout(widget);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        auto* topRow = new QHBoxLayout;
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(8);

        auto* label = new QLabel(QString("<span style='color:%4'>%1</span>  <b>%2</b>  |  <span style='font-size:11px;color:%4'>%3</span>")
            .arg(icon, fileInfo.fileName().toHtmlEscaped(), status, foreground.name()));
        label->setTextFormat(Qt::RichText);
        label->setStyleSheet("border: none;");
        topRow->addWidget(label, 1);
        layout->addLayout(topRow);

        auto samplesIt = m_audioSamples.find(m_audioPaths[i]);
        if (samplesIt != m_audioSamples.end() && !samplesIt->second.samples.empty()) {
            double duration = static_cast<double>(samplesIt->second.samples.size()) /
                              static_cast<double>(samplesIt->second.sampleRate);

            auto* waveFrame = new QFrame;
            waveFrame->setStyleSheet(
                QString("QFrame { background: %1; border: 1px solid %2; border-radius: %3px; }")
                    .arg(Theme::hex(colors.inputBg), Theme::hex(colors.controlBorder), radius));
            auto* frameLayout = new QVBoxLayout(waveFrame);
            frameLayout->setContentsMargins(4, 4, 4, 4);
            frameLayout->setSpacing(0);

            auto* waveform = new MiniWaveformWidget;
            waveform->setFixedHeight(40);
            waveform->setAudioShared(&samplesIt->second.samples,
                                     samplesIt->second.sampleRate, 0.0, duration);
            waveform->setTrimHandlesVisible(false);

            connect(waveform, &MiniWaveformWidget::seekRequested,
                    this, [this, i, waveform](double timeSec) {
                waveform->setPlayhead(timeSec);
                waveform->setPlayheadVisible(true);
                if (m_previewFileIdx == static_cast<int>(i) && m_audioEngine) {
                    int64_t frame = static_cast<int64_t>(
                        timeSec * m_audioSamples[m_audioPaths[i]].sampleRate);
                    m_audioEngine->seekToFrame(frame);
                }
            });
            connect(waveform, &MiniWaveformWidget::playToggleRequested,
                    this, [this, i]() { playAudioFile(i); });
            connect(waveform, &MiniWaveformWidget::shuttleSpeedChanged,
                    this, [this, i](double speed) {
                if (!m_audioEngine) return;
                if (speed == 0.0) {
                    if (m_previewFileIdx == static_cast<int>(i))
                        m_audioEngine->pause();
                } else {
                    if (m_previewFileIdx != static_cast<int>(i))
                        playAudioFile(i);
                    if (m_previewFileIdx == static_cast<int>(i)) {
                        m_audioEngine->setPlaybackSpeed(speed);
                        if (m_audioEngine->transportState() != TransportState::Playing)
                            m_audioEngine->play();
                    }
                }
            });

            frameLayout->addWidget(waveform);
            layout->addWidget(waveFrame);
            m_transcribeWaveforms[i] = waveform;

            auto* controls = new QHBoxLayout;
            controls->setContentsMargins(0, 0, 0, 0);
            controls->setSpacing(6);

            auto* playButton = new QPushButton(QStringLiteral("\u25B6"));
            playButton->setFixedSize(22, 22);
            playButton->setStyleSheet(
                QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                        "border-radius: %4px; font-size: 14px; padding: 0; }"
                        "QPushButton:hover { background: %5; }")
                    .arg(Theme::hex(colors.inputBg), Theme::hex(colors.text),
                         Theme::hex(colors.controlBorder), radius, Theme::hex(colors.surface3)));
            connect(playButton, &QPushButton::clicked, this, [this, i]() {
                playAudioFile(i);
            });
            controls->addWidget(playButton);
            m_transcribePlayBtns[i] = playButton;

            auto* timeLabel = new QLabel(QStringLiteral("00:00.00"));
            timeLabel->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; font-family: monospace; border: none; }")
                    .arg(Theme::rgb(colors.textSecondary)));
            controls->addWidget(timeLabel);
            m_transcribeTimeLabels[i] = timeLabel;

            int durationMinutes = static_cast<int>(duration) / 60;
            int durationSeconds = static_cast<int>(duration) % 60;
            int durationCs = static_cast<int>(duration * 100) % 100;
            auto* durationLabel = new QLabel(QString("/ %1:%2.%3")
                .arg(durationMinutes).arg(durationSeconds, 2, 10, QChar('0')).arg(durationCs, 2, 10, QChar('0')));
            durationLabel->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; font-family: monospace; border: none; }")
                    .arg(Theme::rgb(colors.textTertiary)));
            controls->addWidget(durationLabel);
            controls->addStretch();
            layout->addLayout(controls);
        }

        auto* item = new QListWidgetItem;
        item->setSizeHint(QSize(0, std::max(widget->sizeHint().height(), 140) + 8));
        m_transcribeFileList->addItem(item);
        m_transcribeFileList->setItemWidget(item, widget);
    }
}

} // namespace rt
