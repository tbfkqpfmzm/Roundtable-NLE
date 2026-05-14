/*
 * DockLayoutManager.cpp — Dock layout save / restore / deferred-apply.
 *
 * Extracted from TimelineWorkspace.cpp.
 */

#include "panels/timeline/DockLayoutManager.h"

#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>

#include <spdlog/spdlog.h>

namespace rt {

static constexpr int kDockStateVersion = 4;

DockLayoutManager::DockLayoutManager(Config cfg)
    : m_cfg(std::move(cfg))
{}

// ─────────────────────────────────────────────────────────────────────
// save
// ─────────────────────────────────────────────────────────────────────
void DockLayoutManager::save(QSettings& settings,
                              const QByteArray& dockStateOverride)
{
    if (!m_cfg.innerMainWindow) return;

    // ── Collect edge-column metadata ─────────────────────────────────
    QStringList edgeMeta;
    QSet<QString> edgeColumnDockNames;

    if (m_cfg.edgeSplitter) {
        int hostIndex = m_cfg.edgeSplitter->indexOf(m_cfg.innerMainWindow);

        settings.beginGroup("edgeColumnStates");
        settings.remove("");  // clear stale entries
        int edgeIdx = 0;

        for (int i = 0; i < m_cfg.edgeSplitter->count(); ++i) {
            auto* mw = qobject_cast<QMainWindow*>(m_cfg.edgeSplitter->widget(i));
            if (!mw || mw == m_cfg.innerMainWindow) continue;

            QString side = (i < hostIndex) ? QStringLiteral("left")
                                           : QStringLiteral("right");
            QStringList names;
            for (auto* dock : mw->findChildren<QDockWidget*>()) {
                names << dock->objectName();
                edgeColumnDockNames.insert(dock->objectName());
            }
            if (!names.isEmpty()) {
                edgeMeta << (side + QLatin1Char(':') + names.join(QLatin1Char('|')));
                settings.setValue(QString::number(edgeIdx),
                                  mw->saveState(kDockStateVersion));
                ++edgeIdx;
            }
        }
        settings.endGroup();

        QList<int> splitterSizes = m_cfg.edgeSplitter->sizes();
        QVariantList sizeList;
        for (int s : splitterSizes) sizeList.append(s);
        settings.setValue("edgeSplitterSizes", sizeList);
    }

    // Use the override if provided (e.g. pre-maximize dock state),
    // otherwise capture the current state from the inner QMainWindow.
    QByteArray state = dockStateOverride.isEmpty()
        ? m_cfg.innerMainWindow->saveState(kDockStateVersion)
        : dockStateOverride;
    settings.setValue("dockState", state);
    settings.setValue("edgeColumns", edgeMeta);

    // ── Save per-dock floating geometry ──────────────────────────────
    settings.beginGroup("floatingDocks");
    settings.remove("");
    for (auto it = m_cfg.dockWidgets->cbegin(); it != m_cfg.dockWidgets->cend(); ++it) {
        QDockWidget* dock = it.value();
        if (dock && dock->isFloating() && dock->isVisible()
            && !edgeColumnDockNames.contains(dock->objectName())) {
            settings.setValue(dock->objectName() + "/geometry",
                              dock->saveGeometry());
        }
    }
    settings.endGroup();

    // ── Save list of intentionally-closed docks ──────────────────────
    // When saving with a dockStateOverride (e.g. panel was maximized),
    // all docks appear hidden but most were open before maximize.
    // Skip closedDocks in that case so restoreState's own visibility
    // settings from the override state are honoured.
    //
    // CRITICAL: Only collect closed docks when the host widget is
    // actually visible on screen.  QWidget::isVisible() traverses the
    // entire parent chain — it returns false when ANY ancestor is
    // hidden.  When the app closes while on a different page (e.g.
    // Projects), the TimelineWorkspace (host) is not the current page
    // in the QStackedWidget, so every dock reports isVisible()==false.
    // Adding ALL docks to closedDocks causes the restored layout to
    // have every dock hidden — the central widget (timeline panel)
    // fills the entire workspace, appearing maximized.
    QStringList closedDocks;
    if (dockStateOverride.isEmpty()) {
        bool hostVisible = m_cfg.hostWidget && m_cfg.hostWidget->isVisible();
        if (hostVisible) {
            for (auto it = m_cfg.dockWidgets->cbegin(); it != m_cfg.dockWidgets->cend(); ++it) {
                if (it.value() && !it.value()->isVisible())
                    closedDocks << it.key();
            }
        } else {
            spdlog::info("saveDockLayout: host not visible, "
                         "skipping closedDocks to avoid hiding all docks");
        }
    }
    settings.setValue("closedDocks", closedDocks);

    spdlog::info("TimelineWorkspace dock layout saved (v{}, {} bytes, {} edge cols, {} closed, override={})",
                 kDockStateVersion, state.size(), edgeMeta.size(), closedDocks.size(),
                 dockStateOverride.isEmpty() ? "no" : "yes");
}

// ─────────────────────────────────────────────────────────────────────
// restore
// ─────────────────────────────────────────────────────────────────────
bool DockLayoutManager::restore(QSettings& settings)
{
    if (!m_cfg.innerMainWindow) return false;

    QByteArray state = settings.value("dockState").toByteArray();
    if (state.isEmpty()) {
        spdlog::warn("No saved dock layout found");
        return false;
    }

    QStringList edgeMeta = settings.value("edgeColumns").toStringList();

    QList<QByteArray> edgeColStates;
    settings.beginGroup("edgeColumnStates");
    for (int i = 0; ; ++i) {
        QByteArray blob = settings.value(QString::number(i)).toByteArray();
        if (blob.isEmpty()) break;
        edgeColStates.append(blob);
    }
    settings.endGroup();

    QList<int> savedSplitterSizes;
    QVariantList sizeList = settings.value("edgeSplitterSizes").toList();
    for (const QVariant& v : sizeList)
        savedSplitterSizes.append(v.toInt());

    QMap<QString, QByteArray> floatingGeo;
    settings.beginGroup("floatingDocks");
    for (const QString& key : settings.childGroups()) {
        QByteArray geo = settings.value(key + "/geometry").toByteArray();
        if (!geo.isEmpty())
            floatingGeo.insert(key, geo);
    }
    settings.endGroup();

    QStringList closedDocks = settings.value("closedDocks").toStringList();

    auto* host = m_cfg.hostWidget;
    spdlog::info("restoreDockLayout: {} bytes, {} edge cols, widget visible={}, size={}x{}",
                 state.size(), edgeMeta.size(),
                 host ? host->isVisible() : false,
                 host ? host->width() : 0,
                 host ? host->height() : 0);

    if (!host || !host->isVisible() || host->width() < 100 || host->height() < 100) {
        spdlog::info("Deferring dock layout restore until widget is visible");
        m_pendingDockState      = state;
        m_pendingEdgeColumns    = edgeMeta;
        m_pendingEdgeColStates  = edgeColStates;
        m_pendingSplitterSizes  = savedSplitterSizes;
        m_pendingFloatingGeo    = floatingGeo;
        m_pendingClosedDocks    = closedDocks;
        return true;
    }

    return applyState(state, edgeMeta, edgeColStates, savedSplitterSizes, floatingGeo, closedDocks);
}

// ─────────────────────────────────────────────────────────────────────
// clearPendingState
// ─────────────────────────────────────────────────────────────────────
void DockLayoutManager::clearPendingState()
{
    m_pendingDockState.clear();
    m_pendingEdgeColumns.clear();
    m_pendingEdgeColStates.clear();
    m_pendingSplitterSizes.clear();
    m_pendingFloatingGeo.clear();
    m_pendingClosedDocks.clear();
}

// ─────────────────────────────────────────────────────────────────────
// applyPendingState  (called from host showEvent)
// ─────────────────────────────────────────────────────────────────────
bool DockLayoutManager::applyPendingState()
{
    if (m_pendingDockState.isEmpty()) return false;

    QByteArray state               = m_pendingDockState;
    QStringList edgeMeta           = m_pendingEdgeColumns;
    QList<QByteArray> edgeStates   = m_pendingEdgeColStates;
    QList<int> splitterSizes       = m_pendingSplitterSizes;
    QMap<QString, QByteArray> fGeo = m_pendingFloatingGeo;
    QStringList closedDocks        = m_pendingClosedDocks;

    m_pendingDockState.clear();
    m_pendingEdgeColumns.clear();
    m_pendingEdgeColStates.clear();
    m_pendingSplitterSizes.clear();
    m_pendingFloatingGeo.clear();
    m_pendingClosedDocks.clear();

    auto* host = m_cfg.hostWidget;
    // Defer one more tick so the layout has settled after the show.
    // Suppress repaints during the restore to avoid visible thrashing.
    QTimer::singleShot(0, host, [this, state, edgeMeta, edgeStates,
                                  splitterSizes, fGeo, closedDocks]() {
        spdlog::info("Applying deferred dock state ({} bytes, size={}x{})",
                     state.size(),
                     m_cfg.hostWidget ? m_cfg.hostWidget->width() : 0,
                     m_cfg.hostWidget ? m_cfg.hostWidget->height() : 0);
        if (m_cfg.hostWidget)
            m_cfg.hostWidget->setUpdatesEnabled(false);
        applyState(state, edgeMeta, edgeStates, splitterSizes, fGeo, closedDocks);
        if (m_cfg.hostWidget) {
            m_cfg.hostWidget->setUpdatesEnabled(true);
            m_cfg.hostWidget->update();
        }
        // applyState reparents dock widgets between QMainWindows. Qt's
        // setUpdatesEnabled(false) on the host propagates WA_UpdatesDisabled
        // to all current children, but the matching setUpdatesEnabled(true)
        // only re-enables widgets that are still children at that moment.
        // Any dock that was reparented to an edge column during applyState
        // (and its descendants) stays stuck with updates disabled — which
        // manifests as panels rendering as blank/white surfaces (notably the
        // Program Monitor viewport). Force-enable every known dock and
        // recursively re-enable updates on its descendants.
        if (m_cfg.dockWidgets) {
            for (auto it = m_cfg.dockWidgets->begin();
                 it != m_cfg.dockWidgets->end(); ++it) {
                QDockWidget* dock = it.value();
                if (!dock) continue;
                dock->setUpdatesEnabled(true);
                const auto descendants = dock->findChildren<QWidget*>();
                for (QWidget* w : descendants) {
                    if (!w->updatesEnabled())
                        w->setUpdatesEnabled(true);
                }
                dock->update();
            }
        }
    });
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// applyState  (private — the main restore logic)
// ─────────────────────────────────────────────────────────────────────
bool DockLayoutManager::applyState(const QByteArray& state,
                                    const QStringList& edgeColumnMeta,
                                    const QList<QByteArray>& edgeColStates,
                                    const QList<int>& savedSplitterSizes,
                                    const QMap<QString, QByteArray>& floatingGeo,
                                    const QStringList& closedDocks)
{
    if (!m_cfg.innerMainWindow || state.isEmpty()) return false;

    m_closedDockNames = QSet<QString>(closedDocks.begin(), closedDocks.end());

    auto* sb = m_cfg.innerMainWindow->statusBar();
    if (sb) sb->hide();

    // ── Step 1: Destroy old edge columns ─────────────────────────────
    if (m_cfg.edgeSplitter) {
        QList<QMainWindow*> oldEdgeCols;
        for (int i = 0; i < m_cfg.edgeSplitter->count(); ++i) {
            auto* mw = qobject_cast<QMainWindow*>(m_cfg.edgeSplitter->widget(i));
            if (mw && mw != m_cfg.innerMainWindow)
                oldEdgeCols.append(mw);
        }
        for (auto* mw : oldEdgeCols) {
            for (auto* dock : mw->findChildren<QDockWidget*>()) {
                mw->removeDockWidget(dock);
                dock->setParent(m_cfg.hostWidget);
                dock->hide();
            }
            mw->hide();
            mw->setParent(nullptr);
            delete mw;
        }
    }

    // ── Step 2: Parse edge column metadata and create empty shells ───
    struct EdgeColInfo {
        QMainWindow* mw;
        bool isLeft;
        QList<QDockWidget*> docks;
        int stateIdx;
    };
    QList<EdgeColInfo> edgeColInfos;
    QSet<QString> edgeDockNames;

    int edgeIdx = 0;
    for (const QString& entry : edgeColumnMeta) {
        int colonPos = entry.indexOf(QLatin1Char(':'));
        if (colonPos < 0) continue;
        bool isLeft = (entry.left(colonPos) == QLatin1String("left"));
        QStringList dockNames = entry.mid(colonPos + 1)
                                     .split(QLatin1Char('|'), Qt::SkipEmptyParts);
        if (dockNames.isEmpty()) continue;

        QList<QDockWidget*> docks;
        for (const QString& name : dockNames) {
            auto it = m_cfg.dockWidgets->find(name);
            if (it != m_cfg.dockWidgets->end() && it.value()) {
                docks.append(it.value());
                edgeDockNames.insert(name);
            }
        }
        if (docks.isEmpty()) { ++edgeIdx; continue; }

        auto* edgeMW = new QMainWindow(m_cfg.edgeSplitter);
        edgeMW->setObjectName(QStringLiteral("EdgeColumn"));
        edgeMW->setWindowFlags(Qt::Widget);
        edgeMW->setDockNestingEnabled(true);
        edgeMW->setAnimated(false);
        edgeMW->setMinimumWidth(150);
        edgeMW->setTabPosition(Qt::TopDockWidgetArea, QTabWidget::North);
        edgeMW->setTabPosition(Qt::BottomDockWidgetArea, QTabWidget::North);
        edgeMW->setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::North);
        edgeMW->setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);
        edgeMW->setDockOptions(
            QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks |
            QMainWindow::GroupedDragging);

        auto* placeholder = new QWidget(edgeMW);
        placeholder->setMaximumSize(0, 0);
        edgeMW->setCentralWidget(placeholder);

        edgeMW->setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
        edgeMW->setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
        edgeMW->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
        edgeMW->setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);

        auto* edgeSb = edgeMW->statusBar();
        if (edgeSb) edgeSb->hide();

        int hostIndex = m_cfg.edgeSplitter->indexOf(m_cfg.innerMainWindow);
        if (isLeft)
            m_cfg.edgeSplitter->insertWidget(hostIndex, edgeMW);
        else
            m_cfg.edgeSplitter->insertWidget(hostIndex + 1, edgeMW);

        edgeMW->show();

        edgeColInfos.append({edgeMW, isLeft, docks, edgeIdx});
        ++edgeIdx;
    }

    // ── Step 3: Apply splitter sizes ─────────────────────────────────
    if (m_cfg.edgeSplitter && !savedSplitterSizes.isEmpty()
        && savedSplitterSizes.size() == m_cfg.edgeSplitter->count()) {
        m_cfg.edgeSplitter->setSizes(savedSplitterSizes);
    } else if (m_cfg.edgeSplitter && m_cfg.edgeSplitter->count() > 1) {
        int totalW = m_cfg.edgeSplitter->width();
        int colW = std::max(200, totalW / 5);
        QList<int> sizes;
        for (int i = 0; i < m_cfg.edgeSplitter->count(); ++i) {
            if (m_cfg.edgeSplitter->widget(i) == m_cfg.innerMainWindow)
                sizes.append(totalW - colW * (m_cfg.edgeSplitter->count() - 1));
            else
                sizes.append(colW);
        }
        m_cfg.edgeSplitter->setSizes(sizes);
    }

    if (m_cfg.edgeSplitter)
        m_cfg.edgeSplitter->updateGeometry();
    m_cfg.innerMainWindow->updateGeometry();
    // A6: replaced full processEvents() with a targeted flush of only
    // QEvent::LayoutRequest events.  The previous call re-entered the
    // event loop and could process resize / paint / deferred-delete
    // events mid-layout, which contributed to the dock-animation crash
    // class.  This narrower flush still propagates the splitter sizes
    // but does not run user code from arbitrary other event handlers.
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);

    // ── Step 4: Remove edge-bound docks from inner MW before restore ─
    for (const QString& name : edgeDockNames) {
        auto it = m_cfg.dockWidgets->find(name);
        if (it != m_cfg.dockWidgets->end() && it.value()) {
            m_cfg.innerMainWindow->removeDockWidget(it.value());
            it.value()->setParent(m_cfg.hostWidget);
            it.value()->hide();
        }
    }

    // ── Step 5: Restore inner MW dock layout ─────────────────────────
    bool ok = m_cfg.innerMainWindow->restoreState(state, kDockStateVersion);
    if (!ok) ok = m_cfg.innerMainWindow->restoreState(state, 3);
    if (!ok) ok = m_cfg.innerMainWindow->restoreState(state, 2);
    if (!ok) ok = m_cfg.innerMainWindow->restoreState(state, 1);
    if (!ok) ok = m_cfg.innerMainWindow->restoreState(state, 0);

    if (ok)
        spdlog::info("TimelineWorkspace dock layout restored (size={}x{})",
                     m_cfg.innerMainWindow->width(), m_cfg.innerMainWindow->height());
    else
        spdlog::warn("Failed to restore dock layout ({} bytes, all versions tried)",
                     state.size());

    // ── Step 6: Move docks into edge columns and restore their state ─
    for (auto& info : edgeColInfos) {
        for (auto* dock : info.docks) {
            info.mw->addDockWidget(Qt::TopDockWidgetArea, dock);
            dock->setVisible(true);
            dock->show();
        }

        bool edgeRestored = false;
        if (info.stateIdx < edgeColStates.size()
            && !edgeColStates[info.stateIdx].isEmpty()) {
            edgeRestored = info.mw->restoreState(
                edgeColStates[info.stateIdx], kDockStateVersion);
            if (edgeRestored) {
                for (auto* dock : info.docks) {
                    dock->setVisible(true);
                    dock->show();
                }
            }
        }
        if (!edgeRestored && info.docks.size() > 1) {
            for (int d = 1; d < info.docks.size(); ++d)
                info.mw->tabifyDockWidget(info.docks.first(), info.docks[d]);
        }

        if (m_cfg.installEdgeGuard)
            m_cfg.installEdgeGuard(info.mw);
    }

    // ── Step 7: Re-apply splitter sizes ──────────────────────────────
    if (m_cfg.edgeSplitter && !savedSplitterSizes.isEmpty()
        && savedSplitterSizes.size() == m_cfg.edgeSplitter->count()) {
        m_cfg.edgeSplitter->setSizes(savedSplitterSizes);
    }

    // ── Ensure non-closed docks are visible ──────────────────────────
    // Safety: if closedDocks contains ALL known docks, something went
    // wrong during save (e.g. host was hidden so every dock reported
    // isVisible()==false).  Ignore the closed list in that case so the
    // layout doesn't appear maximized with all panels invisible.
    if (!m_closedDockNames.isEmpty() &&
        m_closedDockNames.size() >= m_cfg.dockWidgets->size()) {
        spdlog::warn("restoreDockLayout: ALL {} docks in closedDocks "
                     "— ignoring closedDocks (likely corrupt save)",
                     m_closedDockNames.size());
        m_closedDockNames.clear();
    }
    for (auto it = m_cfg.dockWidgets->cbegin(); it != m_cfg.dockWidgets->cend(); ++it) {
        QDockWidget* dock = it.value();
        if (!dock) continue;
        if (m_closedDockNames.contains(it.key()))
            dock->hide();
        else if (!dock->isVisible())
            dock->show();
    }

    // ── Restore per-dock floating geometry ────────────────────────────
    for (auto it = floatingGeo.cbegin(); it != floatingGeo.cend(); ++it) {
        auto dockIt = m_cfg.dockWidgets->find(it.key());
        if (dockIt != m_cfg.dockWidgets->end() && dockIt.value()
            && dockIt.value()->isFloating()) {
            dockIt.value()->restoreGeometry(it.value());
        }
    }

    return ok;
}

} // namespace rt
