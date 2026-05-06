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
