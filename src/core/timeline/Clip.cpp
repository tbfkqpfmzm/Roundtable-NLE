/*
 * Clip.cpp — Base clip implementation.
 * Step 3: Core Data Model
 */

#include "timeline/Clip.h"

#include <atomic>

namespace rt {

// Thread-safe unique ID generation
static std::atomic<uint64_t> s_idCounter{1};

Clip::Clip(ClipType type)
    : m_type(type)
    , m_id(s_idCounter.fetch_add(1, std::memory_order_relaxed))
{
}

Clip::~Clip() = default;

} // namespace rt

