/*
 * TimelineWorkspaceToggleMaximize.cpp — Panel maximize/restore logic extracted
 * from TimelineWorkspace.cpp.
 *
 * Contains: togglePanelMaximize(), helper classes (EdgeEventLogger),
 * and static helper logParentChain().
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "panels/timeline/TimelinePanel.h"

#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLayout>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPoint>
#include <QTimer>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

namespace rt {

// --- Panel maximize/restore logic (tilde key) ---
// Helper: log parent chain
static void logParentChain(QWidget* w, const char* label) {
    QStringList chain;
    QWidget* cur = w;
    while (cur) {
        chain << QString::fromUtf8(cur->metaObject()->className()) + ":" + cur->objectName();
        cur = cur->parentWidget();
    }
    spdlog::info("[togglePanelMaximize][{}] Parent chain: {}", label, chain.join(" <- ").toStdString());
}

// Event filter for show/hide events
class EdgeEventLogger : public QObject {
public:
    EdgeEventLogger(QObject* parent = nullptr) : QObject(parent) {}
protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            spdlog::info("[EdgeEventLogger] SHOW {} {}",
                         obj->metaObject()->className(), obj->objectName().toStdString());
        } else if (event->type() == QEvent::Hide) {
            spdlog::info("[EdgeEventLogger] HIDE {} {}",
                         obj->metaObject()->className(), obj->objectName().toStdString());
        }
        return QObject::eventFilter(obj, event);
    }
};

void TimelineWorkspace::togglePanelMaximize() {
    // --- Force show/raise on edge QMainWindows and their dock widgets ---
    if (m_edgeSplitter) {
        for (int i = 0; i < m_edgeSplitter->count(); ++i) {
            QMainWindow* edgeWin = qobject_cast<QMainWindow*>(m_edgeSplitter->widget(i));
            if (edgeWin && edgeWin != m_innerMainWindow) {
                // Install event logger
                static EdgeEventLogger* logger = nullptr;
                if (!logger) logger = new EdgeEventLogger(this);
                edgeWin->installEventFilter(logger);
                for (QDockWidget* dock : edgeWin->findChildren<QDockWidget*>()) {
                    dock->installEventFilter(logger);
                }
                edgeWin->show();
                edgeWin->raise();
                for (QDockWidget* dock : edgeWin->findChildren<QDockWidget*>()) {
                    dock->show();
                    dock->raise();
                }
            }
        }
    }

    // --- QTimer::singleShot to re-show edge columns after event loop ---
    if (m_edgeSplitter) {
        QTimer::singleShot(0, this, [this]() {
            for (int i = 0; i < m_edgeSplitter->count(); ++i) {
                QMainWindow* edgeWin = qobject_cast<QMainWindow*>(m_edgeSplitter->widget(i));
                if (edgeWin && edgeWin != m_innerMainWindow) {
                    edgeWin->show();
                    edgeWin->raise();
                    for (QDockWidget* dock : edgeWin->findChildren<QDockWidget*>()) {
                        dock->show();
                        dock->raise();
                    }
                    QRect geom = edgeWin->geometry();
                    spdlog::info("[togglePanelMaximize][QTimer] Edge QMainWindow '%s' geom=[%d,%d %dx%d] visible=%s", 
                        edgeWin->objectName().toStdString().c_str(),
                        geom.x(), geom.y(), geom.width(), geom.height(),
                        edgeWin->isVisible() ? "true" : "false");
                    logParentChain(edgeWin, "QTimer1");
                    for (QDockWidget* dock : edgeWin->findChildren<QDockWidget*>()) {
                        logParentChain(dock, "QTimer1-Dock");
                    }
                }
            }
            // --- Double-deferred workaround ---
            QTimer::singleShot(0, this, [this]() {
                for (int i = 0; i < m_edgeSplitter->count(); ++i) {
                    QMainWindow* edgeWin = qobject_cast<QMainWindow*>(m_edgeSplitter->widget(i));
                    if (edgeWin && edgeWin != m_innerMainWindow) {
                        edgeWin->show();
                        edgeWin->raise();
                        for (QDockWidget* dock : edgeWin->findChildren<QDockWidget*>()) {
                            dock->show();
                            dock->raise();
                        }
                        QRect geom = edgeWin->geometry();
                        spdlog::info("[togglePanelMaximize][QTimer2] Edge QMainWindow '%s' geom=[%d,%d %dx%d] visible=%s", 
                            edgeWin->objectName().toStdString().c_str(),
                            geom.x(), geom.y(), geom.width(), geom.height(),
                            edgeWin->isVisible() ? "true" : "false");
                        logParentChain(edgeWin, "QTimer2");
                        for (QDockWidget* dock : edgeWin->findChildren<QDockWidget*>()) {
                            logParentChain(dock, "QTimer2-Dock");
                        }
                    }
                }
                // Log focus widget
                QWidget* fw = QApplication::focusWidget();
                if (fw) {
                    spdlog::info("[togglePanelMaximize][QTimer2] Focus widget: %s %s",
                                 fw->metaObject()->className(), fw->objectName().toStdString().c_str());
                    logParentChain(fw, "FocusWidget");
                }
            });
        });
    }

    // --- Re-log splitter child widget widths after forced show/raise ---
    if (m_edgeSplitter) {
        QStringList widgetInfo;
        for (int i = 0; i < m_edgeSplitter->count(); ++i) {
            QWidget* w = m_edgeSplitter->widget(i);
            if (w) {
                widgetInfo << QString("%1: width=%2 visible=%3").arg(w->objectName(), QString::number(w->width()), w->isVisible() ? "true" : "false");
            }
        }
        spdlog::info("[togglePanelMaximize] (after force show) Splitter widget widths: {}", widgetInfo.join(" | ").toStdString());
    }

    // --- Log splitter child widget widths after restore ---
    if (m_edgeSplitter) {
        QStringList widgetInfo;
        for (int i = 0; i < m_edgeSplitter->count(); ++i) {
            QWidget* w = m_edgeSplitter->widget(i);
            if (w) {
                widgetInfo << QString("%1: width=%2 visible=%3").arg(w->objectName(), QString::number(w->width()), w->isVisible() ? "true" : "false");
            }
        }
        spdlog::info("[togglePanelMaximize] Splitter widget widths after restore: {}", widgetInfo.join(" | ").toStdString());
    }

    // --- Log dock widget areas and geometry after restore ---
    for (auto it = m_dockWidgets.begin(); it != m_dockWidgets.end(); ++it) {
        QDockWidget* dock = it.value();
        if (dock) {
            QMainWindow* parentWin = nullptr;
            QWidget* parent = dock->parentWidget();
            while (parent) {
                parentWin = qobject_cast<QMainWindow*>(parent);
                if (parentWin) break;
                parent = parent->parentWidget();
            }
            Qt::DockWidgetArea area = Qt::NoDockWidgetArea;
            if (parentWin) {
                area = parentWin->dockWidgetArea(dock);
            }
            QRect geom = dock->geometry();
            spdlog::info("[togglePanelMaximize] Dock '%s' area=%d geom=[%d,%d %dx%d] visible=%s parentWin=%s", 
                dock->objectName().toStdString().c_str(),
                static_cast<int>(area),
                geom.x(), geom.y(), geom.width(), geom.height(),
                dock->isVisible() ? "true" : "false",
                parentWin ? parentWin->objectName().toStdString().c_str() : "null");
        }
    }

    // --- Log edge QMainWindow geometry and visibility ---
    if (m_edgeSplitter) {
        for (int i = 0; i < m_edgeSplitter->count(); ++i) {
            QMainWindow* edgeWin = qobject_cast<QMainWindow*>(m_edgeSplitter->widget(i));
            if (edgeWin && edgeWin != m_innerMainWindow) {
                QRect geom = edgeWin->geometry();
                spdlog::info("[togglePanelMaximize] Edge QMainWindow '%s' geom=[%d,%d %dx%d] visible=%s", 
                    edgeWin->objectName().toStdString().c_str(),
                    geom.x(), geom.y(), geom.width(), geom.height(),
                    edgeWin->isVisible() ? "true" : "false");
            }
        }
    }

    if (!m_panelsBuilt || !m_innerMainWindow)
        return;

    static QByteArray s_savedDockState;
    static QList<int> s_savedSplitterSizes;
    static QWidget* s_maximizedPanel = nullptr;
    static QWidget* s_originalParent = nullptr;
    static QLayout* s_originalLayout = nullptr;
    static int s_originalIndex = -1;
    static QMainWindow* s_originalMainWindow = nullptr;
    static int s_originalSplitterIndex = -1;
    static Qt::DockWidgetArea s_originalDockArea = Qt::NoDockWidgetArea;

    if (m_panelMaximized) {
        // --- RESTORE ---
        if (s_maximizedPanel && s_originalParent) {
            if (this->layout())
                this->layout()->removeWidget(s_maximizedPanel);
            QDockWidget* dock = qobject_cast<QDockWidget*>(s_maximizedPanel);
            if (dock && s_originalMainWindow) {
                // Restore to correct QMainWindow (edge or central)
                Qt::DockWidgetArea restoreArea = s_originalDockArea;
                if (restoreArea == Qt::NoDockWidgetArea)
                    restoreArea = Qt::LeftDockWidgetArea;
                s_originalMainWindow->addDockWidget(restoreArea, dock);
                // If edge splitter, reinsert QMainWindow at correct index
                if (m_edgeSplitter && s_originalMainWindow != m_innerMainWindow && s_originalSplitterIndex >= 0) {
                    int idx = m_edgeSplitter->indexOf(s_originalMainWindow);
                    if (idx != -1 && idx != s_originalSplitterIndex) {
                        m_edgeSplitter->widget(idx)->setParent(nullptr);
                        m_edgeSplitter->insertWidget(s_originalSplitterIndex, s_originalMainWindow);
                    } else if (idx == -1) {
                        m_edgeSplitter->insertWidget(s_originalSplitterIndex, s_originalMainWindow);
                    }
                    s_originalMainWindow->show();
                }
                // --- Ensure all edge columns are present and visible ---
                if (m_edgeSplitter) {
                    for (int i = 0; i < m_edgeSplitter->count(); ++i) {
                        QMainWindow* edgeWin = qobject_cast<QMainWindow*>(m_edgeSplitter->widget(i));
                        if (edgeWin && edgeWin != m_innerMainWindow) {
                            edgeWin->show();
                        }
                    }
                }
            } else if (!dock && s_originalLayout && s_originalIndex >= 0) {
                QBoxLayout* box = qobject_cast<QBoxLayout*>(s_originalLayout);
                if (box) {
                    box->insertWidget(s_originalIndex, s_maximizedPanel);
                } else {
                    s_originalLayout->addWidget(s_maximizedPanel);
                }
                s_maximizedPanel->setParent(s_originalParent);
            } else {
                s_maximizedPanel->setParent(s_originalParent);
            }
            s_maximizedPanel->show();
            // --- Ensure all edge columns are present and visible (for non-dock panels too) ---
            if (m_edgeSplitter) {
                for (int i = 0; i < m_edgeSplitter->count(); ++i) {
                    QMainWindow* edgeWin = qobject_cast<QMainWindow*>(m_edgeSplitter->widget(i));
                    if (edgeWin && edgeWin != m_innerMainWindow) {
                        edgeWin->show();
                    }
                }
            }
        }
        // Restore dock state AFTER reparenting
        if (!s_savedDockState.isEmpty())
            m_innerMainWindow->restoreState(s_savedDockState, 4);

        // --- Ensure all splitter widgets are visible BEFORE restoring sizes ---
        if (m_edgeSplitter) {
            for (int i = 0; i < m_edgeSplitter->count(); ++i) {
                QWidget* w = m_edgeSplitter->widget(i);
                if (w) w->show();
            }
        }

        // Debug: log splitter sizes before restore
        if (m_edgeSplitter) {
            QList<int> beforeSizes = m_edgeSplitter->sizes();
            QStringList beforeStrs;
            for (int sz : beforeSizes) beforeStrs << QString::number(sz);
            spdlog::info("[togglePanelMaximize] Splitter sizes before restore: [{}]", beforeStrs.join(", ").toStdString());
        }

        // Restore splitter sizes AFTER all edge QMainWindows are present and visible
        if (m_edgeSplitter && !s_savedSplitterSizes.isEmpty()) {
            m_edgeSplitter->setSizes(s_savedSplitterSizes);
            m_edgeSplitter->update();
            QApplication::processEvents();
        }

        // Debug: log splitter sizes after restore
        if (m_edgeSplitter) {
            QList<int> afterSizes = m_edgeSplitter->sizes();
            QStringList afterStrs;
            for (int sz : afterSizes) afterStrs << QString::number(sz);
            spdlog::info("[togglePanelMaximize] Splitter sizes after restore: [{}]", afterStrs.join(", ").toStdString());
        }

        // --- Full edge column rebuild: remove and re-insert all edge QMainWindow widgets at their original indices ---
        if (m_edgeSplitter) {
            struct EdgeInfo { QMainWindow* win; int idx; };
            QVector<EdgeInfo> edgeWins;
            for (int i = 0; i < m_edgeSplitter->count(); ++i) {
                QMainWindow* edgeWin = qobject_cast<QMainWindow*>(m_edgeSplitter->widget(i));
                if (edgeWin && edgeWin != m_innerMainWindow) {
                    edgeWins.append({edgeWin, i});
                }
            }
            for (const EdgeInfo& info : edgeWins) {
                info.win->setParent(nullptr);
            }
            for (const EdgeInfo& info : edgeWins) {
                m_edgeSplitter->insertWidget(info.idx, info.win);
                info.win->show();
                info.win->raise();
                for (QDockWidget* dock : info.win->findChildren<QDockWidget*>()) {
                    dock->show();
                    dock->raise();
                }
                spdlog::info("[togglePanelMaximize][rebuild] Re-inserted and showed Edge QMainWindow '%s' at index %d",
                             info.win->objectName().toStdString().c_str(), info.idx);
            }
            if (!s_savedSplitterSizes.isEmpty()) {
                m_edgeSplitter->setSizes(s_savedSplitterSizes);
                m_edgeSplitter->update();
                QApplication::processEvents();
            }
        }

        // Show all dock widgets after dock state restore
        for (auto it = m_dockWidgets.begin(); it != m_dockWidgets.end(); ++it) {
            QDockWidget* dock = it.value();
            if (dock) dock->setVisible(true);
        }
        for (QObject* child : this->children()) {
            QWidget* w = qobject_cast<QWidget*>(child);
            if (w) w->setVisible(true);
        }
        m_panelMaximized = false;
        m_maximizedDock = nullptr;
        s_maximizedPanel = nullptr;
        s_originalParent = nullptr;
        s_originalLayout = nullptr;
        s_originalIndex = -1;
        s_originalMainWindow = nullptr;
        s_originalSplitterIndex = -1;
        return;
    } else {
        // --- MAXIMIZE ---
        if (m_panelMaximized) {
            return;
        }
        QWidget* targetWidget = nullptr;
        QPoint globalMouse = QCursor::pos();
        // 1. Try QDockWidget under mouse
        for (auto it = m_dockWidgets.begin(); it != m_dockWidgets.end(); ++it) {
            QDockWidget* dock = it.value();
            if (dock && dock->isVisible() && dock->rect().contains(dock->mapFromGlobal(globalMouse))) {
                targetWidget = dock;
                break;
            }
        }
        // 2. Try TimelinePanel under mouse
        if (!targetWidget && m_timelinePanel && m_timelinePanel->isVisible() && m_timelinePanel->rect().contains(m_timelinePanel->mapFromGlobal(globalMouse))) {
            targetWidget = m_timelinePanel;
        }
        // 3. Try focused dock
        if (!targetWidget) {
            QWidget* fw = QApplication::focusWidget();
            for (auto it = m_dockWidgets.begin(); it != m_dockWidgets.end(); ++it) {
                QDockWidget* dock = it.value();
                if (dock && dock->isAncestorOf(fw)) {
                    targetWidget = dock;
                    break;
                }
            }
        }
        // 4. Try focused TimelinePanel
        if (!targetWidget && m_timelinePanel && m_timelinePanel->isAncestorOf(QApplication::focusWidget())) {
            targetWidget = m_timelinePanel;
        }
        if (!targetWidget)
            return;

        s_savedDockState = m_innerMainWindow->saveState(4);
        m_dockStateBeforeMaximize = s_savedDockState;
        if (m_edgeSplitter)
            s_savedSplitterSizes = m_edgeSplitter->sizes();

        s_maximizedPanel = targetWidget;
        s_originalParent = targetWidget->parentWidget();
        s_originalLayout = s_originalParent ? s_originalParent->layout() : nullptr;
        s_originalIndex = -1;
        s_originalMainWindow = nullptr;
        s_originalSplitterIndex = -1;
        QDockWidget* dock = qobject_cast<QDockWidget*>(targetWidget);
        if (dock) {
            QWidget* parent = dock->parentWidget();
            QMainWindow* mainWin = nullptr;
            while (parent) {
                mainWin = qobject_cast<QMainWindow*>(parent);
                if (mainWin) break;
                parent = parent->parentWidget();
            }
            if (mainWin) {
                s_originalMainWindow = mainWin;
                if (mainWin == m_innerMainWindow) {
                    s_originalDockArea = m_innerMainWindow->dockWidgetArea(dock);
                } else if (m_edgeSplitter) {
                    for (int i = 0; i < m_edgeSplitter->count(); ++i) {
                        if (m_edgeSplitter->widget(i) == mainWin) {
                            s_originalSplitterIndex = i;
                            break;
                        }
                    }
                    s_originalDockArea = mainWin->dockWidgetArea(dock);
                }
            }
        }
        if (s_originalLayout) {
            for (int i = 0; i < s_originalLayout->count(); ++i) {
                if (s_originalLayout->itemAt(i) && s_originalLayout->itemAt(i)->widget() == targetWidget) {
                    s_originalIndex = i;
                    break;
                }
            }
            s_originalLayout->removeWidget(targetWidget);
        }
        targetWidget->setParent(this);
        for (QObject* child : this->children()) {
            QWidget* w = qobject_cast<QWidget*>(child);
            if (w && w != targetWidget)
                w->setVisible(false);
        }
        QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(this->layout());
        if (!layout) {
            layout = new QVBoxLayout(this);
            layout->setContentsMargins(0, 0, 0, 0);
            setLayout(layout);
        }
        layout->addWidget(targetWidget);
        targetWidget->show();

        m_maximizedDock = qobject_cast<QDockWidget*>(targetWidget);
        m_panelMaximized = true;
    }
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
