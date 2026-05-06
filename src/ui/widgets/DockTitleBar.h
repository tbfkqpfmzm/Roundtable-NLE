/*
 * DockTitleBar.h — Custom title bar widget for QDockWidget panels.
 *
 * Replaces the default QDockWidget title bar with a polished custom one:
 *   - Left-aligned title text
 *   - Subtle separator at bottom
 *   - Themed via Theme::colors()
 *   - Supports drag-to-move and double-click-to-float
 *
 * Usage:   dock->setTitleBarWidget(new DockTitleBar(dock, "Panel Name"));
 */

#pragma once

#include <QWidget>

class QLabel;
class QDockWidget;
class QMainWindow;
class QTabBar;

namespace rt {

class DockTitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit DockTitleBar(QDockWidget* dock, const QString& title,
                          QWidget* parent = nullptr);

    /// Change the title text at runtime.
    void setTitle(const QString& title);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    /// Re-apply theme colors/fonts (call after Theme::apply()).
    void applyTheme();

    /// Auto-hide when dock is in a tab group (title is already shown in the tab).
    void updateVisibility();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;

 private:
    void buildUI();
    bool isTabbed() const;
    void updateTitleLayout();

 protected:
    void resizeEvent(QResizeEvent* event) override;

 private:
    QDockWidget* m_dock{nullptr};
    QLabel*      m_titleLabel{nullptr};
    bool         m_tabbed{false};
    bool         m_isAudioMeters{false};
};

} // namespace rt
