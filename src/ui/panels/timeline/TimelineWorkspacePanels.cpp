/*
 * TimelineWorkspacePanels.cpp - Panel creation and dock layout for TimelineWorkspace.
 * Split from TimelineWorkspace.cpp for maintainability.
 */
// **Must** come before any header that pulls in <vulkan/vulkan.h> so
// volk can define VK_NO_PROTOTYPES first.  Without this, calls like
// vkQueueWaitIdle resolve to a direct function symbol instead of a
// function-pointer variable, causing an ACCESS_VIOLATION crash.
#include <volk.h>

#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/DockLayoutManager.h"
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "panels/timeline/ClipRenderers.h"
#include "Theme.h"

// Panels
#include "panels/audio/AudioMixer.h"
// ShotPanel removed ï¿½ merged into PropertiesPanel
#include "panels/characters/CharactersPanel.h"
#include "panels/library/LibraryPanel.h"
#include "panels/effects/EffectsPanel.h"
#include "panels/project/HistoryPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ScopesPanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "media/PlaybackScheduler.h"
#include "panels/project/ProjectBin.h"

#include "widgets/MiniTimeline.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/ColorGradingPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

// Widgets
#include "widgets/DockTitleBar.h"
#include "widgets/ToolButton.h"
#include "widgets/VUMeter.h"
#include "viewport/Viewport.h"
#include "viewport/TransformOverlayWidget.h"

// Core
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/MarkerCommands.h"
#include "project/Project.h"
#include "MainWindow.h"
#include "media/AudioEngine.h"
#include "media/AudioFile.h"
#include "media/AudioPlaybackService.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include "timeline/AudioClip.h"
#include "timeline/EditOperations.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/Timeline.h"
#include "timeline/MediaRelinker.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/EffectCommands.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "stb_image.h"
#endif

#include <QDockWidget>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QSizePolicy>
#include <QSlider>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QApplication>
#include <QChildEvent>
#include <QCursor>
#include <QTabBar>
#include <QMenu>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_set>

// GPU compositing
#include "GpuContext.h"
#include "Compositor.h"
#include "GpuTextureCache.h"
#include "EffectProcessor.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"
#include "vulkan/Texture.h"

#include <QAbstractNativeEventFilter>

#ifdef _WIN32
#include <windows.h>  // for __try/__except SEH to catch ACCESS_VIOLATION
#include <windowsx.h> // GET_X_LPARAM, GET_Y_LPARAM
#endif

#include "panels/timeline/DockBehavior.h"

namespace rt {

void TimelineWorkspace::installEdgeGuard(QMainWindow* edgeCol)
{
    if (!edgeCol || !m_innerMainWindow) return;
    auto* guard = EdgeColumnGuard::forColumn(edgeCol);
    if (!guard)
        guard = new EdgeColumnGuard(edgeCol, m_innerMainWindow);
    for (auto* dock : edgeCol->findChildren<QDockWidget*>()) {
        if (!dock->isFloating())
            guard->watchDock(dock);
    }
}

void TimelineWorkspace::buildPanels()
{
    if (m_panelsBuilt) return;

    // Suppress paint events during widget construction to prevent
    // paint -> layout -> repaint recursion while geometry is inconsistent.
    setUpdatesEnabled(false);

    spdlog::info("TimelineWorkspace::buildPanels() - dockable Premiere Pro-style layout");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // -- Floating dock resize filter (Windows only) -----------------------
#ifdef _WIN32
    static bool s_dockResizeFilterInstalled = false;
    if (!s_dockResizeFilterInstalled) {
        QCoreApplication::instance()->installNativeEventFilter(
            new FloatingResizeFallbackFilter());
        s_dockResizeFilterInstalled = true;
    }
#endif

    // -- Nested QMainWindow for dock-widget support -----------------------
    // Using Qt::Widget flag so it acts as a child widget, not a top-level
    // window.  This gives us full QDockWidget rearranging inside the
    // TimelineWorkspace page.
    m_innerMainWindow = new QMainWindow(this);
    m_innerMainWindow->setWindowFlags(Qt::Widget);
    m_innerMainWindow->setDockNestingEnabled(true);
    m_innerMainWindow->setTabPosition(Qt::TopDockWidgetArea, QTabWidget::North);
    m_innerMainWindow->setTabPosition(Qt::BottomDockWidgetArea, QTabWidget::North);
    m_innerMainWindow->setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::North);
    m_innerMainWindow->setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);
    m_innerMainWindow->setAnimated(false);

    // Enable grouped dragging so docks can be separated from tab groups
    // and redocked into other areas (Premiere Pro-style panel rearranging).
    // Explicitly set all needed options: AllowNestedDocks enables splitting
    // within dock areas (dock below/above), AllowTabbedDocks enables tabify.
    m_innerMainWindow->setDockOptions(
        QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks |
        QMainWindow::GroupedDragging);

    // Theme color tokens for panel construction below
    const auto& tc = Theme::colors();
    const auto& m = Theme::metrics();

    // -- Helper: create a dock widget and register in the lookup map ------
    auto makeDock = [this](const QString& title, QWidget* widget,
                           QDockWidget::DockWidgetFeatures features =
                               QDockWidget::DockWidgetMovable |
                               QDockWidget::DockWidgetFloatable |
                               QDockWidget::DockWidgetClosable) -> QDockWidget*
    {
        auto* dock = new QDockWidget(title, m_innerMainWindow);
        dock->setFeatures(features);
        dock->setWidget(widget);
        dock->setObjectName(title);  // for saveState/restoreState
        dock->setTitleBarWidget(new DockTitleBar(dock, title));
        // Inner padding so panel content doesn't sit flush against the border
        dock->setContentsMargins(4, 0, 4, 4);
        m_dockWidgets.insert(title, dock);

        // When a dock floats, Qt re-creates its top-level window with
        // WS_CAPTION and a system caption bar, then hides the custom
        // titleBarWidget (nativeWindowDeco quirk).  We strip WS_CAPTION
        // immediately so Qt falls back to showing our DockTitleBar.
        // WS_THICKFRAME is kept for resize handles.
        connect(dock, &QDockWidget::topLevelChanged,
                dock, [dock](bool floating) {
            if (floating) {
                dock->show();
                dock->raise();
#ifdef _WIN32
                QTimer::singleShot(0, dock, [dock]() {
                    if (!dock->isFloating()) return;
                    WId wid = dock->effectiveWinId();
                    if (!wid) return;
                    HWND hwnd = reinterpret_cast<HWND>(wid);
                    HWND topHwnd = GetAncestor(hwnd, GA_ROOT);
                    if (topHwnd) hwnd = topHwnd;

                    // SAFETY: Do NOT touch the main application window.
                    // Verify the HWND we're about to modify belongs to a
                    // floating dock widget, not the top-level app window.
                    QWidget* ownerWidget = QWidget::find(
                        reinterpret_cast<WId>(hwnd));
                    if (!ownerWidget) return;
                    // The HWND must be either the dock itself, a
                    // QDockWidgetGroupWindow, or a direct floating
                    // container ï¿½ never the QMainWindow application.
                    if (qobject_cast<QMainWindow*>(ownerWidget)
                        && !qobject_cast<QDockWidget*>(ownerWidget)
                        && ownerWidget->objectName() != QStringLiteral("EdgeColumn")) {
                        return; // this is the app's main window ï¿½ skip
                    }

                    LONG style = GetWindowLong(hwnd, GWL_STYLE);
                    style &= ~WS_CAPTION;
                    style |= WS_THICKFRAME;
                    SetWindowLong(hwnd, GWL_STYLE, style);
                    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                                 | SWP_FRAMECHANGED);
                    // Force Qt to re-evaluate nativeWindowDeco ï¿½ updating
                    // the internal layout shows the custom titleBarWidget.
                    if (auto* dw = qobject_cast<QDockWidget*>(ownerWidget)) {
                        // Toggle features to force QDockWidgetLayout refresh
                        auto f = dw->features();
                        dw->setFeatures(f ^ QDockWidget::DockWidgetClosable);
                        dw->setFeatures(f);
                    }
                });
                installDockResizeSubclass(dock);
#endif
            }
        });

        return dock;
    };

    // =====================================================================
    //  TOP ROW PANELS -- each in its own dock widget
    // =====================================================================

    // -- Project Bin ------------------------------------------------------
    m_projectBin = new ProjectBin(this);
    m_projectBin->setMinimumWidth(180);
    if (m_mediaPool)
        m_projectBin->setMediaPool(m_mediaPool);
    if (m_mediaSourceService)
        m_projectBin->setMediaSourceService(m_mediaSourceService);
    // Auto-create a default project when user creates a sequence with no project open.
    // Ownership is transferred to MainWindow via autoProjectCreated signal.
    connect(m_projectBin, &ProjectBin::projectCreated,
            this, [this](Project* project) {
        m_projectBin->setProject(project);
        setProject(project);
        emit autoProjectCreated(project);
    });
    auto* dockProjectBin = makeDock("Project Bin", m_projectBin);

    // -- Source Monitor ---------------------------------------------------
    m_sourceMonitor = new SourceMonitor(this);
    m_sourceMonitor->setMinimumWidth(200);
    if (m_mediaPool)
        m_sourceMonitor->setMediaPool(m_mediaPool);
    if (m_mediaSourceService)
        m_sourceMonitor->setMediaSourceService(m_mediaSourceService);
    if (m_audioEngine)
        m_sourceMonitor->setAudioEngine(m_audioEngine);

    // Handle drops when pool isn't set on the SourceMonitor yet
    connect(m_sourceMonitor, &SourceMonitor::dropReceived,
            this, [this](uint64_t handle) {
        if (m_mediaPool && m_sourceMonitor)
            m_sourceMonitor->loadClip(handle, m_mediaPool);
    });

    // Handle sequence drops into the Source Monitor
    connect(m_sourceMonitor, &SourceMonitor::sequenceDropReceived,
            this, [this](size_t seqIdx) {
        if (!m_project || !m_sourceMonitor) return;
        if (seqIdx >= m_project->sequenceCount()) return;

        auto* seq = m_project->sequence(seqIdx);
        if (!seq) return;

        QString name = QString::fromStdString(seq->name());
        int64_t dur  = seq->duration();
        double  fps  = 24.0;

        // Provide a frame provider that composites the inner sequence
        SourceMonitor::SequenceFrameProvider provider =
            [this, seqIdx](int64_t tick, uint32_t w, uint32_t h, bool scrub)
                -> std::shared_ptr<CachedFrame>
        {
            if (!m_project || seqIdx >= m_project->sequenceCount())
                return nullptr;
            auto* innerTimeline = m_project->sequence(seqIdx);
            if (!innerTimeline || innerTimeline == m_timeline)
                return nullptr;

            // Temporarily swap to the inner timeline for compositing
            Timeline* outerTimeline = m_timeline;
            m_timeline = innerTimeline;
            auto frame = compositeFrame(tick, w, h, scrub);
            m_timeline = outerTimeline;
            return frame;
        };

        m_sourceMonitor->loadSequence(seqIdx, name, dur, fps, std::move(provider));
    });

    // Mutual exclusion: stop timeline when source monitor starts playing
    connect(m_sourceMonitor, &SourceMonitor::playbackStarted,
            this, [this]() {
        if (m_playbackController &&
            m_playbackController->state() != PlayState::Stopped &&
            m_playbackController->state() != PlayState::Paused) {
            m_playbackController->stop();
        }
        // Source monitor takes over the engine's track sources, so timeline
        // must reload its own sources the next time it needs audio.
        invalidateAudioSources();
    });

    auto* dockSourceMonitor = makeDock("Source Monitor", m_sourceMonitor);

    // -- Program Monitor --------------------------------------------------
    m_programMonitor = new ProgramMonitor(this);
    m_programMonitor->setMinimumWidth(200);
    if (m_timeline) m_programMonitor->setTimeline(m_timeline);
    if (m_playbackController) m_programMonitor->setController(m_playbackController);
    auto* dockProgramMonitor = makeDock("Program Monitor", m_programMonitor);

    // -- Properties (transition/clip property editor, docked) ---------------
    m_propertiesPanel = new PropertiesPanel(this);
    if (m_commandStack) m_propertiesPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_propertiesPanel->setTimeline(m_timeline);
    if (m_modelManager) m_propertiesPanel->setModelManager(m_modelManager);
    if (m_shotPresetManager) m_propertiesPanel->setShotPresetManager(m_shotPresetManager);
#ifdef ROUNDTABLE_HAS_SPINE
    m_propertiesPanel->setAnimationNamesProvider(
        [this](const std::string& charName, const std::string& outfit, int stance)
            -> std::vector<std::string> {
        const std::string key = charName + "|" + outfit + "|" + std::to_string(stance);
        if (!m_compositeService) return {};
        {
            auto* cached = m_compositeService->findAnimNames(key);
            if (cached) return *cached;
        }
        auto sharedData = m_compositeService->findSpineSharedData(key);
        for (auto& [cid, state] : m_compositeService->spineCache()) {
            if (state && state->shared && state->shared == sharedData
                && state->engine.isLoaded()) {
                auto anims = state->engine.animation().listAnimations();
                std::vector<std::string> names;
                names.reserve(anims.size());
                for (const auto& info : anims) {
                    if (info.name == "talk_end" || info.name == "talk_start")
                        continue;
                    names.push_back(info.name);
                }
                m_compositeService->storeAnimNames(key, names);
                return names;
            }
        }
        if (sharedData && !sharedData->skelBytes.empty() && !sharedData->atlasText.empty()) {
            SpineEngine tmpEngine;
            if (tmpEngine.loadSkeletonFromBuffers(
                    sharedData->skelBytes, sharedData->atlasText,
                    sharedData->atlasDir,
                    sharedData->skelPath, sharedData->atlasPath)) {
                auto anims = tmpEngine.animation().listAnimations();
                std::vector<std::string> names;
                names.reserve(anims.size());
                for (const auto& info : anims) {
                    if (info.name == "talk_end" || info.name == "talk_start")
                        continue;
                    names.push_back(info.name);
                }
                m_compositeService->storeAnimNames(key, names);
                return names;
            }
        }
        return {};
    });
#endif
    // Video character animations: list .mov files in the AnimationVideoCache dir
    m_propertiesPanel->setVideoAnimNamesProvider(
        [](const std::string& charName, const std::string& outfit)
            -> std::vector<std::string> {
        namespace fs = std::filesystem;
        std::vector<std::string> names;
        // Search across all format subdirectories
        static const char* fmtDirs[] = {"H264_Green", "H264_Blue", "H264_Custom", "ProRes"};
        fs::path dir;
        for (const auto* fmt : fmtDirs) {
            auto candidate = fs::path("assets/Converted") / fmt / charName / outfit;
            std::error_code ec2;
            if (fs::exists(candidate, ec2)) { dir = candidate; break; }
        }
        if (dir.empty()) dir = fs::path("assets/Converted") / "H264_Green" / charName / outfit;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string stem = entry.path().stem().string();
            // Skip talk variants ï¿½ they're the same animation with mouth open
            if (stem.size() > 5 && stem.substr(stem.size() - 5) == "_talk")
                continue;
            names.push_back(stem);
        }
        std::sort(names.begin(), names.end());
        return names;
    });
    auto* dockProperties = makeDock("Properties", m_propertiesPanel);

    // -- Effect Controls (replaces Properties in the dock) ----------------
    m_effectControlsPanel = new EffectControlsPanel(this);
    if (m_commandStack) m_effectControlsPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_effectControlsPanel->setTimeline(m_timeline);
    auto* dockEffectControls = makeDock("Effect Controls", m_effectControlsPanel);

    //

    // -- Color Correction (dedicated color grading panel) ------------------
    m_ColorGradingPanel = new ColorGradingPanel(this);
    if (m_commandStack) m_ColorGradingPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_ColorGradingPanel->setTimeline(m_timeline);
    auto* dockColorGrading = makeDock("Color Correction", m_ColorGradingPanel);

    // -- Effects ----------------------------------------------------------
    m_effectsPanel = new EffectsPanel(this);
    if (m_commandStack) m_effectsPanel->setCommandStack(m_commandStack);
    auto* dockEffects = makeDock("Effects", m_effectsPanel);

    // -- History ----------------------------------------------------------
    m_historyPanel = new HistoryPanel(this);
    if (m_commandStack) m_historyPanel->setCommandStack(m_commandStack);
    auto* dockHistory = makeDock("History", m_historyPanel);

    // -- Scopes (Waveform / Vectorscope / Histogram) ----------------------
    m_scopesPanel = new ScopesPanel(this);
    auto* dockScopes = makeDock("Scopes", m_scopesPanel);

    // -- Essential Graphics -----------------------------------------------
    m_GraphicsEditorPanel = new GraphicsEditorPanel(this);
    if (m_commandStack) m_GraphicsEditorPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_GraphicsEditorPanel->setTimeline(m_timeline);
    auto* dockEssentialGraphics = makeDock("Essential Graphics", m_GraphicsEditorPanel);

    // -- Library (Characters / Backgrounds / Videos / Audio tabs) --------
    m_libraryPanel = new LibraryPanel(this);
    m_charactersPanel = m_libraryPanel->charactersPanel();
    m_charactersPanel->setModelManager(m_modelManager);
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_compositeService)
        m_charactersPanel->setAnimVideoCache(m_compositeService->animVideoCache());
    // Refresh the characters tree whenever a cache render completes
    // (callback fires on a worker thread, so use queued invocation).
    if (m_compositeService && m_compositeService->animVideoCache()) {
        m_compositeService->animVideoCache()->setCompletionCallback(
            [panel = m_charactersPanel](const std::string&, const std::string&,
                                         const std::string&, bool) {
                QMetaObject::invokeMethod(panel, &CharactersPanel::refresh,
                                          Qt::QueuedConnection);
            });
    }
#endif
    m_charactersPanel->setMediaPool(m_mediaPool);
    m_libraryPanel->refreshCurrentTab();
    // Re-link asset ? walk timeline and update every clip referencing oldPath.
    connect(m_libraryPanel, &LibraryPanel::mediaRelinkRequested,
            this, [this](const QString& oldPath, const QString& newPath) {
        const std::string oldStd = oldPath.toStdString();
        const std::string newStd = newPath.toStdString();
        int n = 0;
        if (m_timeline)
            n += MediaRelinker::relinkPath(m_timeline, oldStd, newStd);
        if (m_shotPresetManager)
            n += MediaRelinker::relinkPresetBackground(m_shotPresetManager, oldStd, newStd);
        if (n > 0) {
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            if (m_timelinePanel) m_timelinePanel->update();
        }
    });
    // Double-click a Library asset ? preview it in the Source Monitor.
    connect(m_libraryPanel, &LibraryPanel::loadInSourceMonitor,
            this, [this](const QString& filePath) {
        if (!m_sourceMonitor || !m_mediaPool || filePath.isEmpty()) return;
        const auto handle = m_mediaPool->open(std::filesystem::path(filePath.toStdString()));
        if (handle == 0) return;
        m_sourceMonitor->loadClip(handle, m_mediaPool);
    });
    auto* dockCharacters = makeDock("Library", m_libraryPanel);

    // -- Audio Mixer ------------------------------------------------------
    m_audioMixer = new AudioMixer(this);
    if (m_timeline) m_audioMixer->setTimeline(m_timeline);
    if (m_audioEngine) m_audioMixer->setAudioEngine(m_audioEngine);
    if (m_commandStack) m_audioMixer->setCommandStack(m_commandStack);
    auto* dockAudioMixer = makeDock("Audio Mixer", m_audioMixer);

    // -- Shots panel removed: character/shot controls merged into Properties --

    // -- Audio Meters (VU Meter) ------------------------------------------
    auto* meterContainer = new QWidget;
    meterContainer->setMinimumWidth(30);
    meterContainer->setMaximumWidth(200);
    meterContainer->setStyleSheet(QStringLiteral("background: %1;")
        .arg(Theme::hex(tc.surface0)));
    auto* meterLayout = new QVBoxLayout(meterContainer);
    meterLayout->setContentsMargins(m.spacingXs, m.spacingMd, m.spacingXs, m.spacingMd);
    meterLayout->setSpacing(m.spacingXs);

    m_timelineVUMeter = new VUMeter(meterContainer);
    m_timelineVUMeter->setChannelCount(2);
    m_timelineVUMeter->setOrientation(VUMeter::Orientation::Vertical);
    m_timelineVUMeter->setScaleVisible(true);
    m_timelineVUMeter->setPeakHoldEnabled(true);
    m_timelineVUMeter->setMinimumHeight(100);
    meterLayout->addWidget(m_timelineVUMeter, 1);

    auto* dbLabel = new QLabel(QStringLiteral("dB"));
    dbLabel->setAlignment(Qt::AlignCenter);
    dbLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 9px; border: none;")
        .arg(Theme::hex(tc.textTertiary)));
    meterLayout->addWidget(dbLabel);

    auto* dockAudioMeters = makeDock("Audio Meters", meterContainer);
    // Minimum width so the stacked "Audio\nMeters" title is never cropped.
    dockAudioMeters->setMinimumWidth(60);

    // =====================================================================
    //  CENTRAL WIDGET: Tool Column + Toolbar + Timeline Panel
    // =====================================================================
    auto* centralContainer = new QWidget;
    centralContainer->setMinimumHeight(200);
    auto* centralLayout = new QHBoxLayout(centralContainer);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    // -- Left: Vertical Tool Column (Premiere Pro style) ------------------
    auto* toolColumn = new QWidget;
    toolColumn->setMinimumWidth(48);
    toolColumn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    toolColumn->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; }")
        .arg(Theme::hex(tc.surface0)));
    auto* toolColumnLayout = new QVBoxLayout(toolColumn);
    toolColumnLayout->setContentsMargins(4, 6, 4, 0);
    toolColumnLayout->setSpacing(0);
    toolColumnLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    auto* btnSelection = new ToolButton(ToolButton::Selection);
    btnSelection->setFixedSize(40, 34);
    btnSelection->setToolTip(QStringLiteral("Selection Tool (V)"));
    btnSelection->setChecked(true);

    auto* btnRazor = new ToolButton(ToolButton::Razor);
    btnRazor->setFixedSize(40, 34);
    btnRazor->setToolTip(QStringLiteral("Razor Tool (C)"));

    auto* btnRipple = new ToolButton(ToolButton::Ripple);
    btnRipple->setFixedSize(40, 34);
    btnRipple->setToolTip(QStringLiteral("Ripple Edit Tool (B)"));

    auto* btnRolling = new ToolButton(ToolButton::Rolling);
    btnRolling->setFixedSize(40, 34);
    btnRolling->setToolTip(QStringLiteral("Rolling Edit Tool (N)"));

    auto* btnSlip = new ToolButton(ToolButton::Slip);
    btnSlip->setFixedSize(40, 34);
    btnSlip->setToolTip(QStringLiteral("Slip Tool (Y)"));

    auto* btnSlide = new ToolButton(ToolButton::Slide);
    btnSlide->setFixedSize(40, 34);
    btnSlide->setToolTip(QStringLiteral("Slide Tool (U)"));

    auto* btnText = new ToolButton(ToolButton::Text);
    btnText->setFixedSize(40, 34);
    btnText->setToolTip(QStringLiteral("Text Tool (T)"));

    auto* btnZoom = new ToolButton(ToolButton::Zoom);
    btnZoom->setFixedSize(40, 34);
    btnZoom->setToolTip(QStringLiteral("Zoom Tool (Z) Ã¢â‚¬â€ Click: Zoom In, Alt+Click: Zoom Out"));

    // Helper to add a thin horizontal separator line between tool buttons
    auto addToolSep = [&]() {
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Plain);
        sep->setFixedHeight(1);
        sep->setStyleSheet(QStringLiteral(
            "QFrame { color: %1; }")
            .arg(Theme::hex(tc.border)));
        toolColumnLayout->addSpacing(3);
        toolColumnLayout->addWidget(sep);
        toolColumnLayout->addSpacing(3);
    };

    toolColumnLayout->addWidget(btnSelection);
    addToolSep();
    toolColumnLayout->addWidget(btnRazor);
    addToolSep();
    toolColumnLayout->addWidget(btnRipple);
    addToolSep();
    toolColumnLayout->addWidget(btnRolling);
    addToolSep();
    toolColumnLayout->addWidget(btnSlip);
    addToolSep();
    toolColumnLayout->addWidget(btnSlide);
    addToolSep();
    toolColumnLayout->addWidget(btnText);
    addToolSep();
    toolColumnLayout->addWidget(btnZoom);
    toolColumnLayout->addStretch();

    m_toolButtons[0] = btnSelection;
    m_toolButtons[1] = btnRipple;
    m_toolButtons[2] = btnRolling;
    m_toolButtons[3] = btnRazor;
    m_toolButtons[4] = btnSlip;
    m_toolButtons[5] = btnSlide;
    m_toolButtons[6] = btnText;
    m_toolButtons[7] = btnZoom;

    // -- Center: Toolbar + Timeline Panel ---------------------------------
    auto* centerContainer = new QWidget;
    auto* centerLayout = new QVBoxLayout(centerContainer);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Sequence tab bar row (top line) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    m_sequenceTabBar = new QTabBar;
    m_sequenceTabBar->setTabsClosable(true);
    m_sequenceTabBar->setExpanding(false);
    m_sequenceTabBar->setMovable(true);
    m_sequenceTabBar->setDrawBase(false);
    m_sequenceTabBar->setDocumentMode(true);
    m_sequenceTabBar->setStyleSheet(QStringLiteral(
        "QTabBar { background: %1; }"
        "QTabBar::tab { background: %1; color: %2; padding: 4px 12px; "
        "border: none; border-right: 1px solid %3; font-size: 12px; }"
        "QTabBar::tab:selected { background: %4; color: %5; font-weight: bold; }"
        "QTabBar::tab:hover { background: %6; }"
        "QTabBar::close-button { image: none; width: 12px; height: 12px; "
        "subcontrol-position: right; padding: 2px; }"
        "QTabBar::close-button:hover { background: %7; border-radius: 3px; }")
        .arg(Theme::hex(tc.surface1))
        .arg(Theme::hex(tc.textTertiary))
        .arg(Theme::hex(tc.border))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.textPrimary))
        .arg(Theme::hex(tc.surface2))
        .arg(Theme::hex(tc.error)));
    m_sequenceTabBar->addTab(QStringLiteral("Sequence 1"));
    connect(m_sequenceTabBar, &QTabBar::currentChanged, this, [this](int index) {
        if (m_suppressTabChange || index < 0) return;
        emit sequenceTabChanged(static_cast<size_t>(index));
    });

    // Close tab (Premiere Pro style ï¿½ don't allow closing the last sequence)
    connect(m_sequenceTabBar, &QTabBar::tabCloseRequested, this, [this](int index) {
        if (!m_project || m_project->sequenceCount() <= 1) return;
        emit sequenceTabClosed(static_cast<size_t>(index));
    });

    // Right-click context menu on tabs
    m_sequenceTabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sequenceTabBar, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        int tabIdx = m_sequenceTabBar->tabAt(pos);
        if (tabIdx < 0 || !m_project) return;

        QMenu menu(m_sequenceTabBar);
        QAction* renameAction = menu.addAction("Rename Sequence...");
        QAction* dupAction    = menu.addAction("Duplicate Sequence");
        menu.addSeparator();
        QAction* closeAction  = menu.addAction("Close");
        closeAction->setEnabled(m_project->sequenceCount() > 1);

        QAction* chosen = menu.exec(m_sequenceTabBar->mapToGlobal(pos));
        if (chosen == renameAction) {
            emit sequenceTabRenameRequested(static_cast<size_t>(tabIdx));
        } else if (chosen == dupAction) {
            emit sequenceTabDuplicateRequested(static_cast<size_t>(tabIdx));
        } else if (chosen == closeAction) {
            emit sequenceTabClosed(static_cast<size_t>(tabIdx));
        }
    });

    centerLayout->addWidget(m_sequenceTabBar);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Timeline toolbar row (Premiere Pro style) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Layout: [Timecode] | [Nest] [Snap] [Linked] [Marker] [Settings] [CC] | [spacer] | [-] [zoom slider] [+]
    auto* toolbar = new QWidget;
    toolbar->setFixedHeight(30);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget#timelineToolbar { background: %1; border-top: 1px solid %2; "
        "border-bottom: 1px solid %3; }")
        .arg(Theme::hex(tc.surface2))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.border)));
    toolbar->setObjectName(QStringLiteral("timelineToolbar"));

    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(6, 0, 6, 0);
    toolbarLayout->setSpacing(2);

    // Timecode counter (Premiere-style blue background)
    m_timelineTimecode = new QLabel(QStringLiteral("00;00;00;00"));
    m_timelineTimecode->setAlignment(Qt::AlignCenter);
    m_timelineTimecode->setFixedWidth(100);
    m_timelineTimecode->setFixedHeight(22);
    m_timelineTimecode->setStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 12px; "
        "font-weight: bold; color: %1; background: %2; "
        "padding: 1px 4px; border-radius: 2px; }")
        .arg(Theme::hex(tc.textBright))
        .arg(Theme::hex(tc.surface0)));
    toolbarLayout->addWidget(m_timelineTimecode);

    toolbarLayout->addSpacing(8);

    // Premiere Pro icon-style toolbar button
    QString tbIconStyle = QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: none; "
        "padding: 2px; font-size: 14px; min-width: 24px; min-height: 24px; }"
        "QToolButton:hover { background: %2; border-radius: 3px; }"
        "QToolButton:checked { color: %3; background: %4; border-radius: 3px; }")
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.accent))
        .arg(Theme::hex(tc.accentSubtle));

    // 1) Insert or overwrite sequences as nests or individual clips
    auto* btnNest = new QToolButton;
    btnNest->setText(QStringLiteral("\u29C9"));  // Ã¢Â§â€° (nested squares)
    btnNest->setToolTip(tr("Insert or overwrite sequences as nests or individual clips"));
    btnNest->setCheckable(true);
    btnNest->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnNest);

    // 2) Snap in Timeline (magnet)
    auto* btnSnap = new QToolButton;
    btnSnap->setText(QStringLiteral("\U0001F9F2"));  // Ã°Å¸Â§Â² magnet
    btnSnap->setToolTip(tr("Snap in Timeline (N)"));
    btnSnap->setCheckable(true);
    btnSnap->setChecked(true);
    btnSnap->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnSnap);

    // 3) Linked Selection (chain link)
    auto* btnLinked = new QToolButton;
    btnLinked->setText(QStringLiteral("\U0001F517"));  // Ã°Å¸â€â€” link
    btnLinked->setToolTip(tr("Linked Selection"));
    btnLinked->setCheckable(true);
    btnLinked->setChecked(true);
    btnLinked->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnLinked);

    // 4) Add Marker
    auto* btnMarker = new QToolButton;
    btnMarker->setText(QStringLiteral("\u2666"));  // Ã¢â„¢Â¦ diamond
    btnMarker->setToolTip(tr("Add Marker (M)"));
    btnMarker->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnMarker);

    // 5) Caption Track Options
    auto* btnCC = new QToolButton;
    btnCC->setText(QStringLiteral("CC"));
    btnCC->setToolTip(tr("Caption Track Options"));
    btnCC->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: 1px solid %1; "
        "padding: 1px 3px; font-size: 11px; font-weight: bold; min-width: 22px; "
        "min-height: 16px; border-radius: 2px; }"
        "QToolButton:hover { background: %2; color: %3; border-color: %3; }")
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.textPrimary)));
    toolbarLayout->addWidget(btnCC);

    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbarLayout->addWidget(spacer);

    auto* zoomLabel = new QLabel(QStringLiteral("\u2212"));
    zoomLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 0 2px;")
        .arg(Theme::hex(tc.textTertiary)));
    toolbarLayout->addWidget(zoomLabel);

    auto* zoomSlider = new QSlider(Qt::Horizontal);
    zoomSlider->setRange(10, 500);
    zoomSlider->setValue(100);
    zoomSlider->setFixedWidth(120);
    zoomSlider->setToolTip("Timeline Zoom");
    zoomSlider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { background: %1; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %2; width: 10px; margin: -3px 0; border-radius: 5px; }"
        "QSlider::handle:horizontal:hover { background: %3; }")
        .arg(Theme::hex(tc.border))
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::hex(tc.textPrimary)));
    toolbarLayout->addWidget(zoomSlider);

    auto* zoomPlusLabel = new QLabel(QStringLiteral("+"));
    zoomPlusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 0 2px;")
        .arg(Theme::hex(tc.textTertiary)));
    toolbarLayout->addWidget(zoomPlusLabel);

    centerLayout->addWidget(toolbar);

    // Timeline Panel
    m_timelinePanel = new TimelinePanel(this);
    if (m_timeline) m_timelinePanel->setTimeline(m_timeline);
    if (m_commandStack) m_timelinePanel->setCommandStack(m_commandStack);
    if (m_shortcutManager) m_timelinePanel->setShortcutManager(m_shortcutManager);
    if (m_mediaPool) m_timelinePanel->setMediaPool(m_mediaPool);
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_compositeService && m_compositeService->animVideoCache())
        m_timelinePanel->setAnimVideoCache(m_compositeService->animVideoCache());
#endif
    centerLayout->addWidget(m_timelinePanel, 1);

    // Connect zoom slider to timeline layout engine
    connect(zoomSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_timelinePanel) {
            m_timelinePanel->layoutEngine().setPixelsPerSecond(static_cast<double>(value));
            m_timelinePanel->update();
            for (auto* child : m_timelinePanel->findChildren<QWidget*>())
                child->update();
        }
    });

    // Connect linked selection toggle
    connect(btnLinked, &QToolButton::toggled, this, [this](bool checked) {
        if (m_timelinePanel) {
            m_timelinePanel->setLinkedSelectionEnabled(checked);
        }
    });

    // Connect Add Marker button
    connect(btnMarker, &QToolButton::clicked, this, [this]() {
        if (m_timeline && m_playbackController && m_commandStack) {
            int64_t tick = m_playbackController->currentTick();
            m_commandStack->execute(
                std::make_unique<AddMarkerCommand>(m_timeline, tick, "Marker"));
            if (m_timelinePanel) m_timelinePanel->update();
        }
    });

    // Connect Nest Sequences toggle
    connect(btnNest, &QToolButton::toggled, this, [this, btnNest](bool checked) {
        btnNest->setToolTip(checked
            ? tr("Insert sequences as nested clips")
            : tr("Insert sequences as individual clips"));
        if (m_timelinePanel)
            m_timelinePanel->setNestSequencesEnabled(checked);
    });

    // Connect Caption Track Options button
    connect(btnCC, &QToolButton::clicked, this, [this, btnCC]() {
        QMenu menu(btnCC);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3; }"
            "QMenu::item:selected { background: %4; }")
            .arg(Theme::hex(Theme::colors().surface1),
                 Theme::hex(Theme::colors().textPrimary),
                 Theme::hex(Theme::colors().border),
                 Theme::hex(Theme::colors().accent)));
        menu.addAction(tr("Add Caption Track"), this, [this]() {
            if (m_timeline) {
                m_timeline->addVideoTrack("Captions");
                if (m_timelinePanel) m_timelinePanel->rebuildTracks();
            }
        });
        menu.addAction(tr("Hide Caption Track"), this, [this]() {
            if (m_timelinePanel)
                m_timelinePanel->setCaptionTrackVisible(false);
        });
        menu.addAction(tr("Show Caption Track"), this, [this]() {
            if (m_timelinePanel)
                m_timelinePanel->setCaptionTrackVisible(true);
        });
        menu.exec(btnCC->mapToGlobal(QPoint(0, btnCC->height())));
    });

    centralLayout->addWidget(centerContainer);

    // Wrap tool column in its own dock widget so it can be moved independently
    auto* dockTools = makeDock("Tools", toolColumn,
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    m_innerMainWindow->setCentralWidget(centralContainer);

    // =====================================================================
    //  DOCK LAYOUT -- Premiere Pro default arrangement
    //
    //  Top row:  [Project Bin | Source Monitor | Program Monitor | Effect Controls]
    //  Right of central: Audio Meters
    //  Effect Controls area: Effects, Keyframes, History, Audio Mixer tabbed
    // =====================================================================

    // Add Project Bin to top dock area
    m_innerMainWindow->addDockWidget(Qt::TopDockWidgetArea, dockProjectBin);

    // Split Project Bin horizontally to add Source Monitor beside it
    m_innerMainWindow->splitDockWidget(dockProjectBin, dockSourceMonitor, Qt::Horizontal);

    // Split Source Monitor horizontally to add Program Monitor beside it
    m_innerMainWindow->splitDockWidget(dockSourceMonitor, dockProgramMonitor, Qt::Horizontal);

    // Split Program Monitor horizontally to add Effect Controls beside it
    m_innerMainWindow->splitDockWidget(dockProgramMonitor, dockEffectControls, Qt::Horizontal);

    // Tab the remaining panels onto Effect Controls dock area
    m_innerMainWindow->tabifyDockWidget(dockEffectControls, dockEssentialGraphics);
    m_innerMainWindow->tabifyDockWidget(dockEssentialGraphics, dockColorGrading);
    m_innerMainWindow->tabifyDockWidget(dockColorGrading, dockEffects);
    m_innerMainWindow->tabifyDockWidget(dockEffects, dockHistory);
    m_innerMainWindow->tabifyDockWidget(dockHistory, dockAudioMixer);
    m_innerMainWindow->tabifyDockWidget(dockAudioMixer, dockScopes);
    m_innerMainWindow->tabifyDockWidget(dockScopes, dockProperties);
    m_innerMainWindow->tabifyDockWidget(dockProperties, dockCharacters);

    // Raise Effect Controls to be the active tab
    dockEffectControls->raise();

    // Enable tab drag-to-undock: install event filter on each dock tab bar
    // so dragging a tab vertically out of the bar detaches the corresponding
    // dock widget and starts a floating drag, mimicking Premiere Pro behavior.
    // Watch for new tab bars created by dock rearrangement and auto-configure
    // them (elide, context menu, drag filter).  Also handles initial setup.
    auto* tabDragFilter = new DockTabDragFilter(m_innerMainWindow, m_innerMainWindow);
    auto* tabBarWatcher = new DockTabBarWatcher(m_innerMainWindow, m_innerMainWindow);
    tabBarWatcher->setWorkspace(this);
    tabBarWatcher->setDragFilter(tabDragFilter);
    m_innerMainWindow->installEventFilter(tabBarWatcher);

    for (auto* tb : m_innerMainWindow->findChildren<QTabBar*>())
        tabBarWatcher->watchTabBar(tb);

    // -- Full-height edge docking (Premiere Pro style) --------------------
    // Wrap the inner QMainWindow in a horizontal QSplitter so that edge-
    // docked panels become splitter children ï¿½ naturally full height.
    // DockEdgeDragWatcher creates EdgeColumnPanels in the splitter on drop.
    m_edgeSplitter = new QSplitter(Qt::Horizontal, this);
    m_edgeSplitter->setHandleWidth(2);
    m_edgeSplitter->setChildrenCollapsible(false);
    m_edgeSplitter->addWidget(m_innerMainWindow);
    auto* edgeWatcher = new DockEdgeDragWatcher(m_innerMainWindow, m_edgeSplitter, m_edgeSplitter);
    tabDragFilter->setEdgeDragWatcher(edgeWatcher);

    // Audio Meters dock on the right side of the central timeline area
    m_innerMainWindow->addDockWidget(Qt::RightDockWidgetArea, dockAudioMeters);

    // Tools dock on the left side of the central timeline area
    m_innerMainWindow->addDockWidget(Qt::LeftDockWidgetArea, dockTools);

    // Set initial dock sizes via resize hints
    dockProjectBin->setMinimumWidth(180);
    dockSourceMonitor->setMinimumWidth(200);
    dockProgramMonitor->setMinimumWidth(200);
    dockEffectControls->setMinimumWidth(400);
    dockAudioMeters->setMaximumWidth(200);
    dockAudioMeters->setMinimumWidth(80);

    mainLayout->addWidget(m_edgeSplitter);

    // -- Wire toolbar/tool buttons to TimelinePanel -----------------------
    auto connectTool = [this](QToolButton* btn, EditTool tool) {
        connect(btn, &QToolButton::clicked, this, [this, tool]() {
            if (m_timelinePanel) m_timelinePanel->setActiveTool(tool);
        });
    };
    connectTool(btnSelection, EditTool::Selection);
    connectTool(btnRipple,    EditTool::Ripple);
    connectTool(btnRolling,   EditTool::Rolling);
    connectTool(btnRazor,     EditTool::Razor);
    connectTool(btnSlip,      EditTool::Slip);
    connectTool(btnSlide,     EditTool::Slide);
    connectTool(btnText,      EditTool::Text);
    connectTool(btnZoom,      EditTool::Zoom);

    connect(btnSnap, &QToolButton::toggled, this, [this](bool checked) {
        if (m_timelinePanel) m_timelinePanel->setSnappingEnabled(checked);
    });

    // Sync tool buttons when tool is changed via keyboard shortcut
    connect(m_timelinePanel, &TimelinePanel::toolChanged,
            this, [=](EditTool tool) {
        btnSelection->setChecked(tool == EditTool::Selection);
        btnRipple->setChecked(tool == EditTool::Ripple);
        btnRolling->setChecked(tool == EditTool::Rolling);
        btnRazor->setChecked(tool == EditTool::Razor);
        btnSlip->setChecked(tool == EditTool::Slip);
        btnSlide->setChecked(tool == EditTool::Slide);
        btnText->setChecked(tool == EditTool::Text);
        btnZoom->setChecked(tool == EditTool::Zoom);
    });

    // =====================================================================
    //  PLAYBACK WIRING -- connect PlaybackController <-> Timeline <-> Monitor
    // =====================================================================
    if (m_playbackController) {
        // Scrubbing: user drags playhead on timeline ruler -> seek + audio scrub
        connect(m_timelinePanel, &TimelinePanel::playheadMoved,
                this, [this](int64_t tick) {
            auto scrubWireT0 = std::chrono::steady_clock::now();

            // Update the timeline toolbar timecode display
            if (m_timelineTimecode && m_timelinePanel) {
                auto tc = m_timelinePanel->layoutEngine().formatTimecode(tick);
                m_timelineTimecode->setText(QString::fromStdString(tc));
            }
            // Pause timeline playback when user scrubs (Premiere Pro behavior)
            if (m_playbackController->isPlaying()) {
                m_playbackController->pause();
            }
            // Pause source monitor when user scrubs in the timeline
            if (m_sourceMonitor && m_sourceMonitor->controller()
                && m_sourceMonitor->controller()->state() != PlayState::Stopped
                && m_sourceMonitor->controller()->state() != PlayState::Paused) {
                m_sourceMonitor->controller()->pause();
            }
            m_playbackController->seekTo(tick);
            ensureAudioSourcesLoaded();
            // Only fire a scrub burst when NOT playing.  During playback,
            // seekTo() already repositions the clock and audio atomically.
            // Calling scrub() during Playing would set the audio transport
            // to Scrubbing?Paused, killing audio output while video
            // continues via wall-clock extrapolation ï¿½ causing desync.
            if (m_audioEngine && !m_playbackController->isPlaying()) {
                // Convert timeline ticks (48000/sec) to audio sample frames
                const uint32_t sr = m_audioEngine->sampleRate();
                const int64_t frame = static_cast<int64_t>(
                    static_cast<double>(tick) / 48000.0 * sr);
                m_audioEngine->scrub(frame);
                // Ensure meter timers are running for scrub audio
                if (m_meterTimer && !m_meterTimer->isActive())
                    m_meterTimer->start();
                if (m_audioMixer)
                    m_audioMixer->ensureMeterTimerRunning();
            }
            // Notify program monitor to re-render (it doesn't see ruler scrubs
            // through its own onScrub slot which only fires for the mini-timeline).
            if (m_programMonitor)
                m_programMonitor->notifyScrub();

            double scrubWireMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - scrubWireT0).count();
            if (scrubWireMs > 6.0) {
                spdlog::info("[PERF] scrubWiring: {:.1f}ms  tick={}", scrubWireMs, tick);
            }
        });

        // Scrubbing from ProgramMonitor mini-timeline
        connect(m_programMonitor, &ProgramMonitor::scrubbed,
                this, [this](int64_t tick) {
            m_playbackController->seekTo(tick);
            ensureAudioSourcesLoaded();
            if (m_audioEngine && !m_playbackController->isPlaying()) {
                const uint32_t sr = m_audioEngine->sampleRate();
                const int64_t frame = static_cast<int64_t>(
                    static_cast<double>(tick) / 48000.0 * sr);
                m_audioEngine->scrub(frame);
                if (m_meterTimer && !m_meterTimer->isActive())
                    m_meterTimer->start();
                if (m_audioMixer)
                    m_audioMixer->ensureMeterTimerRunning();
            }
        });

        // ProgramMonitor MiniTimeline in/out point changes Ã¢â€ â€™ Timeline
        connect(m_programMonitor->miniTimeline(), &MiniTimeline::inPointChanged,
                this, [this](int64_t tick) {
            if (m_timeline) {
                if (tick >= 0)
                    EditOperations::setInPoint(*m_timeline, tick);
                else
                    m_timeline->setInPoint(-1);
                if (m_timelinePanel) m_timelinePanel->updateInOutRange();
            }
        });
        connect(m_programMonitor->miniTimeline(), &MiniTimeline::outPointChanged,
                this, [this](int64_t tick) {
            if (m_timeline) {
                if (tick >= 0)
                    EditOperations::setOutPoint(*m_timeline, tick);
                else
                    m_timeline->setOutPoint(-1);
                if (m_timelinePanel) m_timelinePanel->updateInOutRange();
            }
        });

        // I/O key presses inside ProgramMonitor Ã¢â€ â€™ set in/out on timeline
        connect(m_programMonitor, &ProgramMonitor::inPointRequested,
                this, [this]() {
            if (m_timeline && m_playbackController) {
                EditOperations::setInPoint(*m_timeline, m_playbackController->currentTick());
                if (m_timelinePanel) m_timelinePanel->updateInOutRange();
                syncProgramMonitorInOut();
            }
        });
        connect(m_programMonitor, &ProgramMonitor::outPointRequested,
                this, [this]() {
            if (m_timeline && m_playbackController) {
                EditOperations::setOutPoint(*m_timeline, m_playbackController->currentTick());
                if (m_timelinePanel) m_timelinePanel->updateInOutRange();
                syncProgramMonitorInOut();
            }
        });

        // During playback: controller updates playhead position in timeline panel
        // Throttled to every 3rd call (~20fps) during playback to reduce Qt
        // paint event backlog.  The render thread handles actual frame display
        // at the correct project fps independently.
        m_playbackController->onPositionChanged = [this](int64_t tick) {
            static int s_posThrottle = 0;
            const bool playing = m_playbackController &&
                                 m_playbackController->isPlaying();
            const bool doUpdate = !playing || (++s_posThrottle % 3 == 0);

            if (doUpdate) {
                if (m_timelinePanel)
                    m_timelinePanel->setPlayheadPosition(tick);
                if (m_effectControlsPanel)
                    m_effectControlsPanel->setPlayheadTick(tick);
                // Update timeline toolbar timecode during playback
                if (m_timelineTimecode && m_timelinePanel) {
                    auto tc = m_timelinePanel->layoutEngine().formatTimecode(tick);
                    m_timelineTimecode->setText(QString::fromStdString(tc));
                }
            }
            scheduleAudioPlaybackWindowRefresh();
        };

        // State change: load audio sources BEFORE playback starts
        m_playbackController->onStateChanged = [this](PlayState state) {
            if (state == PlayState::Playing || state == PlayState::Shuttling) {
                // Start meter timer for VU meter updates during playback
                if (m_meterTimer && !m_meterTimer->isActive())
                    m_meterTimer->start();
                if (m_audioMixer)
                    m_audioMixer->ensureMeterTimerRunning();
                // Pause source monitor if it's active (mutual exclusion)
                // Use pause() to preserve source playhead position
                if (m_sourceMonitor && m_sourceMonitor->controller()
                    && m_sourceMonitor->controller()->state() != PlayState::Stopped
                    && m_sourceMonitor->controller()->state() != PlayState::Paused) {
                    m_sourceMonitor->controller()->pause();
                }
                if (m_audioPlayback && m_audioPlayback->needsPlaybackWindowRefresh()) {
                    scheduleAudioPlaybackWindowRefresh();
                } else {
                    warmAudioCacheAsync();
                }
            }
            // Wake the pipeline's FrameClock + FramePresenter so they
            // react to the play/pause transition immediately.
            if (m_programMonitor) {
                if (auto* pl = m_programMonitor->pipeline())
                    pl->notifyStateChange();
            }
        };

        m_playbackController->onPlayStarting = [this](int64_t startTick) {
            if (m_programMonitor) {
                // Defer prewarm if background media loading is still active.
                // The compositor evaluates clip keyframes (opacity, position,
                // etc.) during compositeFrame; racing with timeline population
                // from background threads causes use-after-free ACCESS_VIOLATION
                // crashes in Keyframe<float> vector iteration.
                if (isBackgroundWarmupActive()) {
                    spdlog::info("playback start deferred — background warmup still in progress");
                    // Schedule a retry after a short delay, once warmup completes.
                    QTimer::singleShot(100, this, [this, startTick]() {
                        if (!isBackgroundWarmupActive() && m_programMonitor) {
                            prewarmPlaybackResources(startTick,
                                                     m_programMonitor->compositeWidth(),
                                                     m_programMonitor->compositeHeight());
                            m_programMonitor->requestPlaybackPreroll(startTick);
                        }
                    });
                    return;
                }
                prewarmPlaybackResources(startTick,
                                         m_programMonitor->compositeWidth(),
                                         m_programMonitor->compositeHeight());
                m_programMonitor->requestPlaybackPreroll(startTick);
            }
        };

        // Set composite callback -- renders composited frame for Program Monitor
        m_programMonitor->setCompositeCallback(
            [this](int64_t tick, uint32_t w, uint32_t h, bool scrubMode) -> std::shared_ptr<CachedFrame> {
                return compositeFrame(tick, w, h, scrubMode);
            });

        // Wire the playback-resolution dropdown through to the compositor so
        // its ResolutionTier tracks the preview (Full / 1/2 / 1/4 / 1/8).
        if (m_compositeService) {
            m_programMonitor->setPlaybackTierCallback(
                [this](int divisor) {
                    if (!m_compositeService) return;
                    ResolutionTier tier = ResolutionTier::Full;
                    if      (divisor >= 4) tier = ResolutionTier::Quarter;
                    else if (divisor >= 2) tier = ResolutionTier::Half;
                    m_compositeService->setPlaybackTier(tier);
                });
        }

        // Wire VRAM pressure query so the Program Monitor can show warnings
        if (m_compositeService) {
            m_programMonitor->setVramQueryCallback([this]() -> int {
                return m_compositeService ? m_compositeService->vramUsagePercent() : 0;
            });
        }

        // Feed compositor output to Lumetri Scopes when a frame is displayed
        if (m_scopesPanel) {
            connect(m_programMonitor, &ProgramMonitor::frameDisplayed,
                    this, [this](int64_t /*tick*/) {
                // Skip expensive pixel readback when Scopes panel is hidden
                auto* dockScopes = dockForPanel("Scopes");
                if (dockScopes && !dockScopes->isVisible()) return;

                auto frame = m_programMonitor->lastDisplayedFrame();
                if (!frame) return;
                if (!frame->ensurePixels()) return;
                if (frame->pixels.empty() || frame->width == 0 || frame->height == 0) return;
                m_scopesPanel->feedFrame(frame->pixels.data(),
                                         static_cast<int>(frame->width),
                                         static_cast<int>(frame->height));
            });
        }

        // Start the 60 fps polling timer on the Program Monitor
        m_programMonitor->startPolling();
    }

    // =====================================================================
    //  FOCUS MANAGEMENT -- like FCP7 / old Python version
    // =====================================================================
    setFocusPolicy(Qt::StrongFocus);
    for (auto* btn : m_toolButtons)
        if (btn) btn->setFocusPolicy(Qt::NoFocus);

    // =====================================================================
    //  MODIFIER KEYBOARD SHORTCUTS (work even when QLineEdit is focused)
    // =====================================================================
    auto addShortcut = [this](const QKeySequence& key, auto&& fn) {
        auto* sc = new QShortcut(key, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, std::forward<decltype(fn)>(fn));
    };

    // Home / End: go to start / end of timeline
    // These are QShortcuts (not keyPressEvent) so they work when any
    // child widget has focus ï¿½ e.g. after clicking the ruler.
    addShortcut(Qt::Key_Home, [this]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw)) return;  // don't steal from text fields
        if (m_playbackController) m_playbackController->goToStart();
    });
    addShortcut(Qt::Key_End, [this]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw)) return;
        if (m_playbackController) m_playbackController->goToEnd();
    });

    // Shift+I / Shift+O: go to in/out point
    addShortcut(Qt::SHIFT | Qt::Key_I, [this]() {
        if (m_playbackController) m_playbackController->goToInPoint();
    });
    addShortcut(Qt::SHIFT | Qt::Key_O, [this]() {
        if (m_playbackController) m_playbackController->goToOutPoint();
    });
    // Alt+X: clear in/out
    addShortcut(Qt::ALT | Qt::Key_X, [this]() {
        if (m_timeline) {
            EditOperations::clearInOutPoints(*m_timeline);
            if (m_timelinePanel) m_timelinePanel->updateInOutRange();
            syncProgramMonitorInOut();
        }
    });
    // Ctrl+Shift+X: clear in/out points
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_X, [this]() {
        if (m_timeline) {
            EditOperations::clearInOutPoints(*m_timeline);
            if (m_timelinePanel) m_timelinePanel->updateInOutRange();
            syncProgramMonitorInOut();
        }
    });
    // Ctrl+Shift+V: Paste Attributes dialog (Premiere Pro-style)
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_V, [this]() {
        if (m_timelinePanel) m_timelinePanel->showPasteAttributesDialog();
    });
    // Ctrl+Shift+C: Paste Insert (Premiere Pro-style â€” push clips right)
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_C, [this]() {
        if (m_timeline && m_timelinePanel && m_commandStack && !m_timelinePanel->clipboard().empty()) {
            auto cmd = EditOperations::pasteInsert(
                *m_timeline, m_timelinePanel->clipboard(),
                m_playbackController ? m_playbackController->currentTick() : 0);
            if (cmd) {
                m_commandStack->execute(std::move(cmd));
                if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
                invalidateAudioSources();
                invalidateCompositeCache();
                updateTransformOverlay();
                if (m_programMonitor) m_programMonitor->requestRefresh();
                schedulePostEditWork();
            }
        }
    });
    // Ctrl+V: paste at playhead (or paste layer if Essential Graphics focused)
    addShortcut(Qt::CTRL | Qt::Key_V, [this]() {
        // If focus is inside Essential Graphics or Program Monitor with a graphic layer selected, paste layer
        auto* fw = QApplication::focusWidget();
        bool egFocused = m_GraphicsEditorPanel && m_GraphicsEditorPanel->isAncestorOf(fw);
        bool pmFocused = m_programMonitor && m_programMonitor->isAncestorOf(fw);
        if (m_GraphicsEditorPanel && (egFocused || (pmFocused && m_selectedGraphicLayerIdx >= 0))) {
            m_GraphicsEditorPanel->pasteLayer();
            invalidateCompositeCache();
            scheduleOverlayRefresh();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            return;
        }
        if (m_timeline && m_timelinePanel && m_commandStack && !m_timelinePanel->clipboard().empty()) {
            auto cmd = EditOperations::paste(
                *m_timeline, m_timelinePanel->clipboard(),
                m_playbackController ? m_playbackController->currentTick() : 0);
            if (cmd) {
                m_commandStack->execute(std::move(cmd));
                if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
                invalidateAudioSources();
                invalidateCompositeCache();
                updateTransformOverlay();
                if (m_programMonitor) m_programMonitor->requestRefresh();
                schedulePostEditWork();
            }
        }
    });
    // Ctrl+X: cut (populates panel's clipboard so paste works)
    addShortcut(Qt::CTRL | Qt::Key_X, [this]() {
        if (!m_timeline || !m_timelinePanel || !m_commandStack) return;
        auto& cb = m_timelinePanel->mutableClipboard();
        auto cmd = EditOperations::cutSelection(*m_timeline,
            m_timelinePanel->selection(), cb);
        if (cmd) {
            m_timelinePanel->selection().clear();
            m_commandStack->execute(std::move(cmd));
            m_timelinePanel->refreshTrackContents();
            invalidateAudioSources();
            invalidateCompositeCache();
            updateTransformOverlay();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        }
    });
    // Ctrl+C: copy (or copy layer if Essential Graphics focused)
    addShortcut(Qt::CTRL | Qt::Key_C, [this]() {
        // If focus is inside Essential Graphics or Program Monitor with a graphic layer selected, copy layer
        auto* fw = QApplication::focusWidget();
        bool egFocused = m_GraphicsEditorPanel && m_GraphicsEditorPanel->isAncestorOf(fw);
        bool pmFocused = m_programMonitor && m_programMonitor->isAncestorOf(fw);
        if (m_GraphicsEditorPanel && (egFocused || (pmFocused && m_selectedGraphicLayerIdx >= 0))) {
            m_GraphicsEditorPanel->copySelectedLayer();
            return;
        }
        if (!m_timeline || !m_timelinePanel) return;
        // Copy into the panel's clipboard so paste can find it
        EditOperations::copySelection(*m_timeline,
            m_timelinePanel->selection(),
            m_timelinePanel->mutableClipboard());
        // Also populate the attributes clipboard so Ctrl+Shift+V is ready
        m_timelinePanel->copyAttributesFromSelection();
    });
    // Shift+Delete / Shift+Backspace: extract (ripple delete)
    addShortcut(Qt::SHIFT | Qt::Key_Delete, [this]() {
        if (!m_timeline || !m_timelinePanel || !m_commandStack) return;
        auto cmd = EditOperations::rippleDelete(*m_timeline,
            m_timelinePanel->selection());
        if (cmd) {
            m_timelinePanel->selection().clear();
            m_commandStack->execute(std::move(cmd));
            m_timelinePanel->refreshTrackContents();
            invalidateAudioSources();
            invalidateCompositeCache();
            updateTransformOverlay();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            schedulePostEditWork();
        }
    });
    addShortcut(Qt::SHIFT | Qt::Key_Backspace, [this]() {
        if (!m_timeline || !m_timelinePanel || !m_commandStack) return;
        auto cmd = EditOperations::rippleDelete(*m_timeline,
            m_timelinePanel->selection());
        if (cmd) {
            m_timelinePanel->selection().clear();
            m_commandStack->execute(std::move(cmd));
            m_timelinePanel->refreshTrackContents();
            invalidateAudioSources();
            invalidateCompositeCache();
            updateTransformOverlay();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            schedulePostEditWork();
        }
    });
    // Ctrl+A: select all
    addShortcut(Qt::CTRL | Qt::Key_A, [this]() {
        // If the project bin has focus, select items in the bin instead.
        if (m_projectBin && m_projectBin->isAncestorOf(
                QApplication::focusWidget())) {
            m_projectBin->selectAllItems();
            return;
        }
        if (!m_timeline || !m_timelinePanel) return;
        m_timelinePanel->selection().selectAll(*m_timeline);
        emit m_timelinePanel->selectionChanged();
    });
    // Ctrl+Shift+A: deselect all
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_A, [this]() {
        if (!m_timelinePanel) return;
        m_timelinePanel->selection().clear();
        emit m_timelinePanel->selectionChanged();
    });

    // Ctrl+B: New Bin when Project Bin is focused
    addShortcut(Qt::CTRL | Qt::Key_B, [this]() {
        if (m_projectBin && m_projectBin->isAncestorOf(
                QApplication::focusWidget())) {
            m_projectBin->createNewBin();
            return;
        }
    });

    // Ctrl+T: add default transition
    addShortcut(Qt::CTRL | Qt::Key_T, [this]() {
        if (!m_timeline || !m_commandStack || !m_timelinePanel) return;

        auto edge = m_timelinePanel->lastClickedEdge();
        if (!edge.valid) return;

        Track* track = m_timeline->track(edge.clipRef.trackIndex);
        if (!track) return;

        size_t clipIdx = track->findClipIndexById(edge.clipRef.clipId);
        if (clipIdx >= track->clipCount()) return;

        const Clip* clip = track->clip(clipIdx);
        Transition trans;
        trans.duration = kDefaultTransitionDuration;

        if (edge.edge == ClipEdge::Head) {
            const Clip* leftClip = nullptr;
            size_t leftIdx = SIZE_MAX;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                if (ci == clipIdx) continue;
                const Clip* c = track->clip(ci);
                int64_t gap = std::abs(c->timelineOut() - clip->timelineIn());
                if (gap <= 1600) {
                    leftClip = c;
                    leftIdx = ci;
                    break;
                }
            }
            if (leftClip) {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = leftClip->id();
                trans.rightClipId = clip->id();
                trans.editPointTick = clip->timelineIn();
            } else {
                // No neighboring clip: fade in from TRANSPARENT so lower
                // video tracks remain visible beneath the fade.
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = 0;
                trans.rightClipId = clip->id();
                trans.editPointTick = clip->timelineIn();
            }
        } else {
            const Clip* rightClip = nullptr;
            size_t rightIdx = SIZE_MAX;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                if (ci == clipIdx) continue;
                const Clip* c = track->clip(ci);
                int64_t gap = std::abs(c->timelineIn() - clip->timelineOut());
                if (gap <= 1600) {
                    rightClip = c;
                    rightIdx = ci;
                    break;
                }
            }
            if (rightClip) {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = clip->id();
                trans.rightClipId = rightClip->id();
                trans.editPointTick = clip->timelineOut();
            } else {
                // Fade out to TRANSPARENT (lower tracks show through).
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = clip->id();
                trans.rightClipId = 0;
                trans.editPointTick = clip->timelineOut();
            }
        }

        bool alreadyExists = false;
        for (size_t ti2 = 0; ti2 < track->transitionCount(); ++ti2) {
            const Transition* existing = track->transition(ti2);
            if (existing && existing->editPointTick == trans.editPointTick) {
                alreadyExists = true;
                break;
            }
        }
        if (alreadyExists) return;

        auto cmd = std::make_unique<AddTransitionCommand>(
            track, clipIdx, clipIdx, trans);
        m_commandStack->execute(std::move(cmd));

        invalidateCompositeCache();
        if (m_timelinePanel) m_timelinePanel->rebuildTracks();
        if (m_programMonitor) m_programMonitor->requestRefresh();
    });
    // Ctrl+=: zoom in (anchored on the playhead, falls back to viewport
    // center if the playhead is offscreen)
    addShortcut(Qt::CTRL | Qt::Key_Equal, [this]() {
        if (m_timelinePanel) {
            auto& engine = m_timelinePanel->layoutEngine();
            double anchorPx = engine.viewportWidth() * 0.5;
            if (m_playbackController) {
                double playheadPx = engine.timeToPixelX(m_playbackController->currentTick());
                if (playheadPx >= 0.0 && playheadPx <= engine.viewportWidth())
                    anchorPx = playheadPx;
            }
            engine.zoomAt(anchorPx, 1.3);
            m_timelinePanel->notifyZoomChanged();
        }
    });
    // Ctrl+-: zoom out (anchored on the playhead, same fallback)
    addShortcut(Qt::CTRL | Qt::Key_Minus, [this]() {
        if (m_timelinePanel) {
            auto& engine = m_timelinePanel->layoutEngine();
            double anchorPx = engine.viewportWidth() * 0.5;
            if (m_playbackController) {
                double playheadPx = engine.timeToPixelX(m_playbackController->currentTick());
                if (playheadPx >= 0.0 && playheadPx <= engine.viewportWidth())
                    anchorPx = playheadPx;
            }
            engine.zoomAt(anchorPx, 1.0 / 1.3);
            m_timelinePanel->notifyZoomChanged();
        }
    });

    // Ctrl+E: switch to Export tab (Premiere Pro convention)
    addShortcut(Qt::CTRL | Qt::Key_E, [this]() {
        for (QWidget* w = parentWidget(); w; w = w->parentWidget()) {
            if (auto* mw = qobject_cast<MainWindow*>(w)) {
                mw->setCurrentPage(Page::Export);
                break;
            }
        }
    });

    // Give this widget focus so shortcuts work immediately
    setFocus();

    // Wire all panel signals (VU meter, clip selection, drag-drop, track mgmt)
    wirePanelSignals();

    // Create dock layout manager now that all widgets exist
    m_dockLayoutManager = std::make_unique<DockLayoutManager>(
        DockLayoutManager::Config{
            m_innerMainWindow,
            m_edgeSplitter,
            &m_dockWidgets,
            this,
            [this](QMainWindow* col) { installEdgeGuard(col); }
        });

    m_panelsBuilt = true;

    // Snapshot the default dock state so resetToDefaultDockLayout() can
    // recreate the stock Premiere Pro-like arrangement at any time.
    if (m_innerMainWindow)
        m_defaultDockState = m_innerMainWindow->saveState(4);

    spdlog::info("TimelineWorkspace::buildPanels() - dockable layout with {} panels", m_dockWidgets.size());

    setUpdatesEnabled(true);
    updateGeometry();
    repaint();
}



} // namespace rt
