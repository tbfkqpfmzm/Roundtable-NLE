// Precompiled header for roundtable_gpu
// Covers STL + spdlog + Vulkan/VMA — the most commonly included headers.
#pragma once

// STL
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// spdlog (included in 27/30 gpu TUs)
#include <spdlog/spdlog.h>

// glm
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Vulkan (via volk runtime loader)
#include <volk.h>
#include <vk_mem_alloc.h>

// ─── P1.5: vkQueueSubmit lockdown ────────────────────────────────────────────
// Every direct vkQueueSubmit call should route through GpuScheduler::submit.
// The macro below replaces the identifier with a sentinel that fails to
// compile when expanded, guaranteeing that new code (or regressions) that
// reach for the raw API get caught at build time.
//
// volk has already declared `vkQueueSubmit` above as a function-pointer
// variable, so this macro affects only subsequent identifier lookups in
// user code — the declaration itself is preserved.
//
// Opt-out: GpuScheduler.cpp is the one TU that must call vkQueueSubmit
// directly.  It defines ROUNDTABLE_ALLOW_VK_QUEUE_SUBMIT via
// target_source-level COMPILE_DEFINITIONS in src/gpu/CMakeLists.txt.
#ifndef ROUNDTABLE_ALLOW_VK_QUEUE_SUBMIT
#define vkQueueSubmit ROUNDTABLE_USE_GPU_SCHEDULER_INSTEAD_OF_VK_QUEUE_SUBMIT
#endif
