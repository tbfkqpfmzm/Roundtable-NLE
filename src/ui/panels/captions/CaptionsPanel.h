/*
 * CaptionsPanel â€” captions/subtitle editor panel.
 *
 * Displays caption clips from the active sequence's caption tracks,
 * allows editing text/speaker/style, and provides a Transcribe button.
 */

#pragma once

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>

#include <cstdint>
#include <vector>

namespace rt {

class Timeline;
class CaptionClip;
class Track;
class CommandStack;

class CaptionsPanel : public QWidget
{
 Q_OBJECT

public:
 explicit CaptionsPanel(QWidget* parent = nullptr);
 ~CaptionsPanel() override = default;

 void setTimeline(Timeline* timeline);
 void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }

 /// Rebuild the caption list from the active timeline.
 void refresh();

 /// Select and scroll to a specific caption clip by track/clip index.
 void selectCaption(size_t trackIndex, size_t clipIndex);

signals:
 /// Emitted when the user clicks "Transcribe" (selected clip or sequence).
 void transcribeRequested();

 /// Emitted when the user clicks "Clear All Captions".
 void clearAllRequested();

 /// Emitted when a caption is selected (for playhead jump).
 void captionSelected(int64_t timelineIn);

 /// Emitted when caption content changes.
 void captionEdited();

private slots:
 void onListSelectionChanged();
 void onTextChanged();
 void onSpeakerChanged();
 void onAddCaption();
 void onDeleteCaption();
 void onClearAll();
 void onFillGaps();

private:
 void buildUI();
 void applyTheme();
 void updateButtonStates();

 /// Info about a caption clip shown in the list.
 struct CaptionEntry {
 size_t trackIndex{0};
 size_t clipIndex{0};
 CaptionClip* clip{nullptr};
 };

 Timeline* m_timeline{nullptr};
 CommandStack* m_commandStack{nullptr};

 // UI widgets
 QListWidget* m_captionList{nullptr};
 QTextEdit* m_textEdit{nullptr};
 QLineEdit* m_speakerEdit{nullptr};
 QComboBox* m_positionCombo{nullptr};
 QComboBox* m_fontCombo{nullptr};
 QSpinBox* m_fontSizeSpin{nullptr};
 QPushButton* m_transcribeBtn{nullptr};
 QPushButton* m_clearAllBtn{nullptr};
 QPushButton* m_addBtn{nullptr};
 QPushButton* m_deleteBtn{nullptr};
 QPushButton* m_fillGapsBtn{nullptr};
 QLabel* m_timeLabel{nullptr};
 QLabel* m_countLabel{nullptr};
 QWidget* m_editorSection{nullptr};

 // Data
 std::vector<CaptionEntry> m_entries;
 bool m_updatingUI{false};
};

} // namespace rt
