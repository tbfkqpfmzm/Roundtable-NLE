/*
 * MainWindow.cpp - Tabbed workspace (DaVinci Resolve-style page tabs).
 * Step 26 (modularized)
 */

#include "MainWindow.h"
#include "ShortcutManager.h"
#include "Theme.h"
#include "widgets/DockTitleBar.h"
#include "dialogs/ProjectSettingsDialog.h"
#include "dialogs/KeyboardShortcutsDialog.h"
#include "dialogs/AppPreferencesDialog.h"

// Pages / panels
#include "panels/audio/AudioSync.h"
#include "panels/characters/CharacterBrowser.h"
#include "panels/characters/CharacterShotPanel.h"
#include "panels/export/ExportPanel.h"
#include "panels/project/ProjectPanel.h"
#include "panels/characters/ShotComposer.h"
#include "panels/timeline/TimelineWorkspace.h"

// Delegated panel headers (for accessor forwarding)
#include "panels/audio/AudioMixer.h"
#include "panels/effects/EffectsPanel.h"
#include "panels/project/HistoryPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "viewport/Viewport.h"
#include "panels/project/ProjectBin.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

// Core
#include "command/CommandStack.h"
#include "media/AudioEngine.h"
#include "media/FrameCache.h"
#include "media/PlaybackController.h"
#include "spine/ModelManager.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"

#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "spine/ShotPreset.h"
#include "SrtIO.h"

#include "Settings.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QKeyEvent>
#include <QMenuBar>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <set>


namespace rt {


void MainWindow::onUndo()
{
    // If on the Characters page in COMPOSE mode, delegate to ShotComposer's own undo stack
    if (currentPage() == Page::Characters && m_characterShotPanel
        && m_characterShotPanel->currentMode() == CharacterShotPanel::Compose
        && m_characterShotPanel->shotComposer()) {
        m_characterShotPanel->shotComposer()->undo();
        statusBar()->showMessage("Undo (shot)", 2000);
        return;
    }
    if (m_commandStack && m_commandStack->canUndo()) {
        // Save the currently raised dock in the right-side panel group so
        // rebuildTracks() doesn't cause an unwanted dock tab switch.
        QDockWidget* raisedDock = nullptr;
        if (m_timelineWorkspace) {
            for (auto* dock : m_timelineWorkspace->dockWidgets().values()) {
                if (dock->isVisible())
                    raisedDock = dock;
            }
        }

        auto desc = m_commandStack->undoDescription();
        m_commandStack->undo();
        // Refresh timeline UI to reflect the undone change
        if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
            m_timelineWorkspace->timelinePanel()->rebuildTracks();
        // Flush composite cache and refresh monitor + overlay
        if (m_timelineWorkspace)
            m_timelineWorkspace->refreshAfterUndoRedo();
        // Reload audio sources so audio reflects the undone state
        if (m_timelineWorkspace)
            m_timelineWorkspace->invalidateAudioSources();

        // Restore the previously raised dock so undo doesn't switch tabs.
        if (raisedDock)
            raisedDock->raise();

        statusBar()->showMessage(
            QString("Undo: %1").arg(QString::fromStdString(desc)), 2000);
    }
}

void MainWindow::onRedo()
{
    // If on the Characters page in COMPOSE mode, delegate to ShotComposer's own redo stack
    if (currentPage() == Page::Characters && m_characterShotPanel
        && m_characterShotPanel->currentMode() == CharacterShotPanel::Compose
        && m_characterShotPanel->shotComposer()) {
        m_characterShotPanel->shotComposer()->redo();
        statusBar()->showMessage("Redo (shot)", 2000);
        return;
    }
    if (m_commandStack && m_commandStack->canRedo()) {
        // Save the currently raised dock so redo doesn't switch panel tabs.
        QDockWidget* raisedDock = nullptr;
        if (m_timelineWorkspace) {
            for (auto* dock : m_timelineWorkspace->dockWidgets().values()) {
                if (dock->isVisible())
                    raisedDock = dock;
            }
        }

        auto desc = m_commandStack->redoDescription();
        m_commandStack->redo();
        // Refresh timeline UI to reflect the redone change
        if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
            m_timelineWorkspace->timelinePanel()->rebuildTracks();
        // Flush composite cache and refresh monitor + overlay
        if (m_timelineWorkspace)
            m_timelineWorkspace->refreshAfterUndoRedo();
        // Reload audio sources so audio reflects the redone state
        if (m_timelineWorkspace)
            m_timelineWorkspace->invalidateAudioSources();

        // Restore the previously raised dock so redo doesn't switch tabs.
        if (raisedDock)
            raisedDock->raise();

        statusBar()->showMessage(
            QString("Redo: %1").arg(QString::fromStdString(desc)), 2000);
    }
}

void MainWindow::onToggleFullScreen()
{
    m_fullScreenPreview = !m_fullScreenPreview;

    if (m_fullScreenPreview) {
        showFullScreen();
    } else {
        showNormal();
    }

    emit fullScreenPreviewChanged(m_fullScreenPreview);
}

void MainWindow::onAbout()
{
    QString ver = QApplication::applicationVersion();
    QMessageBox::about(this, "About ROUNDTABLE NLE",
        "<h2>ROUNDTABLE NLE v" + ver + "</h2>"
        "<p>GPU-accelerated Non-Linear Video Editor for Spine Animation</p>"
        "<p>Created by <a href='http://youtube.com/@ExportErrorMusic/'>Export/Error Music</a></p>"
        "<p>GitHub: <a href='https://github.com/exporterrormusic/Roundtable-NLE'>https://github.com/exporterrormusic/Roundtable-NLE</a></p>"
        "<p>Support with donations via <a href='https://streamelements.com/exporterrormusic/tip'>StreamElements</a></p>"
        "<p style='margin-top:12px;font-size:smaller;color:gray'>"
        "Licensed under AGPL-3.0. Bundles FFmpeg (LGPL), Qt 6 (LGPL), "
        "spine-cpp (Spine Runtimes License), and other components. "
        "See <i>Help &rarr; Third-Party Licenses</i> for full attribution.</p>");
}

// ═════════════════════════════════════════════════════════════════════════════
// Events
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Guard active export — the render queue is cancelled cleanly in
    // ExportPanel::~ExportPanel, but the user gets no warning that their
    // partially-written output is about to be abandoned.
    if (m_exportPanel && m_exportPanel->isExporting()) {
        auto reply = QMessageBox::question(
            this, QStringLiteral("Export in Progress"),
            QStringLiteral("An export is still running.\n\n"
                           "If you close ROUNDTABLE now, the export will be "
                           "cancelled and the partial output file will be "
                           "incomplete.\n\n"
                           "Cancel the export and close anyway?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }

    // Guard unsaved changes
    if (!checkUnsavedChanges()) {
        event->ignore();
        return;
    }

    // Capture project thumbnail before closing (screenshot of current playhead)
    captureProjectThumbnail();

    // Save audio sync state and active page for current project
    if (m_currentProject && m_audioSync) {
        m_audioSync->saveProjectState(
            QString::fromStdString(m_currentProject->name()));

        auto settings = rt::appSettings();
        settings.setValue("Project/" + QString::fromStdString(m_currentProject->name()) + "/activePage",
                          static_cast<int>(currentPage()));
    }

    // F12: Remember last project for auto-open on next launch
    {
        auto settings = rt::appSettings();
        if (m_currentProject && !m_currentProject->filePath().empty())
            settings.setValue("LastProjectPath",
                              QString::fromStdString(m_currentProject->filePath().string()));
        else
            settings.remove("LastProjectPath");
    }

    saveWorkspace("last_session");

    // Also save to the bundled layout file so the installer always captures
    // the latest panel arrangement.  When running from the build tree this
    // writes to assets/default_layout.bin; when installed it's silently
    // ignored (the file path won't be writable).
    {
        QString layoutFile = QCoreApplication::applicationDirPath()
            + QStringLiteral("/assets/default_layout.bin");
        // Only save if the file already exists OR we're in the dev tree
        // (to avoid spewing layout files in Program Files on first close).
        if (QFileInfo::exists(layoutFile) ||
            QDir(QCoreApplication::applicationDirPath()).exists("assets")) {
            saveWorkspaceToFile(layoutFile);
        }
    }

    spdlog::info("MainWindow closing — workspace saved");
    event->accept();

    // Signal the app to quit.  We use a queued invocation so the close
    // event finishes processing cleanly before the event loop exits.
    QMetaObject::invokeMethod(QApplication::instance(), "quit",
                              Qt::QueuedConnection);
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    // Try shortcut manager first
    if (m_shortcutManager &&
        m_shortcutManager->handleKeyPress(event->key(), event->modifiers()))
    {
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // Global JKL transport routing — works regardless of which panel has focus,
    // but skip when a text input widget (QLineEdit, QTextEdit, QSpinBox) has focus,
    // and skip when the AudioSync panel is active (it has its own transport keys).
    if (event->type() == QEvent::KeyPress && m_playbackController) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->modifiers() == Qt::NoModifier) {
            // Don't intercept if focus is on a text input widget
            QWidget* focused = qApp->focusWidget();
            if (focused && (focused->inherits("QLineEdit") ||
                            focused->inherits("QTextEdit") ||
                            focused->inherits("QPlainTextEdit") ||
                            focused->inherits("QSpinBox") ||
                            focused->inherits("QDoubleSpinBox"))) {
                return QMainWindow::eventFilter(watched, event);
            }

            // Don't intercept transport keys when the AudioSync panel is
            // the active page — AudioSync has its own event filter that
            // routes Space/J/K/L to the selected audio clip.
            if (m_audioSync && focused) {
                if (m_audioSync->isAncestorOf(focused) || focused == m_audioSync) {
                    return QMainWindow::eventFilter(watched, event);
                }
            }

            // Don't intercept transport keys when a modal dialog (e.g.
            // ManualMatchDialog) is active — it handles its own keys.
            if (QWidget* modal = QApplication::activeModalWidget()) {
                if (modal != this) {
                    return QMainWindow::eventFilter(watched, event);
                }
            }

            // Route to Source Monitor's controller when it is focused
            SourceMonitor* sm = sourceMonitor();
            if (sm && focused && (sm->isAncestorOf(focused) || focused == sm)) {
                PlaybackController* ctrl = sm->controller();
                if (ctrl) {
                    switch (keyEvent->key()) {
                    case Qt::Key_J:
                        ctrl->shuttleReverse();
                        return true;
                    case Qt::Key_K:
                        ctrl->shuttlePause();
                        return true;
                    case Qt::Key_L:
                        ctrl->shuttleForward();
                        return true;
                    case Qt::Key_Space:
                        ctrl->togglePlayPause();
                        return true;
                    default:
                        break;
                    }
                }
            }

            switch (keyEvent->key()) {
            case Qt::Key_J:
                m_playbackController->shuttleReverse();
                return true;
            case Qt::Key_K:
                m_playbackController->shuttlePause();
                return true;
            case Qt::Key_L:
                m_playbackController->shuttleForward();
                return true;
            case Qt::Key_Space:
                m_playbackController->togglePlayPause();
                return true;
            case Qt::Key_QuoteLeft:
            case Qt::Key_AsciiTilde:
                if (m_timelineWorkspace)
                    m_timelineWorkspace->togglePanelMaximize();
                return true;
            default:
                break;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// ═════════════════════════════════════════════════════════════════════════════
// Sequence switching
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::switchSequence(size_t index)
{
    if (!m_currentProject) return;
    if (index >= m_currentProject->sequenceCount()) return;
    if (index == m_currentProject->activeSequenceIndex()) return;

    // Stop playback before switching
    if (m_playbackController)
        m_playbackController->stop();

    Timeline* newTimeline = m_currentProject->setActiveSequence(index);
    if (!newTimeline) return;

    // Update MainWindow's own pointer
    m_timeline = newTimeline;

    // Update workspace (TimelinePanel + compositeFrame + loadAudioSources)
    if (m_timelineWorkspace)
        m_timelineWorkspace->setTimeline(newTimeline);

    // Update PlaybackController
    if (m_playbackController)
        m_playbackController->setTimeline(newTimeline);

    // Update ExportPanel
    if (m_exportPanel) {
        m_exportPanel->setTimeline(newTimeline);
        if (m_currentProject) m_exportPanel->setProject(m_currentProject.get());
    }

    // Refresh the bin to update the bold/active indicator
    if (auto* bin = projectBin())
        bin->refreshSequences();

    // Refresh sequence tab bar to highlight the active tab
    if (m_timelineWorkspace)
        m_timelineWorkspace->refreshSequenceTabs();

    // Rebuild timeline UI
    if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
        m_timelineWorkspace->timelinePanel()->rebuildTracks();

    // Refresh ProgramMonitor so it shows the new sequence content
    if (auto* pm = programMonitor())
        pm->refresh();

    // Switch to Timeline page
    setCurrentPage(Page::Timeline);

    spdlog::info("MainWindow: switched to sequence {} '{}'",
                 index, newTimeline->name());
}

} // namespace rt
