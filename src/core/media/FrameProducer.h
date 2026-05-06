/*
 * FrameProducer — Asynchronous compositor wrapper.
 *
 * Runs on a dedicated thread.  Receives "produce frame" requests
 * from FrameClock (via requestFrame) and calls the compositor callback
 * on its own thread — keeping the TIME_CRITICAL clock thread unblocked.
 * Finished frames are published to a lock-free exchange slot for
 * FramePresenter to consume.
 *
 * Key improvements over the old monolithic renderThreadLoop:
 *   - Completely decoupled from both clock and presentation.
 *   - Holds the last good frame and republishes it on cache miss,
 *     so the presenter always has something to display.
 *   - Supports scrub/seek (paused) requests via a separate path.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace rt {

class PlaybackController;
struct CachedFrame;

class FrameProducer
{
public:
    /// Compositor function signature
    /// (wraps TimelineWorkspace::compositeFrame)
    using CompositeCallback = std::function<
        std::shared_ptr<CachedFrame>(int64_t tick, uint32_t w, uint32_t h, bool scrub)>;

    FrameProducer();
    ~FrameProducer();

    FrameProducer(const FrameProducer&) = delete;
    FrameProducer& operator=(const FrameProducer&) = delete;

    // ── Configuration ────────────────────────────────────────────────

    void setCompositeCallback(CompositeCallback cb) { m_compositeCB = std::move(cb); }
    void setController(PlaybackController* c) { m_controller = c; }

    /// Thread-safe resolution update.
    void setOutputResolution(uint32_t w, uint32_t h, int divisor = 1);

    // ── Lifecycle ────────────────────────────────────────────────────

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept
    { return m_running.load(std::memory_order_relaxed); }

    // ── Frame production ─────────────────────────────────────────────

    /// Queue a frame request for the given tick (called from FrameClock).
    /// Non-blocking: pushes to request queue and returns immediately.
    void requestFrame(int64_t tick);

    /// Queue a scrub/seek request for the producer thread (paused mode).
    /// Non-blocking: pushes to the scrub request slot and returns
    /// immediately.  The producer thread picks it up and runs the
    /// compositor off the UI thread.
    void requestScrubFrame(int64_t tick, uint32_t w, uint32_t h, bool scrub);

    // ── Frame exchange (consumed by FramePresenter) ──────────────────

    struct FrameExchange {
        std::shared_ptr<CachedFrame> frame;
        int64_t tick{0};
        bool    hasNew{false};
    };

    /// Lock the exchange and read the latest frame.
    /// Returns true if a new frame was available.
    bool consumeFrame(std::shared_ptr<CachedFrame>& outFrame, int64_t& outTick);

    /// Wait for a new frame (for paused mode or presenter thread).
    /// Returns false if timed out.
    bool waitForFrame(std::chrono::milliseconds timeout);

    /// Notify waiters (wake presenter thread).
    void notifyPresenter() { m_exchangeCV.notify_one(); }

    // ── Queries ──────────────────────────────────────────────────────

    [[nodiscard]] std::shared_ptr<CachedFrame> lastProducedFrame() const;

private:
    struct ScrubRequest {
        int64_t  tick;
        uint32_t w, h;
        bool     scrub;
    };

    void producerLoop();
    void produceFrameImpl(int64_t tick);
    void produceScrubFrameImpl(const ScrubRequest& req);
    void publishFrame(std::shared_ptr<CachedFrame> frame, int64_t tick);

    CompositeCallback m_compositeCB;
    PlaybackController* m_controller{nullptr};

    // Resolution (atomics for lock-free cross-thread access)
    std::atomic<uint32_t> m_outputW{1920};
    std::atomic<uint32_t> m_outputH{1080};
    std::atomic<int>      m_resDivisor{2};

    // Worker thread
    std::thread       m_thread;
    std::atomic<bool> m_running{false};

    // Request queue (FrameClock → producer thread)
    std::mutex              m_reqMtx;
    std::condition_variable m_reqCV;
    std::deque<int64_t>     m_pendingTicks;
    std::optional<ScrubRequest> m_pendingScrub;  // latest scrub request (replaces prior)

    // Frame exchange slot (producer → presenter)
    mutable std::mutex       m_exchangeMtx;
    FrameExchange            m_exchange;
    std::condition_variable  m_exchangeCV;

    // Last good frame (for re-publish on cache miss)
    std::shared_ptr<CachedFrame> m_lastGoodFrame;
};

} // namespace rt
