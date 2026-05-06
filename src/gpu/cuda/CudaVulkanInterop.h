/*
 * CudaVulkanInterop — Zero-copy frame sharing between CUDA and Vulkan.
 *
 * Uses VK_KHR_external_memory_win32 to export Vulkan device memory as
 * a Win32 HANDLE, then import into CUDA via cuImportExternalMemory.
 * This lets NVDEC output be read directly by Vulkan compute shaders
 * (NV12→BGRA) without any CPU-side PCIe transfer.
 *
 * Requires ROUNDTABLE_HAS_CUDA. Without it, frames go through the
 * CPU upload path.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rt {

// Forward declarations
class CudaContext;

/// Represents a shared CUDA↔Vulkan memory allocation (linear buffer).
/// Contains raw handles — use move semantics for ownership transfer.
struct SharedAllocation
{
    void*    cudaDevicePtr{nullptr};  // CUdeviceptr (linear GPU buffer)
    void*    vulkanBuffer{nullptr};   // VkBuffer
    void*    vulkanMemory{nullptr};   // VkDeviceMemory (exportable)
    void*    cudaExtMemory{nullptr};  // CUexternalMemory
    void*    winHandle{nullptr};      // HANDLE (Win32 for export)
    uint32_t width{0};
    uint32_t height{0};
    size_t   sizeBytes{0};           // Total buffer size (W*H*3/2 for NV12)
    size_t   allocSize{0};           // Actual VkDeviceMemory allocation size
    bool     valid{false};

    SharedAllocation() = default;

    // Move semantics — ownership transfer for pooled allocations
    SharedAllocation(SharedAllocation&& other) noexcept = default;
    SharedAllocation& operator=(SharedAllocation&& other) noexcept = default;

    // Prevent accidental copies (raw pointers would be shallow-copied)
    SharedAllocation(const SharedAllocation&) = delete;
    SharedAllocation& operator=(const SharedAllocation&) = delete;
};

class CudaVulkanInterop
{
public:
    explicit CudaVulkanInterop(CudaContext& cuda);
    ~CudaVulkanInterop();

    // Non-copyable
    CudaVulkanInterop(const CudaVulkanInterop&) = delete;
    CudaVulkanInterop& operator=(const CudaVulkanInterop&) = delete;

    /// Initialize interop — requires Vulkan device with external memory extensions.
    bool init(void* vkDevice, void* vkPhysicalDevice);

    /// Shut down and free all shared allocations.
    void shutdown();

    /// Check if interop is available and initialized.
    [[nodiscard]] bool isAvailable() const noexcept { return m_available; }

    /// Acquire a shared NV12 buffer accessible from both CUDA and Vulkan.
    /// Prefers returning a pooled allocation with matching dimensions.
    /// Size = width * height * 3 / 2 bytes (NV12 format).
    [[nodiscard]] std::unique_ptr<SharedAllocation> allocate(
        uint32_t width, uint32_t height);

    /// Release a shared allocation back to the pool (or destroy if pool is full).
    void free(std::unique_ptr<SharedAllocation> alloc);

    /// Set the maximum number of pooled (idle) allocations.
    /// Default is computed from GPU VRAM in init() (typically 8–16).
    void setPoolCapacity(size_t cap) noexcept { m_poolCapacity = cap; }

    /// Copy NV12 planes from CUDA device memory to the shared Vulkan buffer.
    /// srcY/srcUV are raw CUDA device pointers (CUdeviceptr cast to void*).
    /// This is a GPU→GPU copy — no PCIe transfer involved.
    /// The shared buffer layout: Y plane at offset 0 (W×H bytes),
    ///   UV plane at offset W*H (W*H/2 bytes).
    /// Signals an external timeline semaphore when done (no CPU stall).
    /// Use vkSemaphore() / lastSignalValue() to pass to Vulkan wait.
    bool copyNv12FromCuda(SharedAllocation& alloc,
                          const void* srcY, int yPitch,
                          const void* srcUV, int uvPitch,
                          uint32_t width, uint32_t height);

    /// The Vulkan timeline semaphore signaled after each CUDA copy.
    /// Pass this to vkQueueSubmit waitSemaphores so Vulkan waits for CUDA.
    [[nodiscard]] void* vkSemaphore() const noexcept { return m_vkSemaphore; }

    /// The timeline value most recently signaled by CUDA.
    [[nodiscard]] uint64_t lastSignalValue() const noexcept { return m_semaphoreValue; }

private:
    CudaContext& m_cuda;
    bool         m_available{false};
    void*        m_vkDevice{nullptr};
    void*        m_vkPhysicalDevice{nullptr};

    // Vulkan function pointer (loaded via vkGetDeviceProcAddr)
    void*        m_pfnGetMemoryWin32Handle{nullptr};
    void*        m_pfnGetSemaphoreWin32Handle{nullptr};

    // Device-local memory type index compatible with external export
    uint32_t     m_memoryTypeIndex{UINT32_MAX};

    // Cached memory properties (queried once in init(), reused in allocations)
    void*        m_memPropsRaw{nullptr};  // VkPhysicalDeviceMemoryProperties

    // ── CUDA↔Vulkan timeline semaphore ───────────────────────────────
    void*        m_vkSemaphore{nullptr};         // VkSemaphore (timeline)
    void*        m_cudaExternalSemaphore{nullptr}; // CUexternalSemaphore
    void*        m_semaphoreWinHandle{nullptr};  // HANDLE (Win32)
    uint64_t     m_semaphoreValue{0};            // monotonically increasing

    // Track live allocations for cleanup
    std::vector<SharedAllocation*> m_liveAllocations;

    // ── Allocation pool (reuse instead of alloc/free per frame) ──────
    std::vector<std::unique_ptr<SharedAllocation>> m_pool;
    size_t m_poolCapacity{4};

    /// Actually create a new shared allocation (bypasses pool).
    [[nodiscard]] std::unique_ptr<SharedAllocation> allocateNew(
        uint32_t width, uint32_t height);

    /// Actually destroy a shared allocation (bypasses pool).
    void freeImmediate(std::unique_ptr<SharedAllocation> alloc);

    // ── Allocation helpers (break down allocateNew) ──────────────────
    [[nodiscard]] bool createVulkanBuffer(SharedAllocation& alloc, void* vkDevice);
    [[nodiscard]] uint32_t findCompatibleMemoryType(SharedAllocation& alloc, void* vkDevice,
                                                     void* vkPhysicalDevice);
    [[nodiscard]] bool allocateAndBindMemory(SharedAllocation& alloc, void* vkDevice,
                                              uint32_t memTypeIdx);
    [[nodiscard]] bool exportWin32Handle(SharedAllocation& alloc, void* vkDevice);
    /// Import Vulkan memory into CUDA via cuImportExternalMemory.
    /// CUDA-only — does not require VkDevice.
    [[nodiscard]] bool importIntoCuda(SharedAllocation& alloc);

    // ── Semaphore helpers ────────────────────────────────────────────
    bool createTimelineSemaphore(void* vkDevice);
    void destroyTimelineSemaphore();
};

} // namespace rt
