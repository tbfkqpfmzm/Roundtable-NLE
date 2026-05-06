// Precompiled header for roundtable_core
// Covers STL + spdlog + glm — the most commonly included headers.
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
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// spdlog (included in 39/70+ core TUs)
#include <spdlog/spdlog.h>

// glm
#include <glm/glm.hpp>
