/*
 * ThemeStyleControls.cpp — Control QSS: inputs, buttons, checkboxes,
 * sliders, combos.
 */

#include "ThemeStyleBuilder.h"

namespace rt::theme_style {

QString controlStyles(const StyleContext& context)
{
    const auto& c = context.colors;
    const auto& m = context.metrics;

    return QStringLiteral(R"qss(
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: %4px;
    padding: 4px 8px;
    min-height: %5px;
    selection-background-color: %6;
}

QTextEdit, QPlainTextEdit {
    min-height: 72px;
}

QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border-color: %7;
}

QLineEdit:disabled, QTextEdit:disabled, QPlainTextEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    color: %8;
    background: %9;
}

QLineEdit[readOnly="true"] {
    background: %10;
    color: %11;
}

QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
    background: transparent;
    border: none;
    width: 16px;
}

QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-bottom: 5px solid %11;
    width: 0px;
    height: 0px;
}

QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid %11;
    width: 0px;
    height: 0px;
}

QComboBox {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: %4px;
    padding: 0px 24px 0px 10px;
    min-height: %5px;
}

QComboBox:hover {
    border-color: %12;
}

QComboBox:focus {
    border-color: %7;
}

QComboBox::drop-down {
    border: none;
    width: 28px;
    subcontrol-position: right center;
}

QComboBox::down-arrow {
    image: none;
    border-left: 6px solid transparent;
    border-right: 6px solid transparent;
    border-top: 6px solid %11;
    width: 0px;
    height: 0px;
}

QComboBox QAbstractItemView {
    background: %13;
    color: %2;
    border: 1px solid %3;
    border-radius: %4px;
    selection-background-color: %14;
    selection-color: %15;
    outline: none;
    padding: 4px;
}

QComboBox QAbstractItemView::item {
    padding: 6px 10px;
    min-height: 28px;
    border-radius: %16px;
}

QComboBox QAbstractItemView::item:hover {
    background: %17;
}

QPushButton {
    background: %18;
    color: %2;
    border: 1px solid %19;
    border-radius: %4px;
    padding: 5px 14px;
    font-weight: 600;
    min-height: %20px;
}

QPushButton:hover {
    background: %17;
    border-color: %12;
}

QPushButton:pressed {
    background: %21;
}

QPushButton:disabled {
    color: %8;
    background: %10;
    border-color: %22;
}

QPushButton:checked {
    background: %14;
    color: %15;
    border-color: %14;
}

QPushButton#rt-btn-primary, QPushButton#PrimaryBtn, QPushButton#CreateBtn, QPushButton#ExportBtn, QPushButton#AddQueueBtn {
    background: %23;
    color: white;
    border: none;
    font-weight: 700;
}

QPushButton#rt-btn-primary:hover, QPushButton#PrimaryBtn:hover, QPushButton#CreateBtn:hover, QPushButton#ExportBtn:hover, QPushButton#AddQueueBtn:hover {
    background: %24;
}

QPushButton#rt-btn-primary:pressed, QPushButton#ExportBtn:pressed {
    background: %25;
}

QPushButton#rt-btn-success, QPushButton#SaveBtn {
    background: %26;
    color: white;
    border: none;
    font-weight: 700;
}

QPushButton#rt-btn-success:hover, QPushButton#SaveBtn:hover {
    background: %27;
}

QPushButton#rt-btn-danger, QPushButton#DangerBtn, QPushButton#CancelBtn, QPushButton#LayerToolBtnDanger {
    background: %28;
    color: %29;
    border: none;
}

QPushButton#rt-btn-danger:hover, QPushButton#DangerBtn:hover, QPushButton#CancelBtn:hover, QPushButton#LayerToolBtnDanger:hover {
    background: %30;
}

QPushButton#rt-btn-ghost, QPushButton#GhostBtn, QPushButton#SecondaryBtn, QPushButton#BrowseBtn, QPushButton#TransportBtn,
QPushButton#ResetViewBtn, QPushButton#LayerToolBtn {
    background: transparent;
    border: 1px solid %22;
    color: %11;
}

QPushButton#rt-btn-ghost:hover, QPushButton#GhostBtn:hover, QPushButton#SecondaryBtn:hover, QPushButton#BrowseBtn:hover,
QPushButton#TransportBtn:hover, QPushButton#ResetViewBtn:hover, QPushButton#LayerToolBtn:hover {
    background: %17;
    color: %2;
}

QPushButton#rt-btn-subtle {
    background: %31;
    border: 1px solid %22;
    color: %11;
}

QPushButton#rt-btn-subtle:hover {
    background: %13;
    border-color: %12;
}

QCheckBox, QRadioButton {
    color: %2;
    spacing: 8px;
}

QCheckBox::indicator, QRadioButton::indicator {
    width: 16px;
    height: 16px;
    border: 1px solid %19;
    background: %1;
}

QCheckBox::indicator {
    border-radius: %16px;
}

QRadioButton::indicator {
    border-radius: 8px;
}

QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: %14;
    border-color: %14;
}

QSlider::groove:horizontal {
    height: 4px;
    background: %19;
    border-radius: 2px;
}

QSlider::handle:horizontal {
    background: %14;
    width: 14px;
    height: 14px;
    margin: -5px 0px;
    border-radius: 7px;
}

QSlider::sub-page:horizontal {
    background: %14;
    border-radius: 2px;
}
)qss")
        .arg(Theme::rgb(c.inputBg))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.inputBorder))
        .arg(m.radiusMd)
        .arg(m.controlHeightSm - 8)
        .arg(Theme::rgb(c.inputSelection))
        .arg(Theme::rgb(c.inputBorderFocus))
        .arg(Theme::rgb(c.textDisabled))
        .arg(Theme::rgb(c.surface0))
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.textSecondary))
        .arg(Theme::rgb(c.borderLight))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.accent))
        .arg(Theme::rgb(c.highlightedText))
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.controlBgHover))
        .arg(Theme::rgb(c.controlBg))
        .arg(Theme::rgb(c.controlBorder))
        .arg(m.controlHeightSm - 10)
        .arg(Theme::rgb(c.controlBgActive))
        .arg(Theme::rgb(c.border))
        .arg(Theme::rgb(c.primaryBtnBg))
        .arg(Theme::rgb(c.primaryBtnHover))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.successBtnBg))
        .arg(Theme::rgb(c.successBtnHover))
        .arg(Theme::rgb(c.dangerBg))
        .arg(Theme::rgb(c.dangerText))
        .arg(Theme::rgb(c.dangerBgHover))
        .arg(Theme::rgb(c.surface2));
}

} // namespace rt::theme_style