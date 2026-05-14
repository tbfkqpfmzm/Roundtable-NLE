/*
 * VulkanViewport.cpp â€” Zero-copy GPU viewport via native swapchain.
 *
 * Uses GpuContext's Vulkan device to create a Win32 surface + swapchain,
 * then renders the compositor output via a fullscreen textured quad.
 * Falls back to CPU QPainter if GPU init fails.
 */

#include "viewport/VulkanViewport.h"
#include "Theme.h"
#include "GpuContext.h"
#include "GpuScheduler.h"
#include "vulkan/Swapchain.h"
#include "vulkan/Texture.h"
#include "media/FrameCache.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <QPainter>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace rt {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Helpers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

#if 0 // Currently unused
static std::vector<uint32_t> loadSpirv(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint32_t> spirv(static_cast<size_t>(size) / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(spirv.data()), size);
    return spirv;
}

static VkShaderModule createShaderModule(VkDevice device,
                                          const std::vector<uint32_t>& spirv)
{
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode    = spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}
#endif // unused shader helpers

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Construction / Destruction
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

VulkanViewport::VulkanViewport(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(160, 90);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Dark background for CPU fallback path
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Theme::colors().surface0);
    setPalette(pal);

    // Force this widget to become a native window (HWND) BEFORE initGpu().
    // createWindowContainer() inside initGpu() parents the Vulkan surface
    // HWND to the nearest native ancestor.  If VulkanViewport itself has
    // an HWND, the surface becomes a direct child and Win32 automatically
    // clips it to VulkanViewport's bounds — preventing the surface from
    // overflowing into sibling widgets (control bar, transport, etc.).
    setAttribute(Qt::WA_NativeWindow);
    winId();  // materialise the HWND now

    // Try to init GPU surface
    if (GpuContext::get().isInitialized()) {
        initGpu();
    }

    if (!m_gpuActive) {
        spdlog::info("VulkanViewport: GPU display not available, using CPU fallback");
    }

    // Resize debounce — see kResizeDebounceMs in the header.
    m_resizeDebounceTimer = new QTimer(this);
    m_resizeDebounceTimer->setSingleShot(true);
    m_resizeDebounceTimer->setInterval(kResizeDebounceMs);
    connect(m_resizeDebounceTimer, &QTimer::timeout, this, [this]() {
        if (!m_gpuActive) return;
        m_swapchainDirty = true;
        if (m_sourceView != VK_NULL_HANDLE)
            refresh();
        else
            presentClearFrame();
    });
}

VulkanViewport::~VulkanViewport()
{
    shutdownGpu();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  GPU Initialization
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Resize handling
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::handleResize()
{
    if (!m_gpuActive || !m_swapchain) return;

    auto& gpu = GpuContext::get();
    VkDevice device = gpu.vkDevice();

    // Wait only on the viewport's own in-flight submit before tearing down
    // its framebuffers/swapchain.  Previously we called vkDeviceWaitIdle()
    // here, which stalled the compute queue (compositor mid-frame) and the
    // transfer queue (upload manager).  Under a dock-animation resize storm
    // this happened many times per second and triggered VK_ERROR_DEVICE_LOST.
    // The viewport is the only consumer of these swapchain images, so its
    // own fence + the upload-slot fences are the only synchronization
    // points that matter for swapchain recreate.
    if (m_inFlightFence != VK_NULL_HANDLE) {
        vkWaitForFences(device, 1, &m_inFlightFence, VK_TRUE, 100'000'000);
    }
    // Drain CPU-upload slots — their command buffers reference textures
    // that we keep, not swapchain images, but their submits go through the
    // graphics queue and we want them off the queue before we destroy
    // framebuffers (the present submit waits on m_imageAvailable from a new
    // swapchain image).
    for (auto& slot : m_uploadSlots) {
        if (slot.fence != VK_NULL_HANDLE)
            vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 100'000'000);
    }

    // Destroy old framebuffers
    for (auto fb : m_framebuffers)
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    m_framebuffers.clear();

    // Recreate swapchain — use cached dimensions (thread-safe)
    uint32_t w = m_cachedWidth.load();
    uint32_t h = m_cachedHeight.load();
    m_swapchain->recreate(gpu.device(), w, h);

    // Recreate framebuffers
    createFramebuffers();
    m_swapchainDirty = false;

    // Invalidate the source image so stale composite texture views aren't
    // sampled at the new swapchain size.  The next displayGpuImage() call
    // will set m_imageDirty and update the descriptor set.
    m_sourceView    = VK_NULL_HANDLE;
    m_sourceSampler = VK_NULL_HANDLE;
    m_imageDirty    = true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  GPU Display
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::displayGpuImage(VkImageView imageView, VkSampler sampler,
                                      uint32_t imgWidth, uint32_t imgHeight,
                                      VkSemaphore waitSemaphore)
{
    m_frameWidth  = imgWidth;
    m_frameHeight = imgHeight;

    if (!m_gpuActive) {
        spdlog::warn("[DIAG-VIEWPORT] displayGpuImage: gpuActive=false, skipping");
        return;
    }

    // If GpuContext has transitioned to Failed (VK_ERROR_DEVICE_LOST), every
    // Vulkan handle in the caller's hands — including imageView and
    // waitSemaphore — points at a destroyed device.  Refusing here means
    // ProgramMonitor falls back to the CPU display path, which the safe-mode
    // compositor populates with valid pixels.
    if (!GpuContext::get().isOperational()) {
        return;
    }

    bool viewChanged = false;
    if (imageView != m_sourceView || sampler != m_sourceSampler) {
        m_sourceView    = imageView;
        m_sourceSampler = sampler;
        m_srcW          = imgWidth;
        m_srcH          = imgHeight;
        m_imageDirty    = true;
        viewChanged = true;
    }

    // DIAG: log viewport present attempts
    {
        static int s_vpLog = 0;
        if (++s_vpLog % 5 == 0) {
            spdlog::info("[DIAG-VIEWPORT] displayGpuImage: view=0x{:X} viewChanged={} "
                         "imageDirty={} {}x{}",
                         reinterpret_cast<uint64_t>(imageView), viewChanged,
                         m_imageDirty, imgWidth, imgHeight);
        }
    }

    presentFrame(waitSemaphore);
}

// ── SEH-safe Vulkan wrappers ───────────────────────────────────────────────
// These are plain free functions (no C++ object unwinding) so MSVC allows
// __try/__except.  NVIDIA App (nvspcap64.dll) hooks these Vulkan entry
// points and can crash with a null-pointer deref.

#ifdef _WIN32
static VkResult sehWaitForFences(VkDevice device, VkFence fence,
                                  uint64_t timeoutNs)
{
    __try
    {
        return vkWaitForFences(device, 1, &fence, VK_TRUE, timeoutNs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return VK_ERROR_UNKNOWN;
    }
}

// sehQueueSubmit moved into GpuScheduler::submit (P1.2).
#endif

void VulkanViewport::presentFrame(VkSemaphore waitSemaphore)
{
    std::lock_guard lock(m_presentMtx);

    if (!m_gpuActive || !m_swapchain || m_sourceView == VK_NULL_HANDLE) {
        recycleSemaphores();
        return;
    }

    auto& gpu = GpuContext::get();
    VkDevice device = gpu.vkDevice();

    // Wait for previous frame — bounded timeout.
    {
        auto fenceStart = std::chrono::steady_clock::now();
        VkResult fenceRes;

#ifdef _WIN32
        fenceRes = sehWaitForFences(device, m_inFlightFence, 16'000'000);
        if (fenceRes == VK_ERROR_UNKNOWN)
        {
            // SEH caught an access violation inside vkWaitForFences.  Either
            // a third-party hook (NVIDIA App, OBS) NULL-deref'd, or our own
            // device is dead.  Either way the only safe action is to mark
            // the device as Failed and let the fatal-failure path show the
            // restart dialog — we used to set m_swapchainDirty and limp
            // along forever pretending to work.
            spdlog::error("[VIEWPORT] Access violation in vkWaitForFences "
                          "— marking GPU Failed.");
            GpuContext::get().signalDeviceLost();
            GpuContext::get().tryRecover();  // fires fatal callback
            return;
        }
#else
        fenceRes = vkWaitForFences(device, 1, &m_inFlightFence,
                                    VK_TRUE, 16'000'000);
#endif

        if (fenceRes == VK_TIMEOUT) {
            spdlog::warn("[DIAG-VIEWPORT] presentFrame: fence TIMEOUT (16ms)");
            return;
        }
        auto fenceEnd = std::chrono::steady_clock::now();
        double fenceMs = std::chrono::duration<double, std::milli>(fenceEnd - fenceStart).count();
        static int s_fenceLog = 0;
        if (fenceMs > 2.0 || ++s_fenceLog % 30 == 0) {
            spdlog::info("[DIAG-VIEWPORT] presentFrame: fenceWait={:.1f}ms", fenceMs);
        }
    }

    // Recycle consumed semaphores (kept alive to avoid handle-reuse race
    // with compositor thread — nvoglv64.dll NULL-deref bug).
    recycleSemaphores();

    // Handle resize
    if (m_swapchainDirty || m_swapchain->needsRecreation()) {
        handleResize();
        if (!m_swapchain || m_swapchain->needsRecreation()) return;

        // handleResize() sets m_sourceView = VK_NULL_HANDLE to invalidate
        // stale composite textures.  We can't proceed with a NULL image
        // view — vkUpdateDescriptorSets would NULL-deref the driver.
        if (m_sourceView == VK_NULL_HANDLE)
            return;
    }

    // Acquire swapchain image
    uint32_t imageIndex = m_swapchain->acquireNextImage(device, m_imageAvailable,
                                                         5'000'000); // 5ms
    if (imageIndex == UINT32_MAX) {
        handleResize();
        return;
    }

    vkResetFences(device, 1, &m_inFlightFence);

    // Update descriptor set if source image changed
    if (m_imageDirty) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = m_sourceSampler ? m_sourceSampler : m_fallbackSampler;
        imgInfo.imageView   = m_sourceView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_descriptorSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        m_imageDirty = false;
    }

    // Record command buffer
    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_commandBuffer, &beginInfo);

    // Begin render pass
    VkClearValue clearVal{};
    clearVal.color = {{0.07f, 0.07f, 0.09f, 1.0f}};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_renderPass;
    rpBegin.framebuffer       = m_framebuffers[imageIndex];
    rpBegin.renderArea.extent = m_swapchain->extent();
    rpBegin.clearValueCount   = 1;
    rpBegin.pClearValues      = &clearVal;

    vkCmdBeginRenderPass(m_commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Bind pipeline and draw fullscreen quad
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // Apply zoom/pan: expanding the VkViewport beyond the swapchain extent
    // makes the triangle render larger, and the scissor clips to the window.
    float swW = static_cast<float>(m_swapchain->extent().width);
    float swH = static_cast<float>(m_swapchain->extent().height);

    // Compute aspect-ratio-aware viewport for the source image
    float imgAspect = (m_srcW > 0 && m_srcH > 0)
        ? static_cast<float>(m_srcW) / static_cast<float>(m_srcH)
        : 16.0f / 9.0f;
    float winAspect = swW / std::max(swH, 1.0f);

    // ~5% padding on each side (kFitPadding=0.95) so the frame never
    // touches the viewport edges.  Mirrors the same constant in
    // Viewport.cpp's CPU path; per CLAUDE_IMPROVEMENT_PLAN user request.
    // When the user zooms in (m_viewZoom > 1), the viewport rect
    // expands past the swapchain bounds and the scissor clips — i.e.
    // the padding only matters at fit (zoom = 1).
    constexpr float kFitPadding = 0.95f;

    float baseW, baseH, baseX, baseY;
    if (winAspect > imgAspect) {
        // Window wider than image — pillarbox
        baseH = swH * kFitPadding;
        baseW = baseH * imgAspect;
        baseX = (swW - baseW) * 0.5f;
        baseY = (swH - baseH) * 0.5f;
    } else {
        // Window taller than image — letterbox
        baseW = swW * kFitPadding;
        baseH = baseW / imgAspect;
        baseX = (swW - baseW) * 0.5f;
        baseY = (swH - baseH) * 0.5f;
    }

    VkViewport vp{};
    vp.x        = baseX + m_viewPanX + (1.0f - m_viewZoom) * baseW * 0.5f;
    vp.y        = baseY + m_viewPanY + (1.0f - m_viewZoom) * baseH * 0.5f;
    vp.width    = baseW * m_viewZoom;
    vp.height   = baseH * m_viewZoom;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;

    // Store normalized (0-1) rect — render thread only writes floats,
    // UI thread reads them in computeFrameRect().  No widget access here.
    m_gpuFrameRect = QRectF(
        static_cast<double>(vp.x     / swW),
        static_cast<double>(vp.y     / swH),
        static_cast<double>(vp.width / swW),
        static_cast<double>(vp.height/ swH));

    vkCmdSetViewport(m_commandBuffer, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.extent = m_swapchain->extent();
    vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);

    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    vkCmdDraw(m_commandBuffer, 3, 1, 0, 0); // fullscreen triangle

    vkCmdEndRenderPass(m_commandBuffer);
    vkEndCommandBuffer(m_commandBuffer);

    // Submit
    VkPipelineStageFlags waitStages[2] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // m_imageAvailable
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT           // compositor semaphore
    };
    VkSemaphore waitSemaphores[2] = { m_imageAvailable, VK_NULL_HANDLE };
    uint32_t waitCount = 1;
    if (waitSemaphore != VK_NULL_HANDLE) {
        waitSemaphores[1] = waitSemaphore;
        waitCount = 2;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = waitCount;
    submitInfo.pWaitSemaphores      = waitSemaphores;
    submitInfo.pWaitDstStageMask    = waitStages;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &m_commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &m_renderFinished;

    {
        // P1.2: route through GpuScheduler.  The SEH guard that used
        // to live here is now built into the scheduler, so a driver-
        // side access violation still surfaces as VK_ERROR_UNKNOWN
        // (which the scheduler logs as "SEH access violation"); we
        // pick that up here and escalate to device-lost.
        rt::GpuSubmission sub{};
        sub.cmd                  = m_commandBuffer;
        sub.queue                = rt::GpuQueueKind::Graphics;
        sub.waitSemaphoreCount   = submitInfo.waitSemaphoreCount;
        sub.waitSemaphores       = submitInfo.pWaitSemaphores;
        sub.waitStages           = submitInfo.pWaitDstStageMask;
        sub.signalSemaphoreCount = 1;
        sub.signalSemaphores     = &m_renderFinished;
        sub.completionFence      = m_inFlightFence;
        sub.tag                  = "VulkanViewport::presentFrame";

        VkResult submitRes = rt::GpuContext::get().scheduler().submit(sub);
#ifdef _WIN32
        if (submitRes == VK_ERROR_UNKNOWN) {
            spdlog::error("[VIEWPORT] Access violation in vkQueueSubmit "
                          "— marking GPU Failed.");
            GpuContext::get().signalDeviceLost();
            GpuContext::get().tryRecover();
            return;
        }
#else
        (void)submitRes;
#endif

        // Present runs on the present queue (not the scheduler's set yet
        // — the swapchain owns it directly).  Future P1.x revision can
        // pull this into the scheduler too once we model present as a
        // submission kind.
        m_swapchain->present(gpu.device().presentQueue(), imageIndex, m_renderFinished);
    }

    // Recycle the just-used inter-queue semaphore by storing it so it
    // stays alive (never destroyed mid-frame).  Semaphores are destroyed
    // only during shutdownGpu() to avoid a handle-reuse race with the
    // compositor thread (nvoglv64.dll NULL-deref at +0xf39ed4).
    if (waitSemaphore != VK_NULL_HANDLE)
        m_recycledSemaphores.push_back(waitSemaphore);

    emit frameDisplayed();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Deferred semaphore recycling
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::recycleSemaphores()
{
    if (m_recycledSemaphores.empty())
        return;
    // Return semaphores to GpuContext's shared binary-semaphore pool so
    // CompositeEngine's next acquireFrameSemaphore() can reuse them
    // instead of allocating a fresh VkSemaphore every frame (~60/sec of
    // leaked handles over a session — eventually starves the driver).
    //
    // SAFETY: recycleSemaphores() is called at the top of presentFrame(),
    // immediately AFTER vkWaitForFences on m_inFlightFence has succeeded.
    // That fence corresponds to the PREVIOUS frame's submit, whose wait
    // stage already consumed any semaphore that was pushed into this list
    // from a still-earlier frame.  Anything currently in
    // m_recycledSemaphores is therefore in the unsignaled state and safe
    // to hand to the compositor for re-signaling.
    //
    // We deliberately do NOT destroy them: that was the path that
    // previously caused the nvoglv64.dll NULL-deref handle-reuse race.
    // Pool reuse avoids destroy/create entirely, so the race can't fire.
    auto& gpu = GpuContext::get();
    for (VkSemaphore sem : m_recycledSemaphores)
        gpu.releaseBinarySemaphore(sem);
    m_recycledSemaphores.clear();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  CPU Fallback Display
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::displayFrame(std::shared_ptr<CachedFrame> frame)
{
    if (!frame || !frame->ensurePixels()) {
        // Don't auto-clear — let the caller (ProgramMonitor) decide
        // whether to clear or keep the previous frame.
        return;
    }

    auto uploadT0 = std::chrono::steady_clock::now();

    m_frameWidth  = frame->width;
    m_frameHeight = frame->height;

    if (m_gpuActive) {
        // GPU-active mode: upload CPU pixels to a private texture and display
        // via the normal GPU path.  This is used during async playback where
        // the compositor output texture is not safe to sample directly (the
        // render thread may overwrite it).  Cost: ~1ms for 960Ã—540 BGRA.
        //
        // THREAD SAFETY: We use VulkanViewport's own m_commandPool (graphics
        // queue family) for the upload instead of GpuContext::cmdPool() which
        // the async render thread uses concurrently for compositing.
        auto& ctx = GpuContext::get();
        VkDevice device = ctx.vkDevice();
        const VkDeviceSize dataSize = static_cast<VkDeviceSize>(frame->pixels.size());

        if (m_uploadSlots.empty()) {
            spdlog::warn("VulkanViewport: no upload slots available");
            return;
        }

        auto& slot = m_uploadSlots[m_nextUploadSlot];
        m_nextUploadSlot = (m_nextUploadSlot + 1) % m_uploadSlots.size();

        VkResult slotFenceRes = vkWaitForFences(device, 1, &slot.fence,
                                                VK_TRUE, 2'000'000);
        if (slotFenceRes == VK_TIMEOUT) {
            return;
        }

        // Free staging buffer from previous upload on this slot (now safe —
        // fence signaled means GPU finished reading).
        if (slot.pendingStagingBuffer != VK_NULL_HANDLE && slot.pendingStagingAllocator) {
            vmaDestroyBuffer(static_cast<VmaAllocator>(slot.pendingStagingAllocator),
                             slot.pendingStagingBuffer,
                             static_cast<VmaAllocation>(slot.pendingStagingAlloc));
            slot.pendingStagingBuffer    = VK_NULL_HANDLE;
            slot.pendingStagingAlloc     = nullptr;
            slot.pendingStagingAllocator = nullptr;
        }

        if (!slot.texture || slot.texture->width() != frame->width ||
            slot.texture->height() != frame->height) {
            slot.texture = std::make_unique<Texture>();
        }

        const bool needCreate = slot.texture->image() == VK_NULL_HANDLE ||
                                slot.texture->width() != frame->width ||
                                slot.texture->height() != frame->height;

        VkCommandBuffer cmd = slot.commandBuffer;
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        Texture::StagingCleanup staging{};
        bool uploadOk = false;
        if (needCreate) {
            // Use R8G8B8A8 (not B8G8R8A8) so the sampler sees the same
            // byteâ†’component mapping as the compositor output image.
            // quad.frag's .bgra swizzle then corrects both paths identically.
            uploadOk = slot.texture->createFromDataBatched(
                ctx.allocator().handle(), device,
                TextureConfig{
                    .width  = frame->width,
                    .height = frame->height,
                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                    .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                },
                frame->pixels.data(), dataSize, cmd, staging);
        } else {
            uploadOk = slot.texture->updateDataBatched(
                frame->pixels.data(), dataSize, cmd, staging);
        }

        vkEndCommandBuffer(cmd);

        if (uploadOk) {
            vkResetFences(device, 1, &slot.fence);
            {
                // P1.2: route through GpuScheduler (was: raw vkQueueSubmit
                // under ctx.graphicsQueueMutex()).
                rt::GpuSubmission sub{};
                sub.cmd             = cmd;
                sub.queue           = rt::GpuQueueKind::Graphics;
                sub.completionFence = slot.fence;
                sub.tag             = "VulkanViewport::uploadCpu";
                VkResult submitResult = rt::GpuContext::get().scheduler().submit(sub);
                if (submitResult != VK_SUCCESS) {
                    spdlog::warn("VulkanViewport: upload submit failed (VkResult={})", static_cast<int>(submitResult));
                    uploadOk = false;
                }
            }

            if (uploadOk) {
                // Bounded wait for upload — avoids blocking the UI thread
                // indefinitely.  If we timeout, the staging buffer stays
                // alive in the slot until the fence is signaled on next reuse.
                VkResult uploadWait = vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 5'000'000); // 5ms
                if (uploadWait == VK_TIMEOUT) {
                    // Upload still in-flight — keep staging alive in slot for
                    // deferred cleanup when the fence is signaled next time.
                    slot.pendingStagingBuffer    = staging.buffer;
                    slot.pendingStagingAlloc     = staging.allocation;
                    slot.pendingStagingAllocator = staging.allocator;
                    staging.buffer = VK_NULL_HANDLE; // prevent destroy() from freeing
                    staging.allocation = nullptr;
                    staging.destroy();
                    return;
                }
            } else {
                // Submit failed — re-signal the fence so this slot stays usable.
                // Submit an empty command buffer to signal the fence.
                VkCommandBuffer emptyCmd = slot.commandBuffer;
                vkResetCommandBuffer(emptyCmd, 0);
                VkCommandBufferBeginInfo emptyBegin{};
                emptyBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                emptyBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(emptyCmd, &emptyBegin);
                vkEndCommandBuffer(emptyCmd);
                // P1.2: route through GpuScheduler.  Empty command
                // buffer just re-signals the slot's fence so the slot
                // stays usable after a failed upload.
                rt::GpuSubmission emptySub{};
                emptySub.cmd             = emptyCmd;
                emptySub.queue           = rt::GpuQueueKind::Graphics;
                emptySub.completionFence = slot.fence;
                emptySub.tag             = "VulkanViewport::uploadFenceResignal";
                rt::GpuContext::get().scheduler().submit(emptySub);
            }
        }

        staging.destroy();

        if (!uploadOk) {
            spdlog::warn("VulkanViewport: CPU upload texture data failed");
            return;
        }

        // Display the upload texture via the normal GPU path
        displayGpuImage(slot.texture->imageView(),
                        slot.texture->sampler(),
                        frame->width, frame->height);
        {
            double uploadMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - uploadT0).count();
            if (uploadMs > 3.0) {
                spdlog::info("[PERF] VulkanViewport::displayFrame(CPU): {:.1f}ms  {}x{}  slot={}",
                             uploadMs, frame->width, frame->height, m_nextUploadSlot);
            }
        }
        return;
    }

    uint32_t stride = frame->stride > 0 ? frame->stride : frame->width * 4;
    m_cpuFrameRef = std::move(frame);
    m_cpuImage = QImage(m_cpuFrameRef->pixels.data(),
                        static_cast<int>(m_cpuFrameRef->width),
                        static_cast<int>(m_cpuFrameRef->height),
                        static_cast<int>(stride),
                        QImage::Format_ARGB32);
    update();
}

void VulkanViewport::displayFrame(const CachedFrame& frame)
{
    if (frame.pixels.empty()) {
        clearFrame();
        return;
    }

    m_frameWidth  = frame.width;
    m_frameHeight = frame.height;

    if (!m_gpuActive) {
        uint32_t stride = frame.stride > 0 ? frame.stride : frame.width * 4;
        m_cpuImage = QImage(static_cast<int>(frame.width),
                            static_cast<int>(frame.height),
                            QImage::Format_ARGB32);
        for (uint32_t y = 0; y < frame.height; ++y) {
            std::memcpy(m_cpuImage.scanLine(static_cast<int>(y)),
                        frame.pixels.data() + y * stride,
                        static_cast<size_t>(frame.width) * 4);
        }
        m_cpuFrameRef.reset();
        update();
    }
}

void VulkanViewport::clearFrame()
{
    m_cpuImage = QImage();
    m_cpuFrameRef.reset();
    m_frameWidth  = 0;
    m_frameHeight = 0;
    m_sourceView  = VK_NULL_HANDLE;

    if (m_gpuActive) {
        presentClearFrame();
    } else {
        update();
    }
}

void VulkanViewport::presentClearFrame()
{
    std::lock_guard lock(m_presentMtx);

    if (!m_gpuActive || !m_swapchain) return;

    auto& gpu = GpuContext::get();
    VkDevice device = gpu.vkDevice();

    {
        VkResult fenceRes = vkWaitForFences(device, 1, &m_inFlightFence,
                                            VK_TRUE, 2'000'000);
        if (fenceRes == VK_TIMEOUT) {
            return;
        }
    }

    // Recycle consumed semaphores (kept alive to avoid handle-reuse race).
    recycleSemaphores();

    if (m_swapchainDirty || m_swapchain->needsRecreation()) {
        handleResize();
        if (!m_swapchain || m_swapchain->needsRecreation()) return;
    }

    uint32_t imageIndex = m_swapchain->acquireNextImage(device, m_imageAvailable,
                                                         5'000'000); // 5ms
    if (imageIndex == UINT32_MAX) {
        handleResize();
        return;
    }

    vkResetFences(device, 1, &m_inFlightFence);

    // Record command buffer with clear-only render pass (no draw)
    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_commandBuffer, &beginInfo);

    VkClearValue clearVal{};
    clearVal.color = {{0.07f, 0.07f, 0.09f, 1.0f}};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_renderPass;
    rpBegin.framebuffer       = m_framebuffers[imageIndex];
    rpBegin.renderArea.extent = m_swapchain->extent();
    rpBegin.clearValueCount   = 1;
    rpBegin.pClearValues      = &clearVal;

    vkCmdBeginRenderPass(m_commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(m_commandBuffer);
    vkEndCommandBuffer(m_commandBuffer);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    // P1.2: route through GpuScheduler.  This is the empty-clear path
    // used when no source frame is available — wait on image-available
    // and signal render-finished so the swapchain present chain stays
    // intact.
    rt::GpuSubmission sub{};
    sub.cmd                  = m_commandBuffer;
    sub.queue                = rt::GpuQueueKind::Graphics;
    sub.waitSemaphoreCount   = 1;
    sub.waitSemaphores       = &m_imageAvailable;
    sub.waitStages           = &waitStage;
    sub.signalSemaphoreCount = 1;
    sub.signalSemaphores     = &m_renderFinished;
    sub.completionFence      = m_inFlightFence;
    sub.tag                  = "VulkanViewport::clearPresent";
    rt::GpuContext::get().scheduler().submit(sub);

    m_swapchain->present(gpu.device().presentQueue(), imageIndex, m_renderFinished);

    emit frameDisplayed();
}

void VulkanViewport::refresh()
{
    if (m_gpuActive && m_sourceView != VK_NULL_HANDLE) {
        presentFrame();
    } else {
        update();
    }
}

void VulkanViewport::scheduleDeferredRedraw()
{
    // Coalesce: if a redraw is already queued, don't queue another.
    bool expected = false;
    if (!m_redrawQueued.compare_exchange_strong(expected, true))
        return;

    QMetaObject::invokeMethod(this, [this]() {
        m_redrawQueued.store(false, std::memory_order_release);
        if (m_gpuActive)
            refresh();
        else
            update();
    }, Qt::QueuedConnection);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Qt Events
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }

    if (m_gpuActive) { --s_paintDepth; return; } // GPU path handles its own rendering

    QPainter painter(this);
    painter.fillRect(rect(), Theme::colors().surface0);

    if (m_cpuImage.isNull()) { --s_paintDepth; return; }

    // Aspect-ratio-correct fit
    QSizeF imgSize(m_cpuImage.width(), m_cpuImage.height());
    QSizeF widgetSize(width(), height());
    imgSize.scale(widgetSize, Qt::KeepAspectRatio);

    QRectF drawRect(
        (width() - imgSize.width()) / 2.0,
        (height() - imgSize.height()) / 2.0,
        imgSize.width(), imgSize.height());

    painter.drawImage(drawRect, m_cpuImage);

    --s_paintDepth;
}

QSize VulkanViewport::nativeSurfaceSize() const
{
#ifdef _WIN32
    if (m_nativeWindow) {
        HWND hwnd = reinterpret_cast<HWND>(m_nativeWindow->winId());
        if (hwnd) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            int w = cr.right  - cr.left;
            int h = cr.bottom - cr.top;
            if (w > 0 && h > 0)
                return QSize(w, h);
        }
    }
#endif
    return size();  // fallback to QWidget logical size
}

void VulkanViewport::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;

    QWidget::resizeEvent(event);

    const QSize surfaceSize = nativeSurfaceSize();
    // Cache dimensions for thread-safe handleResize (called from render thread)
    m_cachedWidth.store(std::max(1u, static_cast<uint32_t>(surfaceSize.width())));
    m_cachedHeight.store(std::max(1u, static_cast<uint32_t>(surfaceSize.height())));

    // Debounce swapchain recreation. Dock animations / drags emit dozens of
    // resize events per second; recreating the swapchain on each one wedges
    // the driver. Restart the timer — when the user stops resizing for
    // kResizeDebounceMs, the slot will rebuild the swapchain once.
    if (m_gpuActive && m_resizeDebounceTimer) {
        m_resizeDebounceTimer->start();
    }
    emit resized();

    s_inResize = false;
}

void VulkanViewport::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);

    // Vulkan native surfaces don't follow Qt's normal paint/expose loop:
    // moving the widget (e.g. dock rearrange, floating-window drag) does
    // NOT fire resizeEvent or paintEvent, so the last-presented swapchain
    // content stays on screen at the OLD position, producing a visible
    // "echo" of stale UI alongside the new position.  Schedule a deferred
    // redraw so the surface refreshes once the move settles.  Coalesced
    // by scheduleDeferredRedraw so a rapid drag doesn't queue dozens of
    // present calls.
    if (m_gpuActive && m_sourceView != VK_NULL_HANDLE) {
        scheduleDeferredRedraw();
    }
}

QSize VulkanViewport::sizeHint() const
{
    return QSize(640, 360);
}

QSize VulkanViewport::minimumSizeHint() const
{
    return QSize(160, 90);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Zoom & Pan
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::resetZoomPan()
{
    m_viewZoom = 1.0f;
    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    scheduleDeferredRedraw();
    emit viewZoomChanged(m_viewZoom);
}

void VulkanViewport::setViewPan(float px, float py)
{
    m_viewPanX = px;
    m_viewPanY = py;
    scheduleDeferredRedraw();
}

void VulkanViewport::setViewZoom(float zoom)
{
    m_viewZoom = std::clamp(zoom, 0.1f, 20.0f);
    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    scheduleDeferredRedraw();
    emit viewZoomChanged(m_viewZoom);
}

void VulkanViewport::zoomToFill()
{
    float swW = static_cast<float>(width());
    float swH = static_cast<float>(height());
    float imgAspect = (m_srcW > 0 && m_srcH > 0)
        ? static_cast<float>(m_srcW) / static_cast<float>(m_srcH)
        : 16.0f / 9.0f;
    float winAspect = swW / std::max(swH, 1.0f);

    // Fill = zoom enough so the shorter dimension fills the window
    if (winAspect > imgAspect)
        m_viewZoom = winAspect / imgAspect;  // window wider → zoom to fill width
    else
        m_viewZoom = imgAspect / winAspect;  // window taller → zoom to fill height

    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    scheduleDeferredRedraw();
    emit viewZoomChanged(m_viewZoom);
}

void VulkanViewport::wheelEvent(QWheelEvent* event)
{
    float delta = static_cast<float>(event->angleDelta().y());
    if (delta == 0.0f) {
        event->ignore();
        return;
    }

    float factor = (delta > 0) ? 1.1f : (1.0f / 1.1f);
    float newZoom = std::clamp(m_viewZoom * factor, 0.1f, 20.0f);

    // Zoom toward mouse position â€” pan is relative to center
    // (rendering: vp.x = centerX - zoom*baseW/2 + panX)
    QPointF mousePos = event->position();
    float mx = static_cast<float>(mousePos.x()) - width()  * 0.5f;
    float my = static_cast<float>(mousePos.y()) - height() * 0.5f;

    float zoomRatio = newZoom / m_viewZoom;
    m_viewPanX += (1.0f - zoomRatio) * (mx - m_viewPanX);
    m_viewPanY += (1.0f - zoomRatio) * (my - m_viewPanY);

    m_viewZoom = newZoom;

    scheduleDeferredRedraw();

    emit viewZoomChanged(m_viewZoom);
    event->accept();
}

void VulkanViewport::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        resetZoomPan();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool VulkanViewport::eventFilter(QObject* watched, QEvent* event)
{
    // The native QWindow receives all input inside createWindowContainer.
    // Intercept wheel + double-click events and forward to our handlers.
    if (watched == m_nativeWindow) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            const QSize surfaceSize = nativeSurfaceSize();
            m_cachedWidth.store(std::max(1u, static_cast<uint32_t>(surfaceSize.width())));
            m_cachedHeight.store(std::max(1u, static_cast<uint32_t>(surfaceSize.height())));
            if (m_gpuActive && isVisible() && m_resizeDebounceTimer) {
                // Same debounce as QWidget::resizeEvent — drags fire many
                // resizes per second; recreating the swapchain on every one
                // wedges the driver.
                m_resizeDebounceTimer->start();
            }
            emit resized();
        }
        if (event->type() == QEvent::Wheel) {
            wheelEvent(static_cast<QWheelEvent*>(event));
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            mouseDoubleClickEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace rt
