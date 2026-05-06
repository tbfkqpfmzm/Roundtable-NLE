/*
 * ThemeStyleCore.cpp — Core QSS: base widgets, docks, menus, toolbars,
 * scrollbars, tabs, splitters, status bar.
 */

#include "ThemeStyleBuilder.h"

namespace rt::theme_style {

QString coreStyles(const StyleContext& context)
{
    const auto& c = context.colors;
    const auto& t = context.typography;
    const auto& m = context.metrics;

    return QStringLiteral(R"qss(
/* ── Universal Font ── */
* {
    font-family: "%1";
    font-size: %2px;
}

QWidget {
    background: %3;
    color: %4;
    selection-background-color: %5;
    selection-color: %6;
}

QMainWindow, QDialog {
    background: %3;
}

/* ── Dock Widgets ── */
QDockWidget {
    color: %4;
    font-size: %7px;
    font-weight: 600;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
    margin: 3px;
    padding: 4px;
    border: %8px solid %9;
}

QDockWidget::title {
    background: %10;
    color: %11;
    padding: 0px;
    margin: 0px;
    border: none;
    height: 0px;
    min-height: 0px;
    max-height: 0px;
}

QDockWidget::close-button, QDockWidget::float-button {
    background: transparent;
    border: none;
    padding: 0px;
    width: 0px;
    height: 0px;
    icon-size: 0px;
}

/* ── Menu Bar ── */
QMenuBar {
    background: %12;
    border-bottom: 1px solid %9;
    spacing: 0px;
}

QMenuBar::item {
    padding: 5px 10px;
    background: transparent;
    color: %4;
}

QMenuBar::item:selected {
    background: %13;
    border-radius: %14px;
}

QMenuBar::item:pressed {
    background: %15;
    color: %6;
}

/* ── Menus ── */
QMenu {
    background: %16;
    border: 1px solid %9;
    border-radius: %17px;
    padding: 4px 0px;
}

QMenu::item {
    padding: 6px 30px 6px 20px;
    color: %4;
}

QMenu::item:selected {
    background: %15;
    color: %6;
    border-radius: %14px;
}

QMenu::item:disabled {
    color: %18;
}

QMenu::separator {
    height: 1px;
    background: %19;
    margin: 4px 8px;
}

/* ── ToolBar ── */
QToolBar {
    background: %12;
    border: none;
    spacing: 2px;
    padding: 2px;
}

QToolBar::separator {
    width: 1px;
    background: %19;
    margin: 4px 4px;
}

QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: %14px;
    padding: 4px;
    color: %4;
}

QToolButton:hover {
    background: %13;
    border-color: %20;
}

QToolButton:checked {
    background: %15;
    color: %6;
}

QToolButton:pressed {
    background: %21;
}

/* ── Status Bar ── */
QStatusBar {
    background: %12;
    border-top: 1px solid %9;
    color: %22;
    font-size: %7px;
}

QStatusBar::item {
    border: none;
}

/* ── Tab Bar ── */
QTabBar {
    qproperty-elideMode: 3;
}

QTabBar::tab {
    background: %12;
    color: %22;
    padding: 6px 14px;
    border-top: 2px solid transparent;
    border-bottom: none;
    border-left: none;
    border-right: none;
    margin-right: 1px;
    font-size: %7px;
}

QTabBar::tab:selected {
    background: %23;
    color: %4;
    border-top-color: %15;
}

QTabBar::tab:hover:!selected {
    background: %13;
    color: %4;
}

QTabWidget::pane {
    border: none;
    background: %23;
}

/* ── Splitters ── */
QSplitter::handle, QMainWindow::separator {
    background: %9;
    width: 6px;
    height: 6px;
}

QSplitter::handle:hover, QMainWindow::separator:hover {
    background: %15;
}

/* ── ScrollBars ── */
QScrollBar:vertical {
    background: %24;
    width: 16px;
    margin: 0px;
    border: none;
}

QScrollBar::handle:vertical {
    background: %25;
    min-height: 40px;
    border-radius: 7px;
    margin: 1px;
}

QScrollBar:horizontal {
    background: %24;
    height: 16px;
    margin: 0px;
    border: none;
}

QScrollBar::handle:horizontal {
    background: %25;
    min-width: 40px;
    border-radius: 7px;
    margin: 1px;
}

QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
    background: %26;
}

QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page {
    width: 0px;
    height: 0px;
    background: transparent;
}
)qss")
        .arg(t.fontFamily)              // 1
        .arg(t.sizeBody)                // 2
        .arg(Theme::rgb(c.window))      // 3
        .arg(Theme::rgb(c.textPrimary)) // 4
        .arg(Theme::rgb(c.inputSelection)) // 5
        .arg(Theme::rgb(c.highlightedText)) // 6
        .arg(t.sizeCaption)             // 7
        .arg(m.borderMedium)            // 8
        .arg(Theme::rgb(c.border))      // 9
        .arg(Theme::rgb(c.dockTitleBg)) // 10
        .arg(Theme::rgb(c.dockTitleText)) // 11
        .arg(Theme::rgb(c.surface2))    // 12
        .arg(Theme::rgb(c.controlBgHover)) // 13
        .arg(m.radiusSm)                // 14
        .arg(Theme::rgb(c.accent))      // 15
        .arg(Theme::rgb(c.surface3))    // 16
        .arg(m.radiusMd)                // 17
        .arg(Theme::rgb(c.textTertiary)) // 18
        .arg(Theme::rgb(c.separator))   // 19
        .arg(Theme::rgb(c.borderLight)) // 20
        .arg(Theme::rgb(c.controlBgActive)) // 21
        .arg(Theme::rgb(c.textSecondary)) // 22
        .arg(Theme::rgb(c.surface1))    // 23
        .arg(Theme::rgb(c.scrollbarTrack)) // 24
        .arg(Theme::rgb(c.scrollbarThumb)) // 25
        .arg(Theme::rgb(c.scrollbarThumbHover)); // 26
}

} // namespace rt::theme_style