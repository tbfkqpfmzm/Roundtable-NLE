# Extract GPU orchestration from CompositeServiceFrame.cpp into CompositeServiceGpuOrchestration.cpp
# Step 3 of the modularization plan

$srcFile = "src/gpu/CompositeServiceFrame.cpp"
$newFile = "src/gpu/CompositeServiceGpuOrchestration.cpp"

$content = [System.IO.File]::ReadAllText((Resolve-Path $srcFile))

# Find GPU block boundaries
$gpuStart = $content.IndexOf("// Try GPU compositing first.")
$cpuStart = $content.IndexOf("// Multiple layers", $gpuStart + 100)

Write-Host "GPU_START_OFFSET=$gpuStart"
Write-Host "CPU_START_OFFSET=$cpuStart"
Write-Host "GPU_BLOCK_SIZE=$($cpuStart - $gpuStart) chars"

# Extract GPU block
$gpuBlock = $content.Substring($gpuStart, $cpuStart - $gpuStart)
Write-Host "GPU_BLOCK_LINES:" ($gpuBlock -split "`r`n" | Measure-Object | Select-Object -ExpandProperty Count)

# The GPU block currently starts at "// Try GPU compositing first." and includes:
#   - m_gpuCompositeState check
#   - if (m_gpuCompositeState == 1) { ... entire GPU composite path ... }
#   - The "GPU failed" fallthrough warning after the closing brace
#
# We need to wrap this in a function that returns a shared_ptr<CachedFrame> or nullptr.

# Build the new function body. The inline code has early returns and fallthroughs.
# For the function, we change:
#   - Early return `return result;` -> `return result;` (same, function returns)
#   - Fallthrough to CPU -> `return nullptr;` (caller handles CPU fallback)

# Read before the GPU block (everything before "// Try GPU compositing first.")
$beforeGpu = $content.Substring(0, $gpuStart)

# Read after the GPU block (everything after "// Multiple layers" comment)
$afterGpu = $content.Substring($cpuStart)

# Build replacement: just a function call + nullptr check
$callReplacement = @"
    perfTlayers = std::chrono::high_resolution_clock::now();

    // GPU compositing path — extracted to tryCompositeOnGpu()
    {
        int effectLayerCount = 0, effectPassCount = 0, transitionCount = 0;
        auto gpuResult = tryCompositeOnGpu(layers, outW, outH, tick, scrubMode,
                                            perfLog, perfT0, perfTlayers,
                                            effectLayerCount, effectPassCount,
                                            transitionCount);
        if (gpuResult) {
            return gpuResult;
        }
    }
    // GPU failed — fall through to CPU path below
    spdlog::warn("compositeFrame: GPU composite failed, falling back to CPU");

"@

# Build the new .cpp file content
$newCpp = "/*`n * CompositeServiceGpuOrchestration.cpp - GPU composite orchestration.`n * Extracted from CompositeServiceFrame.cpp (Step 3).`n */`n`n#include `"CompositeService.h`"`n#include `"Compositor.h`"`n#include `"GpuContext.h`"`n#include `"GpuTextureCache.h`"`n#include `"GpuWorkSubmission.h`"`n#include `"StagingRing.h`"`n#include `"Texture.h`"`n#include `"TransitionRenderer.h`"`n#include `"EffectProcessor.h`"`n`n#include `"media/FrameCache.h`"`n`n#include <spdlog/spdlog.h>`n`nnamespace rt {`n`n" + @"
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
" + $gpuBlock + @"
    return nullptr;
}

} // namespace rt
"@

Write-Host "NEW_CPP_SIZE=$($newCpp.Length) chars"

# Write the new file
[System.IO.File]::WriteAllText((Resolve-Path $newFile), $newCpp)

# Replace the inline GPU block in the original file
$newContent = $beforeGpu + $callReplacement + $afterGpu
Write-Host "NEW_SRC_SIZE=$($newContent.Length) chars"
Write-Host "OLD_SRC_SIZE=$($content.Length) chars"
Write-Host "DIFF=$($content.Length - $newContent.Length) chars removed"

[System.IO.File]::WriteAllText((Resolve-Path $srcFile), $newContent)

Write-Host "DONE: Created $newFile and updated $srcFile"
