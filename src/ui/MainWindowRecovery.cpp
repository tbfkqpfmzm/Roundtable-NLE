/*
 * MainWindowRecovery.cpp - Auto-save, crash recovery, and recent files.
 * Split from MainWindowProject.cpp.
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

#include "project/AutoSave.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "spine/ShotPreset.h"
#include "SrtIO.h"

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

#include "Settings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <set>


namespace rt {

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Auto-save
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â


// ═══════════════════════════════════════════════════════════════════════════
//  Restore from Auto-Save
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::onRestoreFromAutoSave()
{
    if (!m_currentProject) {
        QMessageBox::information(this, "Restore from Auto-Save",
            "No project is open. Open a project first.");
        return;
    }

    auto projPath = m_currentProject->filePath();
    if (projPath.empty()) {
        QMessageBox::information(this, "Restore from Auto-Save",
            "Project has not been saved yet — no auto-saves exist.");
        return;
    }

    auto folder = AutoSave::autoSaveFolder(projPath);
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec)) {
        QMessageBox::information(this, "Restore from Auto-Save",
            "No auto-save folder found for this project.");
        return;
    }

    // Collect all auto-save files sorted newest-first
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
        uintmax_t size;
    };
    std::vector<Entry> files;
    for (auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".rtp") continue;
        auto wt = entry.last_write_time(ec);
        if (ec) continue;
        files.push_back({entry.path(), wt, entry.file_size(ec)});
    }

    if (files.empty()) {
        QMessageBox::information(this, "Restore from Auto-Save",
            "No auto-save files found.");
        return;
    }

    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.time > b.time; });

    // Build a list of display names
    QStringList items;
    for (const auto& f : files) {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            f.time - std::filesystem::file_time_type::clock::now()
            + std::chrono::system_clock::now());
        auto tt = std::chrono::system_clock::to_time_t(sctp);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &tt);
#else
        localtime_r(&tt, &local);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
        double sizeMB = static_cast<double>(f.size) / (1024.0 * 1024.0);
        items.append(QString("%1  (%2 MB)")
            .arg(buf)
            .arg(sizeMB, 0, 'f', 1));
    }

    bool ok = false;
    QString selected = QInputDialog::getItem(
        this, "Restore from Auto-Save",
        "Select an auto-save to restore:\n"
        "(Current unsaved changes will be lost)",
        items, 0, false, &ok);

    if (!ok || selected.isEmpty()) return;

    int idx = items.indexOf(selected);
    if (idx < 0 || idx >= static_cast<int>(files.size())) return;

    auto reply = QMessageBox::warning(this, "Restore from Auto-Save",
        QString("This will replace the current project state with:\n\n%1\n\n"
                "Any unsaved changes will be lost. Continue?")
            .arg(QString::fromStdString(files[static_cast<size_t>(idx)].path.filename().string())),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    showBusyIndicator(tr("Restoring auto-save..."));
    ProjectSerializer serializer;
    auto project = serializer.load(files[static_cast<size_t>(idx)].path);
    if (project) {
        project->setFilePath(projPath);
        project->setModified(true);
        setCurrentProject(std::move(project));
        hideBusyIndicator();
        statusBar()->showMessage("Restored from auto-save", 5000);
        spdlog::info("Restored project from auto-save: {}",
                     files[static_cast<size_t>(idx)].path.string());
    } else {
        hideBusyIndicator();
        QMessageBox::warning(this, "Restore Failed",
            "Could not load the selected auto-save file.");
    }
}
void MainWindow::onAutoSave()
{
    if (!m_currentProject) return;

    bool audioSyncDirty = false;
    std::vector<uint8_t> currentAudioSyncBlob;
    if (m_audioSync) {
        currentAudioSyncBlob = m_audioSync->serializeToBlob();
        audioSyncDirty = (currentAudioSyncBlob != m_lastSavedAudioSyncBlob);
    }

    if (!m_currentProject->isModified() && !audioSyncDirty) return;

    auto projPath = m_currentProject->filePath();
    if (projPath.empty()) return;  // not yet saved to disk

    // Save to "Roundtable Auto-Save" folder with timestamped filename
    auto folder = AutoSave::autoSaveFolder(projPath);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        spdlog::warn("Auto-save: failed to create folder '{}': {}",
                     folder.string(), ec.message());
        return;
    }

    auto savePath = AutoSave::makeTimestampedPath(
        folder, m_currentProject->name());

    // Capture AudioSync state into blob before serializing
    if (m_audioSync)
        m_currentProject->setAudioSyncBlob(std::move(currentAudioSyncBlob));

    ProjectSerializer serializer;
    if (serializer.save(*m_currentProject, savePath)) {
        // Prune old auto-saves (keep max 20)
        auto s = rt::appSettings();
        size_t maxKeep = static_cast<size_t>(s.value("MaxAutoSaves", 20).toInt());
        AutoSave::pruneAutoSaves(folder, maxKeep);

        spdlog::info("Auto-saved to {}", savePath.string());

        statusBar()->showMessage("Auto-saved", 2000);
    }
}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Crash recovery
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void MainWindow::checkCrashRecovery()
{
    auto settings = rt::appSettings();
    QString lastPath = settings.value("LastProjectPath").toString();
    if (lastPath.isEmpty()) return;

    auto projPath = std::filesystem::path(lastPath.toStdWString());

    // Check for auto-saves in the "Roundtable Auto-Save" folder
    bool recovered = false;
    if (AutoSave::hasRecoverableAutoSave(projPath)) {
        auto newestAutoSave = AutoSave::findNewestAutoSave(projPath);
        if (!newestAutoSave.empty()) {
            auto reply = QMessageBox::question(
                this, "Recover Auto-Save",
                QString("An auto-save was found for:\n\n%1\n\n"
                        "Auto-save: %2\n\n"
                        "This auto-save is newer than your last saved version.\n"
                        "\n"
                        "Click Yes to restore your unsaved work.\n"
                        "Click No to load the last manually saved version "
                        "(changes since then will be lost).")
                    .arg(lastPath)
                    .arg(QString::fromStdString(newestAutoSave.filename().string())),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

            if (reply == QMessageBox::Yes) {
                ProjectSerializer serializer;
                auto project = serializer.load(newestAutoSave);
                if (project) {
                    project->setFilePath(projPath);
                    setCurrentProject(std::move(project));
                    statusBar()->showMessage("Recovered from auto-save", 5000);
                    spdlog::info("Recovered project from auto-save: {}",
                                 newestAutoSave.string());
                    recovered = true;
                } else {
                    QMessageBox::warning(this, "Recovery Failed",
                        "Could not load the auto-save file. Opening the last saved version.");
                }
            }
        }
    }

}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Recent files
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void MainWindow::addToRecentFiles(const QString& filePath)
{
    auto settings = rt::appSettings();
    QStringList recent = settings.value("RecentFiles").toStringList();

    recent.removeAll(filePath);
    recent.prepend(filePath);

    // Keep at most 10 entries
    while (recent.size() > 10)
        recent.removeLast();

    settings.setValue("RecentFiles", recent);
    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    if (!m_recentProjectsMenu) return;
    m_recentProjectsMenu->clear();

    auto settings = rt::appSettings();
    QStringList recent = settings.value("RecentFiles").toStringList();

    if (recent.isEmpty()) {
        m_recentProjectsMenu->setEnabled(false);
        return;
    }

    m_recentProjectsMenu->setEnabled(true);
    for (const QString& path : recent) {
        QFileInfo fi(path);
#if !defined(ROUNDTABLE_DEBUG) && !defined(ROUNDTABLE_DEV_BUILD)
        // Skip dev-only projects in release builds
        if (QFileInfo::exists(fi.absolutePath() + "/_dev"))
            continue;
#endif
        auto* act = m_recentProjectsMenu->addAction(fi.fileName());
        act->setData(path);
        connect(act, &QAction::triggered, this, [this, path]() {
            ProjectSerializer serializer;
            auto proj = serializer.load(path.toStdString());
            if (proj) {
                setCurrentProject(std::move(proj));
                addToRecentFiles(path);
                statusBar()->showMessage("Opened: " + QFileInfo(path).fileName(), 3000);
            } else {
                QMessageBox::warning(this, "Error", "Failed to open " + path);
            }
        });
    }

    m_recentProjectsMenu->addSeparator();
    m_recentProjectsMenu->addAction("Clear Recent", this, [this]() {
        auto s = rt::appSettings();
        s.remove("RecentFiles");
        updateRecentFilesMenu();
    });
}

} // namespace rt
