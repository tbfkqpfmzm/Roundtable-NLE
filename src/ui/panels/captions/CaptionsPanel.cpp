/*
 * CaptionsPanel.cpp â€” captions/subtitle editor panel.
 */

#include "panels/captions/CaptionsPanel.h"
#include "Theme.h"
#include "Constants.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/CaptionClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <spdlog/spdlog.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QSplitter>
#include <QHeaderView>
#include <QScrollBar>

#include <algorithm>

namespace rt {

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Helpers
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static QString ticksToTimecode(int64_t ticks, double fps = 30.0)
{
 if (ticks < 0) return "00:00:00:00";
 int64_t totalFrames = static_cast<int64_t>(ticks * fps / static_cast<double>(kTicksPerSecond));
 int frames = static_cast<int>(totalFrames % static_cast<int64_t>(fps));
 int64_t totalSec = totalFrames / static_cast<int64_t>(fps);
 int seconds = static_cast<int>(totalSec % 60);
 int minutes = static_cast<int>((totalSec / 60) % 60);
 int hours = static_cast<int>(totalSec / 3600);
 return QString("%1:%2:%3:%4")
 .arg(hours, 2, 10, QLatin1Char('0'))
 .arg(minutes, 2, 10, QLatin1Char('0'))
 .arg(seconds, 2, 10, QLatin1Char('0'))
 .arg(frames, 2, 10, QLatin1Char('0'));
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Construction
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
CaptionsPanel::CaptionsPanel(QWidget* parent)
 : QWidget(parent)
{
 buildUI();
 applyTheme();
}

void CaptionsPanel::buildUI()
{
 auto* mainLayout = new QVBoxLayout(this);
 mainLayout->setContentsMargins(4, 4, 4, 4);
 mainLayout->setSpacing(4);

 // â”€â”€ Top action bar â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 auto* actionBar = new QHBoxLayout;
 actionBar->setSpacing(6);

 m_transcribeBtn = new QPushButton("\xF0\x9F\x8E\x99 Transcribe", this);
 m_transcribeBtn->setObjectName("transcribeBtn");
 m_transcribeBtn->setFixedHeight(30);
 m_transcribeBtn->setToolTip("Transcribe the selected audio/video clip on the timeline");
 actionBar->addWidget(m_transcribeBtn);

 m_clearAllBtn = new QPushButton("\xF0\x9F\x97\x91 Clear All", this);
 m_clearAllBtn->setObjectName("clearAllBtn");
 m_clearAllBtn->setFixedHeight(30);
 m_clearAllBtn->setToolTip("Remove all caption clips from the timeline");
 actionBar->addWidget(m_clearAllBtn);

 m_fillGapsBtn = new QPushButton("\xE2\x96\xB6 Fill Gaps", this);
 m_fillGapsBtn->setFixedHeight(30);
 m_fillGapsBtn->setToolTip("Extend selected caption to fill gap before the next caption");
 actionBar->addWidget(m_fillGapsBtn);

 actionBar->addStretch();

 m_addBtn = new QPushButton("+", this);
 m_addBtn->setFixedSize(30, 30);
 m_addBtn->setToolTip("Add empty caption at playhead");
 actionBar->addWidget(m_addBtn);

 m_deleteBtn = new QPushButton("\xE2\x9C\x95", this);
 m_deleteBtn->setFixedSize(30, 30);
 m_deleteBtn->setToolTip("Delete selected caption");
 actionBar->addWidget(m_deleteBtn);

 mainLayout->addLayout(actionBar);

 // â”€â”€ Caption count + info bar â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 m_countLabel = new QLabel("No captions", this);
 m_countLabel->setObjectName("captionCountLabel");
 mainLayout->addWidget(m_countLabel);

 // â”€â”€ Caption list â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 m_captionList = new QListWidget(this);
 m_captionList->setSelectionMode(QAbstractItemView::SingleSelection);
 m_captionList->setMinimumHeight(100);
 mainLayout->addWidget(m_captionList, 1);

 // â”€â”€ Editor section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 m_editorSection = new QWidget(this);
 auto* editorFrame = new QFrame(m_editorSection);
 editorFrame->setObjectName("captionEditorFrame");
 auto* editorOuter = new QVBoxLayout(m_editorSection);
 editorOuter->setContentsMargins(0, 0, 0, 0);
 editorOuter->addWidget(editorFrame);

 auto* editorLayout = new QVBoxLayout(editorFrame);
 editorLayout->setContentsMargins(6, 6, 6, 6);
 editorLayout->setSpacing(4);

 // Time label
 m_timeLabel = new QLabel("--:--:--:-- \xE2\x86\x92 --:--:--:--", editorFrame);
 m_timeLabel->setObjectName("captionTimeLabel");
 editorLayout->addWidget(m_timeLabel);

 // Speaker
 auto* speakerRow = new QHBoxLayout;
 speakerRow->addWidget(new QLabel("Speaker:", editorFrame));
 m_speakerEdit = new QLineEdit(editorFrame);
 m_speakerEdit->setPlaceholderText("Speaker name...");
 speakerRow->addWidget(m_speakerEdit);
 editorLayout->addLayout(speakerRow);

 // Position row
 auto* posRow = new QHBoxLayout;
 posRow->addWidget(new QLabel("Position:", editorFrame));
 m_positionCombo = new QComboBox(editorFrame);
 m_positionCombo->addItems({"Bottom", "Top", "Middle"});
 m_positionCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
 posRow->addWidget(m_positionCombo);
 editorLayout->addLayout(posRow);

 // Font row
 auto* fontRow = new QHBoxLayout;
 fontRow->addWidget(new QLabel("Font:", editorFrame));
 m_fontCombo = new QComboBox(editorFrame);
 m_fontCombo->addItems({"Arial", "Helvetica", "Roboto", "Open Sans", "Noto Sans"});
 m_fontCombo->setEditable(true);
 m_fontCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
 fontRow->addWidget(m_fontCombo);

 m_fontSizeSpin = new QSpinBox(editorFrame);
 m_fontSizeSpin->setRange(8, 200);
 m_fontSizeSpin->setValue(32);
 m_fontSizeSpin->setSuffix("px");
 m_fontSizeSpin->setFixedWidth(70);
 fontRow->addWidget(m_fontSizeSpin);
 editorLayout->addLayout(fontRow);

 // Text edit
 m_textEdit = new QTextEdit(editorFrame);
 m_textEdit->setPlaceholderText("Caption text...");
 m_textEdit->setMaximumHeight(80);
 editorLayout->addWidget(m_textEdit);

 mainLayout->addWidget(m_editorSection);

 // â”€â”€ Connections â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 connect(m_captionList, &QListWidget::currentRowChanged,
 this, &CaptionsPanel::onListSelectionChanged);
 connect(m_textEdit, &QTextEdit::textChanged,
 this, &CaptionsPanel::onTextChanged);
 connect(m_speakerEdit, &QLineEdit::textChanged,
 this, &CaptionsPanel::onSpeakerChanged);
 connect(m_transcribeBtn, &QPushButton::clicked,
 this, &CaptionsPanel::transcribeRequested);
 connect(m_clearAllBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onClearAll);
 connect(m_addBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onAddCaption);
 connect(m_deleteBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onDeleteCaption);
 connect(m_fillGapsBtn, &QPushButton::clicked,
 this, &CaptionsPanel::onFillGaps);

 updateButtonStates();
}

void CaptionsPanel::applyTheme()
{
 const auto& tc = Theme::colors();
 setStyleSheet(QString(
 "QFrame#captionEditorFrame { border: 1px solid %1; border-radius: 4px; }"
 "QLabel#captionTimeLabel { color: %2; font-family: 'Consolas', monospace; font-size: 11px; }"
 "QLabel#captionCountLabel { color: %3; font-size: 10px; padding: 1px 2px; }"
 "QListWidget { background: %4; color: %5; border: 1px solid %1; }"
 "QTextEdit { background: %4; color: %5; border: 1px solid %1; }"
 "QLineEdit { background: %4; color: %5; border: 1px solid %1; padding: 2px 4px; }"
 "QLabel { color: %5; }"
 "QPushButton { background: %6; color: %5; border: 1px solid %1; border-radius: 3px; padding: 4px 10px; }"
 "QPushButton:hover { background: %7; }"
 "QPushButton:disabled { color: %8; background: %9; }"
 "QPushButton#transcribeBtn { background: %10; font-weight: bold; }"
 "QPushButton#transcribeBtn:hover { background: %11; }"
 "QPushButton#clearAllBtn { background: %12; color: %13; }"
 "QPushButton#clearAllBtn:hover { background: %14; }"
 )
 .arg(Theme::hex(tc.border)) // 1
 .arg(Theme::hex(tc.accent)) // 2
 .arg(Theme::hex(tc.textSecondary)) // 3
 .arg(Theme::hex(tc.surface1)) // 4
 .arg(Theme::hex(tc.textPrimary)) // 5
 .arg(Theme::hex(tc.surface2)) // 6
 .arg(Theme::hex(tc.surface3)) // 7
 .arg(Theme::hex(tc.textDisabled)) // 8
 .arg(Theme::hex(tc.surface1)) // 9
 .arg(Theme::hex(tc.primaryBtnBg)) // 10
 .arg(Theme::hex(tc.primaryBtnHover))// 11
 .arg(Theme::hex(tc.dangerBg)) // 12
 .arg(Theme::hex(tc.dangerText)) // 13
 .arg(Theme::hex(tc.dangerBgHover)));// 14
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Timeline binding
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void CaptionsPanel::setTimeline(Timeline* timeline)
{
 m_timeline = timeline;
 refresh();
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Refresh â€” rebuild caption list from timeline
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void CaptionsPanel::refresh()
{
 m_updatingUI = true;
 m_entries.clear();
 m_captionList->clear();

 if (m_timeline) {
 for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
 Track* track = m_timeline->track(ti);
 if (track->type() != TrackType::Caption) continue;

 for (size_t ci = 0; ci < track->clipCount(); ++ci) {
 Clip* clip = track->clip(ci);
 if (clip->clipType() != ClipType::Caption) continue;

 auto* cc = static_cast<CaptionClip*>(clip);
 CaptionEntry entry;
 entry.trackIndex = ti;
 entry.clipIndex = ci;
 entry.clip = cc;
 m_entries.push_back(entry);
 }
 }

 // Sort by timeline position
 std::sort(m_entries.begin(), m_entries.end(),
 [](const CaptionEntry& a, const CaptionEntry& b) {
 return a.clip->timelineIn() < b.clip->timelineIn();
 });

 // Populate list
 for (const auto& entry : m_entries) {
 QString tc_in = ticksToTimecode(entry.clip->timelineIn());
 QString text = QString::fromStdString(entry.clip->text());
 if (text.length() > 60) text = text.left(57) + "...";

 QString speaker = QString::fromStdString(entry.clip->speaker());
 QString label;
 if (!speaker.isEmpty())
 label = QString("[%1] %2 %3").arg(tc_in, speaker, text);
 else
 label = QString("[%1] %2").arg(tc_in, text);

 m_captionList->addItem(label);
 }
 }

 m_updatingUI = false;

 // Update count label
 if (m_entries.empty()) {
 m_countLabel->setText("No captions");
 } else {
 double totalSec = 0.0;
 for (const auto& e : m_entries)
 totalSec += static_cast<double>(e.clip->duration()) / kTicksPerSecond;
 m_countLabel->setText(QString("%1 caption%2 \xC2\xB7 %3 total")
 .arg(m_entries.size())
 .arg(m_entries.size() == 1 ? "" : "s")
 .arg(QString::number(totalSec, 'f', 1) + "s"));
 }

 // Clear editor if no entries
 if (m_entries.empty()) {
 m_textEdit->clear();
 m_speakerEdit->clear();
 m_timeLabel->setText("--:--:--:-- \xE2\x86\x92 --:--:--:--");
 }

 updateButtonStates();
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Select a specific caption
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void CaptionsPanel::selectCaption(size_t trackIndex, size_t clipIndex)
{
 for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
 if (m_entries[i].trackIndex == trackIndex &&
 m_entries[i].clipIndex == clipIndex) {
 m_captionList->setCurrentRow(i);
 return;
 }
 }
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Slots
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void CaptionsPanel::onListSelectionChanged()
{
 if (m_updatingUI) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 m_updatingUI = true;

 const auto& entry = m_entries[row];
 CaptionClip* cc = entry.clip;

 m_textEdit->setPlainText(QString::fromStdString(cc->text()));
 m_speakerEdit->setText(QString::fromStdString(cc->speaker()));
 m_positionCombo->setCurrentIndex(static_cast<int>(cc->position()));
 m_fontCombo->setCurrentText(QString::fromStdString(cc->fontFamily()));
 m_fontSizeSpin->setValue(cc->fontSize());

 QString tcIn = ticksToTimecode(cc->timelineIn());
 QString tcOut = ticksToTimecode(cc->timelineOut());
 m_timeLabel->setText(QString("%1 \u2192 %2").arg(tcIn, tcOut));

 m_updatingUI = false;

 emit captionSelected(cc->timelineIn());
}

void CaptionsPanel::onTextChanged()
{
 if (m_updatingUI) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 CaptionClip* cc = m_entries[row].clip;
 std::string newText = m_textEdit->toPlainText().toStdString();
 std::string oldText = cc->text();
 if (newText == oldText) return;

 // Apply directly (don't call refresh â€” it destroys cursor position)
 cc->setText(newText);

 CaptionsPanel* self = this;
 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Edit Caption Text",
 [cc, newText, self]() { cc->setText(newText); self->refresh(); },
 [cc, oldText, self]() { cc->setText(oldText); self->refresh(); }));
 }

 // Update list item text inline.
 // Use the captured row index instead of currentItem() to avoid a
 // dangling-pointer crash: QTextEdit::textChanged is emitted during
 // key-press processing (inside QTextDocumentPrivate::finishEdit),
 // and Qt delivers signals synchronously.  If the QListWidget item
 // was destroyed between the initial row lookup and this update
 // (e.g. via re-entrant refresh from a nested signal), currentItem()
 // would return a freed pointer.  item(row) is NULL-safe when the
 // row is out of range.
 m_updatingUI = true;
 QString tc_in = ticksToTimecode(cc->timelineIn());
 QString text = m_textEdit->toPlainText();
 if (text.length() > 60) text = text.left(57) + "...";
 QString speaker = QString::fromStdString(cc->speaker());
 QString label;
 if (!speaker.isEmpty())
 label = QString("[%1] %2 %3").arg(tc_in, speaker, text);
 else
 label = QString("[%1] %2").arg(tc_in, text);
 if (row >= 0 && row < m_captionList->count()) {
     if (auto* item = m_captionList->item(row))
         item->setText(label);
 }
 m_updatingUI = false;

 emit captionEdited();
}

void CaptionsPanel::onSpeakerChanged()
{
 if (m_updatingUI) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 CaptionClip* cc = m_entries[row].clip;
 std::string newSpeaker = m_speakerEdit->text().toStdString();
 std::string oldSpeaker = cc->speaker();
 if (newSpeaker == oldSpeaker) return;

 // Apply directly
 cc->setSpeaker(newSpeaker);

 CaptionsPanel* self = this;
 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Edit Caption Speaker",
 [cc, newSpeaker, self]() { cc->setSpeaker(newSpeaker); self->refresh(); },
 [cc, oldSpeaker, self]() { cc->setSpeaker(oldSpeaker); self->refresh(); }));
 }

 // Update list item text inline.
 // Use captured row index (not currentItem()) to avoid dangling pointer
 // — see onTextChanged() for rationale.
 m_updatingUI = true;
 QString tc_in = ticksToTimecode(cc->timelineIn());
 QString text = QString::fromStdString(cc->text());
 if (text.length() > 60) text = text.left(57) + "...";
 QString speaker = m_speakerEdit->text();
 QString label;
 if (!speaker.isEmpty())
 label = QString("[%1] %2 %3").arg(tc_in, speaker, text);
 else
 label = QString("[%1] %2").arg(tc_in, text);
 if (row >= 0 && row < m_captionList->count()) {
     if (auto* item = m_captionList->item(row))
         item->setText(label);
 }
 m_updatingUI = false;

 emit captionEdited();
}

void CaptionsPanel::onAddCaption()
{
 if (!m_timeline) return;

 // Find or create a caption track
 Track* captionTrack = nullptr;
 for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
 if (m_timeline->track(i)->type() == TrackType::Caption) {
 captionTrack = m_timeline->track(i);
 break;
 }
 }
 if (!captionTrack) {
 captionTrack = m_timeline->addCaptionTrack();
 }

 // Create a 3-second caption at the playhead
 int64_t playhead = m_timeline->playheadPosition();
 auto clip = std::make_unique<CaptionClip>();
 clip->setTimelineIn(playhead);
 clip->setDuration(kTicksPerSecond * 3); // 3 seconds
 clip->setText("New caption");
 uint64_t clipId = clip->id();

 // Capture for lambda â€” we need shared ownership of the clip for undo
 auto clipShared = std::make_shared<std::unique_ptr<CaptionClip>>(std::move(clip));
 Timeline* tl = m_timeline;
 CaptionsPanel* self = this;

 auto doIt = [tl, clipShared, self]() {
 // Find caption track
 Track* trk = nullptr;
 for (size_t i = 0; i < tl->trackCount(); ++i) {
 if (tl->track(i)->type() == TrackType::Caption) {
 trk = tl->track(i);
 break;
 }
 }
 if (!trk) trk = tl->addCaptionTrack();
 if (*clipShared) {
 trk->addClip(std::move(*clipShared));
 }
 self->refresh();
 emit self->captionEdited();
 };

 auto undoIt = [tl, clipId, clipShared, self]() {
 for (size_t i = 0; i < tl->trackCount(); ++i) {
 auto* trk = tl->track(i);
 if (trk->type() != TrackType::Caption) continue;
 auto removed = trk->removeClipById(clipId);
 if (removed) {
 *clipShared = std::unique_ptr<CaptionClip>(
 static_cast<CaptionClip*>(removed.release()));
 self->refresh();
 emit self->captionEdited();
 return;
 }
 }
 };

 if (m_commandStack) {
 spdlog::info("CaptionsPanel: executing Add Caption command");
 m_commandStack->execute(std::make_unique<LambdaCommand>(
 "Add Caption", doIt, undoIt));
 } else {
 spdlog::warn("CaptionsPanel: no CommandStack, executing directly");
 doIt();
 }

 refresh();
 emit captionEdited();
}

void CaptionsPanel::onDeleteCaption()
{
 if (!m_timeline) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 const auto& entry = m_entries[row];
 uint64_t clipId = entry.clip->id();
 Timeline* tl = m_timeline;
 CaptionsPanel* self = this;

 // Find the caption track
 Track* track = tl->track(entry.trackIndex);
 if (!track) return;

 // Remove the clip now (execute)
 auto removed = track->removeClipById(clipId);
 if (!removed) return;

 auto clipShared = std::make_shared<std::unique_ptr<Clip>>(std::move(removed));

 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Delete Caption",
 [tl, clipId, clipShared, self]() {
 // Redo: remove again
 for (size_t i = 0; i < tl->trackCount(); ++i) {
 auto* trk = tl->track(i);
 if (trk->type() != TrackType::Caption) continue;
 auto re = trk->removeClipById(clipId);
 if (re) {
 *clipShared = std::move(re);
 self->refresh();
 emit self->captionEdited();
 return;
 }
 }
 },
 [tl, clipShared, self]() {
 // Undo: re-add the clip
 Track* trk = nullptr;
 for (size_t i = 0; i < tl->trackCount(); ++i) {
 if (tl->track(i)->type() == TrackType::Caption) {
 trk = tl->track(i);
 break;
 }
 }
 if (!trk || !*clipShared) return;
 trk->addClip(std::move(*clipShared));
 self->refresh();
 emit self->captionEdited();
 }));
 }

 refresh();
 emit captionEdited();
}

void CaptionsPanel::onClearAll()
{
 if (!m_timeline || m_entries.empty()) return;

 emit clearAllRequested();
}

void CaptionsPanel::onFillGaps()
{
 if (!m_timeline) return;

 int row = m_captionList->currentRow();
 if (row < 0 || row >= static_cast<int>(m_entries.size())) return;

 const auto& entry = m_entries[row];
 CaptionClip* cc = entry.clip;
 Track* track = m_timeline->track(entry.trackIndex);
 if (!track) return;

 // Find this clip's index in the track and check for a next clip
 size_t idx = track->findClipIndexById(cc->id());
 if (idx >= track->clipCount()) return;
 if (idx + 1 >= track->clipCount()) return; // no next clip, nothing to fill

 const Clip* nextClip = track->clip(idx + 1);
 int64_t gapEnd = nextClip->timelineIn();
 int64_t currentEnd = cc->timelineOut();
 if (gapEnd <= currentEnd) return; // no gap

 int64_t oldDuration = cc->duration();
 int64_t newDuration = gapEnd - cc->timelineIn();

 // Apply directly
 cc->setDuration(newDuration);

 CaptionsPanel* self = this;
 uint64_t clipId = cc->id();
 if (m_commandStack) {
 m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
 "Fill Caption Gap",
 [cc, newDuration, self]() { cc->setDuration(newDuration); self->refresh(); emit self->captionEdited(); },
 [cc, oldDuration, self]() { cc->setDuration(oldDuration); self->refresh(); emit self->captionEdited(); }));
 }

 refresh();

 // Re-select the same caption
 for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
 if (m_entries[i].clip->id() == clipId) {
 m_captionList->setCurrentRow(i);
 break;
 }
 }

 emit captionEdited();
}

void CaptionsPanel::updateButtonStates()
{
 bool hasCaptions = !m_entries.empty();
 bool hasSelection = m_captionList->currentRow() >= 0;

 m_deleteBtn->setEnabled(hasSelection);
 m_fillGapsBtn->setEnabled(hasSelection);
 m_clearAllBtn->setEnabled(hasCaptions);
 m_editorSection->setVisible(hasCaptions);
}

} // namespace rt
