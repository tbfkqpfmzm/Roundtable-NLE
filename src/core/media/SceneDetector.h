/*
 * SceneDetector — detect hard cuts in video media using frame-pair HSV analysis.
 *
 * Implements a ContentDetector-style algorithm:
 *   1. Open video with a dedicated VideoDecoder (software, low-res via sws_scale)
 *   2. For each sequential frame: convert to HSV, compute weighted channel deltas
 *   3. Adaptive threshold from rolling window of scores (AdaptiveDetector style)
 *   4. Report cut frame numbers via callback
 *
 * All computation runs on a worker thread. Call detectAsync() to start,
 * poll/wait via callbacks, and cancel() to abort early.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

namespace rt {

/// A detected scene cut point.
struct DetectedCut
{
    int64_t frameNumber{0};   ///< Frame index in the source media
    double  timeSeconds{0.0}; ///< Time in seconds
    float   score{0.0f};      ///< Detection score (higher = more confident)
};

/// Progress callback: (percent 0–1, current frame, total frames).
using SceneDetectProgressFn = std::function<void(float percent, int64_t frame, int64_t total)>;

/// Completion callback: delivers the final list of detected cuts.
using SceneDetectCompleteFn = std::function<void(std::vector<DetectedCut> cuts)>;

/// Error callback.
using SceneDetectErrorFn = std::function<void(std::string message)>;

class SceneDetector
{
public:
    SceneDetector() = default;
    ~SceneDetector();

    // Non-copyable, non-movable
    SceneDetector(const SceneDetector&) = delete;
    SceneDetector& operator=(const SceneDetector&) = delete;

    /// Start asynchronous scene detection on a video file.
    /// @param mediaPath    Path to the video file.
    /// @param startFrame   First frame to analyze (0-based).
    /// @param endFrame     Last frame to analyze (exclusive). -1 = end of file.
    /// @param threshold    Sensitivity (lower = more cuts detected). Default 27.0.
    /// @param onProgress   Called periodically from worker thread.
    /// @param onComplete   Called once from worker thread when finished.
    /// @param onError      Called from worker thread on failure.
    void detectAsync(const std::filesystem::path& mediaPath,
                     int64_t startFrame, int64_t endFrame,
                     float threshold,
                     SceneDetectProgressFn onProgress,
                     SceneDetectCompleteFn onComplete,
                     SceneDetectErrorFn onError);

    /// Cancel a running detection. Safe to call from any thread.
    void cancel();

    /// Is a detection currently running?
    [[nodiscard]] bool isRunning() const noexcept { return m_running.load(); }

private:
    void workerFunc(std::filesystem::path mediaPath,
                    int64_t startFrame, int64_t endFrame,
                    float threshold,
                    SceneDetectProgressFn onProgress,
                    SceneDetectCompleteFn onComplete,
                    SceneDetectErrorFn onError);

    std::thread       m_worker;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_running{false};
};

} // namespace rt
