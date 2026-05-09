/*
 * SpinePrerenderer — offline pre-renderer for Spine character animations.
 *
 * Takes a character identity (name, outfit, animation) and renders the
 * full animation loop to a VP9+alpha WebM file at the character's native
 * pixel resolution.  Uses the GPU SpineRenderer for rendering and falls
 * back to the CPU software rasterizer if no GPU is available.
 *
 * This is the "rendering half" of the pre-rendered animation cache system.
 * AnimationVideoCache manages the cache inventory and triggers renders;
 * this class does the actual frame-by-frame rendering to WebM.
 *
 * Usage:
 *   SpinePrerenderer renderer;
 *   renderer.setAssetsDir("assets");
 *   auto result = renderer.render({
 *       .characterName = "Crown",
 *       .outfit = "default",
 *       .animationName = "idle",
 *       .outputPath = "assets/cache/animations/Crown/default/idle.webm",
 *       .fps = 60,
 *       .crf = 18
 *   });
 */

#pragma once

#ifdef ROUNDTABLE_HAS_SPINE

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace rt {

/// Configuration for a single pre-render job
/// Encoder format preference for pre-render jobs
enum class PrerenderFormat : uint8_t { Auto, GreenScreen, BlueScreen, CustomColor, HEVCPackedAlpha, ProRes4444 };

struct PrerenderJob
{
    std::string           characterName;
    std::string           outfit{"default"};
    std::string           animationName{"idle"};
    std::filesystem::path outputPath;          ///< Output .mov/.webm path
    int                   fps{60};
    int                   crf{18};             ///< VP9 quality (0-63, lower=better)
    float                 paddingFactor{0.9f}; ///< Viewport padding (0.9 = 10% margin)
    bool                  isTalking{false};    ///< Render with talk animation blended
    PrerenderFormat       format{PrerenderFormat::Auto}; ///< Encoder format preference
    // Chroma key background colour (used for GreenScreen/BlueScreen/CustomColor formats)
    uint8_t               chromaKeyR{0};
    uint8_t               chromaKeyG{255};
    uint8_t               chromaKeyB{0};};

/// Result of a pre-render job
struct PrerenderResult
{
    bool                  success{false};
    std::filesystem::path outputPath;
    uint32_t              width{0};            ///< Rendered frame width
    uint32_t              height{0};           ///< Rendered frame height
    int64_t               frameCount{0};       ///< Total frames rendered
    float                 duration{0.0f};      ///< Animation duration in seconds
    uint64_t              fileSizeBytes{0};     ///< Output file size
    std::string           error;               ///< Error message if !success
};

/// Progress callback: (framesRendered, totalFrames) → should return false to cancel
using PrerenderProgressFn = std::function<bool(int64_t, int64_t)>;

class SpinePrerenderer
{
public:
    SpinePrerenderer();
    ~SpinePrerenderer();

    SpinePrerenderer(const SpinePrerenderer&) = delete;
    SpinePrerenderer& operator=(const SpinePrerenderer&) = delete;

    /// Set the assets directory (containing "characters/" subdirectory)
    void setAssetsDir(const std::string& dir) { m_assetsDir = dir; }

    /// Force CPU-only rendering (skip GPU path even if Vulkan is available).
    void setForceCPU(bool force) { m_forceCPU = force; }

    /// Render a single animation to a VP9+alpha WebM file.
    /// This is a blocking call — run on a background thread for UI responsiveness.
    /// Thread-safe: creates a per-thread CommandPool and serialises graphics
    /// queue submissions through GpuContext::graphicsQueueMutex().
    /// @param job      Job configuration
    /// @param progress Optional progress callback (called per frame)
    /// @return Result with success status, dimensions, frame count, file size
    PrerenderResult render(const PrerenderJob& job,
                           PrerenderProgressFn progress = nullptr);

private:
    /// Render using the GPU SpineRenderer + Vulkan readback
    PrerenderResult renderGPU(const PrerenderJob& job,
                              PrerenderProgressFn progress);

    /// Render using the CPU software triangle rasterizer (fallback)
    PrerenderResult renderCPU(const PrerenderJob& job,
                              PrerenderProgressFn progress);

    std::string m_assetsDir{"assets"};
    bool        m_forceCPU{false};
};

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
