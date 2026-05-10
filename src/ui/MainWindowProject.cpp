/*
 * MainWindowProject.cpp - Project lifecycle & serialization extracted from MainWindow.cpp.
 *
 * Contains: collectTimelineMedia helper, project directory management,
 * project CRUD handlers, save/saveAs, SRT import/export, thumbnail capture,
 * auto-save, crash recovery, and recent files management.
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
#include "QtHelpers.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

// Core
#include "command/CommandStack.h"
#include "media/AudioEngine.h"
#include "media/FrameCache.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include "media/PlaybackScheduler.h"
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
#include <QFile>
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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Project management helpers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

QString MainWindow::projectsDirectory() const
{
    // F14: Check for user-configured projects directory
    auto settings = rt::appSettings();
    QString customDir = settings.value("ProjectsDirectory").toString();
    if (!customDir.isEmpty() && QDir(customDir).exists())
        return customDir;

    // Use a "projects" folder next to the executable
    QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);
    // Walk up from bin/Release to the workspace root
    // (exe is at build/bin/Release/roundtable.exe)
    // Try to find a "projects" folder by walking up
    for (int i = 0; i < 5; ++i) {
        if (dir.exists("projects")) {
            return dir.absoluteFilePath("projects");
        }
        dir.cdUp();
    }
    // Fallback: create next to exe
    QString defaultDir = appDir + "/projects";
    QDir defDir(defaultDir);
    if (!defDir.exists()) {
        defDir.mkpath(".");
    }
    // Verify it's writable — if not, fall back to user data dir
    QFileInfo fi(defaultDir + "/.wtest");
    QFile testFile(defaultDir + "/.wtest");
    if (testFile.open(QIODevice::WriteOnly)) {
        testFile.close();
        testFile.remove();
        return defaultDir;
    }
    spdlog::warn("projectsDirectory: {} is not writable, falling back to userDataDir",
                 defaultDir.toStdString());
    return rt::userDataDir() + "/projects";
}

bool MainWindow::checkUnsavedChanges()
{
    bool audioSyncDirty = false;
    if (m_currentProject && m_audioSync)
        audioSyncDirty = (m_audioSync->serializeToBlob() != m_lastSavedAudioSyncBlob);

    if (!m_currentProject || (!m_currentProject->isModified() && !audioSyncDirty))
        return true; // nothing to save

    QString name = QString::fromStdString(m_currentProject->name());
    auto reply = QMessageBox::question(
        this, "Unsaved Changes",
        QString("Project '%1' has unsaved changes.\n\n"
                "Do you want to save before continuing?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (reply == QMessageBox::Cancel)
        return false; // user cancelled

    if (reply == QMessageBox::Save) {
        onSaveProject();
    }
    // Discard or Save â€” OK to proceed
    return true;
}

void MainWindow::refreshProjectsList()
{
    if (!m_projectPanel) return;

    QString projDir = projectsDirectory();
    QDir dir(projDir);
    if (!dir.exists()) {
        dir.mkpath(".");
        m_projectPanel->setProjects({});
        return;
    }

    // Migrate any flat-layout project files into subfolders
    {
        QStringList flatFilters;
        flatFilters << "*.rtp";
        auto flatFiles = dir.entryInfoList(flatFilters, QDir::Files);
        for (const auto& fi : flatFiles) {
            QString name = fi.baseName();
            QString subFolder = projDir + "/" + name;
            QDir().mkpath(subFolder);
            QFile::rename(fi.absoluteFilePath(), subFolder + "/" + fi.fileName());
            // Move .bak if it exists
            QString bakPath = fi.absoluteFilePath() + ".bak";
            if (QFile::exists(bakPath))
                QFile::rename(bakPath, subFolder + "/" + fi.fileName() + ".bak");
            // Move thumbnail from thumbs/ if it exists
            QString thumbsDir = projDir + "/thumbs";
            for (const auto& ext : {".png", ".jpg"}) {
                QString thumbPath = thumbsDir + "/" + name + ext;
                if (QFile::exists(thumbPath))
                    QFile::rename(thumbPath, subFolder + "/" + name + ext);
            }
            spdlog::info("Migrated project '{}' into subfolder", name.toStdString());
        }
        // Remove empty thumbs/ directory
        QDir thumbsDir(projDir + "/thumbs");
        if (thumbsDir.exists() && thumbsDir.isEmpty())
            thumbsDir.removeRecursively();
    }

    // Scan for .rtp files inside project subfolders
    QStringList filters;
    filters << "*.rtp";

    QFileInfoList entries;
    for (const auto& subDir : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir sub(subDir.absoluteFilePath());
        entries.append(sub.entryInfoList(filters, QDir::Files, QDir::Time));
    }

    // Also include recent projects that live outside the default projects
    // directory (e.g. projects created on external drives).
    auto settings = rt::appSettings();
    QStringList recent = settings.value("RecentFiles").toStringList();
    {
        QSet<QString> seen;
        for (const auto& fi : entries)
            seen.insert(QFileInfo(fi.absoluteFilePath()).absoluteFilePath().toLower());

        const QString projDirPrefix = QDir::toNativeSeparators(projDir).toLower();
        for (const auto& rp : recent) {
            QFileInfo rfi(rp);
            QString normPath = rfi.absoluteFilePath().toLower();
            if (seen.contains(normPath)) continue;
            if (rfi.exists() && rfi.fileName().endsWith(".rtp", Qt::CaseInsensitive)) {
                if (normPath.startsWith(projDirPrefix))
                    continue;
                entries.append(rfi);
                seen.insert(normPath);
            }
        }
    }

    // Sort by modification time (newest first)
    std::sort(entries.begin(), entries.end(),
              [](const QFileInfo& a, const QFileInfo& b) {
                  return a.lastModified() > b.lastModified();
              });

    // Build rich ProjectInfo structs
    QVector<ProjectInfo> projects;
    projects.reserve(entries.size());

    QString currentName;
    if (m_currentProject)
        currentName = QString::fromStdString(m_currentProject->name());

    for (const auto& entry : entries) {
        ProjectInfo info;
        info.name         = entry.baseName();
        info.filePath     = entry.absoluteFilePath();
        info.fileSize     = entry.size();
        info.lastModified = entry.lastModified();
        info.isCurrent    = (!currentName.isEmpty() && info.name == currentName);

        // Read resolution/fps from the project header without
        // fully deserializing (P3: header-only metadata read).
        ProjectSerializer::Metadata meta;
        if (ProjectSerializer::readMetadata(
                entry.absoluteFilePath().toStdString(), meta)) {
            info.resW = meta.resW;
            info.resH = meta.resH;
            info.fps  = meta.fps;
        }

        projects.append(info);
    }

    m_projectPanel->setProjects(projects);

    // Also feed recent projects
    m_projectPanel->setRecentProjects(recent);

    spdlog::info("Refreshed projects list: {} projects found", entries.size());
    statusBar()->showMessage(
        QString("%1 project(s) found").arg(entries.size()), 3000);
}

void MainWindow::setCurrentProject(std::unique_ptr<Project> project)
{
    // ── Pre-move cleanup: stop all background operations BEFORE destroying
    // the old project.  The old project's timeline is about to be destroyed
    // via std::move, but raw pointers (m_timeline, etc.)  and background
    // threads (FrameProducer, audio playback) may still reference it.
    // Without this, a concurrent composite or audio callback reads a
    // dangling timeline pointer → crash during project switching.
    if (m_currentProject) {
        // 1. Stop audio playback / transport
        if (m_playbackController && m_playbackController->isPlaying())
            m_playbackController->stop();

        // 2. Stop the async composite pipeline (FrameProducer thread)
        if (m_timelineWorkspace) {
            if (auto* pm = m_timelineWorkspace->programMonitor()) {
                pm->stopPolling();
                if (auto* pl = pm->pipeline())
                    pl->stop();
            }
        }
        // Also stop source monitor pipeline
        if (m_timelineWorkspace) {
            if (auto* sm = m_timelineWorkspace->sourceMonitor()) {
                if (auto* ctrl = sm->controller()) {
                    if (ctrl->isPlaying()) ctrl->stop();
                }
            }
        }

        // 3. Disconnect old timeline from composite service so the next
        //    poll-timer tick or producer thread doesn't access destroyed data
        if (m_timelineWorkspace) {
            if (auto* pm = m_timelineWorkspace->programMonitor())
                pm->setCompositeCallback(nullptr);
            m_timelineWorkspace->setTimeline(nullptr);
        }
        if (m_playbackController)
            m_playbackController->setTimeline(nullptr);
        // Also disconnect the export panel — otherwise when it is later
        // wired to the new project's timeline, ExportPanel::setTimeline
        // will call removeObserver on the already-destroyed old timeline.
        if (m_exportPanel)
            m_exportPanel->setTimeline(nullptr);
        m_timeline = nullptr;
    }

    // Now safe to destroy the old project and set up the new one
    m_currentProject = std::move(project);
    m_lastSavedAudioSyncBlob = m_currentProject ? m_currentProject->audioSyncBlob()
                                               : std::vector<uint8_t>{};

    if (m_currentProject) {
        // Reset per-project panels so old state doesn't leak into the new project.
        // Scripts, audio, clips, and sessions from the previous project are cleared.
        if (m_audioSync)
            m_audioSync->resetForNewProject();

        // Release all media from the old project and clear the frame cache
        if (m_mediaPool)
            m_mediaPool->closeAll();

        // Clear undo/redo history so old commands don't apply to the new project
        if (m_commandStack)
            m_commandStack->clear();

        QString name = QString::fromStdString(m_currentProject->name());
        if (m_projectPanel)
            m_projectPanel->setCurrentProjectName(name);
        if (auto* bin = projectBin())
            bin->setProjectName(name);
        setWindowTitle(QString("ROUNDTABLE NLE %1 \u2014 %2").arg(ROUNDTABLE_VERSION).arg(name));

        // Wire the project's timeline to ALL consumers so every subsystem
        // reads/writes the same timeline.  Without this, export writes to
        // the App's default timeline while playback reads from the project's
        // timeline, resulting in 0 audio sources loaded.
        if (m_currentProject->timeline()) {
            Timeline* projTimeline = m_currentProject->timeline();

            // Update MainWindow's own pointer (used by export handler)
            m_timeline = projTimeline;

            // Update workspace (TimelinePanel + compositeFrame + loadAudioSources)
            if (m_timelineWorkspace)
                m_timelineWorkspace->setTimeline(projTimeline);

            // Update PlaybackController (transport / duration queries)
            if (m_playbackController)
                m_playbackController->setTimeline(projTimeline);

            // Update ExportPanel
            if (m_exportPanel) {
                m_exportPanel->setTimeline(projTimeline);
                m_exportPanel->setProject(m_currentProject.get());
                if (m_playbackController) m_exportPanel->setPlaybackController(m_playbackController);
                if (m_audioEngine) m_exportPanel->setAudioEngine(m_audioEngine);
                m_exportPanel->setPreviewCallback(
                    [this](int64_t tick, uint32_t w, uint32_t h, bool scrub)
                        -> std::shared_ptr<CachedFrame> {
                        if (m_timelineWorkspace) {
                            m_timelineWorkspace->setForceFullResolution(true);
                            auto result = m_timelineWorkspace->compositeFrame(tick, w, h, scrub);
                            m_timelineWorkspace->setForceFullResolution(false);
                            return result;
                        }
                        return nullptr;
                    });
            }

            spdlog::info("setCurrentProject: all subsystems wired to project timeline (tracks={})",
                         projTimeline->trackCount());

            // Wire project to ProjectBin for sequence management.
            // IMPORTANT: clearAll() must come BEFORE setProject() so that
            // syncListView() (called inside setProject) rebuilds from an
            // empty grid, not from the previous project's stale items.
            // Also force a tree/sync refresh in both view modes since
            // setProject() only calls syncListView() when in list view.
            if (auto* bin = projectBin()) {
                bin->clearAll();
                bin->setCommandStack(m_commandStack);

                // Restore bin media files from saved state only.
                auto savedFiles = m_currentProject->binFiles(); // copy
                spdlog::info("setCurrentProject: project binFiles={} binFolders={}",
                             savedFiles.size(), m_currentProject->binFolders().size());

                bin->setProject(m_currentProject.get());

                // Force-sync both views so the tree and grid are rebuilt
                // from the new project regardless of current view mode.
                bin->refreshAllViews();

                if (!savedFiles.empty())
                    bin->addFiles(savedFiles);

                // Always restore bin folder structure (even when 0 files)
                const auto& projFolders = m_currentProject->binFolders();
                if (!projFolders.empty()) {
                    std::vector<BinFolderState> uiFolders;
                    uiFolders.reserve(projFolders.size());
                    for (const auto& pf : projFolders) {
                        BinFolderState bf;
                        bf.name      = pf.name;
                        bf.expanded  = pf.expanded;
                        bf.childKeys = pf.childKeys;
                        uiFolders.push_back(std::move(bf));
                    }
                    bin->restoreBinFolders(uiFolders);
                }
                spdlog::info("setCurrentProject: restored {} bin files, {} folders",
                             savedFiles.size(), projFolders.size());
            }

            // Wire project to TimelineWorkspace for sequence tabs
            if (m_timelineWorkspace)
                m_timelineWorkspace->setProject(m_currentProject.get());

            // â”€â”€ Apply project framerate to all consumers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            // Without this, the PlaybackController stays at its default
            // (24fps) and the ProgramMonitor gates compositing to 24fps
            // regardless of what the project actually specifies.
            {
                double fps = m_currentProject->settings().frameRate();
                if (fps < 1.0) fps = 60.0; // sane default

                if (m_playbackController)
                    m_playbackController->setFrameRate(fps);

                if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
                    m_timelineWorkspace->timelinePanel()->setFrameRate(fps);

                spdlog::info("setCurrentProject: applied project framerate {:.1f} fps", fps);
            }

            // â”€â”€ Migrate stale normalized positions from old exports â”€â”€â”€â”€â”€â”€
            // Old export code stored normalized 0â€“1 values directly as pixel
            // offsets.  Detect this (both posX/posY in (0.01,0.99)) and
            // convert to real pixel offsets.
            for (size_t ti = 0; ti < projTimeline->trackCount(); ++ti) {
                Track* track = projTimeline->track(ti);
                if (!track || track->type() != TrackType::Video) continue;

                for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                    Clip* clip = track->clip(ci);
                    if (!clip) continue;

                    // Only migrate SpineClip / VideoClip on video tracks
                    if (clip->clipType() != ClipType::Spine &&
                        clip->clipType() != ClipType::Video)
                        continue;

                    float px = clip->positionX().evaluate(0);
                    float py = clip->positionY().evaluate(0);

                    // Heuristic: if both are in (0.01, 0.99), they are normalized
                    // coordinates from the old export (pixel offsets are typically
                    // hundreds of pixels, well outside this range).
                    if (px > 0.01f && px < 0.99f && py > 0.01f && py < 0.99f) {
                        float newPx = (px - 0.5f) * 1920.0f;
                        float newPy = (py - 0.5f) * 1080.0f;
                        clip->positionX().addKeyframe(0, newPx);
                        clip->positionY().addKeyframe(0, newPy);
                        spdlog::info("  Migrated clip '{}' position: ({:.3f},{:.3f}) â†’ ({:.1f},{:.1f})",
                                     clip->label(), px, py, newPx, newPy);
                    }
                }
            }
        }

        // Wire the App-level CommandStack to mark the project modified
        // whenever any command is executed/undone/redone.  The Project has
        // its own CommandStack with a change callback, but UI panels use
        // the App-level one — so without this, isModified() stays false
        // and auto-save never triggers.
        if (m_commandStack) {
            m_commandStack->setChangeCallback([this]() {
                if (m_currentProject)
                    m_currentProject->setModified(true);
            });
        }
    } else {
        if (m_projectPanel)
            m_projectPanel->setCurrentProjectName({});
        if (auto* bin = projectBin())
            bin->setProjectName({});
        setWindowTitle(QString("ROUNDTABLE NLE %1").arg(ROUNDTABLE_VERSION));
    }
}

void MainWindow::onCreateProjectFromPanel(const QString& name, uint32_t resW, uint32_t resH,
                                          double fps, const QString& saveDir)
{
    if (!checkUnsavedChanges()) return;

    spdlog::info("Creating project: {} ({}x{} @ {} fps)",
                 name.toStdString(), resW, resH, fps);

    // Create the project object
    auto project = Project::createNew(name.toStdString());

    // Apply the user's settings directly
    project->settings().setResolution(resW, resH);
    project->settings().setFrameRate(fps);

    // Save it to disk — each project gets its own subfolder
    QString projDir = saveDir.isEmpty() ? projectsDirectory() : saveDir;
    QString projectFolder = projDir + "/" + name;
    QDir projectDir(projectFolder);
    if (!projectDir.exists() && !projectDir.mkpath(".")) {
        spdlog::error("Failed to create project folder: {}",
                      projectFolder.toStdString());
        QMessageBox::warning(this, "Error",
            QString("Failed to create project folder.\n"
                    "The save location may be read-only or the path invalid:\n%1")
                .arg(projDir));
        return;
    }
    // Use wide-string conversion to preserve Unicode characters on Windows
    std::filesystem::path path =
        (projectFolder + "/" + name + ".rtp").toStdWString();
    project->setFilePath(path);

    ProjectSerializer serializer;
    if (serializer.save(*project, path)) {
        spdlog::info("Project saved to: {}", path.string());
        setCurrentProject(std::move(project));
        addToRecentFiles(QString::fromStdString(path.string()));
        refreshProjectsList();
        statusBar()->showMessage(
            QString("Project '%1' created").arg(name), 3000);
    } else {
        spdlog::error("Failed to save new project: {}", path.string());
        QMessageBox::warning(this, "Error",
            QString("Failed to save project '%1'.\n\n"
                    "Check that the destination folder is writable and has\n"
                    "enough free space:\n%2")
                .arg(name, projDir));
    }
}

void MainWindow::onOpenProjectFromPanel(const QString& name)
{
    if (!checkUnsavedChanges()) return;

    spdlog::info("=== OPEN PROJECT START: {} ===", name.toStdString());
    auto t0 = std::chrono::steady_clock::now();
    showBusyIndicator(tr("Opening project..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    // Save state of current project before opening new one
    if (m_currentProject && m_audioSync) {
        spdlog::info("OPEN: saving current project audio sync state");
        m_audioSync->saveProjectState(
            QString::fromStdString(m_currentProject->name()));

        // Save which page was active — but NOT if we're on the Projects page,
        // because we're here specifically to switch projects. Saving 0 (Projects)
        // would cause the next open to stay on the Projects tab.
        Page curPage = currentPage();
        if (curPage != Page::Projects) {
            auto settings = rt::appSettings();
            settings.setValue("Project/" + QString::fromStdString(m_currentProject->name()) + "/activePage",
                              static_cast<int>(curPage));
        }
    }

    // Use the precise file path from the project panel when available,
    // falling back to the standard projects-directory convention.  This
    // ensures projects saved to custom locations or discovered via the
    // recent-files list are opened at their actual on-disk location.
    QString filePath;
    if (m_projectPanel)
        filePath = m_projectPanel->projectFilePath(name);
    if (filePath.isEmpty())
        filePath = projectsDirectory() + "/" + name + "/" + name + ".rtp";

    std::filesystem::path path = filePath.toStdWString();

    spdlog::info("OPEN: calling serializer.load for {}", path.string());
    ProjectSerializer serializer;
    auto project = serializer.load(path);
    auto t1 = std::chrono::steady_clock::now();
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    spdlog::info("OPEN: serializer.load took {} ms", dt);

    if (project) {
        // Normalize display name to the selected project entry.
        if (project->name() != name.toStdString())
            project->setName(name.toStdString());
        project->setFilePath(path);

        spdlog::info("OPEN: calling setCurrentProject");
        setCurrentProject(std::move(project));
        auto t2 = std::chrono::steady_clock::now();
        auto dt2 = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        spdlog::info("OPEN: setCurrentProject took {} ms", dt2);

        spdlog::info("OPEN: calling restoreWorkspace");
        // Prefer the project's own saved layout; fall back to the last
        // session snapshot for older projects that don't have one yet.
        if (!restoreWorkspace("project/" + name))
            restoreWorkspace("last_session");

        // Stay on the current tab (Projects) instead of restoring the
        // last active page for this project.
        setCurrentPage(Page::Projects);

        spdlog::info("OPEN: restoring audio sync state");
        // Restore audio sync state for this project
        // Prefer the blob embedded in the .rtp file (backed up + versioned)
        // over QSettings (which has no backup).
        if (m_audioSync) {
            const auto& blob = m_currentProject->audioSyncBlob();
            spdlog::info("OPEN: AudioSync blob size={}", blob.size());
            if (!blob.empty()) {
                spdlog::info("OPEN: calling deserializeFromBlob");
                m_audioSync->deserializeFromBlob(blob);
                spdlog::info("OPEN: after deserialize — audioPaths.size={}",
                             m_audioSync->audioPaths().size());
            } else {
                spdlog::info("OPEN: blob empty, calling restoreProjectState");
                m_audioSync->restoreProjectState(name);
            }
        } else {
            spdlog::warn("OPEN: m_audioSync is null — cannot restore audio state");
        }

        auto t3 = std::chrono::steady_clock::now();
        auto dt3 = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t0).count();
        spdlog::info("=== OPEN PROJECT COMPLETE: {} total ms ===", dt3);

        addToRecentFiles(QString::fromStdString(path.string()));

        statusBar()->showMessage(
            QString("Opened '%1'").arg(name), 3000);
        hideBusyIndicator();
        QApplication::setOverrideCursor(Qt::ArrowCursor);
    } else {
        hideBusyIndicator();
        QApplication::setOverrideCursor(Qt::ArrowCursor);
        spdlog::error("Failed to load project: {}", path.string());
        QMessageBox::warning(this, "Error",
            QString("Failed to open project '%1'").arg(name));
    }
}

void MainWindow::onDeleteProjectFromPanel(const QString& name, const QString& filePath)
{
    auto reply = QMessageBox::question(this, "Delete Project",
        QString("Delete project '%1'? This cannot be undone.").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    // If the deleted project is the currently open project, close it first
    // so subsequent operations (create, open) don't think it's still open.
    // Important: clean up all references BEFORE destroying the project,
    // otherwise dangling pointers in subsystems cause use-after-free crashes.
    if (m_currentProject) {
        QString currentName = QString::fromStdString(m_currentProject->name());
        if (currentName == name) {
            // 1. Stop audio playback / transport
            if (m_playbackController && m_playbackController->isPlaying())
                m_playbackController->stop();

            // 2. Stop the async composite pipeline (FrameProducer thread)
            if (m_timelineWorkspace) {
                if (auto* pm = m_timelineWorkspace->programMonitor()) {
                    pm->stopPolling();
                    if (auto* pl = pm->pipeline())
                        pl->stop();
                }
            }
            if (m_timelineWorkspace) {
                if (auto* sm = m_timelineWorkspace->sourceMonitor()) {
                    if (auto* ctrl = sm->controller()) {
                        if (ctrl->isPlaying()) ctrl->stop();
                    }
                }
            }

            // 3. Disconnect old timeline from all consumers so no subsystem
            //    accesses destroyed project data.
            if (m_timelineWorkspace) {
                if (auto* pm = m_timelineWorkspace->programMonitor())
                    pm->setCompositeCallback(nullptr);
                m_timelineWorkspace->setTimeline(nullptr);
            }
            if (m_playbackController)
                m_playbackController->setTimeline(nullptr);
            m_timeline = nullptr;

            // 4. Release all media from the old project and clear the frame cache
            if (m_mediaPool)
                m_mediaPool->closeAll();

            // 5. Reset per-project panels
            if (m_audioSync)
                m_audioSync->resetForNewProject();

            // 6. Clear undo/redo history
            if (m_commandStack)
                m_commandStack->clear();

            // 7. Clear export panel references
            if (m_exportPanel) {
                m_exportPanel->setTimeline(nullptr);
                m_exportPanel->setProject(nullptr);
            }

            // 8. Update UI state
            if (m_projectPanel)
                m_projectPanel->setCurrentProjectName({});
            if (auto* bin = projectBin())
                bin->setProjectName({});
            setWindowTitle(QString("ROUNDTABLE NLE %1").arg(ROUNDTABLE_VERSION));

            // 9. Now safe to destroy the old project
            m_lastSavedAudioSyncBlob = {};
            m_currentProject.reset();
        }
    }

    // Determine the project folder:
    // - If filePath is known (external drive project), use its parent folder.
    // - Otherwise fall back to projectsDirectory/name.
    QString projectFolder;
    if (!filePath.isEmpty()) {
        projectFolder = QFileInfo(filePath).absolutePath();
    } else {
        projectFolder = projectsDirectory() + "/" + name;
    }

    bool deleted = QDir(projectFolder).removeRecursively();

    if (deleted) {
        spdlog::info("Deleted project: {} (folder: {})",
                     name.toStdString(), projectFolder.toStdString());
        refreshProjectsList();
        statusBar()->showMessage(
            QString("Project '%1' deleted").arg(name), 3000);
    } else {
        QMessageBox::warning(this, "Error",
            QString("Failed to delete '%1'").arg(name));
    }
}

void MainWindow::onRenameProjectFromPanel(const QString& oldName, const QString& newName)
{
    spdlog::info("Renaming project '{}' -> '{}'", oldName.toStdString(), newName.toStdString());

    QString projDir = projectsDirectory();

    if (QDir(projDir + "/" + newName).exists()) {
        QMessageBox::warning(this, "Error",
            QString("A project named '%1' already exists.").arg(newName));
        return;
    }

    QString oldFolder = projDir + "/" + oldName;
    // Rename files inside, then rename the folder
    QString oldRtp = oldFolder + "/" + oldName + ".rtp";
    QString newRtp = oldFolder + "/" + newName + ".rtp";
    QFile::rename(oldRtp, newRtp);
    QFile::rename(oldRtp + ".bak", newRtp + ".bak");
    QFile::rename(oldFolder + "/" + oldName + ".png", oldFolder + "/" + newName + ".png");
    QFile::rename(oldFolder + "/" + oldName + ".jpg", oldFolder + "/" + newName + ".jpg");
    QString newFolder = projDir + "/" + newName;
    QString newFilePath = newFolder + "/" + newName + ".rtp";
    bool renamed = QDir().rename(oldFolder, newFolder);

    if (renamed) {
        // If the renamed project is the currently loaded one, update it
        if (m_currentProject &&
            QString::fromStdString(m_currentProject->name()) == oldName) {
            m_currentProject->setName(newName.toStdString());
            m_currentProject->setFilePath(newFilePath.toStdWString());
            if (m_projectPanel) m_projectPanel->setCurrentProjectName(newName);
            if (auto* bin = projectBin()) bin->setProjectName(newName);
            setWindowTitle(QString("ROUNDTABLE NLE %1 — %2").arg(ROUNDTABLE_VERSION).arg(newName));
        }
        refreshProjectsList();
        statusBar()->showMessage(
            QString("Renamed '%1' to '%2'").arg(oldName, newName), 3000);
    } else {
        QMessageBox::warning(this, "Error",
            QString("Failed to rename '%1'").arg(oldName));
    }
}

void MainWindow::onDuplicateProjectFromPanel(const QString& name)
{
    spdlog::info("Duplicating project: {}", name.toStdString());

    QString projDir = projectsDirectory();
    QString srcPath = projDir + "/" + name + "/" + name + ".rtp";

    // Find a unique name
    QString newName = name + " (Copy)";
    int counter = 2;
    while (QDir(projDir + "/" + newName).exists()) {
        newName = name + QString(" (Copy %1)").arg(counter++);
    }

    // Create subfolder for duplicate
    QString newFolder = projDir + "/" + newName;
    QDir().mkpath(newFolder);
    QString dstPath = newFolder + "/" + newName + ".rtp";

    if (QFile::copy(srcPath, dstPath)) {
        // Also copy thumbnail if it exists
        QString srcThumb = projDir + "/" + name + "/" + name + ".png";
        if (QFile::exists(srcThumb))
            QFile::copy(srcThumb, newFolder + "/" + newName + ".png");

        refreshProjectsList();
        statusBar()->showMessage(
            QString("Duplicated as '%1'").arg(newName), 3000);
    } else {
        QDir(newFolder).removeRecursively();
        QMessageBox::warning(this, "Error",
            QString("Failed to duplicate '%1'").arg(name));
    }
}

void MainWindow::onRevealProjectInExplorer(const QString& name)
{
    QString projDir = projectsDirectory();
    QString filePath = projDir + "/" + name + "/" + name + ".rtp";
    QFileInfo fi(filePath);
    if (fi.exists()) {
        // On Windows, open Explorer and select the file
        QProcess::startDetached("explorer.exe",
            {"/select,", QDir::toNativeSeparators(fi.absoluteFilePath())});
    }
}

void MainWindow::onNewProjectForMedia(const QString& filePath, int64_t /*atTick*/, size_t /*trackIndex*/)
{
    if (!checkUnsavedChanges()) return;

    // Read media properties to set sequence resolution / frame rate
    uint32_t mediaW = 1920, mediaH = 1080;
    double mediaFps = 30.0;
    if (!filePath.isEmpty() && m_mediaPool) {
        uint64_t h = m_mediaPool->open(filePath.toStdString());
        if (h != 0) {
            const auto* info = m_mediaPool->getInfo(h);
            if (info) {
                if (info->width  > 0) mediaW = info->width;
                if (info->height > 0) mediaH = info->height;
                if (info->fps    > 0) mediaFps = info->fps;
            }
        }
    }

    // Default to 30 fps for still images
    if (mediaFps <= 0.0) mediaFps = 30.0;

    // Derive project name from the dropped file
    QString baseName = filePath.isEmpty()
        ? QStringLiteral("New Project")
        : QFileInfo(filePath).completeBaseName();
    QString projName = baseName;
    // Ensure unique name in the projects directory
    QString projDir = projectsDirectory();
    int counter = 0;
    while (QDir(projDir + "/" + projName).exists()) {
        ++counter;
        projName = baseName + "_" + QString::number(counter);
    }

    spdlog::info("Creating project from dropped media: {} ({}x{} @ {} fps)",
                 projName.toStdString(), mediaW, mediaH, mediaFps);

    auto project = Project::createNew(projName.toStdString());
    project->settings().setResolution(mediaW, mediaH);
    project->settings().setFrameRate(mediaFps);

    // Save the project
    QString projectFolder = projDir + "/" + projName;
    QDir().mkpath(projectFolder);
    // Use wide-string conversion to preserve Unicode characters on Windows
    std::filesystem::path path =
        (projectFolder + "/" + projName + ".rtp").toStdWString();
    project->setFilePath(path);

    ProjectSerializer serializer;
    if (serializer.save(*project, path)) {
        spdlog::info("Project saved to: {}", path.string());
        setCurrentProject(std::move(project));
        refreshProjectsList();
        statusBar()->showMessage(
            QString("Project '%1' created from dropped media").arg(projName), 3000);

        // Switch to the TIMELINE page so the user sees the result
        setCurrentPage(Page::Timeline);
    } else {
        spdlog::error("Failed to save new project: {}", path.string());
        QMessageBox::warning(this, "Error",
            QString("Failed to create project '%1'").arg(projName));
    }
}

void MainWindow::onOpenRecentProjectFromPanel(const QString& filePath)
{
    if (!checkUnsavedChanges()) return;

    spdlog::info("Opening recent project: {}", filePath.toStdString());

    ProjectSerializer serializer;
    auto project = serializer.load(filePath.toStdWString());
    if (project) {
        const QString loadedName = QFileInfo(filePath).baseName();
        if (project->name() != loadedName.toStdString())
            project->setName(loadedName.toStdString());
        project->setFilePath(filePath.toStdString());
        setCurrentProject(std::move(project));
        addToRecentFiles(filePath);

        // Stay on the current tab (Projects) instead of restoring the
        // last active page for this project.
        setCurrentPage(Page::Projects);

        // Restore audio sync state — prefer blob embedded in .rtp over QSettings
        if (m_audioSync) {
            const auto& blob = m_currentProject->audioSyncBlob();
            if (!blob.empty())
                m_audioSync->deserializeFromBlob(blob);
            else
                m_audioSync->restoreProjectState(loadedName);
        }

        statusBar()->showMessage("Opened: " + QFileInfo(filePath).fileName(), 3000);
    } else {
        QMessageBox::warning(this, "Error", "Failed to open " + filePath);
    }
}

void MainWindow::onImportProject(const QString& srcPath)
{
    spdlog::info("Importing project from: {}", srcPath.toStdString());

    QString projDir = projectsDirectory();
    QDir().mkpath(projDir);
    QString baseName = QFileInfo(srcPath).baseName();

    // Find a unique project name
    QString name = baseName;
    int n = 2;
    while (QDir(projDir + "/" + name).exists() ||
           QFile::exists(projDir + "/" + name + ".rtp")) {
        name = baseName + QString(" (%1)").arg(n++);
    }

    // Create project subfolder and copy into it
    QString projectFolder = projDir + "/" + name;
    QDir().mkpath(projectFolder);
    QString dstPath = projectFolder + "/" + name + ".rtp";

    if (QFile::copy(srcPath, dstPath)) {
        // Normalize internal project metadata to the new imported name.
        ProjectSerializer serializer;
        if (auto imported = serializer.load(dstPath.toStdWString())) {
            imported->setName(name.toStdString());
            imported->setFilePath(dstPath.toStdWString());
            imported->setModified(false);
            if (!serializer.save(*imported, dstPath.toStdWString())) {
                spdlog::warn("Import: copied project but failed to rewrite internal name for '{}'",
                             name.toStdString());
            }
        } else {
            spdlog::warn("Import: copied project but could not reload '{}' to normalize metadata",
                         dstPath.toStdString());
        }

        refreshProjectsList();
        statusBar()->showMessage(
            "Imported: " + name, 3000);
    } else {
        QDir(projectFolder).removeRecursively();
        QMessageBox::warning(this, "Error",
            "Failed to import project from " + srcPath);
    }
}

void MainWindow::onExportProject(const QString& name, const QString& dstPath)
{
    spdlog::info("Exporting project '{}' to: {}", name.toStdString(), dstPath.toStdString());

    QString projDir = projectsDirectory();

    // Locate source .rtp (subfolder or flat)
    QString srcPath = projDir + "/" + name + "/" + name + ".rtp";

    if (QFile::copy(srcPath, dstPath)) {
        statusBar()->showMessage(
            "Exported '" + name + "' to " + QFileInfo(dstPath).dir().path(), 3000);
    } else {
        QMessageBox::warning(this, "Error",
            "Failed to export project '" + name + "'");
    }
}

void MainWindow::onImportSrt()
{
    QString path = QFileDialog::getOpenFileName(
        this, "Import SRT Subtitles", QString(),
        "SRT Files (*.srt);;All Files (*)");
    if (path.isEmpty()) return;

    auto entries = parseSrt(std::filesystem::path(path.toStdWString()));
    if (entries.empty()) {
        QMessageBox::information(this, "Import SRT", "No subtitle entries found.");
        return;
    }

    int count = importSrt(*m_timeline, entries);
    statusBar()->showMessage(
        QString("Imported %1 subtitle(s)").arg(count), 3000);
    if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
        m_timelineWorkspace->timelinePanel()->rebuildTracks();
}

void MainWindow::onExportSrt()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Export SRT Subtitles", QString(),
        "SRT Files (*.srt)");
    if (path.isEmpty()) return;

    int count = exportSrt(*m_timeline, std::filesystem::path(path.toStdWString()));
    if (count > 0)
        statusBar()->showMessage(
            QString("Exported %1 subtitle(s)").arg(count), 3000);
    else
        QMessageBox::information(this, "Export SRT",
            "No text/graphic clips found to export.");
}

void MainWindow::onProjectsDirChanged(const QString& newDir)
{
    spdlog::info("Projects directory changed to: {}", newDir.toStdString());
    auto settings = rt::appSettings();
    settings.setValue("ProjectsDirectory", newDir);
    refreshProjectsList();
    statusBar()->showMessage(
        "Projects folder: " + newDir, 3000);
}

void MainWindow::onNewProject()
{
    spdlog::info("File > New Project");
    // Switch to Projects page so user can name the project
    setCurrentPage(Page::Projects);
    if (m_projectPanel)
        m_projectPanel->nameInput()->setFocus();
}

void MainWindow::onOpenProject()
{
    if (!checkUnsavedChanges()) return;

    spdlog::info("File > Open Project");
    QString path = QFileDialog::getOpenFileName(
        this, "Open Project", projectsDirectory(),
        "ROUNDTABLE Projects (*.rtp);;All Files (*)");

    if (path.isEmpty()) return;

    showBusyIndicator(tr("Opening project..."));
    ProjectSerializer serializer;
    auto project = serializer.load(path.toStdWString());
    if (project) {
        const QString loadedName = QFileInfo(path).baseName();
        if (project->name() != loadedName.toStdString())
            project->setName(loadedName.toStdString());
        project->setFilePath(path.toStdWString());
        setCurrentProject(std::move(project));
        if (!restoreWorkspace("project/" + loadedName))
            restoreWorkspace("last_session");
        addToRecentFiles(path);
        // Stay on the current tab (Projects) instead of switching to Timeline
        setCurrentPage(Page::Projects);

        // Restore audio sync state
        if (m_audioSync) {
            const auto& blob = m_currentProject->audioSyncBlob();
            if (!blob.empty())
                m_audioSync->deserializeFromBlob(blob);
            else
                m_audioSync->restoreProjectState(loadedName);
        }

        hideBusyIndicator();
        statusBar()->showMessage("Project opened", 3000);
    } else {
        hideBusyIndicator();
        QMessageBox::warning(this, "Error",
            "Failed to open the selected project file.");
    }
}

void MainWindow::onSaveProject()
{
    spdlog::info("File > Save");
    if (!m_currentProject) {
        statusBar()->showMessage("No project to save", 3000);
        return;
    }

    auto path = m_currentProject->filePath();
    if (path.empty()) {
        onSaveProjectAs();
        return;
    }

    // â”€â”€ Capture bin state into the project before serialization â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        // Persist only what is explicitly in the Project Bin.
        std::vector<std::filesystem::path> binFiles;
        if (auto* bin = projectBin())
            binFiles = bin->allFiles();

        m_currentProject->setBinFiles(binFiles);

        // Capture bin folder structure
        if (auto* bin = projectBin()) {
            auto uiFolders = bin->binFolderState();
            std::vector<Project::BinFolder> projFolders;
            projFolders.reserve(uiFolders.size());
            for (auto& f : uiFolders) {
                Project::BinFolder pf;
                pf.name      = std::move(f.name);
                pf.expanded  = f.expanded;
                pf.childKeys = std::move(f.childKeys);
                projFolders.push_back(std::move(pf));
            }
            m_currentProject->setBinFolders(std::move(projFolders));
            spdlog::info("onSaveProject: captured bin state â€” {} files, {} folders",
                         binFiles.size(), uiFolders.size());
        } else {
            spdlog::warn("onSaveProject: projectBin() returned nullptr â€” bin state NOT saved");
        }
    }

    // Capture AudioSync state into the project blob BEFORE serializing
    if (m_audioSync) {
        auto blob = m_audioSync->serializeToBlob();
        spdlog::info("onSaveProject: AudioSync blob {} bytes", blob.size());
        m_currentProject->setAudioSyncBlob(std::move(blob));
    } else {
        spdlog::warn("onSaveProject: m_audioSync is null");
    }

    ProjectSerializer serializer;
    if (serializer.save(*m_currentProject, path)) {
        m_currentProject->setModified(false);
        m_lastSavedAudioSyncBlob = m_currentProject->audioSyncBlob();

        saveWorkspace("project/" + QString::fromStdString(m_currentProject->name()));
        saveWorkspace("last_session");

        // Auto-capture thumbnail from current playhead frame
        captureProjectThumbnail();

        // Save audio sync state (transcriptions, matches, clips)
        if (m_audioSync)
            m_audioSync->saveProjectState(QString::fromStdString(m_currentProject->name()));

        // Save active page per project
        auto settings = rt::appSettings();
        settings.setValue("Project/" + QString::fromStdString(m_currentProject->name()) + "/activePage",
                          static_cast<int>(currentPage()));

        addToRecentFiles(QString::fromStdString(path.string()));
        statusBar()->showMessage("Project saved", 3000);
    } else {
        QMessageBox::warning(this, "Error", "Failed to save project.");
    }
}

void MainWindow::onSaveProjectAs()
{
    spdlog::info("File > Save As");
    if (!m_currentProject) {
        statusBar()->showMessage("No project to save", 3000);
        return;
    }

    QString path = QFileDialog::getSaveFileName(
        this, "Save Project As", projectsDirectory(),
        "ROUNDTABLE Projects (*.rtp)");

    if (path.isEmpty()) return;

    if (!path.endsWith(".rtp"))
        path += ".rtp";

    // Place the file inside a project subfolder named after the project
    QFileInfo fi(path);
    QString projectName = fi.baseName();
    QString parentDir   = fi.absolutePath();
    QString projectFolder = parentDir + "/" + projectName;
    QDir().mkpath(projectFolder);
    path = projectFolder + "/" + projectName + ".rtp";

    m_currentProject->setFilePath(path.toStdString());
    m_currentProject->setName(projectName.toStdString());

    // â”€â”€ Capture bin state into the project before serialization â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        std::vector<std::filesystem::path> binFiles;
        if (auto* bin = projectBin())
            binFiles = bin->allFiles();
        m_currentProject->setBinFiles(binFiles);

        if (auto* bin = projectBin()) {
            auto uiFolders = bin->binFolderState();
            std::vector<Project::BinFolder> projFolders;
            projFolders.reserve(uiFolders.size());
            for (auto& f : uiFolders) {
                Project::BinFolder pf;
                pf.name      = std::move(f.name);
                pf.childKeys = std::move(f.childKeys);
                projFolders.push_back(std::move(pf));
            }
            m_currentProject->setBinFolders(std::move(projFolders));
            spdlog::info("onSaveProjectAs: captured bin state â€” {} files, {} folders",
                         binFiles.size(), uiFolders.size());
        }
    }

    // Capture AudioSync state into blob before serializing
    if (m_audioSync)
        m_currentProject->setAudioSyncBlob(m_audioSync->serializeToBlob());

    ProjectSerializer serializer;
    if (serializer.save(*m_currentProject, path.toStdString())) {
        m_currentProject->setModified(false);
        m_lastSavedAudioSyncBlob = m_currentProject->audioSyncBlob();

        saveWorkspace("project/" + QString::fromStdString(m_currentProject->name()));
        saveWorkspace("last_session");

        // Auto-capture thumbnail from current playhead frame
        captureProjectThumbnail();

        // Save audio sync state BEFORE moving the project (transcriptions, matches, clips)
        if (m_audioSync)
            m_audioSync->saveProjectState(QFileInfo(path).baseName());

        setCurrentProject(std::move(m_currentProject)); // refresh title

        // Save active page per project (use the new name from path)
        auto settings = rt::appSettings();
        settings.setValue("Project/" + QFileInfo(path).baseName() + "/activePage",
                          static_cast<int>(currentPage()));

        refreshProjectsList();
        addToRecentFiles(path);
        statusBar()->showMessage("Project saved", 3000);
    } else {
        QMessageBox::warning(this, "Error", "Failed to save project.");
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Auto-capture project thumbnail from current playhead frame
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void MainWindow::captureProjectThumbnail()
{
    if (!m_currentProject) return;
    if (!m_projectPanel) return;

    QString projectName = QString::fromStdString(m_currentProject->name());

    // Always composite a 16:9 (project-aspect) frame at the current playhead.
    // Previously a Strategy-1 fallback grabbed the program-monitor viewport widget,
    // which captured letterbox bars when the monitor dock was in portrait orientation.
    // The composited path below renders at exactly the project resolution, so the
    // thumbnail aspect always matches the project (e.g. 1920x1080 -> 480x270).
    if (!m_timelineWorkspace || !m_playbackController) return;

    int64_t tick = m_playbackController->currentTick();

    uint32_t projW = m_currentProject->settings().resolution().width;
    uint32_t projH = m_currentProject->settings().resolution().height;
    if (projW == 0 || projH == 0) { projW = 1920; projH = 1080; }

    double aspect = static_cast<double>(projW) / static_cast<double>(projH);
    uint32_t thumbW = 480;
    uint32_t thumbH = static_cast<uint32_t>(thumbW / aspect);
    thumbW &= ~1u;
    thumbH &= ~1u;

    // Force CPU readback â€” GPU display mode skips pixel readback entirely,
    // leaving CachedFrame::pixels empty and ensurePixels() failing.
    const bool wasGpuMode = m_timelineWorkspace->gpuDisplayMode();
    if (wasGpuMode)
        m_timelineWorkspace->setGpuDisplayMode(false);

    auto frame = m_timelineWorkspace->compositeFrame(tick, thumbW, thumbH, true);

    if (wasGpuMode)
        m_timelineWorkspace->setGpuDisplayMode(true);
    if (!frame) {
        spdlog::warn("captureProjectThumbnail: compositeFrame returned null");
        return;
    }

    if (!frame->ensurePixels()) {
        spdlog::warn("captureProjectThumbnail: ensurePixels failed");
        return;
    }
    if (frame->pixels.empty() || frame->width == 0 || frame->height == 0) {
        spdlog::warn("captureProjectThumbnail: empty frame data");
        return;
    }

    m_projectPanel->setThumbnailFromPixels(
        projectName, frame->pixels.data(), frame->width, frame->height);
    spdlog::info("captureProjectThumbnail: composited thumbnail ({}x{})",
                 frame->width, frame->height);
}


} // namespace rt