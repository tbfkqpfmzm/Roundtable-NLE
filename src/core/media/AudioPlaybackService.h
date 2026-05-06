/*
 * AudioPlaybackService — Timeline audio source loading, decode caching, and
 * background prefetch.
 *
 * Extracted from TimelineWorkspace to separate audio playback concerns from
 * UI orchestration.  Lives in core/media/ — no Qt dependency.
 */

#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

class AudioEngine;
class AudioSampleProvider;
class PlaybackController;
class Timeline;

class AudioPlaybackService {
public:
    AudioPlaybackService();
    ~AudioPlaybackService();

    AudioPlaybackService(const AudioPlaybackService&) = delete;
    AudioPlaybackService& operator=(const AudioPlaybackService&) = delete;

    // ── Dependency injection ────────────────────────────────────────────
    void setTimeline(Timeline* tl)                    { m_timeline = tl; }
    void setAudioEngine(AudioEngine* engine)          { m_audioEngine = engine; }
    void setPlaybackController(PlaybackController* c) { m_playbackController = c; }

    // ── Core operations ─────────────────────────────────────────────────
    /// Scan the timeline for audio clips and feed decoded buffers to the
    /// AudioEngine.  When allowBlockingMisses is false, cache misses are
    /// deferred to the background prefetch thread instead of blocking.
    void loadSources(bool allowBlockingMisses = true);

    /// Load audio sources if not yet loaded, or if the playback window
    /// needs refreshing.  Skips reload during active playback.
    void ensureSourcesLoaded();

    /// Pre-decode audio pages in a background thread for the current
    /// playback window plus a 16-second lookahead.
    void warmCacheAsync();

    /// Cancel any in-flight background decode.
    void cancelWarm();

    /// Block until any in-flight background decode completes.
    void waitForWarm();

    // ── State management ────────────────────────────────────────────────
    /// Invalidate cached audio sources so they are reloaded on next play.
    void invalidateSources();

    /// Live-update a clip's volume/pan/mute without rebuilding the source
    /// list. Used for Effect Controls scrubbing so audio doesn't glitch.
    void updateClipLevels(uint64_t clipId, float volume, float pan, bool muted);

    /// Reset all state (decode cache, providers, stats).  Call on timeline
    /// change (e.g. project open/close).
    void reset();

    /// Whether the playback window needs refreshing at the current
    /// playhead position (for use by UI scheduling code).
    bool needsPlaybackWindowRefresh() const;

    /// Log a performance snapshot of the audio decode cache.
    void logPerfSnapshot(const char* reason);

    [[nodiscard]] bool sourcesLoaded()   const noexcept { return m_sourcesLoaded; }
    [[nodiscard]] bool isTopologyDirty() const noexcept { return m_topologyDirty; }

private:
    // Dependencies (non-owning)
    Timeline*            m_timeline{nullptr};
    AudioEngine*         m_audioEngine{nullptr};
    PlaybackController*  m_playbackController{nullptr};

    // Provider mapping (clipId → window provider)
    std::vector<std::shared_ptr<std::vector<float>>> m_audioBuffers;
    std::unordered_map<uint64_t, std::shared_ptr<AudioSampleProvider>> m_clipProviders;
    bool    m_sourcesLoaded{false};
    bool    m_topologyDirty{true};
    int64_t m_loadedWindowStartFrame{-1};
    int64_t m_loadedWindowEndFrame{-1};

    // Decoded-audio page cache (path+offset → PCM)
    struct CachedDecode {
        std::shared_ptr<std::vector<float>> samples;
        int64_t  startFrame{0};
        uint16_t channels{0};
        int64_t  totalFrames{0};
        size_t   bytes{0};
        uint64_t lastUseSerial{0};
    };
    static constexpr size_t kDecodeCacheBudgetBytes = 256u * 1024u * 1024u;
    std::mutex m_decodeMutex;
    std::unordered_map<std::string, CachedDecode> m_decodeCache;
    uint64_t m_decodeUseSerial{0};
    void pruneDecodeCacheLocked();

    // Performance statistics
    std::atomic<uint64_t> m_cacheRequests{0};
    std::atomic<uint64_t> m_cacheHits{0};
    std::atomic<uint64_t> m_blockingMisses{0};
    std::atomic<uint64_t> m_deferredMisses{0};
    std::atomic<uint64_t> m_prefetchRequests{0};
    std::atomic<uint64_t> m_prefetchBusySkips{0};
    std::atomic<uint64_t> m_prefetchCompletions{0};
    std::atomic<uint64_t> m_prefetchInsertions{0};

    // Background warm-up
    std::future<void> m_warmFuture;
    std::atomic<bool> m_warmCancel{false};
};

} // namespace rt
