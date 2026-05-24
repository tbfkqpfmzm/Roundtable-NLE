/*
 * WorkerBreadcrumb.cpp — see header.
 */

#include "WorkerBreadcrumb.h"

namespace rt {

namespace {
std::atomic<const char*>& g_lastWorkerStep()
{
    // Function-local static so the atomic is zero-initialised before any
    // worker call site can run (avoids the static-init-order trap of a
    // file-scope global).  "idle" is a string literal in .rdata.
    static std::atomic<const char*> s_step{"idle"};
    return s_step;
}
} // namespace

std::atomic<const char*>& lastWorkerStep() noexcept
{
    return g_lastWorkerStep();
}

const char* readLastWorkerStep() noexcept
{
    const char* p = g_lastWorkerStep().load(std::memory_order_relaxed);
    return p ? p : "idle";
}

} // namespace rt
