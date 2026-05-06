#!/usr/bin/env python3
"""Extract GPU and CPU compositing paths from CompositeServiceFrame.cpp into separate functions."""

with open('src/gpu/CompositeServiceFrame.cpp', 'r', encoding='utf-8') as f:
    content = f.read()
with open('_extracted_gpu.txt', 'r', encoding='utf-8') as f:
    gpu_code = f.read()
with open('_extracted_cpu.txt', 'r', encoding='utf-8') as f:
    cpu_code = f.read()

# 1. Replace GPU inline code with function call
gpu_start = content.find('// Try GPU compositing first.')
warn_pos = content.find('spdlog::warn', gpu_start)
end_brace1 = content.find('}', warn_pos)
end_brace2 = content.find('}', end_brace1 + 1)
gpu_end = end_brace2 + 1

gpu_call = (
    '// Try GPU compositing first.\n'
    '    int effectLayerCount = 0, effectPassCount = 0, transitionCount = 0;\n'
    '    std::shared_ptr<CachedFrame> gpuResult = tryCompositeOnGpu(layers, outW, outH, tick, scrubMode, perfLog, perfT0, perfTlayers, effectLayerCount, effectPassCount, transitionCount);\n'
    '    if (gpuResult) {\n'
    '        result = gpuResult;\n'
    '    } else {\n'
    '    spdlog::warn("compositeFrame: GPU composite failed, falling back to CPU");\n'
    '    }\n'
)

content2 = content[:gpu_start] + gpu_call + content[gpu_end:]

# 2. Replace CPU inline code with function call
cpu_start_marker = '// Multiple layers'
cpu_start = content2.find(cpu_start_marker)
cpu_end_marker = '// --- CPU_PATH_END ---'
cpu_end = content2.find(cpu_end_marker, cpu_start)

cpu_call = (
    '// Multiple layers - CPU alpha composite with transforms\n'
    '    result = compositeCpuFallback(layers, outW, outH, tick);\n'
    '// --- CPU_PATH_END ---'
)

content3 = content2[:cpu_start] + cpu_call + content2[cpu_end:]

# 3. Add function definitions before namespace close
ns_close = content3.rfind('} // namespace rt')

# Remove any trailing whitespace/newlines before ns_close
before_ns = content3[:ns_close].rstrip()
after_ns = content3[ns_close:]

gpu_func = (
    '\n\n'
    '// ---- tryCompositeOnGpu - GPU compositing path ----\n'
    'std::shared_ptr<CachedFrame> CompositeService::tryCompositeOnGpu(\n'
    '    const std::vector<LayerInfo>& layers,\n'
    '    uint32_t outW, uint32_t outH,\n'
    '    int64_t tick, bool scrubMode,\n'
    '    bool perfLog,\n'
    '    std::chrono::high_resolution_clock::time_point perfT0,\n'
    '    std::chrono::high_resolution_clock::time_point& perfTlayers,\n'
    '    int& effectLayerCount, int& effectPassCount,\n'
    '    int& transitionCount)\n'
    '{\n'
    + gpu_code +
    '\n    return nullptr;\n'
    '}\n'
)

cpu_func = (
    '\n\n'
    '// ---- compositeCpuFallback - CPU compositing path ----\n'
    'std::shared_ptr<CachedFrame> CompositeService::compositeCpuFallback(\n'
    '    const std::vector<LayerInfo>& layers,\n'
    '    uint32_t outW, uint32_t outH,\n'
    '    int64_t tick)\n'
    '{\n'
    + cpu_code +
    '\n    return nullptr;\n'
    '}\n'
)

content4 = before_ns + gpu_func + cpu_func + '\n' + after_ns

with open('src/gpu/CompositeServiceFrame.cpp', 'w', encoding='utf-8') as f:
    f.write(content4)

print(f'Done. New file size: {len(content4)} chars')
