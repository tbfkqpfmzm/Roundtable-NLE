/*
 * DockLayoutManager.h — Dock layout persistence for TimelineWorkspace.
 *
 * Encapsulates saving, restoring, and deferred-applying of the Qt dock
 * widget layout (including edge columns, floating geometry, and closed
 * dock tracking).  Extracted from TimelineWorkspace to keep it focused
 * on coordination.
 */
#pragma once

#include <functional>

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

class QDockWidget;
class QMainWindow;
class QSettings;
class QSplitter;
class QWidget;

namespace rt {

class DockLayoutManager {
public:
    /// Dependencies injected from the owning TimelineWorkspace.
    struct Config {
        QMainWindow* innerMainWindow{nullptr};
        QSplitter*   edgeSplitter{nullptr};
        QMap<QString, QDockWidget*>* dockWidgets{nullptr};
        QWidget*     hostWidget{nullptr};   // parent for temporarily stashed docks
        std::function<void(QMainWindow*)> installEdgeGuard;
    };

    explicit DockLayoutManager(Config cfg);

    /// Save the dock layout into the given QSettings group.
    /// If \a dockStateOverride is non-empty, use it instead of querying
    /// innerMainWindow->saveState() — needed when a panel is maximized and
    /// the real dock state is temporarily stored elsewhere.
    void save(QSettings& settings,
              const QByteArray& dockStateOverride = {});

    /// Restore a previously saved dock layout.  If the host widget is not
    /// yet visible, the state is stored for deferred application.
    /// Returns true on success (or deferred successfully).
    bool restore(QSettings& settings);

    /// Apply any deferred dock state.  Call from the host widget's
    /// showEvent.  Returns true if state was applied.
    bool applyPendingState();

    /// True when there is deferred state waiting for the next showEvent.
    [[nodiscard]] bool hasPendingState() const noexcept { return !m_pendingDockState.isEmpty(); }

    /// Discard any deferred dock state so it is NOT applied on the next
    /// showEvent.  Used when a default layout reset takes priority over
    /// stale saved data.
    void clearPendingState();

    /// Names of docks the user intentionally closed.
    [[nodiscard]] const QSet<QString>& closedDockNames() const noexcept { return m_closedDockNames; }

private:
    bool applyState(const QByteArray& state,
                    const QStringList& edgeColumnMeta = {},
                    const QList<QByteArray>& edgeColStates = {},
                    const QList<int>& savedSplitterSizes = {},
                    const QMap<QString, QByteArray>& floatingGeo = {},
                    const QStringList& closedDocks = {});

    Config m_cfg;

    // Pending state for deferred restore (applied on next showEvent)
    QByteArray                  m_pendingDockState;
    QStringList                 m_pendingEdgeColumns;
    QList<QByteArray>           m_pendingEdgeColStates;
    QList<int>                  m_pendingSplitterSizes;
    QMap<QString, QByteArray>   m_pendingFloatingGeo;
    QStringList                 m_pendingClosedDocks;

    QSet<QString> m_closedDockNames;
};

} // namespace rt
