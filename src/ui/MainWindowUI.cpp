/*
 * MainWindowUI.cpp — Tabbed-page UI coordinator.
 *
 * Thin coordinator after extracting buildPanels → MainWindowUIBuild.cpp,
 * page navigation → MainWindowUINav.cpp, and workspace/accessors/statusbar
 * → MainWindowUIWorkspace.cpp.
 *
 * Contains: constructor, destructor, dependency injection setters.
 *
 * Sub-files (all in src/ui/):
 *   MainWindowUIBuild.cpp      — buildPanels() (all 5 pages + signal wiring)
 *   MainWindowUINav.cpp        — setupPageTabs(), setCurrentPage/currentPage,
 *                                 toggleNavRail(), onPageTabChanged()
 *   MainWindowUIWorkspace.cpp  — panel accessors, applyDefaultLayout(),
 *                                 saveWorkspace/restoreWorkspace, status bar
 *   MainWindowMenus.cpp        — all build*Menu() methods
 *   MainWindowProject.cpp      — project management (open/save/close)
 *   MainWindowRecovery.cpp     — crash recovery dialog
 */

#include "MainWindow.h"

#include "panels/characters/CharacterShotPanel.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "panels/monitors/SourceMonitor.h"
#include "spine/ModelManager.h"
#include "project/Project.h"

#include "UiScale.h"
#include "Settings.h"

#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <algorithm>


namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═════════════════════════════════════════════════════════════════════════════

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setObjectName("MainWindow");
    setWindowTitle(QString("ROUNDTABLE NLE %1").arg(ROUNDTABLE_VERSION));
    setMinimumSize(1280, 720);

    setDocumentMode(true);

    // Install global event filter for JKL transport keys
    if (qApp) qApp->installEventFilter(this);

    // Auto-save with configurable interval from preferences
    m_autoSaveTimer = new QTimer(this);
    {
        auto s = rt::appSettings();
        int minutes = s.value("AutosaveInterval", 5).toInt();
        m_autoSaveTimer->setInterval(minutes * 60 * 1000);
    }
    connect(m_autoSaveTimer, &QTimer::timeout, this, &MainWindow::onAutoSave);
    m_autoSaveTimer->start();

    setupPageTabs();
    setupStatusBar();

    // ── Per-monitor DPI relayout ────────────────────────────────────────
    // When the window is dragged across monitors with different DPI/scale,
    // Qt updates devicePixelRatio but does NOT automatically re-polish
    // stylesheets, re-evaluate font metrics, or re-run layouts that cached
    // pixel sizes from the old screen. Force a full relayout on every
    // screen change so controls stay the correct apparent size.
    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto installScreenWatch = [this](QWindow* win) {
            if (!win) return;
            connect(win, &QWindow::screenChanged, this,
                    [this](QScreen* newScreen) {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        if (!newScreen) return;
                        spdlog::info("MainWindow: moved to screen '{}' ({}x{} dpr={:.2f} dpi={:.0f})",
                                     newScreen->name().toStdString(),
                                     newScreen->size().width(),
                                     newScreen->size().height(),
                                     newScreen->devicePixelRatio(),
                                     newScreen->logicalDotsPerInch());
                        // Update global UI scale factor for the new screen.
                        // This rescales every widget registered via
                        // UiScale::setScaledFixed*() and emits a signal so
                        // panels can rebuild stylesheet-driven sizes.
                        rt::UiScale::updateForScreen(newScreen);
                        // Re-polish the entire widget tree so any QSS that
                        // depends on font metrics or pixel sizes is rebuilt
                        // for the new DPI, then force a full relayout.
                        if (auto* st = style()) {
                            st->unpolish(this);
                            st->polish(this);
                        }
                        const auto kids = findChildren<QWidget*>();
                        for (QWidget* w : kids) {
                            if (auto* st = w->style()) {
                                st->unpolish(w);
                                st->polish(w);
                            }
                            w->updateGeometry();
                        }
                        updateGeometry();
                        adjustSize();
                        update();
                    });
            // Initialise scale for the screen the window is currently on.
            if (auto* s = win->screen()) {
                rt::UiScale::updateForScreen(s);
            }
        };
        installScreenWatch(windowHandle());
        if (!windowHandle()) {
            QTimer::singleShot(100, this, [this, installScreenWatch]() {
                installScreenWatch(windowHandle());
            });
        }
    });

    spdlog::debug("MainWindow constructed (tabbed pages)");
}

MainWindow::~MainWindow()
{
    m_destroying.store(true, std::memory_order_release);

    // Stop timers before destroying children — prevents timer events
    // from firing into partially-destroyed object.
    if (m_autoSaveTimer) {
        m_autoSaveTimer->stop();
    }

    spdlog::debug("MainWindow destroyed");
}

// ═════════════════════════════════════════════════════════════════════════════
// Dependency injection
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::setTimeline(Timeline* timeline) { m_timeline = timeline; }
void MainWindow::setCommandStack(CommandStack* stack) { m_commandStack = stack; }
void MainWindow::setShortcutManager(ShortcutManager* mgr) { m_shortcutManager = mgr; }
void MainWindow::setAudioEngine(AudioEngine* engine) { m_audioEngine = engine; }
void MainWindow::setPlaybackController(PlaybackController* controller) { m_playbackController = controller; }
void MainWindow::setMediaPool(MediaPool* pool) {
    m_mediaPool = pool;
    // Also give the SourceMonitor a reference for drag-drop loads
    if (auto* sm = sourceMonitor())
        sm->setMediaPool(pool);
}
void MainWindow::setMediaSourceService(MediaSourceService* service) {
    m_mediaSourceService = service;
}
void MainWindow::setModelManager(ModelManager* mgr) {
    m_modelManager = mgr;
    spdlog::info("MainWindow::setModelManager — mgr={}, scanned={}, "
                 "csp={}, tw={}",
                 static_cast<const void*>(mgr),
                 mgr ? mgr->isScanned() : false,
                 static_cast<const void*>(m_characterShotPanel),
                 static_cast<const void*>(m_timelineWorkspace));
    // Propagate to CharacterShotPanel (and its children: CharacterBrowser,
    // ConversionPanel, ShotComposer) so they refresh after ModelManager
    // finishes its async scan.
    if (m_characterShotPanel)
        m_characterShotPanel->setModelManager(mgr);
    // Propagate to TimelineWorkspace so CharactersPanel refreshes its tree
    if (m_timelineWorkspace)
        m_timelineWorkspace->setModelManager(mgr);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
