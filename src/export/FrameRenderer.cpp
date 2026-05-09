/*
 * FrameRenderer.cpp — Timeline evaluation and GPU compositing for export.
 */

#include "FrameRenderer.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <functional>

// Core types
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/VideoClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "media/MediaPool.h"
#include "media/FrameCache.h"
#include "project/Project.h"

// GPU
#include "Compositor.h"
#include "VideoUploader.h"
#include "EffectProcessor.h"

// Effects
#include "effects/LUT.h"

namespace rt {

FrameRenderer::FrameRenderer() = default;
FrameRenderer::~FrameRenderer() { shutdown(); }

bool FrameRenderer::init(const FrameRendererConfig& config, Compositor* compositor)
{
    m_config     = config;
    m_compositor = compositor;
    m_stats      = {};

    // Compositor is optional — without it we produce blank frames
    // (useful for testing the pipeline without GPU)
    if (compositor) {
        spdlog::info("FrameRenderer: Initialized with GPU compositor {}x{}",
                     config.outputWidth, config.outputHeight);
    } else {
        spdlog::info("FrameRenderer: Initialized without GPU (stub mode) {}x{}",
                     config.outputWidth, config.outputHeight);
    }

    m_initialized = true;
    return true;
}

void FrameRenderer::shutdown()
{
    m_compositor  = nullptr;
    m_initialized = false;
}

RenderedFrame FrameRenderer::renderFrame(const Timeline& timeline, int64_t frameIndex)
{
    RenderedFrame result;
    if (!m_initialized) {
        m_lastError = "FrameRenderer: Not initialized";
        return result;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    int64_t tick = frameToTick(frameIndex);

    result.frameIndex = frameIndex;
    result.timestamp  = static_cast<double>(frameIndex) * m_config.fpsDen / m_config.fpsNum;
    result.width      = m_config.outputWidth;
    result.height     = m_config.outputHeight;

    // Evaluate active clips and build compositor layers
    int layerCount = evaluateLayers(timeline, tick);

    if (m_compositor) {
        // Dispatch GPU composite
        if (!m_compositor->compositeSync()) {
            m_lastError = "FrameRenderer: Composite failed at frame " + std::to_string(frameIndex);
            spdlog::error("{}", m_lastError);
            return result;
        }

        // Read back pixels unless GPU-only mode
        if (!m_config.gpuOnly) {
            if (!m_compositor->readbackOutput(result.pixels)) {
                m_lastError = "FrameRenderer: Readback failed at frame " + std::to_string(frameIndex);
                spdlog::error("{}", m_lastError);
                return result;
            }
        }
    } else {
        // Stub mode: produce a solid-color frame (dark gray)
        if (!m_config.gpuOnly) {
            size_t pixelCount = static_cast<size_t>(result.width) * result.height * 4;
            result.pixels.resize(pixelCount);
            for (size_t i = 0; i < pixelCount; i += 4) {
                result.pixels[i]   = 30;   // R
                result.pixels[i+1] = 30;   // G
                result.pixels[i+2] = 30;   // B
                result.pixels[i+3] = 255;  // A
            }
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    ++m_stats.framesRendered;
    m_stats.totalRenderTimeMs += ms;
    m_stats.lastFrameTimeMs = ms;
    m_stats.avgFrameTimeMs = m_stats.totalRenderTimeMs / m_stats.framesRendered;
    m_stats.activeLayers = layerCount;

    return result;
}

RenderedFrame FrameRenderer::renderAtTime(const Timeline& timeline, double timeSeconds)
{
    int64_t frameIndex = static_cast<int64_t>(timeSeconds * m_config.fpsNum / m_config.fpsDen);
    return renderFrame(timeline, frameIndex);
}

int64_t FrameRenderer::renderRange(const Timeline& timeline,
                                    int64_t startFrame, int64_t endFrame,
                                    const FrameCallback& callback)
{
    if (!m_initialized || !callback) return 0;

    int64_t rendered = 0;
    for (int64_t f = startFrame; f <= endFrame; ++f) {
        auto frame = renderFrame(timeline, f);
        if (!frame.isValid()) continue;

        if (!callback(frame)) {
            spdlog::info("FrameRenderer: Render cancelled at frame {}", f);
            break;
        }
        ++rendered;
    }
    return rendered;
}

bool FrameRenderer::setResolution(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return false;
    m_config.outputWidth  = width;
    m_config.outputHeight = height;

    if (m_compositor) {
        return m_compositor->resize(width, height);
    }
    return true;
}

void FrameRenderer::resetStats()
{
    m_stats = {};
}

int64_t FrameRenderer::frameToTick(int64_t frameIndex) const noexcept
{
    // Convert frame index to TimeTick (48000 ticks/second)
    // tick = frameIndex * TICKS_PER_SECOND * fpsDen / fpsNum
    return static_cast<int64_t>(
        static_cast<double>(frameIndex) * 48000.0 * m_config.fpsDen / m_config.fpsNum);
}

int FrameRenderer::evaluateLayers(const Timeline& timeline, int64_t tick, int depth)
{
    // Walk all video tracks (bottom-to-top), find active clips at this time.
    // For each active clip, create a compositor layer with GPU texture.

    std::vector<CompositorLayer> gpuLayers;

    for (size_t ri = timeline.trackCount(); ri > 0; --ri) {
        size_t ti = ri - 1;
        // const_cast is safe: we only call read-only accessors (clipsAtTime,
        // opacity().evaluate, etc.) which are logically const but lack const
        // overloads on KeyframeTrack.
        auto* track = const_cast<Track*>(timeline.track(ti));
        if (!track || track->type() != TrackType::Video) continue;
        if (track->isMuted()) continue;

        auto clips = track->clipsAtTime(tick);
        for (auto* clip : clips) {
            if (!clip || !clip->isEnabled()) continue;

            // ── Decode video frame via MediaPool ────────────────────────
            auto* videoClip = dynamic_cast<VideoClip*>(clip);
            auto* imageClip = dynamic_cast<ImageClip*>(clip);
            auto* seqClip   = dynamic_cast<SequenceClip*>(clip);

            if (seqClip) {
                // Nested sequence clip — recursively composite the inner timeline.
                if (!m_project || depth >= kMaxNestDepth) {
                    gpuLayers.emplace_back();
                    gpuLayers.back().enabled = false;
                    continue;
                }
                size_t seqIdx = seqClip->sequenceIndex();
                const Timeline* nested = m_project->sequence(seqIdx);
                if (!nested) {
                    gpuLayers.emplace_back();
                    gpuLayers.back().enabled = false;
                    continue;
                }
                // Compute local tick within the nested timeline
                int64_t localTick = tick - clip->timelineIn();
                int64_t innerTick = clip->sourceIn()
                    + static_cast<int64_t>(localTick * clip->effectiveSpeed(localTick));
                if (innerTick < 0) innerTick = 0;
                evaluateLayers(*nested, innerTick, depth + 1);
                continue;
            }

            if ((!videoClip && !imageClip) || !m_mediaPool) {
                // Non-video/image clips or no media pool — skip
                if (!videoClip && !imageClip) {
                    gpuLayers.emplace_back();
                    gpuLayers.back().enabled = false;
                }
                continue;
            }

            const std::string& mediaPath = videoClip
                ? videoClip->mediaPath() : imageClip->mediaPath();
            if (mediaPath.empty()) continue;

            uint64_t handle = m_mediaPool->open(mediaPath);
            if (handle == 0) continue;

            int64_t localTick = tick - clip->timelineIn();
            int64_t srcTick   = clip->sourceIn() + static_cast<int64_t>(localTick * clip->effectiveSpeed(localTick));
            if (srcTick < 0) continue;

            int64_t frameNum = 0;
            if (videoClip) {
                double fps = videoClip->sourceFps();
                if (fps <= 0.0) fps = 24.0;
                frameNum = static_cast<int64_t>(ticksToSeconds(srcTick) * fps);

                auto* mediaInfo = m_mediaPool->getInfo(handle);
                if (mediaInfo && mediaInfo->frameCount <= 1) frameNum = 0;
            }
            // ImageClip always uses frame 0

            auto frame = m_mediaPool->getFrame(handle, frameNum, ResolutionTier::Full, false);
            if (!frame || !frame->ensurePixels()) continue;

            // ── Upload to GPU via VideoUploader ─────────────────────────
            CompositorLayer cl;
            cl.enabled = true;

            VkDescriptorImageInfo layerTexInfo{};
            bool hasTexture = false;

            if (m_videoUploader && m_compositor) {
                auto gpuFrame = m_videoUploader->upload(*frame);
                if (gpuFrame && gpuFrame->valid && gpuFrame->vkImageView) {
                    layerTexInfo.sampler     = static_cast<VkSampler>(gpuFrame->vkSampler);
                    layerTexInfo.imageView   = static_cast<VkImageView>(gpuFrame->vkImageView);
                    layerTexInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    hasTexture = true;
                } else {
                    cl.enabled = false;
                }
            } else {
                cl.enabled = false;
            }

            // ── Apply per-clip effects via EffectProcessor ──────────────
            if (hasTexture && m_effectProcessor && m_effectProcessor->isInitialized()
                && clip->effects().hasActiveEffects())
            {
                int64_t localTick2 = tick - clip->timelineIn();
                auto snapshots = clip->effects().evaluate(localTick2);

                // Check for LUT effect and upload its 3D texture if needed
                for (const auto& snap : snapshots) {
                    if (snap.type == EffectType::LUT) {
                        // Find the LUT effect in the clip's effect stack
                        for (size_t ei = 0; ei < clip->effects().effectCount(); ++ei) {
                            auto& fx = clip->effects().effect(ei);
                            if (fx.effectType() == EffectType::LUT && fx.isEnabled()) {
                                auto* lutFx = static_cast<LUT*>(&fx);
                                if (lutFx->hasLUT())
                                    m_effectProcessor->uploadLUT3D(lutFx->lutData(), lutFx->lutSize());
                                break;
                            }
                        }
                        break;
                    }
                }

                if (!snapshots.empty()) {
                    if (m_effectProcessor->processSync(layerTexInfo, snapshots))
                        layerTexInfo = m_effectProcessor->outputDescriptorInfo();
                }
            }

            cl.textureInfo = layerTexInfo;

            // ── Build transform ─────────────────────────────────────────
            float opac = clip->opacity().evaluate(localTick);
            float px = clip->positionX().evaluate(localTick);
            float py = clip->positionY().evaluate(localTick);
            float sx = clip->scaleX().evaluate(localTick);
            float sy = clip->scaleY().evaluate(localTick);
            float rot = clip->rotation().evaluate(localTick);

            cl.transform = Compositor::buildViewportTransform(
                frame->width, frame->height,
                m_config.outputWidth, m_config.outputHeight,
                px, py, sx, sy, rot);

            cl.opacity   = opac;
            cl.blendMode = static_cast<BlendMode>(clip->blendMode());

            // Crop (VideoClip stores as 0–100 percentages, GPU uses 0–1)
            cl.cropLeft   = videoClip->cropLeft()   / 100.0f;
            cl.cropRight  = videoClip->cropRight()  / 100.0f;
            cl.cropTop    = videoClip->cropTop()    / 100.0f;
            cl.cropBottom = videoClip->cropBottom() / 100.0f;

            gpuLayers.push_back(cl);
        }
    }

    // Set layers on compositor
    if (m_compositor && !gpuLayers.empty()) {
        // Filter out disabled placeholder layers
        std::vector<CompositorLayer> enabled;
        for (auto& l : gpuLayers)
            if (l.enabled) enabled.push_back(l);

        if (!enabled.empty())
            m_compositor->setLayers(enabled);
        else
            m_compositor->clearLayers();
    } else if (m_compositor) {
        m_compositor->clearLayers();
    }

    return static_cast<int>(gpuLayers.size());
}

} // namespace rt
