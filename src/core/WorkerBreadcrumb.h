/*
 * WorkerBreadcrumb — per-thread "last phase reached" tag for crash forensics.
 *
 * Worker threads (prefetch / converter) update a thread-local pointer with
 * a string-literal tag at each major phase boundary.  When the SEH handler
 * fires it runs on the FAULTING thread and reads that thread's own slot,
 * so the tag in crash_log.txt actually identifies the crashing thread's
 * last step — instead of whatever a different thread happened to write
 * most recently.
 *
 * Why per-thread (was: a single process-wide std::atomic<const char*>):
 *   A process-wide breadcrumb is last-writer-wins across ALL threads, so
 *   a UI-thread crash during shutdown was being reported with the most
 *   recent prefetch-worker phase tag.  That mislabeled UI-side teardown
 *   AVs as worker bugs.  Per-thread storage fixes the misattribution:
 *   the UI thread (which never sets the breadcrumb) now reports "idle",
 *   correctly telling the reader "this is not a worker crash."
 *
 * The pointer targets string-literal storage (.rdata), so it never
 * dangles.  thread_local on a trivial type is zero-overhead after the
 * first access and safe to read from an SEH handler.
 *
 * Cost: one TLS store per phase (≈ a few ns on x86).  No formatting,
 * no allocation.  Cheap enough to leave on in Release.
 */

#pragma once

namespace rt {

/// Set by worker threads on each phase boundary in convertDecodedToCacheGpu
/// (and any other crash-prone worker code paths that want forensics).
/// Tag must be a string literal or statically-allocated string with
/// process-lifetime storage.  Per-thread.
void setLastWorkerStep(const char* tag) noexcept;

/// Read the most recently set tag for the CALLING thread.  Returns "idle"
/// if this thread never set one.  Safe to call from a crash handler —
/// the SEH dispatcher runs on the faulting thread, so this reads that
/// thread's own breadcrumb.
[[nodiscard]] const char* readLastWorkerStep() noexcept;

} // namespace rt
