/*
 * AudioSyncImportFileList.cpp - Imported audio list widgets and sorting.
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
#include <numeric>

namespace rt {

void AudioSync::addAudioFileListItem(const QString& fullPath)
{
    if (!m_audioFileList) return;

    QFileInfo fileInfo(fullPath);
    QString fileName = fileInfo.fileName();
    qint64 bytes = fileInfo.size();
    QString fileSize;
    if (bytes < 1024)
        fileSize = QString("%1 B").arg(bytes);
    else if (bytes < 1024 * 1024)
        fileSize = QString("%1 KB").arg(bytes / 1024);
    else
        fileSize = QString("%1 MB").arg(bytes / (1024 * 1024));

    QString createdDate = fileInfo.birthTime().isValid()
        ? fileInfo.birthTime().toString("yyyy-MM-dd")
        : fileInfo.lastModified().toString("yyyy-MM-dd");

    QString durationStr = QStringLiteral("\u2014");
    auto samplesIt = m_audioSamples.find(fullPath.toStdString());
    if (samplesIt != m_audioSamples.end() && samplesIt->second.sampleRate > 0 &&
        !samplesIt->second.samples.empty()) {
        double duration = static_cast<double>(samplesIt->second.samples.size()) /
                          static_cast<double>(samplesIt->second.sampleRate);
        int minutes = static_cast<int>(duration) / 60;
        int seconds = static_cast<int>(duration) % 60;
        durationStr = QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
    }

    const auto& colors = Theme::colors();
    const auto radius = QString::number(Theme::metrics().radiusSm);

    size_t fileIndex = SIZE_MAX;
    auto pathStr = fullPath.toStdString();
    for (size_t i = 0; i < m_audioPaths.size(); ++i) {
        if (m_audioPaths[i] == pathStr) {
            fileIndex = i;
            break;
        }
    }

    auto* widget = new QWidget;
    widget->setStyleSheet("background: transparent; border: none;");
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    auto* infoLabel = new QLabel(QString("<b>%1</b><br><span style='font-size:11px;color:%5'>%2  |  %3  |  %4</span>")
        .arg(fileName.toHtmlEscaped(), fileSize, createdDate, durationStr,
             Theme::rgb(colors.textTertiary)));
    infoLabel->setTextFormat(Qt::RichText);
    infoLabel->setStyleSheet("border: none;");
    topRow->addWidget(infoLabel, 1);
    layout->addLayout(topRow);

    if (fileIndex != SIZE_MAX) {
        auto samplesIt2 = m_audioSamples.find(fullPath.toStdString());
        if (samplesIt2 != m_audioSamples.end() && !samplesIt2->second.samples.empty()) {
            double duration = static_cast<double>(samplesIt2->second.samples.size()) /
                              static_cast<double>(samplesIt2->second.sampleRate);

            auto* waveFrame = new QFrame;
            waveFrame->setStyleSheet(
                QString("QFrame { background: %1; border: 1px solid %2; border-radius: %3px; }")
                    .arg(Theme::hex(colors.inputBg), Theme::hex(colors.controlBorder), radius));
            auto* frameLayout = new QVBoxLayout(waveFrame);
            frameLayout->setContentsMargins(4, 4, 4, 4);
            frameLayout->setSpacing(0);

            auto* waveform = new MiniWaveformWidget;
            waveform->setFixedHeight(40);
            waveform->setAudioShared(&samplesIt2->second.samples,
                                     samplesIt2->second.sampleRate, 0.0, duration);
            waveform->setTrimHandlesVisible(false);

            connect(waveform, &MiniWaveformWidget::seekRequested,
                    this, [this, fileIndex, waveform](double timeSec) {
                waveform->setPlayhead(timeSec);
                waveform->setPlayheadVisible(true);
                if (m_previewFileIdx == static_cast<int>(fileIndex) && m_audioEngine) {
                    int64_t frame = static_cast<int64_t>(
                        timeSec * m_audioSamples[m_audioPaths[fileIndex]].sampleRate);
                    m_audioEngine->seekToFrame(frame);
                }
            });
            connect(waveform, &MiniWaveformWidget::playToggleRequested,
                    this, [this, fileIndex]() { playAudioFile(fileIndex); });
            connect(waveform, &MiniWaveformWidget::shuttleSpeedChanged,
                    this, [this, fileIndex](double speed) {
                if (!m_audioEngine) return;
                if (speed == 0.0) {
                    if (m_previewFileIdx == static_cast<int>(fileIndex))
                        m_audioEngine->pause();
                } else {
                    if (m_previewFileIdx != static_cast<int>(fileIndex))
                        playAudioFile(fileIndex);
                    if (m_previewFileIdx == static_cast<int>(fileIndex)) {
                        m_audioEngine->setPlaybackSpeed(speed);
                        if (m_audioEngine->transportState() != TransportState::Playing)
                            m_audioEngine->play();
                    }
                }
            });

            frameLayout->addWidget(waveform);
            layout->addWidget(waveFrame);
            m_fileWaveforms[fileIndex] = waveform;

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
            connect(playButton, &QPushButton::clicked, this, [this, fileIndex]() {
                playAudioFile(fileIndex);
            });
            controls->addWidget(playButton);
            m_filePlayBtns[fileIndex] = playButton;

            auto* timeLabel = new QLabel(QStringLiteral("00:00.00"));
            timeLabel->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; font-family: monospace; border: none; }")
                    .arg(Theme::rgb(colors.textSecondary)));
            controls->addWidget(timeLabel);
            m_fileTimeLabels[fileIndex] = timeLabel;

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
    }

    auto* item = new QListWidgetItem;
    item->setData(Qt::UserRole, fullPath);
    item->setData(Qt::UserRole + 1, fileInfo.birthTime().isValid()
        ? fileInfo.birthTime() : fileInfo.lastModified());
    item->setData(Qt::UserRole + 2, bytes);
    item->setSizeHint(QSize(0, std::max(widget->sizeHint().height(), 160) + 8));
    m_audioFileList->addItem(item);
    m_audioFileList->setItemWidget(item, widget);
}

void AudioSync::sortAudioFileList(int criterion)
{
    if (!m_audioFileList || m_audioPaths.empty()) return;

    spdlog::debug("AudioSync: Clearing audio file list (sort)");
    m_audioFileList->blockSignals(true);
    m_audioFileList->clear();
    m_audioFileList->blockSignals(false);

    std::vector<size_t> indices(m_audioPaths.size());
    std::iota(indices.begin(), indices.end(), 0);

    switch (criterion) {
    case 0:
        std::sort(indices.begin(), indices.end(), [this](size_t left, size_t right) {
            return QFileInfo(QString::fromStdString(m_audioPaths[left])).fileName().toLower()
                 < QFileInfo(QString::fromStdString(m_audioPaths[right])).fileName().toLower();
        });
        break;
    case 1:
        std::sort(indices.begin(), indices.end(), [this](size_t left, size_t right) {
            QFileInfo leftInfo(QString::fromStdString(m_audioPaths[left]));
            QFileInfo rightInfo(QString::fromStdString(m_audioPaths[right]));
            return leftInfo.lastModified() > rightInfo.lastModified();
        });
        break;
    case 2:
        std::sort(indices.begin(), indices.end(), [this](size_t left, size_t right) {
            return QFileInfo(QString::fromStdString(m_audioPaths[left])).size()
                 > QFileInfo(QString::fromStdString(m_audioPaths[right])).size();
        });
        break;
    }

    std::vector<std::string> sorted;
    sorted.reserve(m_audioPaths.size());
    for (size_t index : indices)
        sorted.push_back(m_audioPaths[index]);
    m_audioPaths = std::move(sorted);

    m_audioFileList->clear();
    m_fileWaveforms.clear();
    m_filePlayBtns.clear();
    m_fileTimeLabels.clear();
    for (const auto& path : m_audioPaths)
        addAudioFileListItem(QString::fromStdString(path));
}

} // namespace rt
