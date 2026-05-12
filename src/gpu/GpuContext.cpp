/*
 * GpuContext.cpp — Application-wide Vulkan context singleton.
 */

#include "GpuContext.h"
#include "Compositor.h"
#include "EffectProcessor.h"
#include "GpuResourceManager.h"
#include "Nv12Converter.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"
#include "cuda/CudaVulkanInterop.h"
#include "cuda/CudaContext.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <vk_mem_alloc.h>
#include <volk.h>
#include <thread>
#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Singleton
// ═════════════════════════════════════════════════════════════════════════════

GpuContext& GpuContext::get() noexcept
{
    static GpuContext s_instance;
    return s_instance;
}

GpuContext::~GpuContext()
{
    shutdown();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

bool GpuContext::init(VkSurfaceKHR surface)
{
    if (m_initialized) return true;  // Already done

    spdlog::info("GpuContext: Initializing Vulkan...");

    // 1) Create Vulkan instance with surface extensions
    InstanceConfig instCfg;
    instCfg.extraExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef _WIN32
    instCfg.extraExtensions.push_back("VK_KHR_win32_surface");
#endif
    if (!m_instance.create(instCfg)) {
        spdlog::error("GpuContext: Failed to create Vulkan instance");
        return false;
    }
    spdlog::info("GpuContext: Vulkan instance created (validation={})",
                 m_instance.validationEnabled());

    // 2) Select physical device + create logical device
    if (!m_device.create(m_instance, surface)) {
        spdlog::error("GpuContext: Failed to create Vulkan device");
        m_instance.destroy();
        return false;
    }
    spdlog::info("GpuContext: Using GPU '{}' ({:.1f} GB VRAM)",
                 m_device.gpuInfo().name,
                 m_device.gpuInfo().vramSize / (1024.0 * 1024.0 * 1024.0));

    // 3) Create VMA allocator
    if (!m_allocator.create(m_instance, m_device)) {
        spdlog::error("GpuContext: Failed to create VMA allocator");
        m_device.destroy();
        m_instance.destroy();
        return false;
    }

    // 4) Create command pool for compute/graphics queue
    uint32_t computeFamily = m_device.queueFamilies().compute.value_or(
        m_device.queueFamilies().graphics.value_or(0));
    if (!m_cmdPool.create(m_device.handle(), computeFamily)) {
        spdlog::error("GpuContext: Failed to create command pool");
        m_allocator.destroy();
        m_device.destroy();
        m_instance.destroy();
        return false;
    }

    // 5) If graphics queue is on a different family, create a graphics
    //    command pool so SpineRenderer (which needs render passes) can
    //    submit to the graphics queue correctly.
    uint32_t graphicsFamily = m_device.queueFamilies().graphics.value_or(0);
    if (graphicsFamily != computeFamily) {
        if (!m_graphicsCmdPool.create(m_device.handle(), graphicsFamily)) {
            spdlog::error("GpuContext: Failed to create graphics command pool");
            m_cmdPool.destroy();
            m_allocator.destroy();
            m_device.destroy();
            m_instance.destroy();
            return false;
        }
        spdlog::info("GpuContext: Separate graphics command pool for family {}",
                     graphicsFamily);
    }

    m_initialized = true;
    m_gpuState.store(GpuState::Healthy, std::memory_order_release);
    spdlog::info("GpuContext: Vulkan initialization complete");

    // Initialize shared staging ring (64 MB — absorbs CompositeService's ring)
    m_resourceManager = std::make_unique<GpuResourceManager>();
    m_resourceManager->initStagingRing(m_allocator.handle(), 64u * 1024u * 1024u);

    // Eager-init TransitionRenderer at the default resolution so the
    // ~800 ms shader-compile stall happens here during startup instead
    // of on the first transition during playback.  Any later resize
    // request at a different resolution is handled via resize().
    transitionRenderer(1920, 1080);

    return true;
}

void GpuContext::shutdown()
{
    if (!m_initialized) return;

    spdlog::info("GpuContext: Shutting down Vulkan...");

    // Wait for GPU to finish
    if (m_device.handle())
        m_device.waitIdle();

    // Destroy in reverse order.
    // m_resourceManager must be destroyed BEFORE the VMA allocator and
    // device because its staging ring calls vmaUnmapMemory/vmaDestroyBuffer.
    // If destroyed later (during ~GpuContext static destructor at CRT exit),
    // the VMA allocator is already freed → ACCESS_VIOLATION in nvoglv64.dll.
    m_resourceManager.reset();
    m_cudaVulkanInterop.reset();
    m_nv12Converters.clear();
    m_transitionRenderer.reset();
    m_spineRenderer.reset();
    m_effectProcessors.clear();
    m_compositor.reset();
    m_graphicsCmdPool.destroy();
    m_cmdPool.destroy();
    m_allocator.destroy();
    m_device.destroy();
    m_instance.destroy();

    m_effectProcessorRequests = 0;
    m_effectProcessorCacheHits = 0;
    m_effectProcessorCreations = 0;
    m_nv12ConverterRequests = 0;
    m_nv12ConverterCacheHits = 0;
    m_nv12ConverterCreations = 0;

    m_initialized = false;
    spdlog::info("GpuContext: Vulkan shutdown complete");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Device-lost Recovery (Phase 2.B)
// ═════════════════════════════════════════════════════════════════════════════

bool GpuContext::tryRecover()
{
    GpuState expected = GpuState::DeviceLost;
    if (!m_gpuState.compare_exchange_strong(expected, GpuState::Recovering))
        return false;

    spdlog::warn("[GPU] Device lost — attempting recovery");

    // Step 1: Shutdown current GPU state
    // Note: this resets m_initialized to false
    shutdown();

    // Step 2: Wait a brief moment for driver to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Step 3: Full re-initialize (same init sequence as startup)
    bool ok = init();

    m_gpuState.store(ok ? GpuState::Healthy : GpuState::Failed,
                     std::memory_order_release);

    if (ok) {
        spdlog::info("[GPU] Recovery successful");
    } else {
        spdlog::error("[GPU] Recovery failed — falling back to safe mode");
    }
    return ok;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared Compositor
// ═════════════════════════════════════════════════════════════════════════════

ICompositor* GpuContext::compositor(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);

    if (!m_compositor) {
        auto* vkComp = new Compositor();
        m_compositor.reset(vkComp);

        CompositorConfig cfg;
        cfg.outputWidth  = width;
        cfg.outputHeight = height;

        VkQueue queue = m_device.computeQueue()
                            ? m_device.computeQueue()
                            : m_device.graphicsQueue();

        if (!vkComp->init(m_device, m_allocator, m_cmdPool, queue, cfg)) {
            spdlog::error("GpuContext: Failed to init shared Compositor");
            m_compositor.reset();
            return nullptr;
        }

        spdlog::info("GpuContext: Shared Compositor created ({}x{})", width, height);
    }
    else if (static_cast<Compositor*>(m_compositor.get())->outputWidth() != width ||
             static_cast<Compositor*>(m_compositor.get())->outputHeight() != height) {
        m_compositor->resize(width, height);
    }

    return m_compositor.get();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared EffectProcessor
// ═════════════════════════════════════════════════════════════════════════════

EffectProcessor* GpuContext::effectProcessor(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);
    ++m_effectProcessorRequests;

    const uint64_t key = (static_cast<uint64_t>(width) << 32)
                       | static_cast<uint64_t>(height);
    auto it = m_effectProcessors.find(key);
    if (it != m_effectProcessors.end()) {
        ++m_effectProcessorCacheHits;
        if (m_effectProcessorRequests % 120 == 0) {
            const uint64_t misses = m_effectProcessorRequests - m_effectProcessorCacheHits;
            const double hitRate = m_effectProcessorRequests > 0
                ? (100.0 * static_cast<double>(m_effectProcessorCacheHits)
                   / static_cast<double>(m_effectProcessorRequests))
                : 0.0;
            spdlog::info("[PERF] GpuContext EffectProcessor cache: req={} hit={} miss={} hitRate={:.1f}% entries={}",
                         m_effectProcessorRequests, m_effectProcessorCacheHits,
                         misses, hitRate, m_effectProcessors.size());
        }
        return it->second.get();
    }

    auto processor = std::make_unique<EffectProcessor>();

    EffectProcessorConfig cfg;
    cfg.width  = width;
    cfg.height = height;

    VkQueue queue = m_device.computeQueue()
                        ? m_device.computeQueue()
                        : m_device.graphicsQueue();

    if (!processor->init(m_device, m_allocator, m_cmdPool, queue, cfg)) {
        spdlog::error("GpuContext: Failed to init shared EffectProcessor ({}x{})",
                      width, height);
        return nullptr;
    }

    spdlog::info("GpuContext: Shared EffectProcessor created ({}x{})", width, height);
    auto* result = processor.get();
    m_effectProcessors.emplace(key, std::move(processor));
    ++m_effectProcessorCreations;
    spdlog::info("[PERF] GpuContext EffectProcessor cache MISS: {}x{} -> created entry {} (requests={}, hits={}, creations={})",
                 width, height, m_effectProcessors.size(),
                 m_effectProcessorRequests, m_effectProcessorCacheHits,
                 m_effectProcessorCreations);

    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared SpineRenderer
// ═════════════════════════════════════════════════════════════════════════════

SpineRenderer* GpuContext::spineRenderer(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);

    if (!m_spineRenderer) {
        m_spineRenderer = std::make_unique<SpineRenderer>();

        SpineRendererConfig cfg;
        cfg.renderWidth  = width;
        cfg.renderHeight = height;

        // When using async compute for the compositor, the spine FBO must be
        // accessible from both the graphics and compute queue families.
        auto& qf = m_device.queueFamilies();
        if (qf.graphics.has_value() && qf.compute.has_value() &&
            qf.graphics.value() != qf.compute.value()) {
            cfg.concurrentQueueFamilies = {qf.graphics.value(),
                                           qf.compute.value()};
        }

        // SpineRenderer needs a graphics queue for vertex/fragment shaders
        VkQueue queue = m_device.graphicsQueue()
                            ? m_device.graphicsQueue()
                            : m_device.computeQueue();

        // Must use the graphics-family command pool so command buffers can
        // be submitted to the graphics queue (queue family must match).
        if (!m_spineRenderer->init(m_device, m_allocator, graphicsCmdPool(), queue, cfg)) {
            spdlog::error("GpuContext: Failed to init shared SpineRenderer");
            m_spineRenderer.reset();
            return nullptr;
        }

        spdlog::info("GpuContext: Shared SpineRenderer created ({}x{})", width, height);
    }
    else if (m_spineRenderer->framebuffer().width() != width ||
             m_spineRenderer->framebuffer().height() != height) {
        m_spineRenderer->resize(width, height);
    }

    return m_spineRenderer.get();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared TransitionRenderer
// ═════════════════════════════════════════════════════════════════════════════

TransitionRenderer* GpuContext::transitionRenderer(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);

    if (!m_transitionRenderer) {
        m_transitionRenderer = std::make_unique<TransitionRenderer>();

        TransitionConfig cfg;
        cfg.outputWidth  = width;
        cfg.outputHeight = height;

        VkQueue queue = m_device.computeQueue()
                            ? m_device.computeQueue()
                            : m_device.graphicsQueue();

        if (!m_transitionRenderer->init(m_device, m_allocator, m_cmdPool, queue, cfg)) {
            spdlog::error("GpuContext: Failed to init shared TransitionRenderer");
            m_transitionRenderer.reset();
            return nullptr;
        }

        spdlog::info("GpuContext: Shared TransitionRenderer created ({}x{})", width, height);
    }
    else if (m_transitionRenderer->outputWidth() != width ||
             m_transitionRenderer->outputHeight() != height) {
        m_transitionRenderer->resize(width, height);
    }

    return m_transitionRenderer.get();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared Nv12Converter
// ═════════════════════════════════════════════════════════════════════════════

Nv12Converter* GpuContext::nv12Converter(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);
    ++m_nv12ConverterRequests;

    const uint64_t key = (static_cast<uint64_t>(width) << 32)
                       | static_cast<uint64_t>(height);
    auto it = m_nv12Converters.find(key);
    if (it != m_nv12Converters.end()) {
        ++m_nv12ConverterCacheHits;
        if (m_nv12ConverterRequests % 120 == 0) {
            const uint64_t misses = m_nv12ConverterRequests - m_nv12ConverterCacheHits;
            const double hitRate = m_nv12ConverterRequests > 0
                ? (100.0 * static_cast<double>(m_nv12ConverterCacheHits)
                   / static_cast<double>(m_nv12ConverterRequests))
                : 0.0;
            spdlog::info("[PERF] GpuContext Nv12Converter cache: req={} hit={} miss={} hitRate={:.1f}% entries={}",
                         m_nv12ConverterRequests, m_nv12ConverterCacheHits,
                         misses, hitRate, m_nv12Converters.size());
        }
        return it->second.get();
    }

    auto converter = std::make_unique<Nv12Converter>();

    Nv12ConverterConfig cfg;
    cfg.width  = width;
    cfg.height = height;

    VkQueue queue = m_device.computeQueue()
                        ? m_device.computeQueue()
                        : m_device.graphicsQueue();

    if (!converter->init(m_device, m_allocator, m_cmdPool, queue, cfg)) {
        spdlog::warn("GpuContext: Nv12Converter init failed for {}x{} — falling back to CPU sws_scale",
                     width, height);
        return nullptr;
    }

    spdlog::info("GpuContext: Shared Nv12Converter created ({}x{})", width, height);
    auto* result = converter.get();
    m_nv12Converters.emplace(key, std::move(converter));
    ++m_nv12ConverterCreations;
    spdlog::info("[PERF] GpuContext Nv12Converter cache MISS: {}x{} -> created entry {} (requests={}, hits={}, creations={})",
                 width, height, m_nv12Converters.size(),
                 m_nv12ConverterRequests, m_nv12ConverterCacheHits,
                 m_nv12ConverterCreations);

    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared CudaVulkanInterop
// ═════════════════════════════════════════════════════════════════════════════

CudaVulkanInterop* GpuContext::cudaVulkanInterop()
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);

#ifdef ROUNDTABLE_HAS_CUDA
    if (!m_cudaVulkanInterop) {
        // Need a CudaContext — create one if not yet available
        static CudaContext s_cudaCtx;
        if (!s_cudaCtx.isAvailable()) {
            if (!s_cudaCtx.init()) {
                spdlog::warn("GpuContext: CUDA init failed — "
                             "CudaVulkanInterop not available");
                return nullptr;
            }
        }

        m_cudaVulkanInterop = std::make_unique<CudaVulkanInterop>(s_cudaCtx);
        if (!m_cudaVulkanInterop->init(m_device.handle(),
                                       m_device.physicalDevice())) {
            spdlog::warn("GpuContext: CudaVulkanInterop init failed");
            m_cudaVulkanInterop.reset();
            return nullptr;
        }

        spdlog::info("GpuContext: CudaVulkanInterop created");
    }
    return m_cudaVulkanInterop.get();
#else
    return nullptr;
#endif
}

bool GpuContext::cudaAvailable() const noexcept
{
#ifdef ROUNDTABLE_HAS_CUDA
    // Quick check: can we load the NVIDIA driver DLL?
    HMODULE mod = LoadLibraryExW(L"nvcuda.dll", nullptr,
                                 LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (mod) {
        FreeLibrary(mod);
        return true;
    }
    return false;
#else
    return false;
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
//  Texture readback utility
// ═════════════════════════════════════════════════════════════════════════════

bool GpuContext::readbackTexture(void* texturePtr,
                                  uint32_t width, uint32_t height,
                                  std::vector<uint8_t>& outPixels)
{
    if (!m_initialized || !texturePtr) return false;

    auto* tex = static_cast<Texture*>(texturePtr);
    const VkDeviceSize bufSz = static_cast<VkDeviceSize>(width) * height * 4;

    // Create CPU-visible staging buffer
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = bufSz;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer sb{VK_NULL_HANDLE};
    VmaAllocation sa{nullptr};
    if (vmaCreateBuffer(m_allocator.handle(), &bci, &aci,
                        &sb, &sa, nullptr) != VK_SUCCESS)
        return false;

    // Record copy commands
    VkCommandBuffer cmd = m_cmdPool.beginSingleTime();

    tex->transitionLayout(cmd,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent      = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, tex->image(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb, 1, &rgn);

    tex->transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    m_cmdPool.endSingleTime(cmd, computeQueue());

    // Map, copy, cleanup
    outPixels.resize(static_cast<size_t>(bufSz));
    void* mapped = nullptr;
    vmaMapMemory(m_allocator.handle(), sa, &mapped);
    std::memcpy(outPixels.data(), mapped, bufSz);
    vmaUnmapMemory(m_allocator.handle(), sa);
    vmaDestroyBuffer(m_allocator.handle(), sb, sa);

    return true;
}

} // namespace rt
