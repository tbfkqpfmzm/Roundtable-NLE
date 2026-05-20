/*
 * HardwareDiagnostics — Detect injected overlay DLLs and classify the
 * installed GPU at startup so the app can warn users about known
 * problematic configurations before they hit a hard-to-diagnose crash.
 *
 * Two things this module addresses:
 *
 *   1. Injected overlay hooks (NVIDIA GeForce Experience / ShadowPlay,
 *      OBS Vulkan capture, Discord overlay, RTSS).  These hook our
 *      Vulkan calls and have been observed raising SEH exceptions
 *      (0x000006BA — RPC_S_SERVER_UNAVAILABLE; 0xC0000005 — ACCESS_VIOLATION)
 *      that kill the export pipeline.  We can't safely uninject these
 *      DLLs, but we can detect them and tell the user how to add an
 *      exclusion in the relevant app.
 *
 *   2. GPU architecture classification.  NVIDIA Pascal (GTX 10xx) has a
 *      hardware 2-concurrent-NVENC-session cap on consumer SKUs.  If
 *      the user also has Discord/OBS running, our export's NVENC init
 *      will fail and we'll silently fall back to CPU encoding.  Knowing
 *      this at startup lets us pre-warn and pre-flight.
 *
 * Everything here is best-effort and side-effect free unless explicitly
 * requested.  Calls into the OS use stack buffers only — safe to invoke
 * before Qt is initialised.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rt::HardwareDiagnostics {

/// One known third-party DLL detected in our process.
struct InjectedHook
{
    std::string moduleBaseName;   ///< e.g. "nvspcap64.dll"
    std::string fullPath;         ///< Full path on disk
    std::string sourceApp;        ///< Human-readable app: "NVIDIA GeForce Experience overlay"
    std::string exclusionHint;    ///< Instructions on how to exclude
};

/// Classification of the primary GPU.  Driver-version-dependent details
/// (max NVENC sessions, NVDEC concurrency) are folded in as best-effort.
enum class GpuArchitecture
{
    Unknown,
    NvidiaMaxwell,   ///< GTX 9xx — compute capability 5.x
    NvidiaPascal,    ///< GTX 10xx — compute capability 6.x — 2-session NVENC cap
    NvidiaVolta,     ///< Titan V — compute capability 7.0
    NvidiaTuring,    ///< GTX 16xx / RTX 20xx — compute capability 7.5
    NvidiaAmpere,    ///< RTX 30xx — compute capability 8.x
    NvidiaAda,       ///< RTX 40xx — compute capability 8.9
    NvidiaBlackwell, ///< RTX 50xx
    AmdRadeon,
    IntelArc,
    IntelIntegrated,
    Other,
};

struct GpuClassification
{
    GpuArchitecture arch{GpuArchitecture::Unknown};
    std::string     deviceName;
    uint32_t        vendorId{0};
    uint32_t        deviceId{0};
    uint64_t        vramBytes{0};
    /// True if this GPU is known to have a strict concurrent NVENC
    /// session cap that user-facing apps regularly trip (Pascal consumer
    /// SKUs == 2 sessions; pre-driver-550 only).
    bool            hasStrictNvencSessionCap{false};
};

/// Scan the current process for known injected overlay DLLs.  Safe to
/// call any time; cheap (single EnumProcessModules pass).
std::vector<InjectedHook> scanLoadedHooks();

/// Classify a Vulkan-reported GPU (from rt::GPUInfo) into an
/// architecture family + capability hints.  Pure function — no Vulkan
/// dependency in the header.
GpuClassification classifyGpu(uint32_t vendorId,
                              uint32_t deviceId,
                              const std::string& deviceName,
                              uint64_t vramBytes);

/// Convenience: log the GPU and any hooks at warn-level via spdlog.
/// Call once at app startup right after the Vulkan device is created.
void logAtStartup(const GpuClassification& gpu,
                  const std::vector<InjectedHook>& hooks);

} // namespace rt::HardwareDiagnostics
