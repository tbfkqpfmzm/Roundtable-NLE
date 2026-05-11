/*
 * CrashHandler implementation — Windows SEH + MiniDump.
 */

#include "CrashHandler.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace rt {

// ── Global state (singleton-style) ──────────────────────────────────────────

namespace {

struct CrashHandlerState
{
    bool                                installed{false};
    std::filesystem::path               crashDir;
    CrashHandler::EmergencySaveCallback emergencySave;
    CrashHandler::PostCrashCallback     postCrash;
    CrashInfo                           lastCrashInfo;
    std::mutex                          mu;

#ifdef _WIN32
    LPTOP_LEVEL_EXCEPTION_FILTER        previousFilter{nullptr};
#endif
};

CrashHandlerState& state()
{
    static CrashHandlerState s;
    return s;
}

// ── Timestamp helper ────────────────────────────────────────────────────────

std::string formatTimestamp(const char* fmt)
{
    auto             now = std::chrono::system_clock::now();
    std::time_t      t   = std::chrono::system_clock::to_time_t(now);
    std::tm          tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, fmt);
    return oss.str();
}

std::string timestamp()  { return formatTimestamp("%Y%m%d_%H%M%S"); }
std::string timestampHuman() { return formatTimestamp("%Y-%m-%d %H:%M:%S"); }

// ── Crash log writer ────────────────────────────────────────────────────────

/// Low-level crash log append using Win32 WriteFile.
/// Avoids std::ofstream heap allocations which can fail in a corrupted heap.
void appendCrashLogRaw(const std::filesystem::path& logPath, const std::string& msg)
{
    HANDLE hFile = CreateFileW(
        logPath.wstring().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(hFile, 0, nullptr, FILE_END);
    DWORD written = 0;
    WriteFile(hFile, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);
}

/// Build a timestamped crash log line and write it via the low-level path.
void appendCrashLog(const std::filesystem::path& logPath, const std::string& msg)
{
    try {
        std::filesystem::create_directories(logPath.parent_path());
        std::string line = "[" + timestampHuman() + "] " + msg + "\n";
        appendCrashLogRaw(logPath, line);
    } catch (...) {
        // Best effort — we're in a crash handler, can't throw.
    }
}

// ── Platform-specific crash handlers ────────────────────────────────────────

#ifdef _WIN32

std::string getModuleNameFromAddress(void* addr)
{
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr), &hMod)) {
        char modPath[MAX_PATH] = {};
        if (GetModuleFileNameA(hMod, modPath, MAX_PATH)) {
            std::string fullPath(modPath);
            auto pos = fullPath.find_last_of("\\/");
            return (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
        }
    }
    return "unknown";
}

std::string exceptionCodeToString(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
    default:
    {
        std::ostringstream oss;
        oss << "UNKNOWN(0x" << std::hex << code << ")";
        return oss.str();
    }
    }
}

bool writeMiniDump(const std::filesystem::path& dumpPath,
                   EXCEPTION_POINTERS*          exInfo)
{
    HANDLE hFile = CreateFileW(
        dumpPath.wstring().c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId          = GetCurrentThreadId();
    mdei.ExceptionPointers = exInfo;
    mdei.ClientPointers    = FALSE;

    BOOL result = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        MiniDumpWithDataSegs,
        exInfo ? &mdei : nullptr,
        nullptr,
        nullptr);

    CloseHandle(hFile);
    return result != FALSE;
}

/// Call a noexcept-ish callback inside SEH __try/__except so that an AV
/// inside the callback doesn't kill the crash handler.  This is a free
/// function (no C++ try/catch) so MSVC won't complain about mixing SEH
/// with C++ EH in the caller.
template <typename Fn>
inline void sehCall(Fn&& fn)
{
    __try { fn(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

LONG WINAPI crashExceptionFilter(EXCEPTION_POINTERS* exInfo)
{
    auto& s = state();

    // Build crash info
    CrashInfo info;
    if (exInfo && exInfo->ExceptionRecord) {
        info.exceptionCode    = exInfo->ExceptionRecord->ExceptionCode;
        info.exceptionAddress = exInfo->ExceptionRecord->ExceptionAddress;

        // Identify the module containing the crash address
        std::string moduleName = getModuleNameFromAddress(info.exceptionAddress);

        HMODULE hMod = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(info.exceptionAddress), &hMod);

        auto addrOffset = reinterpret_cast<uintptr_t>(info.exceptionAddress)
                        - reinterpret_cast<uintptr_t>(hMod);

        info.summary = "Unhandled exception: " +
                       exceptionCodeToString(info.exceptionCode) +
                       " at address " + [&]{
                           std::ostringstream oss;
                           oss << "0x" << std::hex
                               << reinterpret_cast<uintptr_t>(info.exceptionAddress)
                               << " in " << moduleName
                               << " (offset 0x" << addrOffset << ")";
                           return oss.str();
                       }();
    } else {
        info.summary = "Unhandled exception (no exception record available)";
    }

    // ── Prepare paths ─────────────────────────────────────────────────────
    std::filesystem::path dumpDir = s.crashDir;
    try {
        std::filesystem::create_directories(dumpDir);
    } catch (...) {
    }
    auto logPath = dumpDir / "crash_log.txt";
    info.logFilePath = logPath;

    // 1. Attempt emergency auto-save (SEH-protected via helper)
    if (s.emergencySave) {
        sehCall([&] { s.emergencySave(); });
    }

    // 2. Write crash log FIRST (before minidump — if MiniDumpWriteDump
    //    crashes with a nested exception the process dies immediately).
    appendCrashLog(logPath, info.summary);

    // 3. Write minidump (SEH-protected via helper)
    auto dumpPath = dumpDir / ("crash_" + timestamp() + ".dmp");
    {
        bool dumpOk = false;
        sehCall([&] { dumpOk = writeMiniDump(dumpPath, exInfo); });
        if (dumpOk)
            info.dumpFilePath = dumpPath;
    }
    if (!info.dumpFilePath.empty()) {
        appendCrashLog(logPath, "  Dump written: " + info.dumpFilePath.string());
    }

    // 4. Capture and write stack trace (SEH-protected via helper)
    {
        sehCall([&] { SymInitialize(GetCurrentProcess(), NULL, TRUE); });

        void* stack[64] = {};
        WORD frames = CaptureStackBackTrace(0, 64, stack, NULL);

        for (WORD i = 0; i < frames; ++i) {
            DWORD64 addr = (DWORD64)(stack[i]);
            std::string mn = getModuleNameFromAddress(stack[i]);

            HMODULE hm = nullptr;
            GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)stack[i], &hm);

            std::string symName;
            sehCall([&] {
                struct SymbolInfo {
                    SYMBOL_INFO base;
                    char name[256];
                };
                SymbolInfo sym{};
                sym.base.SizeOfStruct = sizeof(SYMBOL_INFO);
                sym.base.MaxNameLen   = 255;
                if (SymFromAddr(GetCurrentProcess(), addr, NULL, &sym.base))
                    symName = sym.base.Name;
            });

            std::ostringstream oss;
            oss << "  [" << i << "] 0x" << std::hex << addr;
            if (!symName.empty())
                oss << " " << symName;
            if (!mn.empty() && hm)
                oss << " (" << mn << "+0x" << std::hex << (addr - (DWORD64)hm) << ")";
            appendCrashLog(logPath, oss.str());
        }
        sehCall([&] { SymCleanup(GetCurrentProcess()); });
    }

    s.lastCrashInfo = info;

    // 5. Post-crash callback (SEH-protected via helper)
    if (s.postCrash) {
        sehCall([&] { s.postCrash(info); });
    }

    // Terminate immediately — we already wrote our own dump and log.
    // Handing to WER via EXCEPTION_CONTINUE_SEARCH keeps the exe locked
    // for an indeterminate time, blocking rebuilds.
    TerminateProcess(GetCurrentProcess(), exInfo->ExceptionRecord->ExceptionCode);

    // Unreachable, but keep the fallback for safety.
    return EXCEPTION_CONTINUE_SEARCH;
}

#else
// POSIX stub — signals
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <execinfo.h>

namespace {
struct sigaction s_oldSigsegv, s_oldSigabrt, s_oldSigfpe, s_oldSigbus;

void crashSignalHandler(int sig, siginfo_t* info, void* /*context*/)
{
    auto& s = state();

    CrashInfo ci;
    ci.summary = std::string("Signal ") + strsignal(sig);
    if (info) {
        ci.exceptionAddress = info->si_addr;
    }

    // Emergency save
    if (s.emergencySave) {
        try { s.emergencySave(); } catch (...) {}
    }

    // Collect backtrace
    void* frames[64];
    int   nFrames = backtrace(frames, 64);
    char** syms   = backtrace_symbols(frames, nFrames);
    if (syms) {
        std::ostringstream oss;
        for (int i = 0; i < nFrames; ++i)
            oss << syms[i] << "\n";
        ci.stackTrace = oss.str();
        free(syms);
    }

    // Write crash log
    auto logPath = s.crashDir / "crash_log.txt";
    ci.logFilePath = logPath;
    appendCrashLog(logPath, ci.summary);
    if (!ci.stackTrace.empty())
        appendCrashLog(logPath, ci.stackTrace);

    s.lastCrashInfo = ci;

    if (s.postCrash) {
        try { s.postCrash(ci); } catch (...) {}
    }

    // Re-raise to get default behavior (core dump, etc.)
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, nullptr);
    raise(sig);
}
} // anon
#endif

} // anonymous namespace

// ── Public API ──────────────────────────────────────────────────────────────

void CrashHandler::install(const std::filesystem::path& crashDir)
{
    auto& s = state();
    std::lock_guard lock(s.mu);

    if (s.installed) {
        spdlog::warn("CrashHandler::install() — already installed");
        return;
    }

    s.crashDir = crashDir;

    try {
        std::filesystem::create_directories(crashDir);
    } catch (const std::exception& e) {
        spdlog::error("CrashHandler: failed to create crash dir '{}': {}",
                       crashDir.string(), e.what());
    }

    installPlatformHandler();
    s.installed = true;

    spdlog::info("CrashHandler installed — crash dir: {}", crashDir.string());
}

void CrashHandler::uninstall()
{
    auto& s = state();
    std::lock_guard lock(s.mu);

    if (!s.installed) return;

    uninstallPlatformHandler();
    s.installed = false;

    spdlog::info("CrashHandler uninstalled");
}

bool CrashHandler::isInstalled() noexcept
{
    return state().installed;
}

void CrashHandler::setCrashDirectory(const std::filesystem::path& dir)
{
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.crashDir = dir;
    try {
        std::filesystem::create_directories(dir);
    } catch (...) {
    }
}

const std::filesystem::path& CrashHandler::crashDirectory() noexcept
{
    return state().crashDir;
}

void CrashHandler::setEmergencySaveCallback(EmergencySaveCallback cb)
{
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.emergencySave = std::move(cb);
}

void CrashHandler::setPostCrashCallback(PostCrashCallback cb)
{
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.postCrash = std::move(cb);
}

void CrashHandler::writeCrashLog(const std::string& message)
{
    auto& s = state();
    appendCrashLog(s.crashDir / "crash_log.txt", message);
}

std::filesystem::path CrashHandler::nextDumpPath()
{
    auto& s = state();
    return s.crashDir / ("crash_" + timestamp() + ".dmp");
}

std::filesystem::path CrashHandler::crashLogPath()
{
    return state().crashDir / "crash_log.txt";
}

CrashInfo CrashHandler::lastCrash()
{
    auto& s = state();
    std::lock_guard lock(s.mu);
    return s.lastCrashInfo;
}

bool CrashHandler::hasPreviousCrashLog()
{
    auto logPath = crashLogPath();
    return std::filesystem::exists(logPath) &&
           std::filesystem::file_size(logPath) > 0;
}

// ── Platform handler install / uninstall ────────────────────────────────────

void CrashHandler::installPlatformHandler()
{
#ifdef _WIN32
    state().previousFilter = SetUnhandledExceptionFilter(crashExceptionFilter);
#else
    struct sigaction sa{};
    sa.sa_sigaction = crashSignalHandler;
    sa.sa_flags     = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &s_oldSigsegv);
    sigaction(SIGABRT, &sa, &s_oldSigabrt);
    sigaction(SIGFPE,  &sa, &s_oldSigfpe);
    sigaction(SIGBUS,  &sa, &s_oldSigbus);
#endif
}

void CrashHandler::uninstallPlatformHandler()
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(state().previousFilter);
    state().previousFilter = nullptr;
#else
    sigaction(SIGSEGV, &s_oldSigsegv, nullptr);
    sigaction(SIGABRT, &s_oldSigabrt, nullptr);
    sigaction(SIGFPE,  &s_oldSigfpe,  nullptr);
    sigaction(SIGBUS,  &s_oldSigbus,  nullptr);
#endif
}

} // namespace rt
