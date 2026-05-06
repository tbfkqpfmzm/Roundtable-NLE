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
    vkDeviceWaitIdle(device);

    // Destroy old framebuffers
    for (auto fb : m_framebuffers)
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    m_framebuffers.clear();

    // Recreate swapchain â€” use cached dimensions (thread-safe)
    uint32_t w = m_cachedWidth.load();
    uint32_t h = m_cachedHeight.load();
    m_swapchain->recreate(gpu.device(), w, h);

    // Recreate framebuffers
    createFramebuffers();
    m_swapchainDirty = false;
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

void VulkanViewport::presentFrame(VkSemaphore waitSemaphore)
{
    std::lock_guard lock(m_presentMtx);

    if (!m_gpuActive || !m_swapchain || m_sourceView == VK_NULL_HANDLE) {
        return;
    }

    auto& gpu = GpuContext::get();
    VkDevice device = gpu.vkDevice();

    // Wait for previous frame — bounded timeout.  During playback the
    // presenter thread (not the Qt event loop) calls this, so up to
    // 16ms is acceptable.  The previous 2ms timeout caused silent frame
    // drops when the GPU was busy with compositor texture uploads (~10ms
    // for GPU-cache-miss frames), producing visible judder.
    {
        auto fenceStart = std::chrono::steady_clock::now();
        VkResult fenceRes = vkWaitForFences(device, 1, &m_inFlightFence,
                                            VK_TRUE, 16'000'000); // 16ms in ns
        if (fenceRes == VK_TIMEOUT) {
            spdlog::warn("[DIAG-VIEWPORT] presentFrame: fence TIMEOUT (16ms)");
            return; // previous frame still in flight — skip
        }
        auto fenceEnd = std::chrono::steady_clock::now();
        double fenceMs = std::chrono::duration<double, std::milli>(fenceEnd - fenceStart).count();
        static int s_fenceLog = 0;
        if (fenceMs > 2.0 || ++s_fenceLog % 30 == 0) {
            spdlog::info("[DIAG-VIEWPORT] presentFrame: fenceWait={:.1f}ms", fenceMs);
        }
    }

    // Handle resize
    if (m_swapchainDirty || m_swapchain->needsRecreation()) {
        handleResize();
        if (!m_swapchain || m_swapchain->needsRecreation()) return;
    }

    // Acquire swapchain image â€” bounded timeout prevents UI thread stall
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

    float baseW, baseH, baseX, baseY;
    if (winAspect > imgAspect) {
        // Window wider than image â€” pillarbox
        baseH = swH;
        baseW = swH * imgAspect;
        baseX = (swW - baseW) * 0.5f;
        baseY = 0.0f;
    } else {
        // Window taller than image â€” letterbox
        baseW = swW;
        baseH = swW / imgAspect;
        baseX = 0.0f;
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
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT              // compositor semaphore
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
        std::lock_guard qLock(gpu.graphicsQueueMutex());
        vkQueueSubmit(gpu.graphicsQueue(), 1, &submitInfo, m_inFlightFence);
        // Present inside mutex â€” presentQueue may alias graphicsQueue
        m_swapchain->present(gpu.device().presentQueue(), imageIndex, m_renderFinished);
    }

    emit frameDisplayed();
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
                TextureConfig{frame->width, frame->height,
                              VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT},
                frame->pixels.data(), dataSize, cmd, staging);
        } else {
            uploadOk = slot.texture->updateDataBatched(
                frame->pixels.data(), dataSize, cmd, staging);
        }

        vkEndCommandBuffer(cmd);

        if (uploadOk) {
            VkSubmitInfo submitInfo{};
            submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers    = &cmd;
            vkResetFences(device, 1, &slot.fence);
            {
                std::lock_guard qLock(ctx.graphicsQueueMutex());
                VkResult submitResult = vkQueueSubmit(ctx.graphicsQueue(), 1, &submitInfo, slot.fence);
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
                VkSubmitInfo emptySubmit{};
                emptySubmit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                emptySubmit.commandBufferCount = 1;
                emptySubmit.pCommandBuffers    = &emptyCmd;
                {
                    std::lock_guard qLock(ctx.graphicsQueueMutex());
                    vkQueueSubmit(ctx.graphicsQueue(), 1, &emptySubmit, slot.fence);
                }
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
    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = &m_imageAvailable;
    submitInfo.pWaitDstStageMask    = &waitStage;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &m_commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &m_renderFinished;

    {
        std::lock_guard qLock(gpu.graphicsQueueMutex());
        vkQueueSubmit(gpu.graphicsQueue(), 1, &submitInfo, m_inFlightFence);
        m_swapchain->present(gpu.device().presentQueue(), imageIndex, m_renderFinished);
    }

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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Qt Events
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::paintEvent(QPaintEvent* /*event*/)
{
    if (m_gpuActive) return; // GPU path handles its own rendering

    QPainter painter(this);
    painter.fillRect(rect(), Theme::colors().surface0);

    if (m_cpuImage.isNull()) return;

    // Aspect-ratio-correct fit
    QSizeF imgSize(m_cpuImage.width(), m_cpuImage.height());
    QSizeF widgetSize(width(), height());
    imgSize.scale(widgetSize, Qt::KeepAspectRatio);

    QRectF drawRect(
        (width() - imgSize.width()) / 2.0,
        (height() - imgSize.height()) / 2.0,
        imgSize.width(), imgSize.height());

    painter.drawImage(drawRect, m_cpuImage);
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
    QWidget::resizeEvent(event);

    const QSize surfaceSize = nativeSurfaceSize();
    // Cache dimensions for thread-safe handleResize (called from render thread)
    m_cachedWidth.store(std::max(1u, static_cast<uint32_t>(surfaceSize.width())));
    m_cachedHeight.store(std::max(1u, static_cast<uint32_t>(surfaceSize.height())));
    if (m_gpuActive) {
        m_swapchainDirty = true;

        // Floating live-resizes can leave the native child surface showing
        // stale stretched contents until another present arrives. Force an
        // immediate re-present (or clear if no source image exists).
        if (m_sourceView != VK_NULL_HANDLE)
            refresh();
        else
            presentClearFrame();
    }
    emit resized();
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
    if (m_gpuActive)
        presentFrame();
    else
        update();
    emit viewZoomChanged(m_viewZoom);
}

void VulkanViewport::setViewPan(float px, float py)
{
    m_viewPanX = px;
    m_viewPanY = py;
    if (m_gpuActive)
        presentFrame();
    else
        update();
}

void VulkanViewport::setViewZoom(float zoom)
{
    m_viewZoom = std::clamp(zoom, 0.1f, 20.0f);
    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    if (m_gpuActive)
        presentFrame();
    else
        update();
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
    if (m_gpuActive)
        presentFrame();
    else
        update();
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

    if (m_gpuActive)
        presentFrame();
    else
        update();

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
            if (m_gpuActive && isVisible()) {
                m_swapchainDirty = true;
                if (m_sourceView != VK_NULL_HANDLE)
                    refresh();
                else
                    presentClearFrame();
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
