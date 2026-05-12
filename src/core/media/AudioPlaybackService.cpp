/*
 * AudioPlaybackService.cpp — Timeline audio source loading, decode caching,
 * and background prefetch.
 *
 * Logic extracted from TimelineWorkspaceAudio.cpp.  No Qt dependency.
 */

#include "media/AudioPlaybackService.h"

#include "media/AudioEngine.h"
#include "media/AudioFile.h"
#include "media/PlaybackController.h"

#include "effects/Effect.h"
#include "effects/EffectStack.h"

#include "timeline/AudioClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>
#include <spdlog/spdlog.h>

namespace rt {

// ─── Anonymous helpers (moved verbatim from TimelineWorkspaceAudio.cpp) ─────

namespace {

constexpr int64_t kTimelineAudioWindowBehindFrames       = 4 * 48000;
constexpr int64_t kTimelineAudioWindowAheadFrames        = 16 * 48000;
constexpr int64_t kTimelineAudioWindowRefreshMarginFrames = 1500 * 48;
constexpr int64_t kAudioDecodePageFrames                 = 4 * 48000;

struct TimelineAudioWindow {
    int64_t startFrame{0};
    int64_t endFrame{0};
};

struct TimelineAudioRegion {
    int64_t timelineStartFrame{0};
    int64_t sourceStartFrame{0};
    int64_t sourceFrameCount{0};
    int64_t clipSourceOffsetFrames{0};
    int64_t fullClipSourceFrames{0};
};

struct TimelineAudioProviderState {
    std::shared_ptr<const std::vector<float>> buffer;
    int64_t totalFrames{0};
    int64_t startFrame{0};
    uint32_t channels{0};
    uint32_t sampleRate{48000};
    int64_t clipSourceOffsetFrames{0};
    int64_t fullClipSourceFrames{0};
};

struct CachedAudioPageRequest {
    std::string path;
    std::string cacheKey;
    int64_t startFrame{0};
    int64_t frameCount{0};
};

struct CachedAudioRegionView {
    std::shared_ptr<std::vector<float>> buffer;
    uint32_t channels{0};
    int64_t totalFrames{0};
};

/// Lock-free windowed sample provider — one per active audio clip.
class TimelineAudioWindowProvider final : public AudioSampleProvider {
public:
    [[nodiscard]] AudioSourceView currentView() const override
    {
        AudioSourceView view;
        auto state = m_state.load(std::memory_order_acquire);
        if (!state || !state->buffer || state->buffer->empty() || state->channels == 0 ||
            state->totalFrames <= 0) {
            return view;
        }
        view.buffer     = state->buffer;
        view.samples    = state->buffer->data();
        view.totalFrames = state->totalFrames;
        view.startFrame = state->startFrame;
        view.channels   = state->channels;
        view.sampleRate = state->sampleRate;
        return view;
    }

    void updateWindow(std::shared_ptr<const std::vector<float>> buffer,
                      int64_t startFrame, int64_t totalFrames,
                      uint32_t channels, uint32_t sampleRate,
                      int64_t clipSourceOffsetFrames, int64_t fullClipSourceFrames)
    {
        auto state = std::make_shared<TimelineAudioProviderState>();
        state->buffer               = std::move(buffer);
        state->totalFrames          = totalFrames;
        state->startFrame           = startFrame;
        state->channels             = channels;
        state->sampleRate           = sampleRate;
        state->clipSourceOffsetFrames = clipSourceOffsetFrames;
        state->fullClipSourceFrames   = fullClipSourceFrames;
        m_state.store(std::move(state), std::memory_order_release);
    }

    void clearWindow()
    {
        m_state.store(std::shared_ptr<const TimelineAudioProviderState>{}, std::memory_order_release);
    }

    [[nodiscard]] float clipFrameForNormalizedPosition(float pos) const
    {
        auto state = m_state.load(std::memory_order_acquire);
        if (!state || state->totalFrames <= 0) return -1.0f;
        return static_cast<float>(state->clipSourceOffsetFrames) +
               std::clamp(pos, 0.0f, 1.0f) * static_cast<float>(state->totalFrames);
    }

private:
    std::atomic<std::shared_ptr<const TimelineAudioProviderState>> m_state;
};

// ─── Small utility functions ────────────────────────────────────────────────

std::string makeAudioPageCacheKey(const std::string& path, int64_t startFrame)
{
    return path + "|" + std::to_string(startFrame);
}

std::vector<CachedAudioPageRequest> makeAudioPageRequests(const std::string& path,
                                                          int64_t startFrame,
                                                          int64_t frameCount)
{
    std::vector<CachedAudioPageRequest> requests;
    if (path.empty() || frameCount <= 0) return requests;

    const int64_t regionStartFrame = std::max<int64_t>(0, startFrame);
    const int64_t regionEndFrame   = regionStartFrame + frameCount;
    const int64_t firstPageStart   = (regionStartFrame / kAudioDecodePageFrames) * kAudioDecodePageFrames;

    for (int64_t pageStart = firstPageStart; pageStart < regionEndFrame; pageStart += kAudioDecodePageFrames) {
        // Always request full-page decodes so the cache entry covers the
        // entire page.  Previous code truncated the last page to regionEndFrame
        // which left partial pages in the cache — when a later clip needed
        // the same page but more frames, the cached partial entry was reused,
        // silently dropping audio.
        requests.push_back(CachedAudioPageRequest{
            path,
            makeAudioPageCacheKey(path, pageStart),
            pageStart,
            kAudioDecodePageFrames
        });
    }
    return requests;
}

bool tryAssembleCachedAudioRegionLocked(auto& cache, uint64_t& useSerial,
                                        const std::vector<CachedAudioPageRequest>& requests,
                                        int64_t regionStartFrame, int64_t regionFrameCount,
                                        CachedAudioRegionView& out)
{
    if (requests.empty() || regionFrameCount <= 0) return false;

    uint32_t channels = 0;
    std::shared_ptr<std::vector<float>> singlePageBuffer;
    int64_t singlePageStartFrame = 0;
    int64_t singlePageTotalFrames = 0;
    for (const auto& request : requests) {
        auto it = cache.find(request.cacheKey);
        if (it == cache.end() || !it->second.samples || it->second.samples->empty() || it->second.totalFrames <= 0)
            return false;
        if (channels == 0)
            channels = it->second.channels;
        else if (channels != it->second.channels)
            return false;
        it->second.lastUseSerial = ++useSerial;
        if (requests.size() == 1) {
            singlePageBuffer     = it->second.samples;
            singlePageStartFrame = it->second.startFrame;
            singlePageTotalFrames = it->second.totalFrames;
        }
    }

    if (requests.size() == 1 &&
        singlePageStartFrame == regionStartFrame &&
        singlePageTotalFrames == regionFrameCount) {
        out.buffer     = std::move(singlePageBuffer);
        out.channels   = channels;
        out.totalFrames = regionFrameCount;
        return true;
    }

    auto stitched = std::make_shared<std::vector<float>>();
    stitched->resize(static_cast<size_t>(regionFrameCount * channels));

    size_t copiedFrames = 0;
    const int64_t regionEndFrame = regionStartFrame + regionFrameCount;
    for (const auto& request : requests) {
        auto it = cache.find(request.cacheKey);
        if (it == cache.end() || !it->second.samples || it->second.totalFrames <= 0) return false;

        const int64_t pageStart = request.startFrame;
        const int64_t pageEnd   = request.startFrame + it->second.totalFrames;
        const int64_t copyStart = std::max<int64_t>(regionStartFrame, pageStart);
        const int64_t copyEnd   = std::min<int64_t>(regionEndFrame, pageEnd);
        if (copyEnd <= copyStart) continue;

        const size_t frameOffset  = static_cast<size_t>(copyStart - pageStart);
        const size_t framesToCopy = static_cast<size_t>(copyEnd - copyStart);
        const size_t srcOffset = frameOffset * channels;
        const size_t dstOffset = copiedFrames * channels;
        std::memcpy(stitched->data() + dstOffset,
                    it->second.samples->data() + srcOffset,
                    framesToCopy * channels * sizeof(float));
        copiedFrames += framesToCopy;
    }

    if (copiedFrames == 0) return false;
    if (copiedFrames != static_cast<size_t>(regionFrameCount))
        stitched->resize(copiedFrames * channels);

    out.buffer     = std::move(stitched);
    out.channels   = channels;
    out.totalFrames = static_cast<int64_t>(copiedFrames);
    return true;
}

int64_t requestedClipSourceFrames(const AudioClip& clip)
{
    return static_cast<int64_t>(std::ceil(
        static_cast<double>(clip.duration()) * std::abs(clip.speed())));
}

TimelineAudioWindow timelineAudioWindowForTick(int64_t tick)
{
    TimelineAudioWindow window;
    window.startFrame = std::max<int64_t>(0, tick - kTimelineAudioWindowBehindFrames);
    window.endFrame   = std::max(window.startFrame + 1, tick + kTimelineAudioWindowAheadFrames);
    return window;
}

TimelineAudioWindow currentTimelineAudioWindow(const PlaybackController* controller)
{
    const int64_t tick = controller ? std::max<int64_t>(0, controller->currentTick()) : 0;
    return timelineAudioWindowForTick(tick);
}

std::vector<TimelineAudioWindow> prefetchTimelineAudioWindows(const PlaybackController* controller)
{
    const int64_t tick = controller ? std::max<int64_t>(0, controller->currentTick()) : 0;
    std::vector<TimelineAudioWindow> windows;
    windows.push_back(timelineAudioWindowForTick(tick));
    windows.push_back(timelineAudioWindowForTick(tick + (kTimelineAudioWindowAheadFrames / 2)));
    windows.push_back(timelineAudioWindowForTick(tick + kTimelineAudioWindowAheadFrames));
    return windows;
}

bool isTimelineAudioWindowValid(int64_t startFrame, int64_t endFrame, int64_t /*tick*/)
{
    return (startFrame >= 0 && endFrame > startFrame);
}

bool shouldRefreshTimelineAudioWindow(int64_t startFrame, int64_t endFrame, int64_t tick)
{
    if (!isTimelineAudioWindowValid(startFrame, endFrame, tick)) return true;
    if (tick < startFrame || tick >= endFrame) return true;

    // Near-start margin: only trigger if the window can actually slide back.
    // When startFrame == 0 (timeline start), the window is already at the
    // earliest possible position — re-loading would produce the identical
    // window and just waste CPU scanning 13 tracks × 325 clips (~25ms).
    bool nearStart = (startFrame > 0) &&
                     (tick <= startFrame + kTimelineAudioWindowRefreshMarginFrames);
    bool nearEnd   = tick >= (endFrame - kTimelineAudioWindowRefreshMarginFrames);
    return nearStart || nearEnd;
}

bool shouldLogAudioPerfSnapshot(uint64_t value)
{
    return value > 0 && (value % 16) == 0;
}

std::optional<TimelineAudioRegion> computeTimelineAudioRegion(const AudioClip& clip,
                                                              const TimelineAudioWindow* window)
{
    const int64_t clipTimelineStart = clip.timelineIn();
    const int64_t clipTimelineEnd   = clipTimelineStart + clip.duration();
    int64_t regionTimelineStart = clipTimelineStart;
    int64_t regionTimelineEnd   = clipTimelineEnd;

    if (window) {
        regionTimelineStart = std::max(regionTimelineStart, window->startFrame);
        regionTimelineEnd   = std::min(regionTimelineEnd, window->endFrame);
        if (regionTimelineEnd <= regionTimelineStart) return std::nullopt;
    }

    const double absSpeed = std::max(0.000001, std::abs(clip.speed()));
    const int64_t clipTimelineOffset  = std::max<int64_t>(0, regionTimelineStart - clipTimelineStart);
    const int64_t regionTimelineFrames = std::max<int64_t>(1, regionTimelineEnd - regionTimelineStart);
    const int64_t sourceStartFrame = clip.sourceIn() + static_cast<int64_t>(std::floor(
        static_cast<double>(clipTimelineOffset) * absSpeed));
    const int64_t sourceFrameCount = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(
        static_cast<double>(regionTimelineFrames) * absSpeed)));

    TimelineAudioRegion region;
    region.timelineStartFrame    = regionTimelineStart;
    region.sourceStartFrame      = sourceStartFrame;
    region.sourceFrameCount      = sourceFrameCount;
    region.clipSourceOffsetFrames = std::max<int64_t>(0, sourceStartFrame - clip.sourceIn());
    region.fullClipSourceFrames  = std::max<int64_t>(1, requestedClipSourceFrames(clip));
    return region;
}

} // anonymous namespace

// ─── Construction / destruction ─────────────────────────────────────────────

AudioPlaybackService::AudioPlaybackService() = default;

AudioPlaybackService::~AudioPlaybackService()
{
    m_destroying.store(true);
    cancelWarm();
    waitForWarm();
}

// ─── State management ───────────────────────────────────────────────────────

void AudioPlaybackService::invalidateSources()
{
    m_sourcesLoaded          = false;
    m_topologyDirty          = true;
    m_loadedWindowStartFrame = -1;
    m_loadedWindowEndFrame   = -1;

    // Immediately stop the AudioEngine from mixing stale sources.
    // Without this, the old AudioSampleProvider objects continue playing
    // audio from deleted/moved clips until loadSources() runs.
    if (m_audioEngine)
        m_audioEngine->clearTrackSources();
}

void AudioPlaybackService::updateClipLevels(uint64_t clipId, float volume, float pan, bool muted)
{
    if (m_audioEngine)
        m_audioEngine->updateSourceLevels(clipId, volume, pan, muted);
}

void AudioPlaybackService::reset()
{
    cancelWarm();
    waitForWarm();
    m_warmCancel.store(false);

    m_sourcesLoaded  = false;
    m_topologyDirty  = true;
    m_clipProviders.clear();

    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        m_decodeCache.clear();
        m_decodeUseSerial = 0;
    }

    m_loadedWindowStartFrame = -1;
    m_loadedWindowEndFrame   = -1;

    m_cacheRequests.store(0);
    m_cacheHits.store(0);
    m_blockingMisses.store(0);
    m_deferredMisses.store(0);
    m_prefetchRequests.store(0);
    m_prefetchBusySkips.store(0);
    m_prefetchCompletions.store(0);
    m_prefetchInsertions.store(0);
}

void AudioPlaybackService::cancelWarm()
{
    m_warmCancel.store(true);
}

void AudioPlaybackService::waitForWarm()
{
    if (m_warmFuture.valid())
        m_warmFuture.wait();
}

// ─── Perf logging ───────────────────────────────────────────────────────────

void AudioPlaybackService::logPerfSnapshot(const char* reason)
{
    size_t residentBytes = 0;
    size_t entryCount    = 0;
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        entryCount = m_decodeCache.size();
        for (const auto& [key, entry] : m_decodeCache) {
            (void)key;
            residentBytes += entry.bytes;
        }
    }

    const uint64_t requests    = m_cacheRequests.load();
    const uint64_t hits        = m_cacheHits.load();
    const uint64_t blkMisses   = m_blockingMisses.load();
    const uint64_t defMisses   = m_deferredMisses.load();
    const uint64_t pfReqs      = m_prefetchRequests.load();
    const uint64_t pfBusy      = m_prefetchBusySkips.load();
    const uint64_t pfDone      = m_prefetchCompletions.load();
    const uint64_t pfInserts   = m_prefetchInsertions.load();
    const uint64_t misses      = blkMisses + defMisses;
    const double hitRate = requests > 0
        ? (100.0 * static_cast<double>(hits) / static_cast<double>(requests))
        : 0.0;

    spdlog::info("[PERF] Timeline audio cache [{}]: req={} hit={} miss={} blockingMiss={} deferredMiss={} "
                 "hitRate={:.1f}% entries={} residentMiB={:.1f} prefetchReq={} prefetchBusy={} "
                 "prefetchDone={} prefetchInsert={} window=[{}..{})",
                 reason, requests, hits, misses, blkMisses, defMisses, hitRate,
                 entryCount, static_cast<double>(residentBytes) / (1024.0 * 1024.0),
                 pfReqs, pfBusy, pfDone, pfInserts,
                 m_loadedWindowStartFrame, m_loadedWindowEndFrame);
}

// ─── Cache pruning ──────────────────────────────────────────────────────────

void AudioPlaybackService::pruneDecodeCacheLocked()
{
    size_t totalBytes = 0;
    struct EvictionCandidate {
        std::string key;
        uint64_t lastUseSerial{0};
        size_t bytes{0};
    };
    std::vector<EvictionCandidate> candidates;

    for (const auto& [key, entry] : m_decodeCache) {
        totalBytes += entry.bytes;
        if (entry.samples && entry.samples.use_count() == 1)
            candidates.push_back(EvictionCandidate{key, entry.lastUseSerial, entry.bytes});
    }

    if (totalBytes <= kDecodeCacheBudgetBytes) return;

    std::sort(candidates.begin(), candidates.end(),
              [](const EvictionCandidate& a, const EvictionCandidate& b) {
                  return a.lastUseSerial < b.lastUseSerial;
              });

    size_t reclaimedBytes = 0;
    size_t evictedCount   = 0;
    for (const auto& candidate : candidates) {
        if (totalBytes <= kDecodeCacheBudgetBytes) break;
        auto it = m_decodeCache.find(candidate.key);
        if (it == m_decodeCache.end() || !it->second.samples || it->second.samples.use_count() != 1) continue;
        totalBytes -= it->second.bytes;
        reclaimedBytes += it->second.bytes;
        ++evictedCount;
        m_decodeCache.erase(it);
    }

    if (reclaimedBytes > 0) {
        spdlog::info("pruneAudioDecodeCache: evicted {} page(s), freed {:.1f} MiB, remaining {:.1f} MiB",
                     evictedCount,
                     static_cast<double>(reclaimedBytes) / (1024.0 * 1024.0),
                     static_cast<double>(totalBytes) / (1024.0 * 1024.0));
    }
}

// ─── loadSources ────────────────────────────────────────────────────────────

void AudioPlaybackService::loadSources(bool allowBlockingMisses)
{
    if (!m_timeline || !m_audioEngine) {
        spdlog::warn("loadAudioSources: skipped - timeline={} audioEngine={}",
                     (void*)m_timeline, (void*)m_audioEngine);
        return;
    }

    const bool rebuildSourceList = !m_sourcesLoaded || m_topologyDirty;
    std::vector<std::shared_ptr<std::vector<float>>> newBuffers;
    std::vector<AudioTrackSource> sources;
    std::unordered_set<uint64_t> activeClipIds;
    const TimelineAudioWindow audioWindow = currentTimelineAudioWindow(m_playbackController);
    size_t deferredCacheMisses = 0;

    spdlog::debug("loadAudioSources: scanning {} tracks in window [{}..{})",
                  m_timeline->trackCount(), audioWindow.startFrame, audioWindow.endFrame);

    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        auto* track = m_timeline->track(ti);
        if (!track || track->type() != TrackType::Audio) continue;

        spdlog::debug("loadAudioSources: track {} '{}' has {} clips",
                     ti, track->name(), track->clipCount());

        int audioClipsLoaded = 0;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* clip = track->clip(ci);
            if (!clip) continue;

            auto* audioClip = dynamic_cast<AudioClip*>(clip);
            if (!audioClip || !clip->isEnabled()) continue;

            const auto& path = audioClip->mediaPath();
            if (path.empty()) continue;

            if (audioClipsLoaded == 0) {
                spdlog::debug("  clip {} mediaPath='{}' in={} dur={}",
                             ci, path, audioClip->timelineIn(), audioClip->duration());
            }

            const uint64_t clipId = audioClip->id();
            activeClipIds.insert(clipId);
            auto providerIt = m_clipProviders.find(clipId);
            if (providerIt == m_clipProviders.end()) {
                providerIt = m_clipProviders.emplace(
                    clipId, std::make_shared<TimelineAudioWindowProvider>()).first;
            }
            auto provider = std::static_pointer_cast<TimelineAudioWindowProvider>(providerIt->second);

            const auto region = computeTimelineAudioRegion(*audioClip, &audioWindow);
            std::shared_ptr<std::vector<float>> buffer;
            uint32_t ch = 0;
            int64_t cachedFrames = 0;
            if (region) {
                const uint64_t requestCount = m_cacheRequests.fetch_add(1, std::memory_order_relaxed) + 1;
                const auto pageRequests = makeAudioPageRequests(
                    path, region->sourceStartFrame, region->sourceFrameCount);
                {
                    std::lock_guard<std::mutex> lock(m_decodeMutex);
                    CachedAudioRegionView cachedRegion;
                    if (tryAssembleCachedAudioRegionLocked(
                            m_decodeCache, m_decodeUseSerial,
                            pageRequests, region->sourceStartFrame, region->sourceFrameCount,
                            cachedRegion)) {
                        m_cacheHits.fetch_add(1, std::memory_order_relaxed);
                        buffer       = std::move(cachedRegion.buffer);
                        ch           = cachedRegion.channels;
                        cachedFrames = cachedRegion.totalFrames;
                    }
                }

                if (!buffer) {
                    if (!allowBlockingMisses) {
                        m_deferredMisses.fetch_add(1, std::memory_order_relaxed);
                        ++deferredCacheMisses;
                    } else {
                        m_blockingMisses.fetch_add(1, std::memory_order_relaxed);
                        AudioFile file;
                        if (!file.open(path)) {
                            spdlog::warn("loadAudioSources: failed to open '{}'", path);
                        } else {
                            std::vector<CachedAudioPageRequest> missingPages;
                            {
                                std::lock_guard<std::mutex> lock(m_decodeMutex);
                                for (const auto& request : pageRequests) {
                                    if (m_decodeCache.find(request.cacheKey) == m_decodeCache.end())
                                        missingPages.push_back(request);
                                }
                            }

                            for (const auto& request : missingPages) {
                                auto pageBuffer = std::make_shared<std::vector<float>>();
                                const int64_t framesRead = file.readRegionResampled(
                                    request.startFrame, request.frameCount, 48000, *pageBuffer);
                                if (framesRead <= 0 || pageBuffer->empty()) continue;

                                CachedDecode entry;
                                entry.samples    = pageBuffer;
                                entry.startFrame = request.startFrame;
                                entry.channels   = static_cast<uint16_t>(file.info().channels);
                                entry.totalFrames = framesRead;
                                entry.bytes      = pageBuffer->size() * sizeof(float);

                                {
                                    std::lock_guard<std::mutex> lock(m_decodeMutex);
                                    auto [it, inserted] = m_decodeCache.emplace(request.cacheKey, std::move(entry));
                                    it->second.lastUseSerial = ++m_decodeUseSerial;
                                    if (inserted) pruneDecodeCacheLocked();
                                }
                            }

                            {
                                std::lock_guard<std::mutex> lock(m_decodeMutex);
                                CachedAudioRegionView cachedRegion;
                                if (tryAssembleCachedAudioRegionLocked(
                                        m_decodeCache, m_decodeUseSerial,
                                        pageRequests, region->sourceStartFrame, region->sourceFrameCount,
                                        cachedRegion)) {
                                    buffer       = std::move(cachedRegion.buffer);
                                    ch           = cachedRegion.channels;
                                    cachedFrames = cachedRegion.totalFrames;
                                }
                            }
                        }
                    }
                }

                if (shouldLogAudioPerfSnapshot(requestCount))
                    logPerfSnapshot(allowBlockingMisses ? "sync-load" : "playback-refresh");

                if (buffer && !buffer->empty()) {
                    newBuffers.push_back(buffer);

                    if (ch == 2) {
                        const auto& fxStack = audioClip->effects();
                        for (size_t ei = 0; ei < fxStack.effectCount(); ++ei) {
                            const auto& fx = fxStack.effect(ei);
                            if (!fx.isEnabled()) continue;
                            if (fx.effectType() == EffectType::FillLeftWithRight ||
                                fx.effectType() == EffectType::FillRightWithLeft) {
                                auto modified = std::make_shared<std::vector<float>>(*buffer);
                                const size_t numSamples = modified->size();
                                float* d = modified->data();
                                if (fx.effectType() == EffectType::FillLeftWithRight) {
                                    for (size_t s = 0; s < numSamples; s += 2) d[s] = d[s + 1];
                                } else {
                                    for (size_t s = 0; s < numSamples; s += 2) d[s + 1] = d[s];
                                }
                                buffer = std::move(modified);
                                newBuffers.push_back(buffer);
                            }
                        }
                    }
                }
            }

            const int64_t totalRegionFrames = (buffer && ch > 0)
                ? (cachedFrames > 0 ? cachedFrames : static_cast<int64_t>(buffer->size() / ch))
                : 0;

            if (buffer && ch > 0 && totalRegionFrames > 0 && region) {
                audioClip->setChannels(static_cast<uint16_t>(ch));
                provider->updateWindow(buffer,
                                       region->timelineStartFrame,
                                       totalRegionFrames, ch, 48000,
                                       region->clipSourceOffsetFrames,
                                       region->fullClipSourceFrames);
            } else if (allowBlockingMisses || !m_sourcesLoaded) {
                provider->clearWindow();
            }

            if (rebuildSourceList) {
                AudioTrackSource src;
                src.trackId         = clipId;
                src.sampleProvider  = provider;
                src.startFrame      = audioClip->timelineIn();
                src.channels        = ch > 0 ? ch : 2;
                src.sampleRate      = 48000;
                src.volume          = track->volume() * audioClip->volume().evaluate(0);
                src.pan             = std::clamp(track->pan() + audioClip->pan().evaluate(0), -1.0f, 1.0f);
                src.muted           = track->isMuted();
                src.solo            = track->isSoloed();
                src.maintainPitch   = audioClip->maintainPitch();
                src.clipSpeed       = audioClip->speed();

                // Copy active audio effects for real-time playback
                const auto& fxStack = audioClip->effects();
                for (size_t ei = 0; ei < fxStack.effectCount(); ++ei) {
                    const auto& fx = fxStack.effect(ei);
                    if (fx.isEnabled() && isAudioEffect(fx.effectType()))
                        src.audioEffects.push_back(fx.effectType());
                }

                const int64_t tlIn = audioClip->timelineIn();
                const int64_t fullClipSourceFrames = std::max<int64_t>(1, requestedClipSourceFrames(*audioClip));
                for (size_t trI = 0; trI < track->transitionCount(); ++trI) {
                    const Transition* trans = track->transition(trI);
                    if (!trans) continue;
                    int64_t tStart, tEnd;
                    trans->getRange(tStart, tEnd);
                    const int64_t tDur = tEnd - tStart;
                    if (tDur <= 0) continue;

                    if (trans->leftClipId == clipId) {
                        const float fadeStartFrame = std::clamp<float>(
                            static_cast<float>(tStart - tlIn), 0.0f, static_cast<float>(fullClipSourceFrames));
                        const float fadeEndFrame = std::clamp<float>(
                            static_cast<float>(tEnd - tlIn), 0.0f, static_cast<float>(fullClipSourceFrames));
                        auto prevEnv = src.fadeEnvelope;
                        src.fadeEnvelope = [prevEnv, fadeStartFrame, fadeEndFrame, provider](float pos) {
                            float v = 1.0f;
                            const float clipFrame = provider->clipFrameForNormalizedPosition(pos);
                            if (clipFrame >= 0.0f && clipFrame >= fadeStartFrame && fadeEndFrame > fadeStartFrame) {
                                const float t = (clipFrame - fadeStartFrame) / (fadeEndFrame - fadeStartFrame);
                                v = 1.0f - std::clamp(t, 0.0f, 1.0f);
                            }
                            return prevEnv ? prevEnv(pos) * v : v;
                        };
                    }
                    if (trans->rightClipId == clipId) {
                        const float fadeStartFrame = std::clamp<float>(
                            static_cast<float>(tStart - tlIn), 0.0f, static_cast<float>(fullClipSourceFrames));
                        const float fadeEndFrame = std::clamp<float>(
                            static_cast<float>(tEnd - tlIn), 0.0f, static_cast<float>(fullClipSourceFrames));
                        auto prevEnv = src.fadeEnvelope;
                        src.fadeEnvelope = [prevEnv, fadeStartFrame, fadeEndFrame, provider](float pos) {
                            float v = 1.0f;
                            const float clipFrame = provider->clipFrameForNormalizedPosition(pos);
                            if (clipFrame >= 0.0f && clipFrame <= fadeEndFrame && fadeEndFrame > fadeStartFrame) {
                                const float t = (clipFrame - fadeStartFrame) / (fadeEndFrame - fadeStartFrame);
                                v = std::clamp(t, 0.0f, 1.0f);
                            }
                            return prevEnv ? prevEnv(pos) * v : v;
                        };
                    }
                }

                sources.push_back(std::move(src));
            }

            ++audioClipsLoaded;
        }
    }

    if (rebuildSourceList) {
        for (auto it = m_clipProviders.begin(); it != m_clipProviders.end(); ) {
            if (activeClipIds.find(it->first) == activeClipIds.end())
                it = m_clipProviders.erase(it);
            else
                ++it;
        }
        spdlog::info("loadAudioSources: rebuilt {} audio track source(s)", sources.size());
        m_audioEngine->setTrackSources(std::move(sources));
        m_topologyDirty = false;
    } else {
        spdlog::info("loadAudioSources: refreshed audio window for {} provider(s)", activeClipIds.size());
    }

    if (!allowBlockingMisses && deferredCacheMisses > 0) {
        spdlog::info("loadAudioSources: deferred {} cache miss(es) to background audio prefetch",
                     deferredCacheMisses);
        logPerfSnapshot("deferred-miss");
    }

    m_audioEngine->resetStretchers();
    m_audioBuffers = std::move(newBuffers);
    m_sourcesLoaded          = true;
    m_loadedWindowStartFrame = audioWindow.startFrame;
    m_loadedWindowEndFrame   = audioWindow.endFrame;
}

// ─── ensureSourcesLoaded ────────────────────────────────────────────────────

void AudioPlaybackService::ensureSourcesLoaded()
{
    const int64_t currentTick = m_playbackController
        ? std::max<int64_t>(0, m_playbackController->currentTick()) : 0;

    if (m_sourcesLoaded &&
        !shouldRefreshTimelineAudioWindow(m_loadedWindowStartFrame,
                                          m_loadedWindowEndFrame,
                                          currentTick)) {
        return;
    }

    // If the topology changed (clips added/removed/moved), we must
    // reload — otherwise the mixer keeps playing audio from
    // deleted/moved clips.  Use blocking mode so every clip is
    // decoded; non-blocking would silently skip cache misses and
    // produce silence for uncached clips.
    if (m_topologyDirty) {
        loadSources(/*allowBlockingMisses=*/true);
        return;
    }

    // During playback, allow window-slide refreshes so clips beyond
    // the initial 16-second lookahead get decoded instead of silenced.
    // Use blocking mode — this only triggers when the playhead reaches
    // the refresh margin (~1.5s from the window edge), so the decode
    // latency is acceptable.
    loadSources(/*allowBlockingMisses=*/true);
}

// ─── needsPlaybackWindowRefresh ─────────────────────────────────────────────

bool AudioPlaybackService::needsPlaybackWindowRefresh() const
{
    if (!m_playbackController || !m_audioEngine) return false;

    const auto state = m_playbackController->state();
    if (state != PlayState::Playing && state != PlayState::Shuttling) return false;

    // Topology dirty = clips added/removed/moved — must reload urgently
    if (m_topologyDirty) return true;
    // Sources not loaded yet — must load
    if (!m_sourcesLoaded) return true;

    const int64_t currentTick = std::max<int64_t>(0, m_playbackController->currentTick());
    return shouldRefreshTimelineAudioWindow(m_loadedWindowStartFrame,
                                            m_loadedWindowEndFrame,
                                            currentTick);
}

// ─── warmCacheAsync ─────────────────────────────────────────────────────────

void AudioPlaybackService::warmCacheAsync()
{
    if (!m_timeline) return;

    if (m_warmFuture.valid()) {
        if (m_warmFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            m_prefetchBusySkips.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_warmFuture.wait();
    }

    m_warmCancel.store(false);

    struct PendingPage {
        std::string path;
        std::string cacheKey;
        int64_t startFrame{0};
        int64_t frameCount{0};
    };
    std::vector<PendingPage> uncachedPages;
    const auto audioWindows = prefetchTimelineAudioWindows(m_playbackController);
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        for (const auto& audioWindow : audioWindows) {
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                auto* track = m_timeline->track(ti);
                if (!track || track->type() != TrackType::Audio) continue;
                for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                    auto* clip = track->clip(ci);
                    if (!clip) continue;
                    auto* audioClip = dynamic_cast<AudioClip*>(clip);
                    if (!audioClip) continue;
                    const auto& path = audioClip->mediaPath();
                    if (path.empty()) continue;
                    const auto region = computeTimelineAudioRegion(*audioClip, &audioWindow);
                    if (!region) continue;
                    const auto pageRequests = makeAudioPageRequests(
                        path, region->sourceStartFrame, region->sourceFrameCount);
                    for (const auto& request : pageRequests) {
                        if (m_decodeCache.find(request.cacheKey) == m_decodeCache.end()) {
                            uncachedPages.push_back(PendingPage{
                                request.path, request.cacheKey,
                                request.startFrame, request.frameCount
                            });
                        }
                    }
                }
            }
        }
    }

    if (uncachedPages.empty()) return;

    // Deduplicate
    std::sort(uncachedPages.begin(), uncachedPages.end(),
              [](const PendingPage& a, const PendingPage& b) {
                  return a.path != b.path ? a.path < b.path : a.startFrame < b.startFrame;
              });
    uncachedPages.erase(
        std::unique(uncachedPages.begin(), uncachedPages.end(),
                    [](const PendingPage& a, const PendingPage& b) { return a.cacheKey == b.cacheKey; }),
        uncachedPages.end());

    const auto primaryWindow = audioWindows.front();
    m_prefetchRequests.fetch_add(static_cast<uint64_t>(uncachedPages.size()), std::memory_order_relaxed);
    spdlog::info("warmAudioCacheAsync: pre-decoding {} audio page(s) in background for windows [{}..{}) + lookahead",
                 uncachedPages.size(), primaryWindow.startFrame, primaryWindow.endFrame);

    m_warmFuture = std::async(std::launch::async,
        [this, pages = std::move(uncachedPages)]() {
            AudioFile file;
            std::string openPath;
            bool fileReady = false;

            for (const auto& page : pages) {
                if (m_warmCancel.load()) break;

                if (!fileReady || openPath != page.path) {
                    file.close();
                    fileReady = file.open(page.path);
                    openPath  = page.path;
                    if (!fileReady) {
                        spdlog::warn("warmAudioCacheAsync: failed to open '{}'", page.path);
                        continue;
                    }
                }

                auto buffer = std::make_shared<std::vector<float>>();
                const int64_t framesRead = file.readRegionResampled(
                    page.startFrame, page.frameCount, 48000, *buffer);
                if (framesRead <= 0 || buffer->empty()) continue;

                CachedDecode entry;
                entry.startFrame  = page.startFrame;
                entry.channels    = static_cast<uint16_t>(file.info().channels);
                entry.samples     = buffer;
                entry.totalFrames = framesRead;
                entry.bytes       = buffer->size() * sizeof(float);

                {
                    std::lock_guard<std::mutex> lock(m_decodeMutex);
                    if (m_decodeCache.find(page.cacheKey) == m_decodeCache.end()) {
                        entry.lastUseSerial = ++m_decodeUseSerial;
                        m_decodeCache[page.cacheKey] = std::move(entry);
                        m_prefetchInsertions.fetch_add(1, std::memory_order_relaxed);
                        pruneDecodeCacheLocked();
                    }
                }
            }
            m_prefetchCompletions.fetch_add(1, std::memory_order_relaxed);
            logPerfSnapshot("prefetch-complete");
            spdlog::info("warmAudioCacheAsync: background decode complete");
        });
}

} // namespace rt
