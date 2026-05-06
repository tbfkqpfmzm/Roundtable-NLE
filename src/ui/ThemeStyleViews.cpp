/*
 * ThemeStyleViews.cpp — View QSS: GroupBox, labels, lists, tables,
 * progress bars, tooltips, dialogs.
 */

#include "ThemeStyleBuilder.h"

namespace rt::theme_style {

QString viewStyles(const StyleContext& context)
{
    const auto& c = context.colors;
    const auto& t = context.typography;
    const auto& m = context.metrics;

    return QStringLiteral(R"qss(
QGroupBox {
    background: %1;
    border: 1px solid %2;
    border-radius: %3px;
    margin-top: 16px;
    padding: 12px;
    font-weight: 600;
    color: %4;
}

QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0px 6px;
    color: %5;
    background: %1;
}

QFrame#PanelFrame, QWidget#PanelFrame, QWidget#LeftCard, QWidget#RightPanel, QWidget#DetailsSidebar {
    background: %1;
    border: 1px solid %2;
    border-radius: %3px;
}

QWidget#PreviewArea, QWidget#PreviewHeader, QWidget#PreviewToolbar,
QWidget#ControlsBar, QWidget#TransformTabBg {
    background: %6;
    border: 1px solid %2;
    border-radius: %7px;
}

QLabel {
    color: %4;
    background: transparent;
}

QLabel#PanelTitle {
    color: %4;
    font-size: %8px;
    font-weight: 700;
}

QLabel#SectionTitle, QLabel#SectionLabel {
    color: %5;
    font-size: %9px;
    font-weight: 600;
}

QLabel#FieldLabel, QLabel#PropLabel, QLabel#ControlLabel, QLabel#DetailFieldLabel {
    color: %10;
    font-size: %11px;
}

QLabel#StatusLabel, QLabel#EstimateLbl, QLabel#DetailFieldValue {
    color: %10;
}

QLabel#EmptyStateLabel, QLabel#EmptyLabel, QLabel#PlaceholderLabel, QLabel#PreviewPlaceholder {
    color: %12;
    font-style: italic;
}

QListWidget, QTreeWidget, QTableWidget, QTableView, QTreeView, QListView {
    background: %13;
    color: %4;
    border: 1px solid %2;
    border-radius: %7px;
    alternate-background-color: %14;
    selection-background-color: %5;
    selection-color: %15;
    outline: none;
}

QListWidget::item, QTreeWidget::item, QListView::item, QTreeView::item {
    min-height: 26px;
    padding: 4px 8px;
    border-radius: %16px;
}

QListWidget::item:hover, QTreeWidget::item:hover, QListView::item:hover, QTreeView::item:hover {
    background: %17;
}

QListWidget::item:selected, QTreeWidget::item:selected, QListView::item:selected, QTreeView::item:selected {
    background: %5;
    color: %15;
}

QHeaderView::section {
    background: %18;
    color: %10;
    border: none;
    border-right: 1px solid %2;
    border-bottom: 1px solid %2;
    padding: 5px 8px;
    font-weight: 600;
}

QTableWidget::item, QTableView::item {
    padding: 4px 8px;
    border-bottom: 1px solid %19;
}

QTableWidget::item:selected, QTableView::item:selected {
    background: %5;
    color: %15;
}

QProgressBar {
    background: %13;
    border: 1px solid %2;
    border-radius: %7px;
    color: %4;
    text-align: center;
    min-height: 18px;
}

QProgressBar::chunk {
    background: %5;
    border-radius: %16px;
}

QProgressBar#DownloadProgress::chunk {
    background: %20;
}

QToolTip {
    background: %21;
    color: %4;
    border: 1px solid %22;
    border-radius: %16px;
    padding: 5px 8px;
}

QMessageBox, QFileDialog {
    background: %6;
    color: %4;
}
)qss")
        .arg(Theme::rgb(c.panelContentBg))
        .arg(Theme::rgb(c.panelBorder))
        .arg(m.radiusLg)
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.accent))
        .arg(Theme::rgb(c.surface1))
        .arg(m.radiusMd)
        .arg(t.sizeH2)
        .arg(t.sizeCaption)
        .arg(Theme::rgb(c.textSecondary))
        .arg(t.sizeSmall)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface0))
        .arg(Theme::rgb(c.alternateBase))
        .arg(Theme::rgb(c.highlightedText))
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.controlBgHover))
        .arg(Theme::rgb(c.panelHeaderBg))
        .arg(Theme::rgb(c.separator))
        .arg(Theme::rgb(c.success))
        .arg(Theme::rgb(c.toolTipBase))
        .arg(Theme::rgb(c.borderLight));
}

} // namespace rt::theme_style