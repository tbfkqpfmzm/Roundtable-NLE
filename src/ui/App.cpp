/*
 * App.cpp — Application init and subsystem ownership.
 *
 * Step 26: Main Window & Workspace
 */

#include "App.h"
#include "MainWindow.h"
#include "ShortcutManager.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "CompositeService.h"
#include "Theme.h"

#include "command/CommandStack.h"
#include "media/AudioEngine.h"
#include "media/VideoDecoder.h"
#include "media/AVSyncClock.h"
#include "media/MediaPool.h"
#include "media/MediaSourceService.h"
#include "media/CacheCoordinator.h"
#include "media/UnifiedCache.h"
#include "media/FrameCache.h"
#include "media/DiskFrameCache.h"
#include "media/PlaybackController.h"
#include "timeline/Timeline.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#endif

#include "GpuContext.h"
#include "ShutdownPhases.h"

#include "QtHelpers.h"

#include "Settings.h"
#include <QSettings>
#include <spdlog/spdlog.h>

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rt {

namespace {

/// Query installed physical RAM (bytes).  Returns 0 on failure.
size_t queryTotalPhysicalRam()
{
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        return static_cast<size_t>(ms.ullTotalPhys);
#endif
    return 0;
}

/// Compute the in-memory FrameCache budget.
/// Target: ~33% of installed RAM, clamped 4–24 GB.  Short character
/// loops at Half tier need ~1.5 GB per 200-frame clip; this budget
/// ensures 10+ loops stay fully cached with no LRU thrashing.
size_t computeFrameCacheBudget()
{
    const size_t totalRam = queryTotalPhysicalRam();
    constexpr size_t kGiB = 1024ull * 1024ull * 1024ull;
    constexpr size_t kMin = 4  * kGiB;
    constexpr size_t kMax = 24 * kGiB;
    if (totalRam == 0) return 12 * kGiB;
    size_t budget = totalRam / 3;         // 33%
    return std::clamp(budget, kMin, kMax);
}

/// Compute the DiskFrameCache budget.  Disk is much cheaper than RAM,
/// but a huge cache triggers expensive full-directory rescans during
/// LRU enforcement (recursive_directory_iterator + sort + rebuild
/// index on every overflow).  Keep it modest.
size_t computeDiskFrameCacheBudget()
{
    constexpr size_t kGiB = 1024ull * 1024ull * 1024ull;
    return 8 * kGiB;
}

} // namespace

App* App::s_instance = nullptr;

App::App()
{
    s_instance = this;
}

App::~App()
{
    auto& sm = ShutdownManager::instance();

    // Phase 1: Stop all background threads.
    // A10: explicitly stop the PlaybackScheduler (clock + producer +
    // presenter threads) so they can't call into the CompositeService
    // while MainWindow's widget tree is torn down in Phase 3.  The
    // presenter and producer hold callback lambdas that capture
    // CompositeService pointers; without an explicit stop here the
    // join would happen as a side-effect of ProgramMonitor's
    // destructor late in Phase 3.
    sm.advanceTo(ShutdownPhase::Phase1_StopThreads);
    if (m_mainWindow) {
        if (auto* tw = m_mainWindow->timelineWorkspace()) {
            if (auto* pm = tw->programMonitor())
                pm->stopPlaybackPipeline();
        }
    }
    if (m_audioEngine) m_audioEngine->shutdown();
    m_audioEngine.reset();
    if (m_scanThread.joinable()) m_scanThread.join();

    // Phase 2: Disconnect cross-component signals
    sm.advanceTo(ShutdownPhase::Phase2_Disconnect);
    m_playbackController.reset();
    m_syncClock.reset();
    m_mediaPool.reset();

    // Phase 3: Destroy Qt widget tree
    sm.advanceTo(ShutdownPhase::Phase3_DestroyQt);
    m_mainWindow.reset();

    // Phase 4: Destroy GPU resources
    sm.advanceTo(ShutdownPhase::Phase4_DestroyGpu);
    GpuContext::get().shutdown();

    // Final cleanup
    m_modelManager.reset();
    m_shortcutManager.reset();
    m_commandStack.reset();
    m_timeline.reset();

    sm.advanceTo(ShutdownPhase::Phase5_Done);

    if (s_instance == this)
        s_instance = nullptr;
}

bool App::init()
{
    if (m_initialized) return true;

    spdlog::info("App::init() — creating core subsystems");

    // ── Apply theme (load user preference, default: PremiereDark) ────
    {
        auto themeSettings = rt::appSettings();
        int presetIdx = themeSettings.value("ThemePreset", 0).toInt();
        auto preset = static_cast<ThemePreset>(
            std::clamp(presetIdx, 0, 0));
        Theme::apply(preset);
    }

    // ── Hardware decode preference ──────────────────────────────────────
    {
        auto s = rt::appSettings();
        int mode = s.value("HardwareDecodeMode", 0).toInt();
        setForceSoftwareDecode(mode == 1);
        spdlog::info("App: hardware decode mode = {} ({})",
                     mode, mode == 1 ? "software-only" : "auto (prefer GPU)");
    }

    // ── Core subsystems ─────────────────────────────────────────────────
    m_timeline = std::make_unique<Timeline>();
    m_commandStack = std::make_unique<CommandStack>();
    m_shortcutManager = std::make_unique<ShortcutManager>();
    m_audioEngine = std::make_unique<AudioEngine>();
    if (!m_audioEngine->initialize()) {
        spdlog::warn("App: AudioEngine failed to initialize — playback disabled");
    }

    // ── Cache coordinator (system-adaptive budgets) ─────────────────
    m_cacheCoordinator = std::make_unique<CacheCoordinator>();

    // ── Unified cache coordinator (generation + playhead-window) ────
    // Phase B: overlays the existing CPU + GPU caches with a single
    // generation counter and per-media playback-window pinning.  See
    // UnifiedCache.h for the design rationale (coordinator vs full
    // replacement).
    m_unifiedCache = std::make_unique<UnifiedCache>();

    // ── AV sync clock (audio-driven master clock) ────────────────────
    m_syncClock = std::make_unique<AVSyncClock>();
    m_audioEngine->setSyncClock(m_syncClock.get());
    // ── Media pool (shared video decoders + frame cache) ────────
    {
        // Create FrameCache with a reasonable initial budget; the
        // CacheCoordinator will override it with a system-adaptive budget.
        auto frameCache = std::make_shared<FrameCache>(8ull * 1024 * 1024 * 1024);
        m_cacheCoordinator->setFrameCache(frameCache.get());
        m_unifiedCache->setFrameCache(frameCache.get());
        m_mediaPool = std::make_unique<MediaPool>(std::move(frameCache));
    }

    // ── Disk frame cache (persistent second-level cache) ────────
    {
        auto diskCache = std::make_shared<DiskFrameCache>(
            std::filesystem::path(rt::userDataDir().toStdString()) / "cache/frames",
            8ull * 1024 * 1024 * 1024);  // initial budget, coordinator will adjust
        m_cacheCoordinator->setDiskCache(diskCache.get());
        m_mediaPool->setDiskCache(std::move(diskCache));
    }

    // ── MediaSourceService (shared facade over MediaPool) ──────────
    m_mediaSourceService = std::make_unique<MediaSourceService>(m_mediaPool.get());

    // ── Playback controller (transport logic) ───────────────────────
    m_playbackController = std::make_unique<PlaybackController>();
    m_playbackController->setTimeline(m_timeline.get());
    m_playbackController->setAudioEngine(m_audioEngine.get());
    m_playbackController->setSyncClock(m_syncClock.get());
    m_playbackController->setFrameRate(60.0);
    // \u2500\u2500 Model Manager \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
#ifdef ROUNDTABLE_HAS_SPINE
    m_modelManager = std::make_unique<ModelManager>();
    // Start scan in background thread — UI can show while scanning proceeds
    // All characters are now in assets/characters/ (downloaded in-program, not AppData)
    m_scanThread = std::thread([this]() {
        m_modelManager->scan("assets");
    });
#endif
    // ── GPU Context (Vulkan) ─────────────────────────────────────────────
    // Initialize early so subsystems can use GPU resources.
    // Surface is VK_NULL_HANDLE for now — the VulkanViewport will create
    // its own surface later and the device already picks a present queue.
    //
    // Skip GPU init when running in --capture-workspace mode — the capture
    // only needs dock widgets arranged, no rendering.  GPU init can hang
    // during headless capture (driver timeout, missing display, etc.).
    {
        bool capturing = false;
        for (const auto& arg : QCoreApplication::arguments()) {
            if (arg == QStringLiteral("--capture-workspace")) {
                capturing = true;
                break;
            }
        }
        if (!capturing) {
            if (!GpuContext::get().init()) {
                spdlog::warn("App: GPU context failed — falling back to software compositor");
            }
        } else {
            spdlog::info("App: skipping GPU init (--capture-workspace mode)");
        }
    }

    // System-adaptive FrameCache budget has been applied by
    // CacheCoordinator::setFrameCache().  Log it.
    m_cacheCoordinator->logBudgets();

    // Register NLE keyboard defaults
    m_shortcutManager->registerNLEDefaults();
    m_shortcutManager->loadFromSettings();

    m_initialized = true;
    spdlog::info("App::init() — core subsystems ready");
    return true;
}

bool App::createMainWindow()
{
    if (!m_initialized) {
        spdlog::error("App::createMainWindow() called before init()");
        return false;
    }

    spdlog::info("App::createMainWindow() — building UI");

    m_mainWindow = std::make_unique<MainWindow>();

    // A8: Install fatal-GPU-failure callback as soon as the MainWindow
    // exists.  Previously this was done just before showMaximized(), but
    // buildPanels()/applyDefaultLayout() can synchronously trigger Vulkan
    // work (program-monitor probe, thumbnail decode), and if that first
    // GPU work hits VK_ERROR_DEVICE_LOST the dialog wouldn't fire.
    {
        MainWindow* mw = m_mainWindow.get();
        GpuContext::get().setFatalFailureCallback([mw]() {
            if (!mw) return;
            QMetaObject::invokeMethod(mw, [mw]() {
                mw->showGpuFatalError();
            }, Qt::QueuedConnection);
        });
    }

    // Wire subsystems
    m_mainWindow->setTimeline(m_timeline.get());
    m_mainWindow->setCommandStack(m_commandStack.get());
    m_mainWindow->setShortcutManager(m_shortcutManager.get());
    m_mainWindow->setAudioEngine(m_audioEngine.get());
    m_mainWindow->setPlaybackController(m_playbackController.get());
    m_mainWindow->setMediaPool(m_mediaPool.get());
    m_mainWindow->setMediaSourceService(m_mediaSourceService.get());
#ifdef ROUNDTABLE_HAS_SPINE
    m_mainWindow->setModelManager(m_modelManager.get());
#endif

    m_mainWindow->buildPanels();
    m_mainWindow->buildMenuBar();
    m_mainWindow->applyDefaultLayout();

    // ── Handle --capture-workspace ──────────────────────────────────────
    // If launched with --capture-workspace <path>, save the layout and exit
    // before showing the window (used during publish to bundle the default
    // panel arrangement for new users).
    // Optional --load-preset <name> loads a saved workspace preset first.
    {
        QStringList args = QCoreApplication::arguments();
        int idx = args.indexOf("--capture-workspace");
        if (idx >= 0 && idx + 1 < args.size()) {
            QString outputPath = args[idx + 1];

            // If --load-preset was given, load that workspace preset first.
            int presetIdx = args.indexOf("--load-preset");
            if (presetIdx >= 0 && presetIdx + 1 < args.size()) {
                QString presetName = args[presetIdx + 1];
                spdlog::info("Loading workspace preset '{}'", presetName.toStdString());
                m_mainWindow->restoreWorkspacePreset(presetName);
            }

            spdlog::info("Capturing workspace layout to {}", outputPath.toStdString());
            m_mainWindow->saveWorkspaceToFile(outputPath);
            spdlog::info("Workspace captured — exiting");
            // Schedule a clean exit — can't return false because App
            // is partially constructed; use a zero-shot quit instead.
            QMetaObject::invokeMethod(QCoreApplication::instance(), "quit",
                                      Qt::QueuedConnection);
            return true;
        }
    }

    // (Fatal-GPU-failure callback was installed earlier — see A8 block
    // immediately after MainWindow construction.)

    m_mainWindow->showMaximized();

    // Finish async model scan — by now the scan thread has been running
    // in parallel with UI construction so it's likely already done.
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_scanThread.joinable()) {
        m_scanThread.join();
        spdlog::info("App: ModelManager scanned {} characters", m_modelManager->entries().size());
        // Re-set the model manager to refresh the character list
        if (m_mainWindow)
            m_mainWindow->setModelManager(m_modelManager.get());
    }
#endif

    // ── Wire CacheCoordinator + UnifiedCache into CompositeService ───
    // Must happen after MainWindow builds panels (which creates
    // TimelineWorkspace → CompositeService → CompositeEngine).
    {
        auto* tw = m_mainWindow->timelineWorkspace();
        if (tw) {
            auto* cs = tw->compositeService();
            if (cs) {
                cs->setCacheCoordinator(m_cacheCoordinator.get());
                cs->setUnifiedCache(m_unifiedCache.get());
            }
        }
        m_cacheCoordinator->logBudgets();
    }

    spdlog::info("App::createMainWindow() — UI ready");
    return true;
}

} // namespace rt
