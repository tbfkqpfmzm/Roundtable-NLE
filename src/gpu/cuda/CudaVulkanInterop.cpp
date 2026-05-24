/*
 * CudaVulkanInterop.cpp — CUDA↔Vulkan shared memory
 *
 * Two implementations:
 *   1. ROUNDTABLE_HAS_CUDA: VK_KHR_external_memory + CUDA external memory
 *   2. Fallback: stub — frames go through CPU upload
 */

#include "CudaVulkanInterop.h"
#include "CudaContext.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_CUDA

#include <cuda.h>
#include <volk.h>

#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rt {

CudaVulkanInterop::CudaVulkanInterop(CudaContext& cuda)
    : m_cuda(cuda)
{
}

CudaVulkanInterop::~CudaVulkanInterop()
{
    shutdown();
}

bool CudaVulkanInterop::init(void* vkDevice, void* vkPhysicalDevice)
{
    if (!m_cuda.isAvailable()) {
        spdlog::warn("CudaVulkanInterop: CUDA not available");
        return false;
    }

    auto device   = static_cast<VkDevice>(vkDevice);
    auto physDev  = static_cast<VkPhysicalDevice>(vkPhysicalDevice);

    m_vkDevice         = vkDevice;
    m_vkPhysicalDevice = vkPhysicalDevice;

#ifdef _WIN32
    // ── Load vkGetMemoryWin32HandleKHR ──────────────────────────────
    auto pfn = vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR");
    if (!pfn) {
        spdlog::warn("CudaVulkanInterop: vkGetMemoryWin32HandleKHR not available "
                     "(VK_KHR_external_memory_win32 not enabled?)");
        return false;
    }
    m_pfnGetMemoryWin32Handle = reinterpret_cast<void*>(pfn);

    // ── Load vkGetSemaphoreWin32HandleKHR ───────────────────────────
    auto semPfn = vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR");
    if (!semPfn) {
        spdlog::warn("CudaVulkanInterop: vkGetSemaphoreWin32HandleKHR not "
                     "available — falling back to cuStreamSynchronize");
    }
    m_pfnGetSemaphoreWin32Handle = reinterpret_cast<void*>(semPfn);

    // ── Cache memory properties (queried once, reused in allocations) ─
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    // Store in a small struct so we can reuse without re-querying
    m_memPropsRaw = std::malloc(sizeof(memProps));
    std::memcpy(m_memPropsRaw, &memProps, sizeof(memProps));

    m_memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        const auto flags = memProps.memoryTypes[i].propertyFlags;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            m_memoryTypeIndex = i;
            break;
        }
    }
    if (m_memoryTypeIndex == UINT32_MAX) {
        spdlog::warn("CudaVulkanInterop: no device-local memory type found");
        return false;
    }

    // ── Adaptive pool capacity based on GPU VRAM ────────────────────
    // ~1.5 MB per 1080p frame, ~6 MB per 4K frame
    // Scale: 8 pools for <4 GB VRAM, 12 for 4–8 GB, 16 for >8 GB
    size_t vramGB = m_cuda.deviceInfo().totalMemory / (1024 * 1024 * 1024);
    if (vramGB >= 8)       m_poolCapacity = 16;
    else if (vramGB >= 4)  m_poolCapacity = 12;
    else                   m_poolCapacity = 8;

    spdlog::info("CudaVulkanInterop: pool capacity set to {} ({:.0f} GB VRAM)",
                 m_poolCapacity, static_cast<double>(vramGB));

    m_available = true;

    // ── Create CUDA↔Vulkan timeline semaphore ──────────────────────
    if (m_pfnGetSemaphoreWin32Handle) {
        createTimelineSemaphore(m_vkDevice);
    }

    spdlog::info("CudaVulkanInterop: initialized (zero-copy NVDEC → Vulkan, "
                 "memType={})", m_memoryTypeIndex);
    return true;
#else
    spdlog::warn("CudaVulkanInterop: external memory only supported on Windows");
    return false;
#endif
}

// ── Semaphore helpers ─────────────────────────────────────────────────

bool CudaVulkanInterop::createTimelineSemaphore(void* vkDevice)
{
    auto device = static_cast<VkDevice>(vkDevice);

    // Timeline semaphore with export support
    VkSemaphoreTypeCreateInfo timelineCI{};
    timelineCI.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineCI.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCI.initialValue  = 0;

    VkExportSemaphoreCreateInfo exportCI{};
    exportCI.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exportCI.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    exportCI.pNext       = &timelineCI;

    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semCI.pNext = &exportCI;

    VkSemaphore sem{VK_NULL_HANDLE};
    if (vkCreateSemaphore(device, &semCI, nullptr, &sem) != VK_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: failed to create timeline semaphore");
        return false;
    }

    // Export Win32 handle
    VkSemaphoreGetWin32HandleInfoKHR getHandleInfo{};
    getHandleInfo.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    getHandleInfo.semaphore = sem;
    getHandleInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    auto getHandleFn = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        m_pfnGetSemaphoreWin32Handle);

    HANDLE winHandle{nullptr};
    if (getHandleFn(device, &getHandleInfo, &winHandle) != VK_SUCCESS || !winHandle) {
        spdlog::warn("CudaVulkanInterop: failed to export semaphore handle");
        vkDestroySemaphore(device, sem, nullptr);
        return false;
    }

    // Import into CUDA
    CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc{};
    desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32;
    desc.handle.win32.handle = winHandle;

    CUexternalSemaphore cudaSem{};
    CUresult cuRes = cuImportExternalSemaphore(&cudaSem, &desc);
    if (cuRes != CUDA_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: cuImportExternalSemaphore "
                     "failed ({}) — falling back to sync",
                     static_cast<int>(cuRes));
        CloseHandle(winHandle);
        vkDestroySemaphore(device, sem, nullptr);
        return false;
    }

    m_vkSemaphore            = sem;
    m_cudaExternalSemaphore  = cudaSem;
    m_semaphoreWinHandle     = winHandle;
    m_semaphoreValue         = 0;

    spdlog::info("CudaVulkanInterop: timeline semaphore created "
                 "(async CUDA→Vulkan sync)");
    return true;
}

void CudaVulkanInterop::destroyTimelineSemaphore()
{
    if (m_cudaExternalSemaphore) {
        cuDestroyExternalSemaphore(
            static_cast<CUexternalSemaphore>(m_cudaExternalSemaphore));
        m_cudaExternalSemaphore = nullptr;
    }
    if (m_semaphoreWinHandle) {
        CloseHandle(static_cast<HANDLE>(m_semaphoreWinHandle));
        m_semaphoreWinHandle = nullptr;
    }
    if (m_vkSemaphore && m_vkDevice) {
        vkDestroySemaphore(static_cast<VkDevice>(m_vkDevice),
                           static_cast<VkSemaphore>(m_vkSemaphore), nullptr);
        m_vkSemaphore = nullptr;
    }
    m_semaphoreValue = 0;
}

// ── Shutdown ──────────────────────────────────────────────────────────

void CudaVulkanInterop::shutdown()
{
    // Free any remaining live allocations
    for (auto* alloc : m_liveAllocations) {
        if (alloc && alloc->valid) {
            alloc->valid = false;
        }
    }
    m_liveAllocations.clear();

    // Destroy pooled allocations
    for (auto& pooled : m_pool)
        freeImmediate(std::move(pooled));
    m_pool.clear();

    // ── Destroy CUDA↔Vulkan timeline semaphore ─────────────────────
    destroyTimelineSemaphore();

    // Free cached memory properties
    if (m_memPropsRaw) {
        std::free(m_memPropsRaw);
        m_memPropsRaw = nullptr;
    }

    m_available = false;
}

// ── Diagnostic snapshot ───────────────────────────────────────────────

CudaVulkanInterop::Stats CudaVulkanInterop::stats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return Stats{ m_liveAllocations.size(), m_pool.size() };
}

// ── Pool-based allocator ──────────────────────────────────────────────

std::unique_ptr<SharedAllocation> CudaVulkanInterop::allocate(
    uint32_t width, uint32_t height, bool tenBit)
{
    if (!m_available) return nullptr;

    // Fast path under lock: look for a pooled allocation with matching
    // dimensions AND bit-depth.  An NV12 alloc must NEVER satisfy a P010
    // request (size mismatch + the copy routines key on the bit depth
    // for plane offsets).  Lock is dropped before allocateNew() so the
    // slow (Vulkan + CUDA import) path doesn't serialise concurrent
    // first-allocations across workers.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_pool.begin(); it != m_pool.end(); ++it) {
            if ((*it)->width == width && (*it)->height == height
                && (*it)->tenBit == tenBit)
            {
                auto alloc = std::move(*it);
                m_pool.erase(it);
                m_liveAllocations.push_back(alloc.get());
                spdlog::debug("CudaVulkanInterop: reusing pooled {}x{} {}buffer",
                              width, height, tenBit ? "P010 " : "");
                return alloc;
            }
        }
    }

    // No match — create a fresh allocation.  allocateNew acquires the
    // mutex internally for its m_liveAllocations.push_back.
    return allocateNew(width, height, tenBit);
}

void CudaVulkanInterop::free(std::unique_ptr<SharedAllocation> alloc)
{
    if (!alloc || !alloc->valid) return;

    // Decide under lock whether this allocation returns to the pool or
    // gets destroyed.  freeImmediate runs OUTSIDE the lock because it
    // calls into the Vulkan + CUDA destructors, which can take
    // milliseconds and don't touch our shared state.
    std::unique_ptr<SharedAllocation> toDestroy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = std::find(m_liveAllocations.begin(), m_liveAllocations.end(),
                            alloc.get());
        if (it != m_liveAllocations.end())
            m_liveAllocations.erase(it);

        if (m_pool.size() < m_poolCapacity) {
            spdlog::debug("CudaVulkanInterop: returning {}x{} buffer to pool "
                          "(pool size {}→{})",
                          alloc->width, alloc->height,
                          m_pool.size(), m_pool.size() + 1);
            m_pool.push_back(std::move(alloc));
        } else {
            toDestroy = std::move(alloc);
        }
    }

    if (toDestroy)
        freeImmediate(std::move(toDestroy));
}

// ── Allocation helpers ────────────────────────────────────────────────

bool CudaVulkanInterop::createVulkanBuffer(SharedAllocation& alloc, void* vkDevice)
{
    auto device = static_cast<VkDevice>(vkDevice);

    VkExternalMemoryBufferCreateInfoKHR extBufInfo{};
    extBufInfo.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO_KHR;
    extBufInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.pNext = &extBufInfo;
    bufInfo.size  = alloc.sizeBytes;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VkBuffer buffer{VK_NULL_HANDLE};
    if (vkCreateBuffer(device, &bufInfo, nullptr, &buffer) != VK_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: vkCreateBuffer failed");
        return false;
    }
    alloc.vulkanBuffer = buffer;
    return true;
}

uint32_t CudaVulkanInterop::findCompatibleMemoryType(SharedAllocation& alloc,
    void* vkDevice, void* /*vkPhysicalDevice*/)
{
    auto device = static_cast<VkDevice>(vkDevice);

    VkMemoryRequirements memReqs{};
    vkGetBufferMemoryRequirements(device,
        static_cast<VkBuffer>(alloc.vulkanBuffer), &memReqs);

    // Use cached memory properties instead of re-querying each allocation
    auto* memProps = static_cast<VkPhysicalDeviceMemoryProperties*>(m_memPropsRaw);

    uint32_t memTypeIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps->memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) &&
            (memProps->memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            memTypeIdx = i;
            break;
        }
    }

    if (memTypeIdx == UINT32_MAX) {
        spdlog::warn("CudaVulkanInterop: no compatible device-local memory type");
    }
    return memTypeIdx;
}

bool CudaVulkanInterop::allocateAndBindMemory(SharedAllocation& alloc,
    void* vkDevice, uint32_t memTypeIdx)
{
    auto device = static_cast<VkDevice>(vkDevice);

    if (memTypeIdx == UINT32_MAX) return false;

    VkExportMemoryAllocateInfoKHR exportInfo{};
    exportInfo.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_KHR;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext           = &exportInfo;
    allocInfo.allocationSize  = alloc.allocSize;
    allocInfo.memoryTypeIndex = memTypeIdx;

    VkDeviceMemory memory{VK_NULL_HANDLE};
    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: vkAllocateMemory failed");
        return false;
    }
    alloc.vulkanMemory = memory;

    if (vkBindBufferMemory(device,
            static_cast<VkBuffer>(alloc.vulkanBuffer), memory, 0) != VK_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: vkBindBufferMemory failed");
        vkFreeMemory(device, memory, nullptr);
        return false;
    }
    return true;
}

bool CudaVulkanInterop::exportWin32Handle(SharedAllocation& alloc, void* vkDevice)
{
    auto device = static_cast<VkDevice>(vkDevice);

    VkMemoryGetWin32HandleInfoKHR handleInfo{};
    handleInfo.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    handleInfo.memory     = static_cast<VkDeviceMemory>(alloc.vulkanMemory);
    handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    auto getHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        m_pfnGetMemoryWin32Handle);

    HANDLE winHandle{nullptr};
    if (getHandle(device, &handleInfo, &winHandle) != VK_SUCCESS || !winHandle) {
        spdlog::warn("CudaVulkanInterop: vkGetMemoryWin32HandleKHR failed");
        return false;
    }
    alloc.winHandle = winHandle;
    return true;
}

bool CudaVulkanInterop::importIntoCuda(SharedAllocation& alloc)
{
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc{};
    extMemDesc.type                = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
    extMemDesc.handle.win32.handle = static_cast<HANDLE>(alloc.winHandle);
    extMemDesc.size                = alloc.allocSize;

    CUexternalMemory extMem{};
    CUresult cuRes = cuImportExternalMemory(&extMem, &extMemDesc);
    if (cuRes != CUDA_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: cuImportExternalMemory failed ({})",
                     static_cast<int>(cuRes));
        return false;
    }
    alloc.cudaExtMemory = extMem;

    // Map to CUdeviceptr
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc{};
    bufDesc.offset = 0;
    bufDesc.size   = alloc.sizeBytes;

    CUdeviceptr devPtr{};
    cuRes = cuExternalMemoryGetMappedBuffer(&devPtr, extMem, &bufDesc);
    if (cuRes != CUDA_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: cuExternalMemoryGetMappedBuffer "
                     "failed ({})", static_cast<int>(cuRes));
        cuDestroyExternalMemory(extMem);
        return false;
    }
    alloc.cudaDevicePtr = reinterpret_cast<void*>(devPtr);
    return true;
}

// ── allocateNew — orchestrates the helpers ────────────────────────────

std::unique_ptr<SharedAllocation> CudaVulkanInterop::allocateNew(
    uint32_t width, uint32_t height, bool tenBit)
{
    if (!m_available) return nullptr;

#ifdef _WIN32
    m_cuda.pushContext();

    auto device = static_cast<VkDevice>(m_vkDevice);

    auto alloc = std::make_unique<SharedAllocation>();
    alloc->width     = width;
    alloc->height    = height;
    alloc->tenBit    = tenBit;
    // NV12: Y(W*H) + UV(W*H/2)        = W*H*3/2
    // P010: Y(W*H*2) + UV(W*H/2 * 2)  = W*H*3 (each sample = 2 bytes)
    alloc->sizeBytes = tenBit
        ? static_cast<size_t>(width) * height * 3
        : static_cast<size_t>(width) * height * 3 / 2;

    // Step 1: Create VkBuffer with external memory support
    if (!createVulkanBuffer(*alloc, m_vkDevice)) {
        m_cuda.popContext();
        return nullptr;
    }

    // Step 2: Query memory requirements
    VkMemoryRequirements memReqs{};
    vkGetBufferMemoryRequirements(device,
        static_cast<VkBuffer>(alloc->vulkanBuffer), &memReqs);
    alloc->allocSize = memReqs.size;

    // Step 3: Find compatible memory type
    uint32_t memTypeIdx = findCompatibleMemoryType(*alloc, m_vkDevice, m_vkPhysicalDevice);
    if (memTypeIdx == UINT32_MAX) {
        vkDestroyBuffer(device, static_cast<VkBuffer>(alloc->vulkanBuffer), nullptr);
        m_cuda.popContext();
        return nullptr;
    }

    // Step 4: Allocate and bind memory
    if (!allocateAndBindMemory(*alloc, m_vkDevice, memTypeIdx)) {
        vkDestroyBuffer(device, static_cast<VkBuffer>(alloc->vulkanBuffer), nullptr);
        m_cuda.popContext();
        return nullptr;
    }

    // Step 5: Export Win32 handle
    if (!exportWin32Handle(*alloc, m_vkDevice)) {
        vkFreeMemory(device, static_cast<VkDeviceMemory>(alloc->vulkanMemory), nullptr);
        vkDestroyBuffer(device, static_cast<VkBuffer>(alloc->vulkanBuffer), nullptr);
        m_cuda.popContext();
        return nullptr;
    }

    // Step 6: Import into CUDA and map
    if (!importIntoCuda(*alloc)) {
        CloseHandle(static_cast<HANDLE>(alloc->winHandle));
        vkFreeMemory(device, static_cast<VkDeviceMemory>(alloc->vulkanMemory), nullptr);
        vkDestroyBuffer(device, static_cast<VkBuffer>(alloc->vulkanBuffer), nullptr);
        m_cuda.popContext();
        return nullptr;
    }

    alloc->valid = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_liveAllocations.push_back(alloc.get());
    }

    spdlog::debug("CudaVulkanInterop: allocated {}x{} shared {} buffer "
                  "({} bytes, CUDA ptr={:#x})",
                  width, height, tenBit ? "P010" : "NV12", alloc->sizeBytes,
                  reinterpret_cast<CUdeviceptr>(alloc->cudaDevicePtr));

    m_cuda.popContext();
    return alloc;
#else
    return nullptr;
#endif
}

// ── copyNv12FromCuda ──────────────────────────────────────────────────

bool CudaVulkanInterop::copyNv12FromCuda(SharedAllocation& alloc,
    const void* srcY, int yPitch,
    const void* srcUV, int uvPitch,
    uint32_t width, uint32_t height)
{
#ifdef ROUNDTABLE_HAS_CUDA
    if (!alloc.valid || !alloc.cudaDevicePtr || !srcY || !srcUV)
        return false;

    m_cuda.pushContext();

    CUdeviceptr dstPtr   = reinterpret_cast<CUdeviceptr>(alloc.cudaDevicePtr);
    CUdeviceptr srcYPtr  = reinterpret_cast<CUdeviceptr>(srcY);
    CUdeviceptr srcUVPtr = reinterpret_cast<CUdeviceptr>(srcUV);

    // ── Copy Y plane: W×H bytes ─────────────────────────────────────
    CUDA_MEMCPY2D copyY{};
    copyY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copyY.srcDevice     = srcYPtr;
    copyY.srcPitch      = static_cast<size_t>(yPitch);
    copyY.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copyY.dstDevice     = dstPtr;
    copyY.dstPitch      = width;
    copyY.WidthInBytes  = width;
    copyY.Height        = height;

    CUresult res = cuMemcpy2D(&copyY);
    if (res != CUDA_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: cuMemcpy2D Y plane failed ({})",
                     static_cast<int>(res));
        m_cuda.popContext();
        return false;
    }

    // ── Copy UV plane: W × H/2 bytes (NV12 interleaved UV) ──────────
    const VkDeviceSize uvOffset = static_cast<VkDeviceSize>(width) * height;
    CUDA_MEMCPY2D copyUV{};
    copyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copyUV.srcDevice     = srcUVPtr;
    copyUV.srcPitch      = static_cast<size_t>(uvPitch);
    copyUV.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copyUV.dstDevice     = dstPtr + uvOffset;
    copyUV.dstPitch      = width;
    copyUV.WidthInBytes  = width;
    copyUV.Height        = height / 2;

    res = cuMemcpy2D(&copyUV);
    if (res != CUDA_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: cuMemcpy2D UV plane failed ({})",
                     static_cast<int>(res));
        m_cuda.popContext();
        return false;
    }

    // ── Signal external semaphore (async) or fall back to sync ──────
    //
    // The increment + signal pair MUST be atomic across workers — both
    // because a timeline semaphore can only be signalled with
    // monotonically-increasing values, and because callers immediately
    // read lastSignalValue() to wire the value into their Vulkan
    // submit's VkTimelineSemaphoreSubmitInfo.  Two workers racing here
    // without the lock would (a) both observe the same post-increment
    // value, (b) issue duplicate signals on the same timeline value
    // (undefined behaviour per the CUDA external-semaphore docs), and
    // (c) leave at least one Vulkan wait permanently pending — which
    // is the exact failure mode the 02:14:02 → "ZC=0" perf line in
    // logs/perf_log.txt captured.
    if (m_cudaExternalSemaphore) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t signalVal =
            m_semaphoreValue.fetch_add(1, std::memory_order_acq_rel) + 1;
        CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signalParams{};
        signalParams.params.fence.value = signalVal;
        auto cudaSem = static_cast<CUexternalSemaphore>(m_cudaExternalSemaphore);
        CUresult sigRes = cuSignalExternalSemaphoresAsync(
            &cudaSem, &signalParams, 1, nullptr);
        if (sigRes != CUDA_SUCCESS) {
            spdlog::warn("CudaVulkanInterop: cuSignalExternalSemaphoresAsync "
                         "failed ({}) — falling back to sync",
                         static_cast<int>(sigRes));
            cuStreamSynchronize(nullptr);
        }
    } else {
        cuStreamSynchronize(nullptr);
    }

    m_cuda.popContext();
    return true;
#else
    (void)alloc; (void)srcY; (void)yPitch; (void)srcUV; (void)uvPitch;
    (void)width; (void)height;
    return false;
#endif
}

// ── copyP010FromCuda ──────────────────────────────────────────────────
//
// UPGRADE_PLAN item 4: 10-bit twin of copyNv12FromCuda.  Each sample is
// 2 bytes (P010LE/P016LE — 10/16-bit luma packed in the upper bits of a
// 16-bit word).  Plane order matches NV12 (Y then interleaved UV at 4:2:0)
// but every byte count doubles:
//   Y plane:  W * H * 2 bytes at offset 0,    tight dst pitch = W * 2.
//   UV plane: W * H     bytes at offset W*H*2, tight dst pitch = W * 2.
// Source pitches arrive from NVDEC and may include alignment padding —
// cuMemcpy2D handles that via srcPitch.

bool CudaVulkanInterop::copyP010FromCuda(SharedAllocation& alloc,
    const void* srcY, int yPitch,
    const void* srcUV, int uvPitch,
    uint32_t width, uint32_t height)
{
#ifdef ROUNDTABLE_HAS_CUDA
    if (!alloc.valid || !alloc.tenBit || !alloc.cudaDevicePtr || !srcY || !srcUV)
        return false;

    m_cuda.pushContext();

    CUdeviceptr dstPtr   = reinterpret_cast<CUdeviceptr>(alloc.cudaDevicePtr);
    CUdeviceptr srcYPtr  = reinterpret_cast<CUdeviceptr>(srcY);
    CUdeviceptr srcUVPtr = reinterpret_cast<CUdeviceptr>(srcUV);

    const size_t dstPitchBytes = static_cast<size_t>(width) * 2;

    // ── Copy Y plane: W × H × 2 bytes ───────────────────────────────
    CUDA_MEMCPY2D copyY{};
    copyY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copyY.srcDevice     = srcYPtr;
    copyY.srcPitch      = static_cast<size_t>(yPitch);
    copyY.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copyY.dstDevice     = dstPtr;
    copyY.dstPitch      = dstPitchBytes;
    copyY.WidthInBytes  = dstPitchBytes;   // W luma samples × 2 bytes
    copyY.Height        = height;

    CUresult res = cuMemcpy2D(&copyY);
    if (res != CUDA_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: cuMemcpy2D P010 Y plane failed ({})",
                     static_cast<int>(res));
        m_cuda.popContext();
        return false;
    }

    // ── Copy UV plane: W × H/2 × 2 bytes (interleaved CbCr, 4:2:0) ──
    //
    // For P010 the chroma plane is W/2 columns of 16-bit (Cb, Cr) pairs.
    // Each row therefore occupies (W/2) * 2 * 2 = W * 2 bytes.  Height
    // is H/2 rows.  Total = W * H bytes — same overall size as the 8-bit
    // NV12 UV plane, but at half the column count and 16-bit samples.
    const VkDeviceSize uvOffset = static_cast<VkDeviceSize>(width) * height * 2;
    CUDA_MEMCPY2D copyUV{};
    copyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copyUV.srcDevice     = srcUVPtr;
    copyUV.srcPitch      = static_cast<size_t>(uvPitch);
    copyUV.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copyUV.dstDevice     = dstPtr + uvOffset;
    copyUV.dstPitch      = dstPitchBytes;
    copyUV.WidthInBytes  = dstPitchBytes;   // W/2 chroma pairs × 4 bytes = W*2
    copyUV.Height        = height / 2;

    res = cuMemcpy2D(&copyUV);
    if (res != CUDA_SUCCESS) {
        spdlog::warn("CudaVulkanInterop: cuMemcpy2D P010 UV plane failed ({})",
                     static_cast<int>(res));
        m_cuda.popContext();
        return false;
    }

    // ── Signal external semaphore (matches copyNv12FromCuda) ────────
    if (m_cudaExternalSemaphore) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t signalVal =
            m_semaphoreValue.fetch_add(1, std::memory_order_acq_rel) + 1;
        CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signalParams{};
        signalParams.params.fence.value = signalVal;
        auto cudaSem = static_cast<CUexternalSemaphore>(m_cudaExternalSemaphore);
        CUresult sigRes = cuSignalExternalSemaphoresAsync(
            &cudaSem, &signalParams, 1, nullptr);
        if (sigRes != CUDA_SUCCESS) {
            spdlog::warn("CudaVulkanInterop: P010 cuSignalExternalSemaphoresAsync "
                         "failed ({}) — falling back to sync",
                         static_cast<int>(sigRes));
            cuStreamSynchronize(nullptr);
        }
    } else {
        cuStreamSynchronize(nullptr);
    }

    m_cuda.popContext();
    return true;
#else
    (void)alloc; (void)srcY; (void)yPitch; (void)srcUV; (void)uvPitch;
    (void)width; (void)height;
    return false;
#endif
}

// ── freeImmediate ─────────────────────────────────────────────────────

void CudaVulkanInterop::freeImmediate(std::unique_ptr<SharedAllocation> alloc)
{
    if (!alloc || !alloc->valid) return;

#ifdef _WIN32
    auto device = static_cast<VkDevice>(m_vkDevice);

    if (alloc->cudaExtMemory) {
        cuDestroyExternalMemory(
            static_cast<CUexternalMemory>(alloc->cudaExtMemory));
    }
    if (alloc->winHandle) {
        CloseHandle(static_cast<HANDLE>(alloc->winHandle));
    }
    if (alloc->vulkanBuffer) {
        vkDestroyBuffer(device,
            static_cast<VkBuffer>(alloc->vulkanBuffer), nullptr);
    }
    if (alloc->vulkanMemory) {
        vkFreeMemory(device,
            static_cast<VkDeviceMemory>(alloc->vulkanMemory), nullptr);
    }

    spdlog::debug("CudaVulkanInterop: destroyed {}x{} shared buffer",
                  alloc->width, alloc->height);
#endif
}

} // namespace rt

#else // !ROUNDTABLE_HAS_CUDA

namespace rt {

CudaVulkanInterop::CudaVulkanInterop(CudaContext& cuda) : m_cuda(cuda) {}
CudaVulkanInterop::~CudaVulkanInterop() = default;

bool CudaVulkanInterop::init(void* /*vkDevice*/, void* /*vkPhysicalDevice*/)
{
    spdlog::info("CudaVulkanInterop: not available (no CUDA Toolkit). "
                 "Frames upload via CPU staging buffer.");
    return false;
}

void CudaVulkanInterop::shutdown() {}

std::unique_ptr<SharedAllocation> CudaVulkanInterop::allocate(uint32_t, uint32_t, bool)
{
    return nullptr;
}

void CudaVulkanInterop::free(std::unique_ptr<SharedAllocation>) {}

std::unique_ptr<SharedAllocation> CudaVulkanInterop::allocateNew(uint32_t, uint32_t, bool)
{
    return nullptr;
}

void CudaVulkanInterop::freeImmediate(std::unique_ptr<SharedAllocation>) {}

CudaVulkanInterop::Stats CudaVulkanInterop::stats() const { return Stats{}; }

} // namespace rt

#endif // ROUNDTABLE_HAS_CUDA