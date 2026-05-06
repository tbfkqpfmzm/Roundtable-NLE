/*
 * Allocator — Vulkan Memory Allocator (VMA) wrapper.
 *
 * Step 2: Provides GPU memory allocation for buffers and images.
 * Uses AMD's VMA library for sub-allocation, defragmentation, and
 * memory budget tracking. Optimized for the 24GB VRAM on RTX 4090.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

// Forward declare VMA types to avoid including the full header in ours
struct VmaAllocator_T;
struct VmaAllocation_T;
using VmaAllocator  = VmaAllocator_T*;
using VmaAllocation = VmaAllocation_T*;

namespace rt {

class Instance;
class Device;

/// Memory usage statistics.
struct MemoryStats
{
    uint64_t totalAllocatedBytes{0};
    uint64_t totalUsedBytes{0};
    uint64_t deviceLocalUsedBytes{0};    // VRAM used
    uint64_t deviceLocalBudgetBytes{0};  // Total VRAM available
    uint32_t allocationCount{0};
};

/// RAII wrapper for VMA allocator.
class Allocator
{
public:
    Allocator() = default;
    ~Allocator();

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;
    Allocator(Allocator&&) noexcept;
    Allocator& operator=(Allocator&&) noexcept;

    /// Initialize VMA with the given instance and device.
    bool create(const Instance& instance, const Device& device);

    /// Destroy the allocator.
    void destroy();

    /// Get the raw VMA handle.
    [[nodiscard]] VmaAllocator handle() const noexcept { return m_allocator; }

    /// Query current memory usage statistics.
    [[nodiscard]] MemoryStats queryStats() const;

    /// Log memory usage summary.
    void logMemoryUsage() const;

    operator VmaAllocator() const noexcept { return m_allocator; }

private:
    VmaAllocator m_allocator{nullptr};
};

} // namespace rt
