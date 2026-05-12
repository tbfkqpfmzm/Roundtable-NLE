/*
 * ShutdownPhases.h — Ordered shutdown phase coordinator.
 *
 * Replaces ad-hoc teardown with a defined phase system so no component
 * is destroyed while another still references it.
 *
 * Usage:
 *   auto& sm = ShutdownManager::instance();
 *   sm.advanceTo(ShutdownPhase::Phase1_StopThreads);
 *   // ... stop threads ...
 *   sm.advanceTo(ShutdownPhase::Phase2_Disconnect);
 *   // ... disconnect signals ...
 *
 * Thread-safe: all methods are lock-free atomic reads/writes.
 */

#pragma once

#include <atomic>

namespace rt {

enum class ShutdownPhase : int {
    Running = 0,            // Normal operation
    Phase1_StopThreads,     // Stop background threads (FrameProducer, audio, prefetch)
    Phase2_Disconnect,      // Disconnect Qt signals, null back-pointers
    Phase3_DestroyQt,       // Destroy QWidget tree
    Phase4_DestroyGpu,      // Destroy GPU resources (VkDevice, VMA)
    Phase5_Done             // Complete
};

/// Global shutdown coordinator — singleton.
class ShutdownManager {
public:
    static ShutdownManager& instance();

    /// True if any shutdown phase has been entered.
    bool isShuttingDown() const {
        return m_phase.load() != ShutdownPhase::Running;
    }

    /// Current shutdown phase.
    ShutdownPhase currentPhase() const {
        return m_phase.load(std::memory_order_acquire);
    }

    /// True if we have passed (or are at) the given phase.
    bool havePassed(ShutdownPhase phase) const {
        return static_cast<int>(currentPhase()) >= static_cast<int>(phase);
    }

    /// Advance to next phase. Returns false if already past that phase.
    /// Idempotent — advancing to the same phase twice is a no-op.
    bool advanceTo(ShutdownPhase phase) {
        ShutdownPhase cur = m_phase.load(std::memory_order_relaxed);
        if (static_cast<int>(cur) >= static_cast<int>(phase))
            return false;
        m_phase.store(phase, std::memory_order_release);
        return true;
    }

private:
    ShutdownManager() = default;
    ~ShutdownManager() = default;
    ShutdownManager(const ShutdownManager&) = delete;
    ShutdownManager& operator=(const ShutdownManager&) = delete;

    std::atomic<ShutdownPhase> m_phase{ShutdownPhase::Running};
};

inline ShutdownManager& ShutdownManager::instance()
{
    static ShutdownManager s_instance;
    return s_instance;
}

} // namespace rt
