/*
 * AudioSyncUITranscribePage.cpp - Transcribe side-panel page for AudioSync.
 * Split from AudioSyncUI.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "Theme.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QProcess>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <spdlog/spdlog.h>
#include <algorithm>
namespace rt {
void AudioSync::setupTranscribePage()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();
    const auto& t = Theme::typography();
    const auto surf0 = Theme::hex(c.surface0);
    const auto surf1 = Theme::hex(c.surface1);
    const auto surf2 = Theme::hex(c.surface2);
    const auto surf3 = Theme::hex(c.surface3);
    const auto brd   = Theme::hex(c.border);
    const auto brdL  = Theme::hex(c.borderLight);
    const auto txt1  = Theme::hex(c.textPrimary);
    const auto txt2  = Theme::hex(c.textSecondary);
    const auto txt3  = Theme::hex(c.textTertiary);
    const auto txtD  = Theme::hex(c.textDisabled);
    const auto acc   = Theme::hex(c.accent);
    const auto accH  = Theme::hex(c.accentHover);
    const auto accS  = Theme::hex(c.accentSubtle);
    const auto accDm = Theme::hex(c.accentDim);
    const auto inp   = Theme::hex(c.inputBg);
    const auto inpB  = Theme::hex(c.inputBorder);
    const auto sucBg = Theme::hex(c.successBtnBg);
    const auto sucBH = Theme::hex(c.successBtnHover);
    const auto sucTx = Theme::hex(c.success);
    const auto danBg = Theme::hex(c.dangerBg);
    const auto danBH = Theme::hex(c.dangerBgHover);
    const auto danTx = Theme::hex(c.dangerText);
    const auto errC  = Theme::hex(c.error);
    const auto errBg = Theme::hex(c.errorBg);
    const auto warnC = Theme::hex(c.warning);
    const auto ctrlBd = Theme::hex(c.controlBorder);
    const auto scrTr = Theme::hex(c.scrollbarTrack);
    const auto scrTh = Theme::hex(c.scrollbarThumb);
    const auto rad   = QString::number(m.radiusSm);
    const auto radM  = QString::number(m.radiusMd);
    m_transcribePage = new QWidget;
    auto* transcribePageLayout = new QVBoxLayout(m_transcribePage);
    transcribePageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                              m.spacingXxl, m.spacingXxl);
    transcribePageLayout->setSpacing(m.spacingXl);

    // Title row
    auto* transcribeTitleRow = new QHBoxLayout;
    auto* transcribeTitle = new QLabel("Transcribe");
    transcribeTitle->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    transcribeTitleRow->addWidget(transcribeTitle, 1);

    auto* transcribeCloseBtn = new QPushButton("\u2715");
    transcribeCloseBtn->setFixedSize(36, 36);
    transcribeCloseBtn->setCursor(Qt::PointingHandCursor);
    transcribeCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(transcribeCloseBtn, &QPushButton::clicked,
            this, &AudioSync::hideAudioSidePanel);
    transcribeTitleRow->addWidget(transcribeCloseBtn);
    transcribePageLayout->addLayout(transcribeTitleRow);

    // Description
    auto* transcribeDesc = new QLabel(
        "Run Whisper AI transcription on imported audio files.\n"
        "This generates timestamped text segments for sync.");
    transcribeDesc->setWordWrap(true);
    transcribeDesc->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: %1;")
        .arg(Theme::rgb(c.textTertiary)));
    transcribePageLayout->addWidget(transcribeDesc);

    // Model selector
    auto* modelLabel = new QLabel("WHISPER MODEL");
    modelLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    transcribePageLayout->addWidget(modelLabel);

    m_modelCombo = new QComboBox;
    m_modelCombo->addItems({"tiny", "base", "small", "medium", "large-v2", "large-v3"});
    m_modelCombo->setCurrentIndex(2);  // default to "small" for better accuracy
    m_modelCombo->setMinimumHeight(44);
    m_modelCombo->setStyleSheet(QStringLiteral(
        "QComboBox {"
        "  background: %1; color: %2; border: 1px solid %3;"
        "  padding: 10px 14px; border-radius: %4px; font-size: 14px;"
        "}"
        "QComboBox:hover { border-color: %5; }"
        "QComboBox::drop-down {"
        "  subcontrol-origin: padding; subcontrol-position: center right;"
        "  width: 32px; border: none;"
        "}"
        "QComboBox::down-arrow { image: none; border: none; }"
        "QComboBox QAbstractItemView {"
        "  background: %6; color: %7; border: 1px solid %8;"
        "  border-radius: %9px; padding: 4px;"
        "  selection-background-color: %10;"
        "  outline: none;"
        "}"
        "QComboBox QAbstractItemView::item {"
        "  padding: 8px 12px; min-height: 28px;"
        "}"
        "QComboBox QAbstractItemView::item:hover { background: %11; }")
        .arg(inp, txt1, inpB, radM,
             Theme::rgb(c.accent),
             Theme::rgb(c.surface2), txt1, Theme::rgb(c.border),
             radM, Theme::rgb(c.accentDim),
             Theme::rgb(c.surface3)));
    transcribePageLayout->addWidget(m_modelCombo);

    // Transcribe button
    m_transcribeBtn = new QPushButton("\u26A1  Transcribe All");
    m_transcribeBtn->setMinimumHeight(48);
    m_transcribeBtn->setCursor(Qt::PointingHandCursor);
    m_transcribeBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: white; border: none;"
        "  border-radius: %2px; font-size: 16px;"
        "  font-weight: 700; padding: 12px 24px; }"
        "QPushButton:hover { background: %3; }"
        "QPushButton:pressed { background: %4; }")
        .arg(Theme::rgb(c.primaryBtnBg))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.primaryBtnHover))
        .arg(Theme::rgb(c.accent)));
    connect(m_transcribeBtn, &QPushButton::clicked, this, &AudioSync::onTranscribeClicked);
    transcribePageLayout->addWidget(m_transcribeBtn);

    // Progress bar
    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(false);
    m_progressBar->setFixedHeight(12);
    m_progressBar->setStyleSheet(
        QString("QProgressBar { background: %1; border: none; border-radius: 5px; }"
        "QProgressBar::chunk { background: %2; border-radius: 5px; }").arg(inp, sucTx));
    transcribePageLayout->addWidget(m_progressBar);

    // Transcribe status
    m_transcribeStatus = new QLabel;
    m_transcribeStatus->setWordWrap(true);
    m_transcribeStatus->setStyleSheet(QStringLiteral(
        "font-size: 14px; color: %1;")
        .arg(Theme::rgb(c.textTertiary)));
    transcribePageLayout->addWidget(m_transcribeStatus);

    // File list with per-file transcription status indicators
    auto* transcribeFilesLabel = new QLabel("FILES");
    transcribeFilesLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    transcribePageLayout->addWidget(transcribeFilesLabel);

    m_transcribeFileList = new QListWidget;
    m_transcribeFileList->setStyleSheet(
        QString("QListWidget { background: %1; border: 1px solid %2; "
        "border-radius: %3px; font-size: 13px; }"
        "QListWidget::item { padding: 6px 10px; border-bottom: 1px solid %4; }"
        "QListWidget::item:last { border-bottom: none; }")
        .arg(inp, inpB, radM, Theme::rgb(c.borderLight)));
    m_transcribeFileList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_transcribeFileList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        auto* item = m_transcribeFileList->itemAt(pos);
        if (!item) return;
        int row = m_transcribeFileList->row(item);
        if (row < 0 || row >= static_cast<int>(m_audioPaths.size())) return;

        bool hasTranscription = (row < static_cast<int>(m_allTranscriptionResults.size()) &&
                                 !m_allTranscriptionResults[row].segments.empty());

        QMenu menu(m_transcribeFileList);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 6px; padding: 4px; }"
            "QMenu::item { padding: 8px 24px; border-radius: 4px; }"
            "QMenu::item:selected { background: %4; color: %5; }"
            "QMenu::separator { height: 1px; background: %6; margin: 4px 8px; }")
            .arg(Theme::hex(Theme::colors().surface2))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().border))
            .arg(Theme::hex(Theme::colors().accentDim))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().borderLight)));

        QAction* transcribeAct = menu.addAction(
            hasTranscription
                ? QStringLiteral("\U0001F504  Re-transcribe")
                : QStringLiteral("\u25B6  Transcribe"));

        QAction* clearAct = nullptr;
        if (hasTranscription) {
            clearAct = menu.addAction(QStringLiteral("\U0001F5D1  Clear Transcription"));
        }

        menu.addSeparator();

        QAction* removeAct = menu.addAction(QStringLiteral("\u2716  Remove File"));

        QAction* showInExplorerAct = menu.addAction(QStringLiteral("\U0001F4C2  Show in Explorer"));

        QAction* chosen = menu.exec(m_transcribeFileList->viewport()->mapToGlobal(pos));
        if (chosen == transcribeAct) {
            startTranscriptionForFile(static_cast<size_t>(row));
        } else if (chosen == clearAct) {
            clearTranscriptionForFile(static_cast<size_t>(row));
        } else if (chosen == removeAct) {
            // Remove this audio file from the transcribe list
            std::string removed = m_audioPaths[static_cast<size_t>(row)];
            m_audioPaths.erase(m_audioPaths.begin() + row);
            m_audioSamples.erase(removed);
            if (row < static_cast<int>(m_allTranscriptionResults.size()))
                m_allTranscriptionResults.erase(m_allTranscriptionResults.begin() + row);
            // Remove clips from this file
            auto it = std::remove_if(m_clips.begin(), m_clips.end(),
                [&removed](const SyncClip& clip) { return clip.sourceFile == removed; });
            m_clips.erase(it, m_clips.end());
            refreshTranscribeFileList();
            populateClipList();
            populateLeftList();
            updateWorkflowState();
            m_transcribeStatus->setText(QString("Removed file: %1")
                .arg(QString::fromStdString(removed)));
            spdlog::info("AudioSync: Removed audio file '{}' from transcribe list", removed);
        } else if (chosen == showInExplorerAct) {
            QString filePath = QString::fromStdString(m_audioPaths[static_cast<size_t>(row)]);
            QFileInfo fi(filePath);
            if (fi.exists()) {
                QString dir = QDir::toNativeSeparators(fi.absolutePath());
                QProcess::startDetached("explorer", {"/select,", QDir::toNativeSeparators(filePath)});
            }
        }
    });

    // Clear transcription buttons
    auto* clearBtnLayout = new QHBoxLayout;
    clearBtnLayout->setSpacing(m.spacingSm);

    m_clearSelectedTranscriptionBtn = new QPushButton("Clear Selected");
    m_clearSelectedTranscriptionBtn->setCursor(Qt::PointingHandCursor);
    m_clearSelectedTranscriptionBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: 13px;"
        "  padding: 6px 14px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textSecondary))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.surface3)));
    connect(m_clearSelectedTranscriptionBtn, &QPushButton::clicked, this, [this] {
        auto* item = m_transcribeFileList->currentItem();
        if (!item) return;
        int idx = m_transcribeFileList->row(item);
        clearTranscriptionForFile(static_cast<size_t>(idx));
    });
    clearBtnLayout->addWidget(m_clearSelectedTranscriptionBtn);

    m_clearAllTranscriptionsBtn = new QPushButton("Clear All");
    m_clearAllTranscriptionsBtn->setCursor(Qt::PointingHandCursor);
    m_clearAllTranscriptionsBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: 13px;"
        "  padding: 6px 14px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textSecondary))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.surface3)));
    connect(m_clearAllTranscriptionsBtn, &QPushButton::clicked, this, &AudioSync::clearAllTranscriptions);
    clearBtnLayout->addWidget(m_clearAllTranscriptionsBtn);

    transcribePageLayout->addLayout(clearBtnLayout);

    m_audioSidePanelStack->addWidget(m_transcribePage);   // index 2

    // --- MATCH page (index 3) â€” character filter list â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
}

} // namespace rt
