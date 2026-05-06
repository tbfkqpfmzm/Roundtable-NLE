#include "timeline/KeyframeMode.h"

#include <atomic>

namespace rt::KeyframeMode {

namespace {
std::atomic<bool> g_autoEnabled{false};
}

bool isAutoEnabled() noexcept { return g_autoEnabled.load(std::memory_order_relaxed); }
void setAutoEnabled(bool enabled) noexcept { g_autoEnabled.store(enabled, std::memory_order_relaxed); }

} // namespace rt::KeyframeMode
