/*
 * ThemeStyleEditor.cpp — Editor QSS: timeline-specific styles for
 * tracks, clips, playhead, timecodes, and character editor widgets.
 */

#include "ThemeStyleBuilder.h"

namespace rt::theme_style {

QString editorStyles(const StyleContext& context)
{
    const auto& c = context.colors;
    const auto& t = context.typography;
    const auto& m = context.metrics;

    return QStringLiteral(R"qss(
QWidget#TimelinePanel, QWidget#TimelineViewport, QWidget#TrackArea, QWidget#RulerArea {
    background: %1;
    color: %2;
}

QWidget#TrackHeader, QWidget#TrackControls, QWidget#PanelHeader {
    background: %3;
    border-bottom: 1px solid %4;
}

QWidget#TrackRow {
    background: %5;
    border-bottom: 1px solid %4;
}

QWidget#TrackRow[alternate="true"] {
    background: %6;
}

QFrame#ClipVideo, QLabel#ClipVideo {
    background: %7;
    border: 1px solid %8;
    border-radius: %9px;
    color: white;
}

QFrame#ClipAudio, QLabel#ClipAudio {
    background: %10;
    border: 1px solid %8;
    border-radius: %9px;
    color: white;
}

QFrame#ClipImage, QLabel#ClipImage {
    background: %11;
    border: 1px solid %8;
    border-radius: %9px;
    color: white;
}

QFrame#ClipTitle, QLabel#ClipTitle, QFrame#ClipGraphic, QLabel#ClipGraphic {
    background: %12;
    border: 1px solid %8;
    border-radius: %9px;
    color: white;
}

QFrame#ClipCharacter, QLabel#ClipCharacter, QFrame#ClipSpine, QLabel#ClipSpine {
    background: %13;
    border: 1px solid %8;
    border-radius: %9px;
    color: white;
}

QFrame#ClipSelected, QLabel#ClipSelected {
    background: %14;
    border: 2px solid %15;
    border-radius: %9px;
    color: white;
}

QFrame#Transition, QLabel#Transition {
    background: %16;
    border: 1px solid %8;
    border-radius: %9px;
    color: white;
}

QWidget#Playhead {
    background: %15;
    min-width: 2px;
    max-width: 2px;
}

QLabel#TimecodeLabel, QLabel#DurationLabel {
    color: %17;
    font-family: "%18";
    font-size: %19px;
    font-weight: 700;
}

QLabel#AxisLabel {
    color: %17;
    font-weight: 700;
    font-size: %20px;
}

QLabel#ProjectItemName {
    color: %2;
    font-weight: 700;
}

QLabel#ProjectItemMeta, QLabel#CurrentProject {
    color: %17;
}

QLabel#CurrentBadge {
    color: %21;
    font-weight: 700;
}

QLabel#PreviewLabel {
    color: %2;
    font-size: %22px;
    font-weight: 700;
}

QComboBox#OutfitCombo {
    color: %2;
    font-weight: 700;
}

QComboBox#StanceCombo {
    color: %21;
    font-weight: 700;
}

QComboBox#AnimCombo::down-arrow {
    border-top: none;
    border-bottom: 5px solid %17;
}

QPushButton#StarBtn {
    background: %23;
    color: %21;
    border: 1px solid %4;
    border-radius: %24px;
    min-width: 28px;
    max-width: 28px;
    min-height: 28px;
    max-height: 28px;
    padding: 0px;
}

QPushButton#StarBtn:hover {
    background: %25;
}

QPushButton#StarBtn:checked {
    background: %26;
    border: 2px solid %21;
}

QPushButton#StarBtn:disabled {
    background: %23;
    color: %27;
    border-color: %4;
}
)qss")
        .arg(Theme::rgb(c.trackBg))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.panelHeaderBg))
        .arg(Theme::rgb(c.trackDivider))
        .arg(Theme::rgb(c.trackBg))
        .arg(Theme::rgb(c.trackBgAlt))
        .arg(Theme::rgb(c.clipVideo))
        .arg(Theme::rgba(c.textBright, 80))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.clipAudio))
        .arg(Theme::rgb(c.clipImage))
        .arg(Theme::rgb(c.clipGraphic))
        .arg(Theme::rgb(c.clipCharacter))
        .arg(Theme::rgb(c.clipSelected))
        .arg(Theme::rgb(c.playhead))
        .arg(Theme::rgb(c.transition))
        .arg(Theme::rgb(c.textSecondary))
        .arg(t.monoFamily)
        .arg(t.sizeMono)
        .arg(t.sizeCaption)
        .arg(Theme::rgb(c.warning))
        .arg(t.sizeH2)
        .arg(Theme::rgb(c.surface2))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.controlBgHover))
        .arg(Theme::rgb(c.warningBg))
        .arg(Theme::rgb(c.textDisabled));
}

} // namespace rt::theme_style