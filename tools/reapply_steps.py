#!/usr/bin/env python3
"""Reapply Steps 1+2 correctly, then apply Steps 3+4."""
import os

os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

with open('src/gpu/CompositeServiceFrame.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

# ===== Step 1: Add include and remove local struct =====

# 1a. Add include
content = content.replace(
    '#include "CompositeService.h"\n'
    '#include "ClipRenderers.h"\n'
    '#include "CompositeServiceBlend.h"\n'
    '\n'
    '// Media / timeline',
    '#include "CompositeService.h"\n'
    '#include "ClipRenderers.h"\n'
    '#include "CompositeServiceBlend.h"\n'
    '#include "CompositeServiceLayerBuild.h"\n'
    '\n'
    '// Media / timeline'
)

# 1b. Remove local struct LayerInfo
struct_start = content.find('    struct LayerInfo {')
struct_end = content.find('    };', struct_start) + 7
comment_start = content.rfind('    // Collect decoded layers from video tracks', 0, struct_start)
replacement = (
    '    // Collect decoded layers from video tracks (bottom-up for compositing).\n'
    '    // Premiere Pro order: V1 = lowest visual layer, V3 = topmost.\n'
    '    // Tracks are stored with the topmost timeline track at index 0 (V3),\n'
    '    // so we iterate in REVERSE to composite V1 first (bottom), V3 last (top).\n'
    '    // LayerInfo is defined in CompositeServiceLayerBuild.h.\n'
    '    std::vector<LayerInfo> layers;\n'
)
content = content[:comment_start] + replacement + content[struct_end:]

# ===== Step 2: Replace layer loop with function call =====

# Find the loop boundaries
loop_start = content.find('int clipsAtTick = 0;   // count enabled clips that attempted rendering')
loop_end_marker = content.find('    if (layers.empty()) {')
# Find the last push_back before the end marker
layer_push = content.rfind('            layers.push_back(std::move(layer));', loop_start, loop_end_marker)
# After push_back, find the closing braces
end_brace1 = content.find('}', layer_push) + 1
end_brace2 = content.find('}', end_brace1) + 1

loop_code = content[loop_start:end_brace2]

call_code = (
    '    int clipsAtTick = 0;   // count enabled clips that attempted rendering\n'
    '    layers = buildLayersForFrame(tick, outW, outH, scrubMode, playbackNonBlocking,\n'
    '                                 clipsAtTick, lock, gpuSpineUsedThisFrame);\n'
)

content = content[:loop_start] + call_code + content[end_brace2:]

# ===== Add buildLayersForFrame function before } // namespace rt =====
ns_close = content.rfind('} // namespace rt')
# Remove the fetchMediaFrame block that was left in compositeFrame (between scheduler and startup)
# But that's covered by the loop_code extracted above

func_def = (
    '\n'
    '// ---- buildLayersForFrame ----\n'
    'std::vector<LayerInfo> CompositeService::buildLayersForFrame(\n'
    '    int64_t tick, uint32_t outW, uint32_t outH,\n'
    '    bool scrubMode, bool playbackNonBlocking,\n'
    '    int& clipsAtTick,\n'
    '    std::unique_lock<std::mutex>& lock,\n'
    '    bool& gpuSpineUsedThisFrame)\n'
    '{\n'
    + loop_code +
    '\n    return layers;\n'
    '}\n'
)

content = content[:ns_close] + func_def + '\n' + content[ns_close:]

with open('src/gpu/CompositeServiceFrame.cpp', 'w', encoding='utf-8') as f:
    f.write(content)

print('Steps 1+2 reapplied successfully')
print(f'File size: {len(content)} chars')
