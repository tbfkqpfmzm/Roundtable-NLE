/*
 * VulkanViewport — Zero-copy GPU viewport for displaying compositor output.
 *
 * Uses GpuContext's VkDevice to create a native Win32 surface + swapchain,
 * then renders the compositor output via a fullscreen textured quad.
 *
 * Key difference from the old QVulkanWindow approach: shares the SAME VkDevice
 * as the compositor, so VkImageViews can be directly sampled without cross-
 * device copies.  Falls back to CPU (QPainter) if Vulkan init fails.
 *
 * Usage:
 *   auto* vp = new VulkanViewport(parent);
 *   // After compositor runs:
 *   vp->displayGpuImage(compositor->outputImageView(),
 *                       compositor->outputSampler(),
 *                       compositor->outputWidth(),
 *                       compositor->outputHeight());
 */

#pragma once

#include <QWidget>
#include <QWindow>
#include <QImage>
#include <QRectF>
#include <QWheelEvent>
#include <QMouseEvent>

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rt {

struct CachedFrame;
class  Swapchain;
class  Texture;

/// Widget wrapper that provides both GPU and CPU display paths.
/// Drop-in replacement for Viewport in ProgramMonitor.
class VulkanViewport : public QWidget
{
    Q_OBJECT

public:
    explicit VulkanViewport(QWidget* parent = nullptr);
    ~VulkanViewport() override;

    // ── GPU display ─────────────────────────────────────────────────────

    /// Display the compositor output directly (zero-copy GPU path).
    /// @param waitSemaphore  Optional VkSemaphore (timeline) to wait on before
    ///                       reading the image.  Used for inter-queue sync
    ///                       between compute (compositor) and graphics (present).
    void displayGpuImage(VkImageView imageView, VkSampler sampler,
                         uint32_t width, uint32_t height,
                         VkSemaphore waitSemaphore = VK_NULL_HANDLE);

    /// Is the GPU backend active?
    [[nodiscard]] bool isGpuActive() const noexcept { return m_gpuActive; }

    // ── CPU fallback ────────────────────────────────────────────────────

    /// Display a CPU frame (fallback when GPU is unavailable).
    void displayFrame(std::shared_ptr<CachedFrame> frame);
    void displayFrame(const CachedFrame& frame);

    /// Clear to black.
    void clearFrame();

    /// Request a redraw.
    void refresh();

    /// Current frame dimensions.
    [[nodiscard]] uint32_t frameWidth() const noexcept { return m_frameWidth; }
    [[nodiscard]] uint32_t frameHeight() const noexcept { return m_frameHeight; }

    /// Current view zoom level (1.0 = 100%).
    [[nodiscard]] float viewZoom() const noexcept { return m_viewZoom; }

    /// Current view pan offset (widget pixels).
    [[nodiscard]] float viewPanX() const noexcept { return m_viewPanX; }
    [[nodiscard]] float viewPanY() const noexcept { return m_viewPanY; }

    /// Set pan offset programmatically (e.g. from overlay widget).
    void setViewPan(float px, float py);

    /// Set zoom level programmatically (1.0 = 100%).
    void setViewZoom(float zoom);

    /// Zoom to fill the entire widget (no letterbox/pillarbox).
    void zoomToFill();

    /// Source image dimensions shown by GPU.
    [[nodiscard]] uint32_t srcWidth()  const noexcept { return m_srcW; }
    [[nodiscard]] uint32_t srcHeight() const noexcept { return m_srcH; }

    /// GPU viewport rect in widget-local logical pixels.
    /// Updated every present; returns empty if no frame has been presented.
    [[nodiscard]] QRectF gpuFrameRect() const noexcept { return m_gpuFrameRect; }

    /// Reset zoom to 1.0 and pan to (0,0).
    void resetZoomPan();

    /// Access the underlying QWindow (for event filter installation).
    [[nodiscard]] QWindow* nativeWindow() const noexcept { return m_nativeWindow; }

    /// Actual native surface size (HWND client rect).  May differ from
    /// QWidget::size() due to createWindowContainer / DPI quirks.
    /// Falls back to QWidget::size() on non-Windows.
    [[nodiscard]] QSize nativeSurfaceSize() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void frameDisplayed();

    /// Emitted when the viewport zoom level changes.
    void viewZoomChanged(float zoom);

    /// Emitted when the viewport is resized (so the monitor can re-composite).
    void resized();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool initGpu();
    void shutdownGpu();
    bool createSwapchainResources();
    void destroySwapchainResources();
    bool createRenderPass();
    bool createPipeline();
    bool createDescriptorResources();
    bool createSyncObjects();
    bool createFramebuffers();
    void handleResize();
    void presentFrame(VkSemaphore waitSemaphore = VK_NULL_HANDLE);
    void presentClearFrame();

    // ── Native window + Vulkan surface ──────────────────────────────────
    QWindow*          m_nativeWindow{nullptr};
    QWidget*          m_windowContainer{nullptr};
    VkSurfaceKHR      m_surface{VK_NULL_HANDLE};

    // ── Swapchain ───────────────────────────────────────────────────────
    std::unique_ptr<Swapchain> m_swapchain;
    bool m_swapchainDirty{false};

    // ── Render pass + framebuffers ──────────────────────────────────────
    VkRenderPass      m_renderPass{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> m_framebuffers;

    // ── Pipeline ────────────────────────────────────────────────────────
    VkPipeline        m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout  m_pipelineLayout{VK_NULL_HANDLE};
    VkShaderModule    m_vertShader{VK_NULL_HANDLE};
    VkShaderModule    m_fragShader{VK_NULL_HANDLE};

    // ── Descriptors ─────────────────────────────────────────────────────
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};
    VkSampler             m_fallbackSampler{VK_NULL_HANDLE};

    // ── Sync ────────────────────────────────────────────────────────────
    VkSemaphore       m_imageAvailable{VK_NULL_HANDLE};
    VkSemaphore       m_renderFinished{VK_NULL_HANDLE};
    VkFence           m_inFlightFence{VK_NULL_HANDLE};
    VkCommandPool     m_commandPool{VK_NULL_HANDLE};
    VkCommandBuffer   m_commandBuffer{VK_NULL_HANDLE};

    // ── Source image (compositor output) ─────────────────────────────────
    VkImageView       m_sourceView{VK_NULL_HANDLE};
    VkSampler         m_sourceSampler{VK_NULL_HANDLE};
    uint32_t          m_srcW{0};
    uint32_t          m_srcH{0};
    bool              m_imageDirty{false};

    // ── CPU fallback ────────────────────────────────────────────────────
    QImage m_cpuImage;
    std::shared_ptr<CachedFrame> m_cpuFrameRef;

    // ── CPU → GPU upload slots (for async composite display) ───────────
    // Reuse a small ring of upload textures + command buffers + fences so
    // CPU-backed preview frames avoid per-frame Vulkan alloc/destroy and do
    // not overwrite the texture currently being sampled for presentation.
    struct UploadSlot {
        std::unique_ptr<Texture> texture;
        VkCommandBuffer          commandBuffer{VK_NULL_HANDLE};
        VkFence                  fence{VK_NULL_HANDLE};
        // Deferred staging cleanup — kept alive until the slot's fence
        // is signaled so the GPU can finish reading the staging buffer.
        VkBuffer      pendingStagingBuffer{VK_NULL_HANDLE};
        void*         pendingStagingAlloc{nullptr};     // VmaAllocation
        void*         pendingStagingAllocator{nullptr};  // VmaAllocator
    };
    static constexpr uint32_t kUploadSlotCount = 3;
    std::vector<UploadSlot> m_uploadSlots;
    size_t m_nextUploadSlot{0};

    bool     m_gpuActive{false};
    bool     m_gpuInitialized{false};
    uint32_t m_frameWidth{0};
    uint32_t m_frameHeight{0};

    // ── Present serialization ────────────────────────────────────────────
    // Protects VkCommandBuffer recording/submission in presentFrame() and
    // presentClearFrame() from concurrent access by the FramePresenter
    // thread (displayGpuImage path) and the UI thread (zoom/pan/resize).
    std::mutex m_presentMtx;

    // ── Viewport zoom & pan ─────────────────────────────────────────────
    float m_viewZoom{1.0f};
    float m_viewPanX{0.0f};
    float m_viewPanY{0.0f};

    // ── Cached GPU viewport rect (logical pixels) ───────────────────────
    QRectF m_gpuFrameRect;

    // ── Cached widget dimensions for thread-safe resize handling ────────
    // Updated by resizeEvent (UI thread), read by handleResize (render thread).
    std::atomic<uint32_t> m_cachedWidth{1};
    std::atomic<uint32_t> m_cachedHeight{1};
};

} // namespace rt
