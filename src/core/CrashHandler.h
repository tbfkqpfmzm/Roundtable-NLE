/*
 * CrashHandler — Structured exception handling and crash dump writing.
 *
 * Step 29: Auto-save & Crash Recovery
 *
 * Platform-specific crash handling:
 *   - Windows: SetUnhandledExceptionFilter + MiniDumpWriteDump
 *   - Linux/macOS: signal handlers (SIGSEGV, SIGABRT, etc.)
 *
 * On crash:
 *   1. Write a minidump (Windows) or crash log
 *   2. Attempt an emergency auto-save of the current project
 *   3. Log crash info to a file for later analysis
 *
 * Usage:
 *   CrashHandler::install("%LOCALAPPDATA%/ROUNDTABLE/logs/");
 *   CrashHandler::setEmergencySaveCallback([&]{
 *       autoSave.saveNow();
 *   });
 *
 * The crash handler is a global singleton since OS exception handling
 * requires static function pointers.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace rt {

/// Information about a crash event.
struct CrashInfo
{
    std::string         summary;        ///< Brief crash description
    std::string         stackTrace;     ///< Best-effort backtrace
    uint32_t            exceptionCode{0};
    void*               exceptionAddress{nullptr};
    std::filesystem::path dumpFilePath; ///< Path to minidump if written
    std::filesystem::path logFilePath;  ///< Path to crash log
};

/// Global crash handler (singleton).
class CrashHandler
{
public:
    /// Install the crash handler. Call once at application startup.
    /// @param crashDir  Directory where crash dumps and logs are written.
    static void install(const std::filesystem::path& crashDir = "crash_logs");

    /// Uninstall the crash handler (restores default OS behavior).
    static void uninstall();

    /// Is the crash handler currently installed?
    [[nodiscard]] static bool isInstalled() noexcept;

    /// Set the directory for crash dumps and logs.
    static void setCrashDirectory(const std::filesystem::path& dir);
    [[nodiscard]] static const std::filesystem::path& crashDirectory() noexcept;

    /// Set an emergency save callback. Called during crash handling.
    /// MUST be async-signal-safe (no allocation, no locks, simple I/O only).
    /// In practice, we use SEH on Windows which is more forgiving.
    using EmergencySaveCallback = std::function<void()>;
    static void setEmergencySaveCallback(EmergencySaveCallback cb);

    /// Set a post-crash callback (called after dump is written, before exit).
    /// Can be used to show a crash dialog.
    using PostCrashCallback = std::function<void(const CrashInfo&)>;
    static void setPostCrashCallback(PostCrashCallback cb);

    /// Write a crash log entry (can also be called manually for non-fatal errors).
    static void writeCrashLog(const std::string& message);

    /// Get the path where the next crash dump would be written.
    [[nodiscard]] static std::filesystem::path nextDumpPath();

    /// Get the path to the crash log file.
    [[nodiscard]] static std::filesystem::path crashLogPath();

    /// Get information about the last crash (if any).
    /// Returns empty CrashInfo if no crash has occurred.
    [[nodiscard]] static CrashInfo lastCrash();

    /// Check if a crash log exists from a previous session.
    [[nodiscard]] static bool hasPreviousCrashLog();

    // ── Crash marker (Phase 7.A) ────────────────────────────────────────
    /// Write a crash marker file so the next launch can detect the crash.
    /// Written before generating the minidump in the crash handler.
    static void writeCrashMarker(const CrashInfo& info);

    /// Check if a crash marker exists from a previous session.
    [[nodiscard]] static bool hasCrashMarker();

    /// Read the crash marker and return the stored crash info.
    /// Returns empty CrashInfo if no marker or parse failure.
    [[nodiscard]] static CrashInfo readCrashMarker();

    /// Delete the crash marker (called after user dismisses recovery dialog).
    static void clearCrashMarker();

    /// Get the path to the crash marker file.
    [[nodiscard]] static std::filesystem::path crashMarkerPath();

private:
    CrashHandler() = delete;

    // Platform-specific implementation
    static void installPlatformHandler();
    static void uninstallPlatformHandler();
};

} // namespace rt
