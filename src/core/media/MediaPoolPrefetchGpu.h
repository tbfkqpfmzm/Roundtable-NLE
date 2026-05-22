/*
 * MediaPoolPrefetchGpu.h — Vulkan-aware helpers for the GPU-resident
 * prefetch decode path (UPGRADE_PLAN Phase 4).
 *
 * Kept in its own header so the broader prefetch translation units
 * (MediaPoolPrefetchSchedule.cpp, MediaPoolPrefetchDecode.cpp, etc.)
 * are not forced to pull in Vulkan headers — only the two TUs that
 * actually touch GPU state (MediaPoolPrefetchSchedule.cpp for per-worker
 * lifecycle, MediaPoolPrefetchConvertGpu.cpp for the convert + copy
 * pipeline) include this.
 */

#pragma once

#include "vulkan/CommandPool.h"
#include "vulkan/Texture.h"   // Texture::StagingCleanup

#include <vulkan/vulkan_core.h>
#include <deque>
#include <memory>
#include <vector>

namespace rt {

// Forward decls so the helper signature compiles without including
// MediaPool.h here.
class  MediaPool;
struct PrefetchDecoderState;
struct PrefetchTask;
struct DecodedFrame;
struct CachedFrame;

/// Per-prefetch-worker Vulkan resources.  Allocated on the worker stack
/// in MediaPool::prefetchWorker; destroyed when the worker exits.
///
/// cmdPool: spec-correct per-worker VkCommandPool, addresses the May
///   2026 crash documented in UPGRADE_PLAN section C.
/// signalSem: reserved for UPGRADE_PLAN Phase 6 — the compositor-bound
///   inter-queue signal.  Created here so the lifecycle is wired up,
///   not used until PR-5 lands and the flag flips.
/// device: stored solely so the destructor can call vkDestroySemaphore;
///   the cmdPool already remembers its own VkDevice.
struct WorkerGpuState {
    rt::CommandPool cmdPool;
    VkSemaphore     signalSem{VK_NULL_HANDLE};
    VkDevice        device{VK_NULL_HANDLE};

    /// Deferred-cleanup ring (UPGRADE_PLAN replacement for the Phase 4
    /// inline vkWaitForFences). Each entry holds the per-submission
    /// resources that can only be freed once the GPU has finished
    /// executing the submission. pollAndCleanup() walks the deque in
    /// submit-order; the destructor drains anything still in flight.
    ///
    /// Why this works without cross-queue semaphores: the prefetch
    /// worker and the compositor BOTH submit on GpuQueueKind::Compute
    /// via GpuScheduler. Vulkan guarantees in-order execution within a
    /// single VkQueue, so the convert+copy submission is guaranteed to
    /// finish before any later compositor submission that samples the
    /// destination texture — without anyone calling vkWaitForFences.
    /// The fence here is purely for CPU-side staging-buffer cleanup.
    struct PendingSubmit {
        VkFence                              fence{VK_NULL_HANDLE};
        VkCommandBuffer                      cmdBuf{VK_NULL_HANDLE};
        std::vector<Texture::StagingCleanup> staging;
    };
    std::deque<PendingSubmit> pending;

    WorkerGpuState() = default;
    ~WorkerGpuState();

    WorkerGpuState(const WorkerGpuState&) = delete;
    WorkerGpuState& operator=(const WorkerGpuState&) = delete;

    /// True when both cmdPool and signalSem are valid — i.e. the worker
    /// can take the GPU-resident decode path.
    [[nodiscard]] bool ready() const noexcept {
        return cmdPool.handle() != VK_NULL_HANDLE
            && signalSem != VK_NULL_HANDLE;
    }

    /// Non-blocking sweep: free resources for any pending submission
    /// whose fence has signalled. Called at the start of every
    /// convertDecodedToCacheGpu so the deque drains at the natural
    /// rate of subsequent prefetch work. O(N) in completed entries
    /// only; stops at the first not-yet-signalled fence (submit
    /// order = signal order).
    void pollAndCleanup();
};

/// Dispatch helper: wraps the eligibility checks (PR-4 feature flag,
/// wgs validity) around MediaPool::convertDecodedToCacheGpu so callers in
/// translation units that include this header do not need to depend on
/// CompositeService.h or on the WorkerGpuState methods directly.
///
/// Returns nullptr on any ineligibility OR on GPU-path failure;
/// callers must fall back to the CPU convertDecodedToCache path.
std::shared_ptr<CachedFrame> tryConvertDecodedToCacheGpu(
    MediaPool&            pool,
    PrefetchDecoderState& state,
    const PrefetchTask&   task,
    DecodedFrame&         decoded,
    int64_t               frameNumber,
    WorkerGpuState*       wgs);

} // namespace rt
