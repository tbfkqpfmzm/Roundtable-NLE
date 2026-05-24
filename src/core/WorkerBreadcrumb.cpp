/*
 * WorkerBreadcrumb.cpp — see header.
 */

#include "WorkerBreadcrumb.h"

namespace rt {

namespace {
// Per-thread breadcrumb.  Trivial type with a constant initializer, so
// MSVC uses static TLS — initialized to "idle" on thread start, safe to
// read from SEH (no thread-local-init machinery to trip over).
thread_local const char* tl_step = "idle";
} // namespace

void setLastWorkerStep(const char* tag) noexcept
{
    tl_step = tag ? tag : "idle";
}

const char* readLastWorkerStep() noexcept
{
    const char* p = tl_step;
    return p ? p : "idle";
}

} // namespace rt
