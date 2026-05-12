/*
 * GpuUploadManager — Texture upload orchestration for GPU compositing.
 *
 * Extracted from CompositeServiceGpuOrchestration.cpp (Phase 4.1 of
 * permanent fix plan).  Encapsulates all CPU→GPU texture transfer logic:
 *   - Per-layer texture upload (cacheable + pool paths)
 *   - Mask texture upload
 *   - GpuTextureCache pinning / lookup
 *   - StagingRing interaction
 *   - Staging buffer cleanup tracking
 *
 * Thread safety: NOT thread-safe — must be called from the composite thread.
 */

#pragma once

#include "vulkan/Texture.h"

#include <cstdint>
#include <memory>
#include <vector>

// Forward declarations
namespace rt {
class GpuContext;
class GpuTextureCache;
struct LayerInfo;
class StagingRing;

/// Result of a single layer texture upload.
struct GpuUploadResult {
    VkDescriptorImageInfo descriptor{};  ///< GPU descriptor for the uploaded texture
    bool                  success{false}; ///< true if upload succeeded
    bool                  usedRing{false}; ///< true if staging ring was used (no VMA alloc)
    bool                  cacheHit{false}; ///< true if texture was found in GPU cache (no upload)
    bool                  isPacked{false}; ///< packed-alpha flag from cache
    bool                  isPMA{false};    ///< premultiplied-alpha flag from cache
    uint32_t              srcW{0};        ///< source width (after packed-alpha adjustment)
    uint32_t              srcH{0};        ///< source height (after packed-alpha adjustment)
};

class GpuUploadManager {
public:
    /// @param ctx  Application-wide Vulkan context.
    /// @param ring  Persistent staging ring (owned by CompositeService).
    GpuUploadManager(GpuContext& ctx, StagingRing& ring);
    ~GpuUploadManager();

    // Non-copyable, non-movable
    GpuUploadManager(const GpuUploadManager&) = delete;
    GpuUploadManager& operator=(const GpuUploadManager&) = delete;

    // ── Frame lifecycle ────────────────────────────────────────────────

    /// Begin a new composite frame.  Resets the staging ring, unpins all
    /// previously-pinned cache entries, and sets the command buffer that
    /// all upload commands will be recorded into.
    /// @param cmd  The command buffer for this frame's GPU work.
    void beginFrame(VkCommandBuffer cmd);

    /// End the current frame — destroys any staging buffers that were
    /// allocated as fallback (ring-buffer allocations are automatically
    /// reclaimed on the next beginFrame).
    void endFrame();

    // ── Layer texture upload ───────────────────────────────────────────

    /// Upload (or cache-lookup) a single layer's texture.
    ///
    /// Handles all three upload paths:
    ///   1. GPU-cache hit (fastest — no upload, just pin + lookup)
    ///   2. Cacheable miss — creates a new cache-owned texture
    ///   3. Pool texture path — reuses a pool texture with dirty tracking
    ///
    /// @param layer       The layer to upload.
    /// @param poolTex     Pool texture to use for non-cacheable layers.
    /// @param poolMediaId [in/out] Dirty-tracking media ID for pool texture.
    /// @param poolFrameNo [in/out] Dirty-tracking frame number for pool texture.
    /// @param scrubMode   True if scrubbing (influences cacheability).
    /// @return Upload result with descriptor and metadata.
    GpuUploadResult uploadLayer(const LayerInfo& layer,
                                Texture& poolTex,
                                uint64_t& poolMediaId,
                                int64_t& poolFrameNo,
                                bool scrubMode);

    // ── Mask texture upload ────────────────────────────────────────────

    /// Upload a mask texture for a layer.
    /// @param maskPixels  The pre-rasterized mask pixel data (BGRA).
    /// @param maskTex     The texture to upload into (reused across frames).
    /// @param outW        Output width (mask dimensions).
    /// @param outH        Output height.
    /// @param outMaskDesc  [out] Descriptor for the uploaded mask texture.
    /// @return false if upload fails.
    bool uploadMask(const std::vector<uint8_t>& maskPixels,
                    Texture& maskTex,
                    uint32_t outW, uint32_t outH,
                    VkDescriptorImageInfo& outMaskDesc);

    // ── Cache management ───────────────────────────────────────────────

    /// Set the GPU texture cache.  May be null (no caching).
    void setTextureCache(GpuTextureCache* cache) noexcept { m_texCache = cache; }

    /// Get the current GPU texture cache (may be null).
    [[nodiscard]] GpuTextureCache* textureCache() const noexcept { return m_texCache; }

    // ── Shutdown ────────────────────────────────────────────────────────

    /// Release all resources.  Safe to call multiple times.
    void shutdown();

    // ── Accessors ───────────────────────────────────────────────────────

    /// The command buffer being recorded into for the current frame.
    [[nodiscard]] VkCommandBuffer commandBuffer() const noexcept { return m_cmd; }

private:
    /// Try to upload via staging ring first, fall back to batched VMA alloc.
    bool uploadViaRingOrBatched(Texture& tex,
                                const void* data, VkDeviceSize dataSize,
                                uint32_t width, uint32_t height,
                                VkFormat format,
                                VkImageUsageFlags usage,
                                bool isNewTexture);

    /// Try to update via staging ring first, fall back to batched VMA alloc.
    bool updateViaRingOrBatched(Texture& tex,
                                const void* data, VkDeviceSize dataSize);

    GpuContext&           m_ctx;
    StagingRing&          m_ring;
    GpuTextureCache*      m_texCache{nullptr};
    VkCommandBuffer       m_cmd{VK_NULL_HANDLE};

    /// Staging buffers allocated via the batched fallback path.
    /// Destroyed in endFrame().
    std::vector<Texture::StagingCleanup> m_stagingCleanups;
};

} // namespace rt
