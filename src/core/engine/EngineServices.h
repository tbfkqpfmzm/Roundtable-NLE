#pragma once

namespace rt {

class AudioEngine;
class AVSyncClock;
class CommandStack;
class MediaPool;
class ModelManager;
class PlaybackController;
class ShortcutManager;
class Timeline;

struct LegacyServiceBindings
{
    Timeline* timeline{nullptr};
    CommandStack* commandStack{nullptr};
    ShortcutManager* shortcutManager{nullptr};
    AudioEngine* audioEngine{nullptr};
    AVSyncClock* syncClock{nullptr};
    PlaybackController* playbackController{nullptr};
    MediaPool* mediaPool{nullptr};
    ModelManager* modelManager{nullptr};
};

class EngineServices
{
public:
    void bindLegacyServices(LegacyServiceBindings bindings) noexcept
    {
        m_bindings = bindings;
    }

    void clear() noexcept
    {
        m_bindings = {};
    }

    [[nodiscard]] Timeline* timeline() const noexcept { return m_bindings.timeline; }
    [[nodiscard]] CommandStack* commandStack() const noexcept { return m_bindings.commandStack; }
    [[nodiscard]] ShortcutManager* shortcutManager() const noexcept { return m_bindings.shortcutManager; }
    [[nodiscard]] AudioEngine* audioEngine() const noexcept { return m_bindings.audioEngine; }
    [[nodiscard]] AVSyncClock* syncClock() const noexcept { return m_bindings.syncClock; }
    [[nodiscard]] PlaybackController* playbackController() const noexcept { return m_bindings.playbackController; }
    [[nodiscard]] MediaPool* mediaPool() const noexcept { return m_bindings.mediaPool; }
    [[nodiscard]] ModelManager* modelManager() const noexcept { return m_bindings.modelManager; }

    [[nodiscard]] bool hasTimelineEditingServices() const noexcept
    {
        return m_bindings.timeline && m_bindings.commandStack;
    }

    [[nodiscard]] bool hasPlaybackServices() const noexcept
    {
        return m_bindings.playbackController && m_bindings.mediaPool;
    }

    [[nodiscard]] bool hasAudioClockServices() const noexcept
    {
        return m_bindings.audioEngine && m_bindings.syncClock;
    }

private:
    LegacyServiceBindings m_bindings;
};

} // namespace rt