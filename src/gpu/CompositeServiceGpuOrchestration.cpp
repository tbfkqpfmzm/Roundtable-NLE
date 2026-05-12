/*
 * CompositeServiceGpuOrchestration.cpp - GPU composite orchestration.
 * All GPU compositing logic has moved to CompositeEngine::composite().
 * This file now delegates to the engine.
 */

#include "CompositeService.h"
#include "CompositeEngine.h"
#include "Compositor.h"
#include "GpuContext.h"
#include "TransitionRenderer.h"
#include "EffectProcessor.h"

#include "media/FrameCache.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt {

std::shared_ptr<CachedFrame> CompositeService::tryCompositeOnGpu(
    const std::vector<LayerInfo>& layers,
    uint32_t outW, uint32_t outH,
    int64_t tick, bool scrubMode,
    bool perfLog,
    std::chrono::high_resolution_clock::time_point perfT0,
    std::chrono::high_resolution_clock::time_point& perfTlayers,
    int& effectLayerCount, int& effectPassCount,
    int& transitionCount)
{
    if (!m_engine)
        return nullptr;

    auto& ctx = GpuContext::get();
    auto* compositor = static_cast<Compositor*>(ctx.compositor(outW, outH));
    auto* effectProcessor = ctx.effectProcessor(outW, outH);
    auto* transitionRenderer = ctx.transitionRenderer(outW, outH);

    auto perfTgpuUp = perfT0;
    auto perfTcomp = perfT0;

    auto result = m_engine->composite(
        layers, outW, outH, tick, scrubMode, m_gpuDisplayMode,
        compositor, effectProcessor, transitionRenderer,
        perfLog, perfT0, perfTlayers, perfTgpuUp, perfTcomp,
        effectLayerCount, effectPassCount, transitionCount);

    return result;
}

} // namespace rt
