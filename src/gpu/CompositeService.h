/*
 * CompositeService — Timeline video compositing pipeline orchestration.
 *
 * Extracted from TimelineWorkspace to separate the rendering/compositing
 * subsystem from UI orchestration.  Lives in gpu/ — no Qt dependency.
 *
 * Owns: composite frame orchestration, layer building, prewarm, spine
 *       rendering, safe mode, sticky frame fallback, and the
 *       CompositeEngine (GPU compositing pipeline).
 *
 * GPU resources (command buffers, staging ring, texture cache, upload
 * manager, layer pool) are owned by CompositeEngine, accessible via
 * engine().
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward-declared Vulkan types used in the GPU spine zero-copy path.
struct VkDescriptorImageInfo_T;
#include <vulkan/vulkan_core.h>

struct VkDescriptorImageInfo;

#include "CompositeServiceLayerBuild.h"
#include "media/FrameCache.h"
#include "media/MediaSourceService.h"  // ResolutionTier
#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/SpineEngine.h"
#endif

// CompositeEngine is in global scope (not rt::) to work around a C2888
// compiler issue with volk's Vulkan type redefinitions.
class CompositeEngine;

namespace rt {

// Forward declarations
class AnimationVideoCache;
class AudioEngine;
class CacheCoordinator;
class Clip;
class GraphicClip;
class MediaPool;
class ModelManager;
class SpineClip;
class VideoClip;
class Project;
class ShotPresetManager;
class SpineClip;
class Timeline;
class TitleClip;
struct CachedFrame;

class CompositeService {
public:
    CompositeService();
    ~CompositeService();

    /// Suppress all GPU compositing while a modal dialog is active.
    /// Set by the UI layer before showing modal dialogs (QDialog::exec).
    /// During modal dialogs, paint events still fire for widgets behind
    /// the dialog, and invoking the Vulkan compositor from a paint event
    /// cascade can overflow the NVIDIA driver stack (stack overflow in
    /// nvoglv64.dll).  The compositor returns the last good cached frame
    /// when suppressed, avoiding crashes while keeping the display valid.
    static void setModalDialogActive(bool active) noexcept {
        s_modalDialogActive.store(active, std::memory_order_release);
    }
    [[nodiscard]] static bool isModalDialogActive() noexcept {
        return s_modalDialogActive.load(std::memory_order_acquire);
    }

    /// Thread-local re-entrancy counter for compositeFrame().
    /// Unlike the mutex try_lock, this detects recursive calls on the
    /// SAME thread (e.g. signal re-entrancy during GPU compositing).
    /// When non-zero on entry, compositeFrame returns the last good frame
    /// immediately, preventing deep recursive compositing that would
    /// exhaust the call stack and cause a STACK_OVERFLOW crash.
    static int& compositeDepth() noexcept {
        static thread_local int s_depth = 0;
        return s_depth;
    }

private:
    static std::atomic<bool> s_modalDialogActive;

public:
    // Non-copyable
    CompositeService(const CompositeService&) = delete;
    CompositeService& operator=(const CompositeService&) = delete;

    // ── Dependency injection ────────────────────────────────────────────
    void setTimeline(Timeline* t) { m_timeline = t; }
    void setMediaPool(MediaPool* p) { m_mediaPool = p; }
    void setMediaSourceService(MediaSourceService* s) { m_mediaSources = s; }
    void setModelManager(ModelManager* m) { m_modelManager = m; }
    void setProject(Project* p) { m_project = p; }
    void setShotPresetManager(ShotPresetManager* m) { m_shotPresetManager = m; }

    // ── Core compositing ────────────────────────────────────────────────
    std::shared_ptr<CachedFrame> compositeFrame(int64_t tick, uint32_t outW, uint32_t outH,
                                                 bool scrubMode);

    /// Enqueue a prewarm request on the background thread.
    /// Returns immediately — the work runs asynchronously on m_prewarmThread.
    void prewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH);

    /// Scan the timeline window (tick, tick + lookaheadTicks] and pre-open
    /// media files + schedule first-frame prefetch for upcoming clips that
    /// are NOT yet active.  Throttled internally to run at most every ~100ms.
    /// This eliminates the 150-200ms "cold decoder open" stall at every shot
    /// boundary during playback — the decoder is already warm by the time
    /// the playhead reaches the clip.
    void prewarmUpcomingShots(int64_t tick);

    // ── Shutdown ─────────────────────────────────────────────────────────
    /// Gracefully shut down GPU work.  Waits for all in-flight submissions,
    /// destroys GPU resources, and prevents new work from starting.
    void shutdown();

    // ── Cache management ────────────────────────────────────────────────
    /// Invalidate composite result LRU (thread-safe, lock-free flag).
    /// Immediately clears m_lastGoodComposite so stale frames are not
    /// returned during invalidation.  Sets an atomic flag that
    /// compositeFrame() checks on its next acquisition of
    /// m_compositeMutex (the FrameProducer thread is the sole
    /// compositor, so the flag is always picked up promptly).
    void requestCacheInvalidation() {
        m_cacheInvalidateRequested.store(true, std::memory_order_release);
        {
            std::lock_guard lg(m_lastCompositeMtx);
            m_lastGoodComposite.reset();
            m_lastGoodCompositeTick = -1;
        }
    }

    /// Try to invalidate immediately (used when caller holds no lock).
    void invalidateCacheDirect();

    // ── GPU display mode ────────────────────────────────────────────────
    void setGpuDisplayMode(bool on) { m_gpuDisplayMode = on; }
    [[nodiscard]] bool gpuDisplayMode() const noexcept { return m_gpuDisplayMode; }

    // ── Playback resolution tier ────────────────────────────────────────
    // Controls the ResolutionTier compositeFrame() requests from MediaPool
    // for video layers.  Driven by the program-monitor playback-resolution
    // dropdown (Full/1/2/1/4/1/8).  Lower tiers decode fewer pixels, cache
    // smaller frames, and upload less data each frame — the single biggest
    // lever for playback smoothness at the cost of preview resolution.
    void setPlaybackTier(ResolutionTier t) {
        m_playbackTier.store(static_cast<uint8_t>(t), std::memory_order_relaxed);
    }
    [[nodiscard]] ResolutionTier playbackTier() const noexcept {
        return static_cast<ResolutionTier>(
            m_playbackTier.load(std::memory_order_relaxed));
    }

    // Force Full resolution — used by ExportPanel for preview and export
    // frames.  Overrides character video tier from Half to Full.
    void setForceFullResolution(bool force) noexcept {
        m_forceFullResolution.store(force, std::memory_order_relaxed);
    }
    [[nodiscard]] bool forceFullResolution() const noexcept {
        return m_forceFullResolution.load(std::memory_order_relaxed);
    }

    // ── Media handle management ─────────────────────────────────────────
    /// Look up a cached media handle by path. Returns 0 if not found.
    /// Thread-safe via m_openMediaHandlesMutex (accessed by prewarm thread,
    /// FrameProducer thread, and UI thread).
    [[nodiscard]] uint64_t findMediaHandle(const std::string& path) const {
        std::lock_guard lock(m_openMediaHandlesMutex);
        auto it = m_openMediaHandles.find(path);
        return it != m_openMediaHandles.end() ? it->second : 0;
    }
    /// Register (or update) a media handle for the given path.
    void registerMediaHandle(const std::string& path, uint64_t handle) {
        std::lock_guard lock(m_openMediaHandlesMutex);
        m_openMediaHandles[path] = handle;
    }
    void clearMediaHandles() {
        std::lock_guard lock(m_openMediaHandlesMutex);
        m_openMediaHandles.clear();
    }

    // ── Video fallback cache ────────────────────────────────────────────
    struct VideoFallbackInfo {
        std::string videoPath;  // empty = not a video character
        uint64_t    handle{0};
        bool        looked_up{false};
    };
    void clearVideoFallbackCache() { m_videoFallbackCache.clear(); }
    /// Look up a video fallback handle by clip ID. Returns 0 if not found.
    [[nodiscard]] uint64_t findVideoFallbackHandle(uint64_t clipId) const {
        auto it = m_videoFallbackCache.find(clipId);
        return (it != m_videoFallbackCache.end()) ? it->second.handle : 0;
    }

    // ── Title/Graphic rendering ─────────────────────────────────────────
    std::shared_ptr<CachedFrame> renderTitleClip(TitleClip* clip, int64_t tick,
                                                  uint32_t outW, uint32_t outH);
    std::shared_ptr<CachedFrame> renderGraphicClip(GraphicClip* clip, int64_t tick,
                                                    uint32_t outW, uint32_t outH);

    // ── Cache coordinator ───────────────────────────────────────────────
    /// Set the CacheCoordinator for system-adaptive budgets and VRAM
    /// pressure monitoring.  Forwards to CompositeEngine.
    void setCacheCoordinator(rt::CacheCoordinator* coordinator);

    // ── Composite engine access ─────────────────────────────────────────
    [[nodiscard]] CompositeEngine* engine() const noexcept { return m_engine.get(); }

    /// VRAM usage percentage (0-100) from GPU texture cache, or 0 if none.
    [[nodiscard]] int vramUsagePercent() const;

    // ── Composite mutex ──────────────────────────────────────────────────
    // Protects compositeFrame() execution.  FrameProducer is the sole
    // holder — the old try_to_lock pattern has been removed.
    std::mutex& compositeMutex() { return m_compositeMutex; }

    // ── Last good composite ─────────────────────────────────────────────
    // Kept for safe-mode fallback and exception safety.  The normal
    // compositeFrame() path no longer returns stale frames.
    std::shared_ptr<CachedFrame> lastGoodComposite() const {
        std::lock_guard lg(m_lastCompositeMtx);
        return m_lastGoodComposite;
    }

#ifdef ROUNDTABLE_HAS_SPINE
    // ── Spine shared data ───────────────────────────────────────────────
    struct SpineSharedData {
        std::vector<std::vector<uint8_t>> pagePixels;
        std::vector<int> pageWidths;
        std::vector<int> pageHeights;
        std::vector<bool> pagePMA;
        bool pagePixelsUnpremultiplied{false};
        bool boundsCached{false};
        float stableBoundsX{0}, stableBoundsY{0};
        float stableBoundsW{0}, stableBoundsH{0};
        bool gpuTexturesUploaded{false};
        std::string skelPath;
        std::string atlasPath;
        std::vector<uint8_t> skelBytes;
        std::string atlasText;
        std::string atlasDir;
    };

    struct SpineCPUState {
        SpineEngine engine;
        std::shared_ptr<SpineSharedData> shared;
        std::shared_ptr<CachedFrame> cachedFrame;
        int64_t cachedTick{-1};
    };

    static std::string spineCharKey(const SpineClip& clip);
    std::shared_ptr<SpineSharedData> getOrCreateSharedSpineData(
        const SpineClip& clip, const std::string& assetsDir);
    SpineCPUState* getOrCreateSpineState(SpineClip* clip);
    SpineCPUState* tryGetSpineState(SpineClip* clip);
    std::shared_ptr<CachedFrame> renderSpineClip(SpineClip* clip, int64_t tick,
                                                  uint32_t outW, uint32_t outH);
    void preloadSpineAssets();
    void warmNewSpineClips();

    /// Get shared spine data for overlay sizing (null if not loaded).
    [[nodiscard]] const SpineSharedData* getSpineSharedDataForOverlay(
        const std::string& charName, const std::string& outfit, int stance) const;

    // Callback for schedule-based spine loading (set by UI layer)
    using SpineLoadCallback = std::function<void(const std::string& charName,
                                                  const std::string& outfit,
                                                  int stance,
                                                  const std::string& assetsDir)>;
    void setSpineLoadScheduler(SpineLoadCallback cb) { m_spineLoadScheduler = std::move(cb); }

    // Non-blocking spine state integration (called from UI thread after
    // background load completes)
    void integrateSpineSharedData(const std::string& key,
                                  std::shared_ptr<SpineSharedData> shared);
    void removeSpinePendingKey(const std::string& key);
    bool isSpinePending(const std::string& key) const;
    bool addSpinePendingKey(const std::string& key);

    // ── Spine futures ──────────────────────────────────────────────────
    /// Remove completed futures to avoid unbounded growth, then push a new one.
    void drainCompletedSpineFutures();
    void addSpineFuture(std::future<void> f);

    // ── Spine shared-data cache (read-only + keyed insert/lookup) ────
    [[nodiscard]] const std::unordered_map<std::string, std::shared_ptr<SpineSharedData>>&
        spineSharedCache() const { return m_spineSharedCache; }
    [[nodiscard]] std::shared_ptr<SpineSharedData> findSpineSharedData(const std::string& key) const;
    void storeSpineSharedData(const std::string& key, std::shared_ptr<SpineSharedData> data);

    // ── Animation name cache (keyed find/store) ─────────────────────
    [[nodiscard]] const std::vector<std::string>* findAnimNames(const std::string& key) const;
    void storeAnimNames(const std::string& key, std::vector<std::string> names);

    // ── Per-clip spine CPU state cache ──────────────────────────────
    void evictSpineState(uint64_t clipId);
    void purgeDeadSpineStates(const std::unordered_set<uint64_t>& liveIds);
    [[nodiscard]] const std::unordered_map<uint64_t, std::unique_ptr<SpineCPUState>>&
        spineCache() const { return m_spineCache; }

    /// Get the animation video cache (may be null).
    [[nodiscard]] AnimationVideoCache* animVideoCache() const { return m_animVideoCache.get(); }
    /// Create the animation video cache if it doesn't exist.
    void initAnimVideoCache(MediaPool* pool);
#endif

    // ── Safe mode (CPU compositor fallback) ─────────────────────────────
    /// Enter safe mode after persistent GPU failure.  Produces one frame
    /// at most every ~500ms for recovery purposes only — NOT for playback.
    /// Safe mode composites the topmost visible video track using software
    /// blend and displays via the CPU QWidget viewport.
    void setSafeMode(bool on) { m_safeMode.store(on, std::memory_order_release); }
    [[nodiscard]] bool isSafeMode() const noexcept {
        return m_safeMode.load(std::memory_order_acquire);
    }

    /// Minimal safe-mode compositing: produces a single BGRA CachedFrame
    /// from the topmost visible video track at the given tick.  Uses
    /// blocking software decode + MemCpy blend.  Returns nullptr if no
    /// video content is available.  Throttled internally to ~2 calls/sec.
    std::shared_ptr<CachedFrame> compositeSafeMode(int64_t tick,
                                                     uint32_t outW,
                                                     uint32_t outH);

    /// Attempt automatic recovery from safe mode.  Checks GPU health and
    /// if the device is operational again, clears safe mode so the next
    /// compositeFrame() uses the normal GPU path.  Callers should check
    /// isSafeMode() afterward to see if recovery succeeded.
    /// Returns true if recovery was attempted (success or fail); false
    /// if it's too soon to retry (throttled).
    bool tryAutoRecoverFromSafeMode();

    /// Callback invoked when safe mode state changes (entered or exited).
    /// The UI layer (ProgramMonitor) connects to this to show/hide the
    /// safe mode banner.
    using SafeModeCallback = std::function<void(bool safeModeActive)>;
    void setSafeModeCallback(SafeModeCallback cb) { m_safeModeCallback = std::move(cb); }

    // ── Reset (new timeline / project close) ────────────────────────────
    void reset();

private:
    // Layer building (extracted to CompositeServiceLayerBuild.cpp)
    std::vector<LayerInfo> buildLayersForFrame(int64_t tick, uint32_t outW, uint32_t outH,
                                                bool scrubMode, bool playbackNonBlocking,
                                                int& clipsAtTick, bool perfLog,
                                                std::unique_lock<std::mutex>& lock,
                                                bool& gpuSpineUsedThisFrame);

    // Per-clip-type layer builders (extracted to reduce CompositeServiceLayerBuild.cpp)
    struct PerClipContext;
    std::shared_ptr<CachedFrame> resolveMediaFrame(MediaHandle handle, int64_t frameNumber,
                                                    ResolutionTier tier, bool scrubMode) const;
    void buildVideoClipLayer(VideoClip* videoClip, Clip* clip, const PerClipContext& ctx);
#ifdef ROUNDTABLE_HAS_SPINE
    void buildSpineClipLayer(SpineClip* spineClip, Clip* clip, const PerClipContext& ctx);
#endif

    // GPU compositing path (delegates to CompositeEngine)
    std::shared_ptr<CachedFrame> tryCompositeOnGpu(const std::vector<LayerInfo>& layers,
                                                     uint32_t outW, uint32_t outH,
                                                     int64_t tick, bool scrubMode,
                                                     bool perfLog,
                                                     std::chrono::high_resolution_clock::time_point perfT0,
                                                     std::chrono::high_resolution_clock::time_point& perfTlayers,
                                                     int& effectLayerCount, int& effectPassCount,
                                                     int& transitionCount);

    // External dependencies (non-owning)
    Timeline*  m_timeline{nullptr};
    MediaPool* m_mediaPool{nullptr};
    MediaSourceService* m_mediaSources{nullptr};
    ModelManager* m_modelManager{nullptr};
    Project* m_project{nullptr};
    ShotPresetManager* m_shotPresetManager{nullptr};

    // Composite engine (owns GPU compositing pipeline)
    std::unique_ptr<CompositeEngine> m_engine;

    // Reusable composite buffer
    std::shared_ptr<CachedFrame> m_compositeBuffer;

    bool m_isCompositing{false};

    std::mutex m_compositeMutex;
    mutable std::mutex m_lastCompositeMtx;
    std::shared_ptr<CachedFrame> m_lastGoodComposite;
    int64_t m_lastGoodCompositeTick{-1};
    std::atomic<bool> m_cacheInvalidateRequested{false};

    // Track active clip IDs for shot-boundary detection
    std::unordered_set<uint64_t> m_lastActiveClipIds;

    // ── Timeline lookahead prewarm ────────────────────────────────────
    // clip IDs whose decoders have already been proactively prewarmed
    // (file opened + first frame prefetch scheduled) ahead of their
    // timeline-in.  Cleared at shot boundary / reset.
    std::unordered_set<uint64_t> m_prewarmedClipIds;
    // Throttle: only scan the lookahead window every ~100ms during playback.
    std::chrono::steady_clock::time_point m_lastLookaheadScan{};

    // ── Sticky per-clip last-good-frame fallback ──────────────────────
    // When a clip is on-screen but its newest frame hasn't been decoded
    // yet (e.g. Spine skeleton still loading, first loop wrap after
    // scrub), the compositor used to drop the layer entirely — causing
    // visible 100-300ms "character vanishes" / "background renders but
    // character comes in late" pop-in.  Instead we keep the previous
    // good frame displayed until a fresh one arrives.  Map is cleared
    // on reset() and pruned when clip IDs go out of the active set.
    std::unordered_map<uint64_t, std::shared_ptr<CachedFrame>> m_stickyLastClipFrame;
    // Character-level sticky fallback — keyed by a string formed from
    // (characterName|outfit|animation) for SpineClips or by mediaPath
    // for VideoClips.  Used when a BRAND NEW clip (never rendered yet,
    // so no per-clipId sticky exists) starts playing but its frames
    // aren't cached.  Because the video file backing this clip is the
    // same as the file that backed the previous shot of the same
    // character, reusing that frame eliminates the "Modernia vanishes
    // for 13 frames while Spine skeleton loads" and "scrub + play shows
    // black character for 200ms" artefacts.
    std::unordered_map<std::string, std::shared_ptr<CachedFrame>> m_stickyLastCharFrame;

    // Open media handles (shared with preOpenVideoMedia)
    // Protected by its own mutex — the prewarm thread accesses this
    // concurrently with compositeFrame() on the FrameProducer thread.
    std::unordered_map<std::string, uint64_t> m_openMediaHandles;
    mutable std::mutex m_openMediaHandlesMutex;

    // SpineClip video fallback cache
    std::unordered_map<uint64_t, VideoFallbackInfo> m_videoFallbackCache;

    // ── Prewarm thread (Phase 2.A) ─────────────────────────────────────
    struct PrewarmRequest {
        int64_t  tick{0};
        uint32_t outW{0};
        uint32_t outH{0};
    };

    /// Background thread entry point — runs prewarm work off the UI thread.
    void prewarmThreadLoop();

    /// Actual prewarm implementation (moved to background thread).
    void doPrewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH);

    std::thread m_prewarmThread;
    std::mutex              m_prewarmMutex;
    std::condition_variable m_prewarmCv;
    PrewarmRequest          m_prewarmRequest{};
    bool                    m_prewarmPending{false};
    std::atomic<bool> m_compositorReady{false};
    std::atomic<bool> m_destroying{false};

    // ── CPU Compositor Safe Mode (Phase 6) ────────────────────────────
    // Set true when GPU compositing fails persistently and backoff is
    // exhausted.  When active, compositeFrame() uses compositeSafeMode()
    // which composites ONE track only at ~2 fps via software decode+blend.
    // Cleared on reset() or when the user clicks "Reset GPU".
    std::atomic<bool> m_safeMode{false};

    // Throttle safe-mode compositing to at most once every 500ms.
    // compositeSafeMode() checks this before doing heavy work.
    std::chrono::steady_clock::time_point m_lastSafeModeComposite{};

    // Callback invoked when safe mode is entered or exited.
    SafeModeCallback m_safeModeCallback;

    // Throttle auto-recovery checks to at most once every 5 seconds.
    std::chrono::steady_clock::time_point m_lastRecoveryCheck{};

    bool m_gpuDisplayMode{false};
    std::atomic<bool> m_shutdown{false};

    // Playback resolution tier (set via setPlaybackTier, read in compositeFrame).
    // Default Half matches the dropdown default (1/2) and the source monitor.
    std::atomic<uint8_t> m_playbackTier{static_cast<uint8_t>(ResolutionTier::Half)};

    // Force Full resolution override — set by ExportPanel before compositing
    // preview or export frames so characters and video render at full quality
    // regardless of the playback tier dropdown.  Does NOT affect Program/Source
    // Monitor previews (they use the normal playbackTier path).
    std::atomic<bool> m_forceFullResolution{false};

    // Rolling per-second playback perf summary (auto-emitted to perf_log.txt)
    struct PlaybackPerfWindow {
        std::chrono::steady_clock::time_point windowStart{};
        int    frameCount{0};
        int    slowFrameCount{0};      // > 33ms
        int    veryslowFrameCount{0};  // > 50ms
        double totalMs{0.0};
        double maxMs{0.0};
        int    layerSum{0};
        int    gpuHitSum{0};
        int    uploadSum{0};
        int    effectPassSum{0};
        int    transitionSum{0};
    };
    PlaybackPerfWindow m_perfWindow;

#ifdef ROUNDTABLE_HAS_SPINE
    std::unordered_map<std::string, std::shared_ptr<SpineSharedData>> m_spineSharedCache;
    std::unordered_map<std::string, std::vector<std::string>> m_animNameCache;
    std::string m_gpuSpineActiveCharKey;
    /// Per-frame readbacks from GPU Spine rendering, one per character.
    std::vector<std::shared_ptr<struct CachedFrame>> m_gpuSpineReadbacks;
    /// Number of Spine clips rendered on GPU this frame.
    int m_gpuSpineCount{0};
    /// Index into layers[] of the most recently inserted Spine layer.
    int m_gpuSpineInsertedLayer{-1};
    /// Stored spine render state for the single-char zero-copy optimization.
    VkDescriptorImageInfo m_gpuSpineDesc{};
    uint32_t m_gpuSpineW{0};
    uint32_t m_gpuSpineH{0};
    /// Set to true when a Spine clip is GPU-rendered, reset after layer tracking.
    bool m_gpuSpineJustRendered{false};
    /// Layer index of the PREVIOUS Spine character (for updating its layer
    /// when the next character's readback captures its FBO content).
    int m_gpuSpinePrevLayer{-1};
    std::unordered_map<uint64_t, std::unique_ptr<SpineCPUState>> m_spineCache;
    // Sticky last-good pre-rendered frame per Spine clip.
    // Prevents source switching (pre-rendered video <-> live Spine) when
    // tryGetFrame misses during non-blocking playback.
    std::unordered_map<uint64_t, std::shared_ptr<CachedFrame>> m_lastPreRenderedSpineFrame;

    mutable std::mutex m_spinePendingMutex;
    std::unordered_set<std::string> m_spinePendingKeys;
    std::vector<std::future<void>> m_spineLoadFutures;

    std::unique_ptr<AnimationVideoCache> m_animVideoCache;

    SpineLoadCallback m_spineLoadScheduler;
#endif
};

} // namespace rt
