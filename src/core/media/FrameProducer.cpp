/*
 * FrameProducer.cpp — See FrameProducer.h for architecture.
 */

#include "media/FrameProducer.h"
#include "media/FrameCache.h"  // CachedFrame
#include "media/PlaybackController.h"
#include "GpuContext.h"

#include <spdlog/spdlog.h>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

FrameProducer::FrameProducer() = default;

FrameProducer::~FrameProducer()
{
    stop();
}

void FrameProducer::setOutputResolution(uint32_t w, uint32_t h, int divisor)
{
    m_outputW.store(w, std::memory_order_relaxed);
    m_outputH.store(h, std::memory_order_relaxed);
    m_resDivisor.store(divisor, std::memory_order_relaxed);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::start()
{
    if (m_running.load()) return;
    m_running.store(true);
    m_destroying.store(false);
    m_thread = std::thread(&FrameProducer::producerLoop, this);
    spdlog::info("[FrameProducer] Started");
}

void FrameProducer::stop()
{
    if (!m_running.load()) return;
    spdlog::info("[FP-TRACE] FrameProducer::stop() called (running=true, destroying={})",
                 m_destroying.load());
    m_destroying.store(true);
    m_running.store(false);
    m_reqCV.notify_one();       // wake worker if blocked
    m_exchangeCV.notify_one();  // wake presenter if blocked
    if (m_thread.joinable()) m_thread.join();
    spdlog::info("[FrameProducer] Stopped");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Request queue (FrameClock → producer thread)
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::requestFrame(int64_t tick)
{
    {
        std::lock_guard lock(m_reqMtx);
        // Coalesce: a non-empty queue means the producer hasn't picked up
        // the previous request yet, so we're a frame behind.  Replace the
        // back entry (always work on the latest tick) and count the
        // replacement.  When we've replaced kBackpressureLagFrames times
        // in a row without the producer dequeuing, set backpressure so
        // FrameClock skips the next tick instead of piling more on.
        if (!m_pendingTicks.empty()) {
            m_pendingTicks.back() = tick;
            ++m_consecutiveReplacements;
            if (m_consecutiveReplacements >= kBackpressureLagFrames) {
                m_backpressure.store(true, std::memory_order_release);
            }
        } else {
            m_pendingTicks.push_back(tick);
            // Keep the hard ceiling as a sanity bound — the new lag-based
            // logic above should already have engaged backpressure, but
            // this is a belt-and-suspenders cap.
            if (m_pendingTicks.size() >= kMaxPendingFrames)
                m_backpressure.store(true, std::memory_order_release);
        }
    }
    m_reqCV.notify_one();
}

void FrameProducer::requestScrubFrame(int64_t tick, uint32_t w, uint32_t h, bool scrub)
{
    {
        std::lock_guard lock(m_reqMtx);
        // Replace any pending scrub request — only the latest matters.
        m_pendingScrub = ScrubRequest{tick, w, h, scrub};
    }
    m_reqCV.notify_one();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Producer thread
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::producerLoop()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"RT-Producer");
#endif

    spdlog::info("[FrameProducer] Thread started");

    while (m_running.load(std::memory_order_relaxed) && !m_destroying.load(std::memory_order_acquire)) {
        int64_t tick = 0;
        std::optional<ScrubRequest> scrubReq;

        // Wait for a request (scrub requests or clock ticks)
        {
            std::unique_lock lock(m_reqMtx);
            m_reqCV.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return m_pendingScrub.has_value()
                    || !m_pendingTicks.empty()
                    || !m_running.load(std::memory_order_relaxed);
            });
            if (!m_running.load(std::memory_order_relaxed)) break;

            // Clear backpressure — the producer is about to process a frame.
            m_backpressure.store(false, std::memory_order_release);
            m_consecutiveReplacements = 0;

            // Scrub requests have priority over clock ticks
            if (m_pendingScrub.has_value()) {
                scrubReq = *m_pendingScrub;
                m_pendingScrub.reset();
            } else if (!m_pendingTicks.empty()) {
                tick = m_pendingTicks.front();
                m_pendingTicks.pop_front();
            } else {
                continue;
            }
        }

        // Wrap each frame production in try/catch so an exception from
        // compositing doesn't kill the producer thread (which would
        // leave the presenter and clock running with no data source).
        try {
            if (scrubReq) {
                produceScrubFrameImpl(*scrubReq);
            } else {
                produceFrameImpl(tick);
            }
        } catch (const std::exception& e) {
            spdlog::error("[FrameProducer] Exception in produceFrame: {}", e.what());
            // Publish the last good frame instead of nullptr so the presenter
            // keeps displaying a valid frame.  Publishing nullptr causes the
            // presenter to skip presenting entirely — the viewport freezes on
            // whatever was last displayed while the playhead continues advancing,
            // creating an unrecoverable freeze-in-playback state.
            //
            // CRITICAL: Clear the inter-queue semaphore on the cached frame.
            // The old gpuSemaphore was already consumed (waited on) when this
            // frame was first presented.  Re-using it would be Vulkan UB and
            // would crash the NVIDIA driver.
            if (m_lastGoodFrame) {
                m_lastGoodFrame->gpuSemaphore = 0;
                publishFrame(m_lastGoodFrame, tick);
            }
            // If there is no last good frame yet (first frame failed), skip
            // publishing — the presenter will hold its current (null) state
            // and retry on the next deadline.
        } catch (...) {
            spdlog::error("[FrameProducer] Unknown exception in produceFrame");
            if (m_lastGoodFrame) {
                m_lastGoodFrame->gpuSemaphore = 0;
                publishFrame(m_lastGoodFrame, tick);
            }
        }
    }

    spdlog::info("[FrameProducer] Thread exiting");
}



void FrameProducer::produceFrameImpl(int64_t tick)
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    if (!m_compositeCB) return;

    const int div = m_resDivisor.load(std::memory_order_relaxed);
    uint32_t w = m_outputW.load(std::memory_order_relaxed) / static_cast<uint32_t>(div);
    uint32_t h = m_outputH.load(std::memory_order_relaxed) / static_cast<uint32_t>(div);
    w = std::clamp(w, 64u, 3840u);
    h = std::clamp(h, 36u, 2160u);

    auto compStart = std::chrono::steady_clock::now();
    auto frame = m_compositeCB(tick, w, h, /*scrub=*/false);
    auto compEnd = std::chrono::steady_clock::now();
    double compMs = std::chrono::duration<double, std::milli>(compEnd - compStart).count();

    // If compositor returned a valid frame, use it.
    // If it returned nullptr or empty (width==0), re-publish the last
    // good frame so the presenter never shows a blank.
    if (frame && frame->width > 0) {
        m_lastGoodFrame = frame;
        // DIAG: log every produced frame
        {
            static std::atomic<int> s_prodLog{0};
            if (++s_prodLog % 5 == 0) {
                spdlog::info("[DIAG-PRODUCER] tick={} composite={:.1f}ms frame={}x{} "
                             "gpuReady={} gpuView=0x{:X}",
                             tick, compMs, frame->width, frame->height,
                             frame->gpuReady, frame->gpuImageView);
            }
        }
        publishFrame(std::move(frame), tick);
    } else if (frame && frame->width == 0) {
        // Empty sentinel: timeline has no content at this tick.
        // Clear the hold-frame and publish the empty sentinel so the
        // presenter clears the viewport instead of showing stale content.
        m_lastGoodFrame.reset();
        publishFrame(std::move(frame), tick);
    } else if (m_lastGoodFrame) {
        spdlog::info("[DIAG-PRODUCER] tick={} composite={:.1f}ms -> NULL, re-publish lastGood "
                     "gpuView=0x{:X}",
                     tick, compMs, m_lastGoodFrame->gpuImageView);
        // Clear the inter-queue semaphore on the cached frame — the old
        // gpuSemaphore was already consumed (waited on) when this frame
        // was first presented.  Re-using it as a wait semaphore would be
        // undefined behaviour (binary semaphore cannot be waited on twice)
        // and would crash the NVIDIA Vulkan driver.
        m_lastGoodFrame->gpuSemaphore = 0;

        // If the GPU has entered Failed state (device-lost), the cached
        // gpuImageView / gpuSampler are stale handles into a destroyed
        // VkDevice.  Clear them and gpuReady so downstream consumers fall
        // through to the CPU display path (which uses lazyReadback / pixels).
        if (!GpuContext::get().isOperational()) {
            m_lastGoodFrame->gpuImageView = 0;
            m_lastGoodFrame->gpuSampler   = 0;
            m_lastGoodFrame->gpuReady     = false;
        }

        publishFrame(m_lastGoodFrame, tick);
    }
    // else: no frame at all (first frame of playback, cache cold) — skip
}

// ═════════════════════════════════════════════════════════════════════════════
//  Scrub (runs on producer thread — off the UI thread)
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::produceScrubFrameImpl(const ScrubRequest& req)
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    if (!m_compositeCB) return;

    auto frame = m_compositeCB(req.tick, req.w, req.h, req.scrub);
    if (frame && frame->width > 0) {
        m_lastGoodFrame = frame;
    } else if (frame && frame->width == 0) {
        // Empty sentinel: timeline has no content at this tick (e.g., clip
        // was deleted).  Clear the hold-frame so we don't keep re-publishing
        // a stale composite from before the edit.
        m_lastGoodFrame.reset();
    }
    // Always publish whatever we got (even if empty) so the
    // presenter shows the correct state for that tick.
    // If falling back to m_lastGoodFrame, clear its stale semaphore
    // (already consumed by the previous present — re-use is Vulkan UB).
    if (!frame && m_lastGoodFrame)
        m_lastGoodFrame->gpuSemaphore = 0;
    publishFrame(frame ? frame : m_lastGoodFrame, req.tick);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Frame exchange
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::publishFrame(std::shared_ptr<CachedFrame> frame, int64_t tick)
{
    {
        std::lock_guard lock(m_exchangeMtx);
        m_exchange.frame  = std::move(frame);
        m_exchange.tick   = tick;
        m_exchange.hasNew = true;
    }
    m_exchangeCV.notify_one();
}

bool FrameProducer::consumeFrame(std::shared_ptr<CachedFrame>& outFrame, int64_t& outTick)
{
    std::lock_guard lock(m_exchangeMtx);
    if (!m_exchange.hasNew) return false;
    outFrame = m_exchange.frame;
    outTick  = m_exchange.tick;
    m_exchange.hasNew = false;
    return true;
}

bool FrameProducer::waitForFrame(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(m_exchangeMtx);
    return m_exchangeCV.wait_for(lock, timeout, [this] {
        return m_exchange.hasNew;
    });
}

std::shared_ptr<CachedFrame> FrameProducer::lastProducedFrame() const
{
    std::lock_guard lock(m_exchangeMtx);
    return m_exchange.frame;
}

} // namespace rt
