/*
 * GpuContext — Application-wide Vulkan context singleton.
 *
 * Owns the core Vulkan objects shared by all GPU subsystems:
 *   - Instance (validation layers, extensions)
 *   - Device   (physical + logical device, queues)
 *   - Allocator (VMA)
 *   - CommandPool (compute queue)
 *
 * Both the timeline viewport (real-time compositing) and the export
 * path (FrameRenderer) share the same Vulkan device through this context.
 *
 * Lifecycle:
 *   1. App::init() calls GpuContext::instance().init()
 *   2. Subsystems grab handles via GpuContext::instance()
 *   3. App destructor calls GpuContext::instance().shutdown()
 *
 * Thread safety: init/shutdown must be called from the main thread.
 * After init, read-only accessors are safe from any thread.
 */

#pragma once

#include "ICompositor.h"
#include "vulkan/Instance.h"
#include "vulkan/Device.h"
#include "vulkan/Allocator.h"
#include "vulkan/CommandPool.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace rt {

class Compositor;
class EffectProcessor;
class Nv12Converter;
class SpineRenderer;
class TransitionRenderer;

class CudaVulkanInterop;
class GpuResourceManager;

/// GPU device health state for device-lost recovery (Phase 2.B).
enum class GpuState : uint8_t {
    Healthy,      ///< Normal operation
    DeviceLost,   ///< GPU device lost detected (e.g. VK_ERROR_DEVICE_LOST)
    Recovering,   ///< Recovery in progress (tryRecover active)
    Failed        ///< Recovery failed — fall back to CPU safe mode
};

class GpuContext
{
public:
    /// Get the singleton instance.
    static GpuContext& get() noexcept;

    // Non-copyable, non-movable
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /// Initialize Vulkan (instance, device, allocator, command pool).
    /// @param surface  Optional surface for present-queue selection
    ///                 (pass VK_NULL_HANDLE if no window surface yet).
    /// @return true on success.
    bool init(VkSurfaceKHR surface = VK_NULL_HANDLE);

    /// Shut down all Vulkan resources (reverse order).
    void shutdown();

    /// Is the GPU context ready?
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // ── Device-lost recovery (Phase 2.B) ────────────────────────────────

    /// Current GPU device health state.
    [[nodiscard]] GpuState gpuState() const noexcept {
        return m_gpuState.load(std::memory_order_acquire);
    }

    /// True when GPU is healthy and operational.
    [[nodiscard]] bool isOperational() const noexcept {
        return m_gpuState.load(std::memory_order_acquire) == GpuState::Healthy;
    }

    /// Attempt full GPU re-initialization after device lost.
    /// Returns true on success, false if recovery failed.
    bool tryRecover();

    /// Signal that a GPU error (e.g. VK_ERROR_DEVICE_LOST) was detected.
    /// Safe to call from any thread. Next composite call will attempt recovery.
    void signalDeviceLost() noexcept {
        GpuState expected = GpuState::Healthy;
        m_gpuState.compare_exchange_strong(expected, GpuState::DeviceLost);
    }

    // ── Accessors ───────────────────────────────────────────────────────

    [[nodiscard]] Instance&    vkInstance()  noexcept { return m_instance; }
    [[nodiscard]] Device&      device()      noexcept { return m_device; }
    [[nodiscard]] Allocator&   allocator()   noexcept { return m_allocator; }
    [[nodiscard]] CommandPool& cmdPool()     noexcept { return m_cmdPool; }

    /// Command pool for the graphics queue family (needed for render passes).
    /// Falls back to m_cmdPool if graphics and compute share a queue family.
    [[nodiscard]] CommandPool& graphicsCmdPool() noexcept { return m_graphicsCmdPool.handle() ? m_graphicsCmdPool : m_cmdPool; }

    [[nodiscard]] const Instance&    vkInstance()  const noexcept { return m_instance; }
    [[nodiscard]] const Device&      device()      const noexcept { return m_device; }
    [[nodiscard]] const Allocator&   allocator()   const noexcept { return m_allocator; }
    [[nodiscard]] const CommandPool& cmdPool()     const noexcept { return m_cmdPool; }
    [[nodiscard]] const CommandPool& graphicsCmdPool() const noexcept { return m_graphicsCmdPool.handle() ? m_graphicsCmdPool : m_cmdPool; }

    /// Convenience: the compute queue used for compositing / effects.
    [[nodiscard]] VkQueue computeQueue() const noexcept { return m_device.computeQueue(); }

    /// Convenience: the graphics queue used for rendering.
    [[nodiscard]] VkQueue graphicsQueue() const noexcept { return m_device.graphicsQueue(); }

    /// Get the mutex used to serialise submit calls on the compute queue.
    [[nodiscard]] std::mutex& computeQueueMutex() const noexcept { return m_computeQueueMutex; }

    /// Get the mutex used to serialise submit calls on the graphics queue.
    [[nodiscard]] std::mutex& graphicsQueueMutex() const noexcept { return m_graphicsQueueMutex; }

    /// Graphics queue family index (for creating per-thread CommandPools).
    [[nodiscard]] uint32_t graphicsQueueFamilyIndex() const noexcept
    {
        return m_device.queueFamilies().graphics.value_or(0);
    }

    /// Convenience: the raw VkDevice handle.
    [[nodiscard]] VkDevice vkDevice() const noexcept { return m_device.handle(); }

    /// Convenience: the raw VkPhysicalDevice handle.
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return m_device.physicalDevice(); }

    // ── Shared Compositor ───────────────────────────────────────────────

    /// Get or lazily create the shared compositor.
    /// The compositor is configured for the given output resolution.
    ICompositor* compositor(uint32_t width = 1920, uint32_t height = 1080);

    // ── Shared EffectProcessor ──────────────────────────────────────────

    /// Get or lazily create the shared effect processor.
    /// Sized to match the given output resolution.
    EffectProcessor* effectProcessor(uint32_t width = 1920, uint32_t height = 1080);

    // ── Shared SpineRenderer ────────────────────────────────────────────

    /// Get or lazily create the shared Spine renderer.
    /// Sized to match the given output resolution.
    SpineRenderer* spineRenderer(uint32_t width = 1920, uint32_t height = 1080);

    // ── Shared TransitionRenderer ───────────────────────────────────────

    /// Get or lazily create the shared transition renderer.
    /// Sized to match the given output resolution.
    TransitionRenderer* transitionRenderer(uint32_t width = 1920, uint32_t height = 1080);

    // ── Shared Nv12Converter ────────────────────────────────────────────

    /// Get or lazily create the shared NV12 → BGRA GPU converter.
    /// Sized to match the given frame resolution.
    Nv12Converter* nv12Converter(uint32_t width = 1920, uint32_t height = 1080);

    // ── Shared CudaVulkanInterop ────────────────────────────────────────

    /// Get or lazily create the shared CUDA↔Vulkan interop module.
    /// Returns nullptr if CUDA is not available.
    CudaVulkanInterop* cudaVulkanInterop();

    /// True if the NVIDIA CUDA driver is available on this system.
    /// Quick check — does not initialize the full CUDA context.
    [[nodiscard]] bool cudaAvailable() const noexcept;

    // ── Resource Manager ─────────────────────────────────────────────

    /// Get the shared GPU resource manager (fence policies, staging ring, stats).
    [[nodiscard]] GpuResourceManager& resourceManager() noexcept { return *m_resourceManager; }
    [[nodiscard]] const GpuResourceManager& resourceManager() const noexcept { return *m_resourceManager; }

    // ── Utilities ───────────────────────────────────────────────────────

    /// Read back BGRA pixels from a GPU texture to a CPU vector.
    /// The texture must be in SHADER_READ_ONLY_OPTIMAL layout.
    /// Returns true on success.  Safe to call from any thread
    /// (uses computeQueue with internal cmd buffer).
    bool readbackTexture(void* texturePtr,
                         uint32_t width, uint32_t height,
                         std::vector<uint8_t>& outPixels);

private:
    GpuContext() = default;
    ~GpuContext();

    Instance    m_instance;
    Device      m_device;
    Allocator   m_allocator;
    CommandPool m_cmdPool;          ///< compute queue family
    CommandPool m_graphicsCmdPool;    ///< graphics queue family (if different)

    std::unique_ptr<ICompositor>        m_compositor;
    std::unordered_map<uint64_t, std::unique_ptr<EffectProcessor>> m_effectProcessors;
    std::unique_ptr<SpineRenderer>      m_spineRenderer;
    std::unique_ptr<TransitionRenderer> m_transitionRenderer;
    std::unordered_map<uint64_t, std::unique_ptr<Nv12Converter>> m_nv12Converters;
    std::unique_ptr<CudaVulkanInterop>   m_cudaVulkanInterop;
    std::unique_ptr<GpuResourceManager>  m_resourceManager;

    bool m_initialized{false};
    std::atomic<GpuState> m_gpuState{GpuState::Healthy};
    mutable std::mutex m_graphicsQueueMutex;  ///< Serialise graphics queue submits
    mutable std::mutex m_computeQueueMutex;   ///< Serialise compute queue submits
    mutable std::mutex m_subsystemMutex;      ///< Serialise lazy subsystem creation

    uint64_t m_effectProcessorRequests{0};
    uint64_t m_effectProcessorCacheHits{0};
    uint64_t m_effectProcessorCreations{0};
    uint64_t m_nv12ConverterRequests{0};
    uint64_t m_nv12ConverterCacheHits{0};
    uint64_t m_nv12ConverterCreations{0};
};

} // namespace rt
