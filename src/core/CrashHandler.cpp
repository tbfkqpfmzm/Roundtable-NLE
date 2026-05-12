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
#include <excpt.h>   // _resetstkoflw()
#include <stdlib.h>  // _set_purecall_handler, _set_invalid_parameter_handler
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

// ── Vectored Exception Handler (VEH) for stack overflow ──────────────────
// SetUnhandledExceptionFilter runs on the faulting thread's remaining stack.
// When the fault is STACK_OVERFLOW, there is practically NO stack left —
// even the SEH handler cannot run.  A VEH fires before that, while the
// guard page is still present, giving us a chance to call _resetstkoflw()
// and restore a usable stack BEFORE the SEH handler is invoked.
//
// Hooking this as a VEH (first-chance) ensures _resetstkoflw() runs on
// the stack-overflowed thread with just enough stack to make the call.
// After reset, the exception continues to the SEH handler (which now has
// a usable stack) and everything works normally.
static LONG WINAPI stackOverflowVectoredHandler(EXCEPTION_POINTERS* exInfo)
{
    if (exInfo && exInfo->ExceptionRecord &&
        exInfo->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        // Reset the stack — this must be called on the faulting thread
        // while the guard page is still accessible.
        _resetstkoflw();
        // Continue searching for the next handler (the SEH filter).
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI crashExceptionFilter(EXCEPTION_POINTERS* exInfo)
{
    auto& s = state();

    // ── STACK_OVERFLOW recovery ──────────────────────────────────────
    // The VEH above should have reset the stack already, but as a safety
    // net, call _resetstkoflw() again here if this is a stack overflow.
    // This ensures the crash handler has a usable stack even if the VEH
    // wasn't installed or didn't fire for some reason.
    if (exInfo && exInfo->ExceptionRecord &&
        exInfo->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        _resetstkoflw();
    }

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

    // ── Crash marker (Phase 7.A) ──────────────────────────────────────
    // Write a marker file so the next launch can detect this crash and
    // offer recovery options (reset dock layout, workspace, etc.).
    sehCall([&] { CrashHandler::writeCrashMarker(info); });

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

    // Crash marker (Phase 7.A)
    try { CrashHandler::writeCrashMarker(ci); } catch (...) {}

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

    // ── Non-SEH crash handlers ─────────────────────────────────────────
    // These catch termination paths that bypass SetUnhandledExceptionFilter.

    // 1. Pure virtual function call (e.g. calling virtual methods on a
    //    partially-destroyed object).  The default handler calls abort()
    //    which terminates without SEH, leaving NO crash log.
    //    Use raw Win32 calls to avoid ANY heap/stack allocation.
#ifdef _WIN32
    _set_purecall_handler([]() {
        auto logPath = crashLogPath();
        std::string msg = "PURE VIRTUAL FUNCTION CALL — likely use-after-free "
                          "on a partially-destroyed object";
        appendCrashLog(logPath, msg);
        // Write marker using raw Win32 (avoid std::ofstream which allocates)
        auto markerPath = logPath.parent_path() / "crash_marker.txt";
        appendCrashLogRaw(markerPath, "summary=" + msg + "\n");
        // Terminate immediately — nothing safe to do after a purecall
        TerminateProcess(GetCurrentProcess(), 0xC0000005);
    });

    // 2. CRT invalid parameter handler (catches assertions from CRT
    //    functions like isdigit() or fgets() with invalid args).
    _set_invalid_parameter_handler([](
        const wchar_t* expr, const wchar_t* func,
        const wchar_t* file, unsigned int line, uintptr_t /*reserved*/)
    {
        // Convert wchar_t to narrow using Win32 API (stack-safe, no alloc)
        auto toNarrow = [](const wchar_t* ws) -> std::string {
            if (!ws || !*ws) return {};
            int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0,
                                           nullptr, nullptr);
            if (len <= 0) return {};
            std::string result(static_cast<size_t>(len) - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ws, -1, result.data(), len,
                                nullptr, nullptr);
            return result;
        };
        std::ostringstream oss;
        oss << "CRT INVALID PARAMETER:";
        if (expr) oss << " expr=" << toNarrow(expr);
        if (func) oss << " func=" << toNarrow(func);
        if (file) oss << " file=" << toNarrow(file) << ":" << line;
        appendCrashLog(crashLogPath(), oss.str());
    });
#endif

    // 3. std::terminate handler (catches unhandled C++ exceptions that
    //    escape main(), or exceptions thrown during stack unwinding).
    std::set_terminate([]() {
        appendCrashLog(crashLogPath(),
            "std::terminate called — unhandled C++ exception or "
            "exception during stack unwinding");
        // Attempt to re-throw and capture the active exception's type
        try {
            throw;
        } catch (const std::exception& e) {
            appendCrashLog(crashLogPath(),
                std::string("  Active exception: ") + e.what());
        } catch (...) {
            appendCrashLog(crashLogPath(),
                "  Active exception: unknown type (not std::exception)");
        }
        // Re-abort so the OS can generate a crash dump
        abort();
    });

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

// ── Crash marker (Phase 7.A) ────────────────────────────────────────────────

std::filesystem::path CrashHandler::crashMarkerPath()
{
    return state().crashDir / "crash_marker.txt";
}

void CrashHandler::writeCrashMarker(const CrashInfo& info)
{
    try {
        auto path = crashMarkerPath();
        std::ofstream marker(path);
        if (!marker.is_open()) return;

        marker << "crash_time=" << timestamp() << "\n";
        marker << "exception=0x" << std::hex << info.exceptionCode << "\n";
        marker << "address=0x" << std::hex
               << reinterpret_cast<uintptr_t>(info.exceptionAddress) << "\n";
        marker << "summary=" << info.summary << "\n";
        marker << "dump=" << info.dumpFilePath.string() << "\n";
        marker << "log=" << info.logFilePath.string() << "\n";
        marker.close();
    } catch (...) {
        // Best-effort — marker is non-critical
    }
}

bool CrashHandler::hasCrashMarker()
{
    return std::filesystem::exists(crashMarkerPath());
}

CrashInfo CrashHandler::readCrashMarker()
{
    CrashInfo info;
    try {
        auto path = crashMarkerPath();
        if (!std::filesystem::exists(path)) return info;

        std::ifstream marker(path);
        if (!marker.is_open()) return info;

        std::string line;
        while (std::getline(marker, line)) {
            if (line.rfind("summary=", 0) == 0)
                info.summary = line.substr(8);
            else if (line.rfind("exception=0x", 0) == 0)
                info.exceptionCode =
                    static_cast<uint32_t>(std::stoul(line.substr(11), nullptr, 16));
            else if (line.rfind("dump=", 0) == 0)
                info.dumpFilePath = line.substr(5);
            else if (line.rfind("log=", 0) == 0)
                info.logFilePath = line.substr(4);
        }
        marker.close();
    } catch (...) {
    }
    return info;
}

void CrashHandler::clearCrashMarker()
{
    std::error_code ec;
    std::filesystem::remove(crashMarkerPath(), ec);
}

void CrashHandler::installPlatformHandler()
{
#ifdef _WIN32
    // Install VEH first (catches stack overflow before the stack is gone)
    // so _resetstkoflw() can be called to restore a usable stack before
    // the SEH filter runs.  The handler function is a free function so
    // MSVC won't complain about mixing SEH with C++ EH.
    AddVectoredExceptionHandler(/*FirstHandler=*/1, stackOverflowVectoredHandler);

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
