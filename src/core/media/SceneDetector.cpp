/*
 * SceneDetector.cpp — native C++ scene cut detection.
 *
 * Algorithm (inspired by PySceneDetect ContentDetector / AdaptiveDetector):
 *   - Decode every frame sequentially at low resolution
 *   - Convert each frame from BGRA/YUV to HSV
 *   - Compute per-channel mean absolute difference from previous frame
 *   - Weighted sum: 1.0*ΔH + 1.0*ΔS + 1.0*ΔV
 *   - If score exceeds threshold → hard cut detected
 *   - Optional adaptive mode: threshold = max(fixedThreshold, rollingAvg * adaptiveFactor)
 */

#include "media/SceneDetector.h"
#include "media/VideoDecoder.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
}

namespace rt {

// ─────────────────────────────────────────────────────────────────────────────
// HSV helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Convert a single BGRA pixel to HSV (H: 0-180, S: 0-255, V: 0-255)
/// Matches OpenCV's BGR→HSV mapping for consistency with PySceneDetect.
inline void bgraToHsv(uint8_t b, uint8_t g, uint8_t r,
                      uint8_t& h, uint8_t& s, uint8_t& v)
{
    int iMax = std::max({r, g, b});
    int iMin = std::min({r, g, b});
    int delta = iMax - iMin;

    v = static_cast<uint8_t>(iMax);
    s = (iMax == 0) ? 0 : static_cast<uint8_t>((delta * 255) / iMax);

    if (delta == 0) {
        h = 0;
    } else if (iMax == r) {
        h = static_cast<uint8_t>(((30 * (g - b)) / delta + 180) % 180);
    } else if (iMax == g) {
        h = static_cast<uint8_t>((30 * (b - r)) / delta + 60);
    } else {
        h = static_cast<uint8_t>((30 * (r - g)) / delta + 120);
    }
}

/// Compute mean absolute difference between two HSV buffers.
/// Returns (deltaH, deltaS, deltaV) as floats in [0, 255].
struct HsvDelta { float dH, dS, dV; };

HsvDelta computeHsvDelta(const uint8_t* hsvA, const uint8_t* hsvB, int pixelCount)
{
    int64_t sumH = 0, sumS = 0, sumV = 0;
    for (int i = 0; i < pixelCount; ++i) {
        int idxA = i * 3;
        int idxB = i * 3;
        // H is circular in [0,180) — compute circular difference
        int dh = std::abs(static_cast<int>(hsvA[idxA]) - static_cast<int>(hsvB[idxB]));
        if (dh > 90) dh = 180 - dh;
        sumH += dh;
        sumS += std::abs(static_cast<int>(hsvA[idxA + 1]) - static_cast<int>(hsvB[idxB + 1]));
        sumV += std::abs(static_cast<int>(hsvA[idxA + 2]) - static_cast<int>(hsvB[idxB + 2]));
    }
    float n = static_cast<float>(std::max(pixelCount, 1));
    return {
        static_cast<float>(sumH) / n,
        static_cast<float>(sumS) / n,
        static_cast<float>(sumV) / n
    };
}

/// Convert a decoded BGRA frame to a downscaled HSV buffer.
/// Returns the pixel count written, or 0 on failure.
int frameToHsv(const DecodedFrame& frame, SwsContext*& swsCtx,
               int dstW, int dstH,
               std::vector<uint8_t>& bgraScaled,
               std::vector<uint8_t>& hsvOut)
{
    // Set up scaler if needed (from whatever raw format to BGRA at dstW x dstH)
    if (!swsCtx) {
        int srcFmt = frame.rawFormat;
        if (srcFmt < 0) srcFmt = AV_PIX_FMT_BGRA;
        swsCtx = sws_getContext(
            static_cast<int>(frame.width), static_cast<int>(frame.height),
            static_cast<AVPixelFormat>(srcFmt),
            dstW, dstH, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx) return 0;
    }

    // Scale to BGRA at target resolution
    int bgraStride = dstW * 4;
    bgraScaled.resize(static_cast<size_t>(bgraStride * dstH));
    uint8_t* dstData[1] = { bgraScaled.data() };
    int dstLinesize[1] = { bgraStride };

    sws_scale(swsCtx,
              frame.data, frame.linesize,
              0, static_cast<int>(frame.height),
              dstData, dstLinesize);

    // Convert BGRA → HSV
    int pixelCount = dstW * dstH;
    hsvOut.resize(static_cast<size_t>(pixelCount * 3));
    for (int i = 0; i < pixelCount; ++i) {
        int bgraIdx = i * 4;
        bgraToHsv(bgraScaled[bgraIdx + 0],  // B
                  bgraScaled[bgraIdx + 1],  // G
                  bgraScaled[bgraIdx + 2],  // R
                  hsvOut[i * 3 + 0],
                  hsvOut[i * 3 + 1],
                  hsvOut[i * 3 + 2]);
    }
    return pixelCount;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// SceneDetector implementation
// ─────────────────────────────────────────────────────────────────────────────

SceneDetector::~SceneDetector()
{
    cancel();
    if (m_worker.joinable())
        m_worker.join();
}

void SceneDetector::cancel()
{
    m_cancelled.store(true);
}

void SceneDetector::detectAsync(
    const std::filesystem::path& mediaPath,
    int64_t startFrame, int64_t endFrame,
    float threshold,
    SceneDetectProgressFn onProgress,
    SceneDetectCompleteFn onComplete,
    SceneDetectErrorFn onError)
{
    // Wait for previous run
    if (m_worker.joinable())
        m_worker.join();

    m_cancelled.store(false);
    m_running.store(true);

    m_worker = std::thread(&SceneDetector::workerFunc, this,
                           mediaPath, startFrame, endFrame, threshold,
                           std::move(onProgress),
                           std::move(onComplete),
                           std::move(onError));
}

void SceneDetector::workerFunc(
    std::filesystem::path mediaPath,
    int64_t startFrame, int64_t endFrame,
    float threshold,
    SceneDetectProgressFn onProgress,
    SceneDetectCompleteFn onComplete,
    SceneDetectErrorFn onError)
{
    spdlog::info("SceneDetector: starting on '{}' frames [{}, {}), threshold={}",
                 mediaPath.string(), startFrame, endFrame, threshold);

    // Open a dedicated decoder (software only — don't consume NVDEC sessions)
    VideoDecoder decoder;
    if (!decoder.open(mediaPath, /*forceSoftware=*/true,
                      /*maxThreads=*/2, /*sliceOnlyThreading=*/true)) {
        if (onError) onError("Failed to open video: " + decoder.lastError());
        m_running.store(false);
        return;
    }

    const auto& info = decoder.info();
    if (info.frameCount <= 0) {
        if (onError) onError("Video has no frames");
        m_running.store(false);
        return;
    }

    if (endFrame < 0 || endFrame > info.frameCount)
        endFrame = info.frameCount;
    if (startFrame < 0) startFrame = 0;
    int64_t totalFrames = endFrame - startFrame;
    if (totalFrames <= 1) {
        if (onComplete) onComplete({});
        m_running.store(false);
        return;
    }

    // Compute downscale target (PySceneDetect auto_downscale formula)
    // Target ~320px width minimum
    constexpr int kMinWidth = 320;
    int dstW = static_cast<int>(info.width);
    int dstH = static_cast<int>(info.height);
    if (dstW > kMinWidth) {
        float scale = static_cast<float>(kMinWidth) / static_cast<float>(dstW);
        dstW = kMinWidth;
        dstH = std::max(2, static_cast<int>(static_cast<float>(info.height) * scale));
        // Ensure even dimensions for sws_scale
        dstW &= ~1;
        dstH &= ~1;
    }
    int pixelCount = dstW * dstH;

    spdlog::info("SceneDetector: analyzing {} frames at {}x{}", totalFrames, dstW, dstH);

    // Buffers
    std::vector<uint8_t> bgraScaled;
    std::vector<uint8_t> hsvCurr, hsvPrev;
    SwsContext* swsCtx = nullptr;
    std::vector<DetectedCut> cuts;

    // Adaptive threshold: rolling window of recent scores
    constexpr int kAdaptiveWindow = 30; // ~1 second at 30fps
    constexpr float kAdaptiveFactor = 3.0f;
    std::vector<float> recentScores;
    recentScores.reserve(kAdaptiveWindow);

    // Seek to start frame
    if (startFrame > 0) {
        decoder.seekToFrame(startFrame, SeekMode::Precise);
    }

    DecodedFrame cpuFrame;
    DecodedFrame rawFrame;
    bool firstFrame = true;

    for (int64_t fi = startFrame; fi < endFrame; ++fi) {
        if (m_cancelled.load()) {
            spdlog::info("SceneDetector: cancelled at frame {}", fi);
            break;
        }

        // Decode next sequential frame
        if (!decoder.decodeNext(rawFrame)) {
            // Could be end of stream
            break;
        }

        // Transfer from GPU if needed (shouldn't happen with forceSoftware, but safety)
        const DecodedFrame* useFrame = &rawFrame;
        if (rawFrame.isHardware) {
            if (!decoder.transferHardwareFrame(rawFrame, cpuFrame))
                continue;
            useFrame = &cpuFrame;
        }

        // Convert to HSV at downscaled resolution
        int pc = frameToHsv(*useFrame, swsCtx, dstW, dstH, bgraScaled, hsvCurr);
        if (pc <= 0) continue;

        if (firstFrame) {
            firstFrame = false;
            std::swap(hsvCurr, hsvPrev);

            // Report initial progress
            if (onProgress)
                onProgress(0.0f, fi - startFrame, totalFrames);
            continue;
        }

        // Compute HSV delta score (PySceneDetect ContentDetector weights: H=1, S=1, V=1)
        HsvDelta delta = computeHsvDelta(hsvPrev.data(), hsvCurr.data(), pixelCount);
        float score = delta.dH + delta.dS + delta.dV;

        // Adaptive threshold: use the higher of fixed threshold or rolling-avg * factor
        float adaptiveThresh = threshold;
        if (recentScores.size() >= static_cast<size_t>(kAdaptiveWindow)) {
            float avg = std::accumulate(recentScores.begin(), recentScores.end(), 0.0f)
                        / static_cast<float>(recentScores.size());
            adaptiveThresh = std::max(threshold, avg * kAdaptiveFactor);
        }

        // Update rolling window
        if (recentScores.size() >= static_cast<size_t>(kAdaptiveWindow))
            recentScores.erase(recentScores.begin());
        recentScores.push_back(score);

        // Check for cut
        if (score >= adaptiveThresh) {
            double timeSec = decoder.frameToSeconds(fi);
            cuts.push_back({fi, timeSec, score});
            spdlog::debug("SceneDetector: cut at frame {} (t={:.3f}s, score={:.1f}, thresh={:.1f})",
                          fi, timeSec, score, adaptiveThresh);
        }

        std::swap(hsvCurr, hsvPrev);

        // Progress callback (every 50 frames to avoid overhead)
        if (onProgress && ((fi - startFrame) % 50 == 0 || fi == endFrame - 1)) {
            float pct = static_cast<float>(fi - startFrame) / static_cast<float>(totalFrames);
            onProgress(pct, fi - startFrame, totalFrames);
        }
    }

    if (swsCtx) sws_freeContext(swsCtx);

    if (!m_cancelled.load()) {
        spdlog::info("SceneDetector: finished — {} cuts detected", cuts.size());
        if (onComplete) onComplete(std::move(cuts));
    }

    m_running.store(false);
}

} // namespace rt
