/*
 * WorkerBreadcrumb — global "last phase reached" tag for crash forensics.
 *
 * Worker threads (prefetch / converter) update a single atomic pointer
 * with a string-literal tag at each major phase boundary.  When the
 * crash handler fires, it reads the tag and writes it into crash_log.txt
 * so the dump's frame-pointer-only stack is anchored to a human-readable
 * step name — without needing PDB symbols.
 *
 * The pointer is std::atomic<const char*> over string-literal storage
 * (.rdata).  String literals live for the process lifetime, so we never
 * dereference a dangling pointer.  Last-writer-wins is acceptable: the
 * crashing worker is the only one that matters, and its write is the
 * most recent in the steady-state convert pipeline (~one update per
 * phase per frame, ~30-150 Hz).
 *
 * Cost: one relaxed atomic store per phase (≈ 1-2 ns on x86).  No
 * formatting, no allocation.  Cheap enough to leave on in Release.
 */

#pragma once

#include <atomic>

namespace rt {

/// Set by worker threads on each phase boundary in convertDecodedToCacheGpu
/// (and any other crash-prone worker code paths that want forensics).
/// String literal storage — never freed, safe to read from a crash handler.
[[nodiscard]] std::atomic<const char*>& lastWorkerStep() noexcept;

/// Convenience wrapper: relaxed store.  Tag must be a string literal or
/// statically-allocated string with process-lifetime storage.
inline void setLastWorkerStep(const char* tag) noexcept
{
    lastWorkerStep().store(tag, std::memory_order_relaxed);
}

/// Read the most recently set tag.  Returns "idle" before any worker
/// has set one.  Safe to call from a crash handler.
[[nodiscard]] const char* readLastWorkerStep() noexcept;

} // namespace rt
