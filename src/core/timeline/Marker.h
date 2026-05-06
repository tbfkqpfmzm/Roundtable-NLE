/*
 * Marker — a labeled point on the timeline.
 */

#pragma once

#include <cstdint>
#include <string>

namespace rt {

struct Marker
{
    int64_t     time{0};              // Position in TimeTicks
    std::string label;                // User-defined label
    uint32_t    color{0xFF4444FF};    // RGBA color
    
    bool operator<(const Marker& other) const noexcept
    {
        return time < other.time;
    }
};

} // namespace rt
