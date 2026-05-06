/*
 * AutoSave — Periodic automatic project saving with crash recovery.
 *
 * Step 29: Auto-save & Crash Recovery
 *
 * Saves the current project to a "Roundtable Auto-Save" folder alongside
 * the project file, with timestamped filenames like:
 *   ProjectName 2025-06-15 at 14.30.00.rtp
 *
 * Old auto-saves are pruned to keep at most maxAutoSaves copies (default 20).
 *
 * On next launch, if the auto-save folder contains a file newer than the
 * project file, the application can offer recovery.
 *
 * Architecture (no Qt dependency — lives in core):
 *   - The caller (App/MainWindow) drives the timer by calling tick()
 *     periodically (e.g. from a QTimer). AutoSave uses std::chrono
 *     internally to determine when an actual save is needed.
 *   - AutoSave uses ProjectSerializer for the actual save operation.
 *   - Each auto-save file is written atomically (write to .tmp, then rename)
 *     to avoid data loss if the process crashes mid-write.
 *
 * Usage:
 *   AutoSave autoSave;
 *   autoSave.setInterval(std::chrono::minutes(15));
 *   autoSave.setProject(&project);
 *
 *   // In a QTimer callback or game loop:
 *   autoSave.tick();       // saves if interval elapsed + project modified
 *
 *   // On next launch:
 *   if (AutoSave::hasRecoverableAutoSave(projectPath)) {
 *       auto recovered = AutoSave::loadLatestAutoSave(projectPath);
 *   }
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace rt {

class Project;
class ProjectSerializer;

/// Auto-save status for UI feedback.
enum class AutoSaveStatus : uint8_t
{
    Idle,           ///< Waiting for next interval
    Saving,         ///< Currently writing auto-save file
    Saved,          ///< Last auto-save succeeded
    Failed,         ///< Last auto-save failed
    Disabled        ///< Auto-save is turned off
};

/// Auto-save statistics.
struct AutoSaveStats
{
    size_t saveCount{0};           ///< Total successful auto-saves this session
    size_t failCount{0};           ///< Total failed auto-saves this session
    std::chrono::steady_clock::time_point lastSaveTime{};
    AutoSaveStatus status{AutoSaveStatus::Idle};
};

/// Automatic periodic project saver with crash recovery support.
class AutoSave
{
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::seconds;
    using TimePoint = Clock::time_point;

    AutoSave();
    ~AutoSave();

    // Non-copyable
    AutoSave(const AutoSave&) = delete;
    AutoSave& operator=(const AutoSave&) = delete;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the auto-save interval. Default: 15 minutes.
    void setInterval(Duration interval) noexcept;
    [[nodiscard]] Duration interval() const noexcept { return m_interval; }

    /// Set maximum number of auto-save copies to keep. Default: 20.
    void setMaxAutoSaves(size_t max) noexcept { m_maxAutoSaves = max; }
    [[nodiscard]] size_t maxAutoSaves() const noexcept { return m_maxAutoSaves; }

    /// Enable or disable auto-save. Default: enabled.
    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }

    /// Set the project to auto-save. Non-owning pointer.
    void setProject(Project* project) noexcept;
    [[nodiscard]] Project* project() const noexcept { return m_project; }

    /// Set a custom serializer. If not set, a default one is used.
    void setSerializer(ProjectSerializer* serializer) noexcept;

    /// Set a callback invoked after each auto-save attempt (success or failure).
    using StatusCallback = std::function<void(AutoSaveStatus, const std::string& message)>;
    void setStatusCallback(StatusCallback cb);

    // ── Tick (call periodically) ────────────────────────────────────────

    /// Check if it's time to auto-save and do it if needed.
    /// Call this from a timer (e.g. every second).
    /// Returns true if an auto-save was performed.
    bool tick();

    /// Force an immediate auto-save regardless of interval.
    /// Returns true on success.
    bool saveNow();

    // ── Auto-save folder management ────────────────────────────────────

    /// Get the auto-save folder path for a given project path.
    /// Returns parent_dir / "Roundtable Auto-Save" (or temp dir if unsaved).
    [[nodiscard]] static std::filesystem::path autoSaveFolder(
        const std::filesystem::path& projectPath);

    /// Get the auto-save folder for the current project.
    [[nodiscard]] std::filesystem::path currentAutoSaveFolder() const;

    /// Build a timestamped auto-save filename for the given project name.
    [[nodiscard]] static std::filesystem::path makeTimestampedPath(
        const std::filesystem::path& folder,
        const std::string& projectName);

    /// Check if the auto-save folder has a file newer than the project file.
    [[nodiscard]] static bool hasRecoverableAutoSave(
        const std::filesystem::path& projectPath);

    /// Find the newest auto-save file for a project.
    [[nodiscard]] static std::filesystem::path findNewestAutoSave(
        const std::filesystem::path& projectPath);

    /// Load a project from the newest auto-save in the folder.
    [[nodiscard]] static std::unique_ptr<Project> loadLatestAutoSave(
        const std::filesystem::path& projectPath);

    /// Prune old auto-saves, keeping at most maxKeep files.
    static void pruneAutoSaves(const std::filesystem::path& folder,
                               size_t maxKeep);

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] AutoSaveStats stats() const noexcept { return m_stats; }
    [[nodiscard]] AutoSaveStatus status() const noexcept { return m_stats.status; }

    /// Time until the next auto-save (0 if due now).
    [[nodiscard]] Duration timeUntilNextSave() const noexcept;

    /// Reset the timer (e.g. after a manual save).
    void resetTimer() noexcept;

private:
    bool doSave();

    Project*            m_project{nullptr};
    ProjectSerializer*  m_serializer{nullptr};
    bool                m_enabled{true};
    Duration            m_interval{std::chrono::minutes(15)};
    size_t              m_maxAutoSaves{20};
    TimePoint           m_lastSave{};
    AutoSaveStats       m_stats{};
    StatusCallback      m_statusCb;

    // Owns a default serializer if none is provided
    std::unique_ptr<ProjectSerializer> m_ownedSerializer;
};

} // namespace rt
