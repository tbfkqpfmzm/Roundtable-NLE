/*
 * StagingRing — Pre-allocated, persistently-mapped staging buffer for GPU uploads.
 *
 * Eliminates per-frame VMA alloc/free overhead in the compositor upload loop.
 * Uses linear sub-allocation within a single large VkBuffer.
 * After the GPU fence signals (frame complete), call reset() to reuse.
 *
 * Matches Premiere Pro's persistent staging ring architecture.
 */
#pragma once

#include <volk.h>
#include <cstdint>
#include <cstring>
#include <mutex>

struct VmaAllocator_T;
struct VmaAllocation_T;
using VmaAllocator  = VmaAllocator_T*;
using VmaAllocation = VmaAllocation_T*;

namespace rt {

class StagingRing {
public:
    StagingRing() = default;
    ~StagingRing() { destroy(); }

    StagingRing(const StagingRing&) = delete;
    StagingRing& operator=(const StagingRing&) = delete;

    /// Create the buffer.  capacity should be large enough for all layers
    /// in a single frame (e.g. 64 MB for 8 layers × 1920×1080×4).
    bool init(VmaAllocator allocator, VkDeviceSize capacity);
    void destroy();

    /// Reset the sub-allocator.  Call after the frame fence signals.
    void reset() { std::lock_guard lock(m_ringMutex); m_offset = 0; }

    /// Sub-allocate a region, write data, return the buffer + offset.
    /// Returns false if the staging buffer is full (fallback to VMA alloc).
    struct Alloc {
        VkBuffer     buffer{VK_NULL_HANDLE};
        VkDeviceSize offset{0};
        bool         valid{false};
    };

    Alloc alloc(const void* data, VkDeviceSize size);

    [[nodiscard]] bool isInitialized() const { return m_buffer != VK_NULL_HANDLE; }
    [[nodiscard]] VkDeviceSize capacity() const { return m_capacity; }
    [[nodiscard]] VkDeviceSize used() const { return m_offset; }

private:
    VmaAllocator  m_allocator{nullptr};
    VmaAllocation m_allocation{nullptr};
    VkBuffer      m_buffer{VK_NULL_HANDLE};
    void*         m_mapped{nullptr};
    VkDeviceSize  m_capacity{0};
    VkDeviceSize  m_offset{0};
    std::mutex    m_ringMutex;
};

} // namespace rt
