/*
 * MainWindowUI.cpp - UI construction & menus extracted from MainWindow.cpp.
 *
 * Contains: constructor, destructor, setupPageTabs, dependency setters,
 * buildPanels, page navigation, panel accessors, all build*Menu methods,
 * workspace persistence, and setupStatusBar.
 */

#include "MainWindow.h"
#include "ShortcutManager.h"
#include "QtHelpers.h"
#include "Theme.h"
#include "widgets/DockTitleBar.h"
#include "dialogs/ProjectSettingsDialog.h"
#include "dialogs/KeyboardShortcutsDialog.h"
#include "dialogs/AppPreferencesDialog.h"

// Pages / panels
#include "panels/audio/AudioSync.h"
#include "panels/characters/CharacterBrowser.h"
#include "panels/characters/CharacterShotPanel.h"
#include "panels/export/ExportPanel.h"
#include "panels/project/ProjectPanel.h"
#include "panels/characters/ShotComposer.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "panels/library/LibraryPanel.h"

// Spine cache warming after export
#include "spine/AnimationVideoCache.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"

// Delegated panel headers (for accessor forwarding)
#include "panels/audio/AudioMixer.h"
#include "panels/effects/EffectsPanel.h"
#include "panels/project/HistoryPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "viewport/Viewport.h"
#include "panels/project/ProjectBin.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

// Core
#include "command/CommandStack.h"
#include "media/AudioEngine.h"
#include "media/FrameCache.h"
#include "media/PlaybackController.h"
#include "spine/ModelManager.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"

#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "spine/ShotPreset.h"
#include "SrtIO.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QTemporaryDir>
#include <QWindow>
#include <QScreen>
#include <QStyle>

#include "UiScale.h"
#include <QStackedWidget>
#include <QStatusBar>
#include <QButtonGroup>
#include <QPixmap>
#include <QPushButton>
#include <QTabBar>

#include "Settings.h"

#include <map>
#include <set>


namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═════════════════════════════════════════════════════════════════════════════

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setObjectName("MainWindow");
    setWindowTitle(QString("ROUNDTABLE NLE %1").arg(ROUNDTABLE_VERSION));
    setMinimumSize(1280, 720);

    setDocumentMode(true);

    // Install global event filter for JKL transport keys
    if (qApp) qApp->installEventFilter(this);

    // Auto-save with configurable interval from preferences
    m_autoSaveTimer = new QTimer(this);
    {
        auto s = rt::appSettings();
        int minutes = s.value("AutosaveInterval", 5).toInt();
        m_autoSaveTimer->setInterval(minutes * 60 * 1000);
    }
    connect(m_autoSaveTimer, &QTimer::timeout, this, &MainWindow::onAutoSave);
    m_autoSaveTimer->start();

    setupPageTabs();
    setupStatusBar();

    // ── Per-monitor DPI relayout ────────────────────────────────────────
    // When the window is dragged across monitors with different DPI/scale,
    // Qt updates devicePixelRatio but does NOT automatically re-polish
    // stylesheets, re-evaluate font metrics, or re-run layouts that cached
    // pixel sizes from the old screen. Force a full relayout on every
    // screen change so controls stay the correct apparent size.
    QTimer::singleShot(0, this, [this]() {
        auto installScreenWatch = [this](QWindow* win) {
            if (!win) return;
            connect(win, &QWindow::screenChanged, this,
                    [this](QScreen* newScreen) {
                        if (!newScreen) return;
                        spdlog::info("MainWindow: moved to screen '{}' ({}x{} dpr={:.2f} dpi={:.0f})",
                                     newScreen->name().toStdString(),
                                     newScreen->size().width(),
                                     newScreen->size().height(),
                                     newScreen->devicePixelRatio(),
                                     newScreen->logicalDotsPerInch());
                        // Update global UI scale factor for the new screen.
                        // This rescales every widget registered via
                        // UiScale::setScaledFixed*() and emits a signal so
                        // panels can rebuild stylesheet-driven sizes.
                        rt::UiScale::updateForScreen(newScreen);
                        // Re-polish the entire widget tree so any QSS that
                        // depends on font metrics or pixel sizes is rebuilt
                        // for the new DPI, then force a full relayout.
                        if (auto* st = style()) {
                            st->unpolish(this);
                            st->polish(this);
                        }
                        const auto kids = findChildren<QWidget*>();
                        for (QWidget* w : kids) {
                            if (auto* st = w->style()) {
                                st->unpolish(w);
                                st->polish(w);
                            }
                            w->updateGeometry();
                        }
                        updateGeometry();
                        adjustSize();
                        update();
                    });
            // Initialise scale for the screen the window is currently on.
            if (auto* s = win->screen()) {
                rt::UiScale::updateForScreen(s);
            }
        };
        installScreenWatch(windowHandle());
        if (!windowHandle()) {
            QTimer::singleShot(100, this, [this, installScreenWatch]() {
                installScreenWatch(windowHandle());
            });
        }
    });

    spdlog::debug("MainWindow constructed (tabbed pages)");
}

MainWindow::~MainWindow()
{
    spdlog::debug("MainWindow destroyed");
}

// ═════════════════════════════════════════════════════════════════════════════
// Tab system setup
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::setupPageTabs()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    // Central widget: HBox with nav rail on left, content on right
    auto* centralContainer = new QWidget(this);
    auto* hbox = new QHBoxLayout(centralContainer);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    // ── Vertical nav rail (left sidebar) ────────────────────────────────
    m_navRail = new QWidget(centralContainer);
    m_navRail->setObjectName("MainNavRail");
    m_navRail->setFixedWidth(150);
    m_navRail->setStyleSheet(QStringLiteral(
        "#MainNavRail { background: rgb(%1,%2,%3); }")
        .arg(std::max(0, c.surface0.red()   - 4))
        .arg(std::max(0, c.surface0.green() - 4))
        .arg(std::max(0, c.surface0.blue()  - 4)));

    // Separator strip between main nav rail and content — accent glow edge
    auto* navSeparator = new QWidget(centralContainer);
    navSeparator->setFixedWidth(3);
    navSeparator->setStyleSheet(QStringLiteral(
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "  stop:0 %1, stop:0.5 %2, stop:1 %3);")
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent))
        .arg(Theme::rgb(c.surface1)));

    auto* railLayout = new QVBoxLayout(m_navRail);
    railLayout->setContentsMargins(8, m.spacingXl, 8, m.spacingXl);
    railLayout->setSpacing(0);
    railLayout->setAlignment(Qt::AlignTop);

    // Nav button style — icon + label stacked vertically (matches ProjectPanel rail)
    QString navBtnStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; color: %2; font-size: 42px;"
        "  padding: 12px 0; }"
        "QPushButton:hover { background: %3; color: %4; }"
        "QPushButton:pressed { background: %5; color: white; }"
        "QPushButton:checked { background: %6; color: %7; }")
        .arg(m.radiusXl)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent));

    struct NavEntry { const char* icon; const char* label; };
    NavEntry entries[] = {
        {"\U0001F4C1", "PROJECTS"},
        {"\U0001F464", "CHARACTERS"},
        {"\U0001F3B5", "AUDIO"},
        {"\U0001F39E", "TIMELINE"},
        {"\U0001F4E4", "EXPORT"}
    };

    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);

    // Divider block between nav entries — single container widget
    // wrapping a 1px line to prevent DPI sub-pixel drift.
    auto makeDivider = [&]() {
        auto* div = new QWidget;
        div->setFixedHeight(17);
        auto* lay = new QVBoxLayout(div);
        lay->setContentsMargins(16, 0, 16, 0);
        lay->setSpacing(0);
        lay->addStretch();
        auto* line = new QFrame;
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::rgb(c.border)));
        lay->addWidget(line);
        lay->addStretch();
        return div;
    };

    for (int i = 0; i < 5; ++i) {
        auto* btn = new QPushButton(QString::fromUtf8(entries[i].icon));
        btn->setCheckable(true);
        btn->setFixedSize(128, 84);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(navBtnStyle);
        m_navButtons[i] = btn;
        m_navGroup->addButton(btn, i);
        railLayout->addWidget(btn, 0, Qt::AlignHCenter);

        railLayout->addSpacing(4);

        auto* lbl = new QLabel(QString::fromUtf8(entries[i].label));
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setFixedHeight(20);
        lbl->setStyleSheet(QStringLiteral(
            "font-size: 15px; color: %1; font-weight: 800;")
            .arg(Theme::rgb(c.textPrimary)));
        railLayout->addWidget(lbl, 0, Qt::AlignHCenter);

        // Add a divider after each entry except the last
        if (i < 4) {
            railLayout->addWidget(makeDivider());
        }
    }

    railLayout->addStretch();

    // ── Collapse button at the bottom of the nav rail ────────────────
    m_navCollapseBtn = new QPushButton;
    m_navCollapseBtn->setFixedSize(128, 70);
    m_navCollapseBtn->setCursor(Qt::PointingHandCursor);
    m_navCollapseBtn->setToolTip(QStringLiteral("Collapse sidebar"));
    {
        // Render a large « glyph into a pixmap, centered in the button.
        QFont chevFont;
        chevFont.setPixelSize(112);   // proportional to nav buttons
        chevFont.setWeight(QFont::Bold);
        QFontMetrics chevFm(chevFont);
        QString chevText = QStringLiteral("\u00AB");
        int chevW = chevFm.horizontalAdvance(chevText);
        int chevH = chevFm.height();
        qreal dpr = devicePixelRatioF();
        QPixmap chevPix(static_cast<int>(chevW * dpr),
                        static_cast<int>(chevH * dpr));
        chevPix.setDevicePixelRatio(dpr);
        chevPix.fill(Qt::transparent);
        {
            QPainter p(&chevPix);
            p.setFont(chevFont);
            p.setPen(c.textSecondary);
            p.drawText(0, chevFm.ascent(), chevText);
        }
        // Crop to tight bounding rect of the visible glyph so Qt
        // centres it properly (font metrics include leading/descent
        // padding that shifts the glyph upward otherwise).
        {
            QImage img = chevPix.toImage();
            int top = img.height(), bottom = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(img.pixel(x, y)) > 0) {
                        if (y < top) top = y;
                        if (y > bottom) bottom = y;
                        break;
                    }
                }
            }
            if (top <= bottom) {
                chevPix = chevPix.copy(0, top, chevPix.width(), bottom - top + 1);
            }
        }
        int finalW = static_cast<int>(chevPix.width() / dpr);
        int finalH = static_cast<int>(chevPix.height() / dpr);
        m_navCollapseBtn->setIcon(QIcon(chevPix));
        m_navCollapseBtn->setIconSize(QSize(finalW, finalH));
    }
    m_navCollapseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: none; border-radius: %2px;"
        "  padding: 0; min-height: 70px; max-height: 70px;"
        "  min-width: 128px; max-width: 128px; }"
        "QPushButton:hover { background: %3; }")
        .arg(Theme::rgb(c.surface2))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.surface3)));
    railLayout->addWidget(m_navCollapseBtn, 0, Qt::AlignHCenter);

    // Small square expand button — hidden initially, shown when collapsed
    m_navExpandBtn = new QPushButton(QStringLiteral("\u00BB"));  // »
    m_navExpandBtn->setFixedSize(32, 32);
    m_navExpandBtn->setCursor(Qt::PointingHandCursor);
    m_navExpandBtn->setToolTip(QStringLiteral("Expand sidebar"));
    m_navExpandBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: none; border-radius: %2px;"
        "  color: %3; font-size: 16px; font-weight: bold; }"
        "QPushButton:hover { background: %4; color: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.textSecondary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    m_navExpandBtn->hide();
    railLayout->addWidget(m_navExpandBtn, 0, Qt::AlignHCenter);

    connect(m_navCollapseBtn, &QPushButton::clicked, this, &MainWindow::toggleNavRail);
    connect(m_navExpandBtn, &QPushButton::clicked, this, &MainWindow::toggleNavRail);

    hbox->addWidget(m_navRail);
    hbox->addWidget(navSeparator);

    // ── Stacked widget for page content ─────────────────────────────────
    m_pageStack = new QStackedWidget(centralContainer);
    m_pageStack->setFrameShape(QFrame::NoFrame);
    m_pageStack->setLineWidth(0);
    m_pageStack->setMidLineWidth(0);
    m_pageStack->setStyleSheet(QStringLiteral(
        "QStackedWidget { background: %1; border: none; padding: 0; margin: 0; }")
        .arg(Theme::hex(c.surface1)));
    // Zero internal layout margins so child pages start at (0,0) and
    // their sub-rails align vertically with the main nav rail.
    if (auto* lay = m_pageStack->layout())
        lay->setContentsMargins(0, 0, 0, 0);
    hbox->addWidget(m_pageStack, 1);

    // Wire button group to page switching
    connect(m_navGroup, &QButtonGroup::idClicked,
            this, &MainWindow::onPageTabChanged);

    // Select the first button by default — show the Projects page on startup
    setCurrentPage(Page::Projects);

    setCentralWidget(centralContainer);
}

// ═════════════════════════════════════════════════════════════════════════════
// Dependency injection
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::setTimeline(Timeline* timeline) { m_timeline = timeline; }
void MainWindow::setCommandStack(CommandStack* stack) { m_commandStack = stack; }
void MainWindow::setShortcutManager(ShortcutManager* mgr) { m_shortcutManager = mgr; }
void MainWindow::setAudioEngine(AudioEngine* engine) { m_audioEngine = engine; }
void MainWindow::setPlaybackController(PlaybackController* controller) { m_playbackController = controller; }
void MainWindow::setMediaPool(MediaPool* pool) {
    m_mediaPool = pool;
    // Also give the SourceMonitor a reference for drag-drop loads
    if (auto* sm = sourceMonitor())
        sm->setMediaPool(pool);
}
void MainWindow::setMediaSourceService(MediaSourceService* service) {
    m_mediaSourceService = service;
}
void MainWindow::setModelManager(ModelManager* mgr) {
    m_modelManager = mgr;
    spdlog::info("MainWindow::setModelManager — mgr={}, scanned={}, "
                 "csp={}, tw={}",
                 static_cast<const void*>(mgr),
                 mgr ? mgr->isScanned() : false,
                 static_cast<const void*>(m_characterShotPanel),
                 static_cast<const void*>(m_timelineWorkspace));
    // Propagate to CharacterShotPanel (and its children: CharacterBrowser,
    // ConversionPanel, ShotComposer) so they refresh after ModelManager
    // finishes its async scan.
    if (m_characterShotPanel)
        m_characterShotPanel->setModelManager(mgr);
    // Propagate to TimelineWorkspace so CharactersPanel refreshes its tree
    if (m_timelineWorkspace)
        m_timelineWorkspace->setModelManager(mgr);
}

// ═════════════════════════════════════════════════════════════════════════════
// Panel / page creation
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::buildPanels()
{
    if (m_panelsBuilt) return;

    spdlog::info("MainWindow::buildPanels() — creating tabbed pages");

    // ── Page 0: PROJECTS ────────────────────────────────────────────────
    m_projectPanel = new ProjectPanel(this);
    m_pageStack->addWidget(m_projectPanel);

    // ── Page 1: CHARACTERS (combined CharacterBrowser + ShotComposer) ────
    m_characterShotPanel = new CharacterShotPanel(this);
    if (m_modelManager) m_characterShotPanel->setModelManager(m_modelManager);
    {
        // Resolve shot presets directory relative to the application folder.
        // In the dev tree this walks up from build/bin/Release/ to the
        // project root; when installed it uses {app}\assets\presets\shots\.
        // The installer creates this with users-modify permissions so users
        // can save their own presets.
        std::filesystem::path presetsDir;
        QString appDir = QCoreApplication::applicationDirPath();
        QDir dir(appDir);
        bool found = false;
        for (int i = 0; i < 5; ++i) {
            QString candidate = dir.absoluteFilePath("assets/presets/shots");
            if (QDir(candidate).exists()) {
                presetsDir = candidate.toStdString();
                found = true;
                break;
            }
            dir.cdUp();
        }
        if (!found)
            presetsDir = (appDir + "/assets/presets/shots").toStdString();
        std::filesystem::create_directories(presetsDir);
        m_characterShotPanel->setPresetsDirectory(presetsDir);
    }
    m_pageStack->addWidget(m_characterShotPanel);

    // ── Page 2: AUDIO ───────────────────────────────────────────────────
    m_audioSync = new AudioSync(this);
    if (m_commandStack) m_audioSync->setCommandStack(m_commandStack);
    if (m_audioEngine) m_audioSync->setAudioEngine(m_audioEngine);
    m_pageStack->addWidget(m_audioSync);

    // Give AudioSync access to the ShotPresetManager for default shot lookup
    m_audioSync->setShotPresetManager(&m_characterShotPanel->shotComposer()->presetManager());

    // When a script is loaded, refresh the shot list so any newly-referenced
    // characters appear in the COMPOSE character filter (if they are downloaded
    // or have existing user-created shots).  Do NOT auto-create default shots.
    connect(m_audioSync, &AudioSync::scriptLoaded, this, [this](int /*lineCount*/) {
        if (m_characterShotPanel && m_characterShotPanel->shotComposer() && m_audioSync) {
            m_characterShotPanel->shotComposer()->refreshShotList();
        }
    });

    // ── Export to Timeline: wire AudioSync → Timeline population ────────
    connect(m_audioSync, &AudioSync::exportRequested, this, [this]() {
        if (!m_audioSync || !m_currentProject) return;

        // ── Sequence picker: ask user which sequence to export to ────
        Timeline* targetTimeline = nullptr;
        if (m_currentProject->sequenceCount() <= 1) {
            // Only one sequence — use it directly
            targetTimeline = m_currentProject->sequence(0);
        } else {
            QStringList seqNames;
            int activeIdx = static_cast<int>(m_currentProject->activeSequenceIndex());
            for (size_t i = 0; i < m_currentProject->sequenceCount(); ++i) {
                auto* seq = m_currentProject->sequence(i);
                seqNames << (seq ? QString::fromStdString(seq->name()) : QStringLiteral("Sequence %1").arg(i + 1));
            }
            bool ok = false;
            QString chosen = QInputDialog::getItem(
                this, "Export to Sequence",
                "Select target sequence:",
                seqNames, activeIdx, false, &ok);
            if (!ok) return;
            int chosenIdx = seqNames.indexOf(chosen);
            if (chosenIdx < 0) chosenIdx = activeIdx;
            targetTimeline = m_currentProject->sequence(static_cast<size_t>(chosenIdx));

            // Switch to the chosen sequence if different from active
            if (static_cast<size_t>(chosenIdx) != m_currentProject->activeSequenceIndex())
                switchSequence(static_cast<size_t>(chosenIdx));
        }
        if (!targetTimeline) return;

        int count = m_audioSync->exportToTimeline(targetTimeline);
        if (count == 0) {
            QMessageBox::warning(this, "No Confirmed Clips",
                "No confirmed clips to export.\n"
                "Use the \xe2\x9c\x93 button to confirm clips before exporting.");
            return;
        }

        // Refresh the timeline UI. Use refreshTrackContents for lightweight
        // update — audio export may have added new tracks, but rebuildTracks
        // resets all track heights which the user has manually adjusted.
        if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel()) {
            auto* panel = m_timelineWorkspace->timelinePanel();
            panel->rebuildTracks();
            panel->notifyZoomChanged();
        }

        // ── Warm the Spine animation cache ─────────────────────────────
        // Proactively queue pre-renders for unique character/outfit pairs
        // on the timeline, so the timeline turns green (cached) immediately
        // instead of staying orange (uncached).  Cap at 10 pairs to avoid
        // flooding the cache worker with hundreds of jobs in large projects.
        static constexpr int kMaxWarmChars = 10;
        if (auto* cache = m_timelineWorkspace
                ? m_timelineWorkspace->animVideoCacheMutable() : nullptr) {
            std::set<std::pair<std::string, std::string>> queuedChars;
            bool capped = false;
            for (size_t ti = 0; ti < targetTimeline->trackCount() && !capped; ++ti) {
                auto* track = targetTimeline->track(ti);
                if (!track || track->type() != TrackType::Video) continue;
                for (size_t ci = 0; ci < track->clipCount() && !capped; ++ci) {
                    auto* clip = track->clip(ci);
                    if (!clip || clip->clipType() != ClipType::Spine) continue;
                    auto* sc = static_cast<SpineClip*>(clip);
                    auto key = std::make_pair(sc->characterName(),
                                               sc->outfit());
                    if (queuedChars.insert(key).second) {
                        if (queuedChars.size() > static_cast<size_t>(kMaxWarmChars)) {
                            spdlog::debug("Spine cache warm: capped at {}, "
                                          "remaining chars warmed on first use",
                                          kMaxWarmChars);
                            capped = true;
                            break;
                        }
                        cache->queueAllAnimations(sc->characterName(),
                                                   sc->outfit());
                    }
                }
            }
        }

        // Force a full blocking audio reload so every clip is decoded
        // and cached before the user presses Play.  Without this, the
        // first playback after export would hit cache misses for most
        // clips and they'd be silent.  Do NOT invalidate first — that
        // clears cached audio sources and triggers recomposite cascades.
        if (m_timelineWorkspace) {
            m_timelineWorkspace->ensureAudioSourcesLoaded();
        }

        // Add synced audio clips to the bin in a single "VO" folder
        if (auto* bin = projectBin()) {
            // Collect all confirmed audio file paths (deduplicated)
            std::set<std::string> seen;
            std::vector<std::filesystem::path> allAudioPaths;
            for (int i = 0; i < m_audioSync->clipCount(); ++i) {
                const auto& sc = m_audioSync->clip(i);
                if (sc.matchState != 2 || sc.sourceFile.empty()) continue;
                std::filesystem::path p(sc.sourceFile);
                if (seen.insert(p.string()).second)
                    allAudioPaths.push_back(p);
            }
            // Drop everything into a single root-level "VO" folder. Don't
            // call ensureDefaultBins() here — the user wants just one
            // folder created on export, not a full Premiere-style scaffold.
            bin->addFilesToNamedBin(allAudioPaths,
                QStringLiteral("VO"), QString());
            bin->refreshSequences();
        }

        // Switch to the Timeline page
        setCurrentPage(Page::Timeline);

        QMessageBox::information(this, "Export Complete",
            QString("Exported %1 audio clip(s) to the timeline.").arg(count));
    });

    // ── Page 3: TIMELINE (splitter-based layout) ────────────────────────
    m_timelineWorkspace = new TimelineWorkspace(this);
    m_timelineWorkspace->setCommandStack(m_commandStack);
    m_timelineWorkspace->setShortcutManager(m_shortcutManager);
    m_timelineWorkspace->setAudioEngine(m_audioEngine);
    m_timelineWorkspace->setPlaybackController(m_playbackController);
    // MediaPool must be set BEFORE timeline so that AnimationVideoCache
    // is available when preloadSpineAssets() runs — otherwise every
    // SpineClip loads its skeleton+atlas even when pre-rendered video
    // exists (wasting 50+ seconds and ~2 GB RAM).
    m_timelineWorkspace->setMediaPool(m_mediaPool);
    m_timelineWorkspace->setMediaSourceService(m_mediaSourceService);
    m_timelineWorkspace->setModelManager(m_modelManager);
    m_timelineWorkspace->setTimeline(m_timeline);
    m_timelineWorkspace->buildPanels();

    // Propagate GPU display mode from ProgramMonitor → TimelineWorkspace
    // so compositeFrame() can skip CPU readback when VulkanViewport is active.
    if (auto* pm = m_timelineWorkspace->programMonitor())
        m_timelineWorkspace->setGpuDisplayMode(pm->isGpuDisplayEnabled());

    // Wire ShotPresetManager to TimelineWorkspace for video character fallback
    if (m_characterShotPanel && m_characterShotPanel->shotComposer())
        m_timelineWorkspace->setShotPresetManager(&m_characterShotPanel->shotComposer()->presetManager());

    // Wire ShotPresetManager to PropertiesPanel for shot switching
    if (m_timelineWorkspace->propertiesPanel() && m_characterShotPanel && m_characterShotPanel->shotComposer()) {
        m_timelineWorkspace->propertiesPanel()->setShotPresetManager(
            &m_characterShotPanel->shotComposer()->presetManager());
    }

    // Wire animation video cache to ShotComposer for character cache indicators
    if (m_characterShotPanel && m_characterShotPanel->shotComposer())
        m_characterShotPanel->shotComposer()->setAnimVideoCache(m_timelineWorkspace->animVideoCache());

    // Wire animation video cache to CharacterBrowser for conversion badges + cache deletion
    if (m_characterShotPanel && m_characterShotPanel->characterBrowser())
        m_characterShotPanel->characterBrowser()->setAnimVideoCache(m_timelineWorkspace->animVideoCacheMutable());

    // Wire animation video cache (non-const) to ConversionPanel for manual conversion
    if (m_characterShotPanel)
        m_characterShotPanel->setAnimVideoCache(m_timelineWorkspace->animVideoCacheMutable());

    // When a character is downloaded, refresh the Library panel's CharactersPanel tree
    if (m_characterShotPanel && m_characterShotPanel->characterBrowser() && m_timelineWorkspace) {
        connect(m_characterShotPanel->characterBrowser(), &CharacterBrowser::downloadRequested,
                this, [this]() {
            if (m_timelineWorkspace && m_timelineWorkspace->libraryPanel())
                m_timelineWorkspace->libraryPanel()->refreshCurrentTab();
        });
    }

    m_pageStack->addWidget(m_timelineWorkspace);

    // ── Wire ProjectBin sequence signals ────────────────────────────────
    if (auto* bin = m_timelineWorkspace->projectBin()) {
        connect(bin, &ProjectBin::sequenceOpened, this, &MainWindow::switchSequence);
        connect(bin, &ProjectBin::sequencesChanged, this, [this]() {
            if (!m_currentProject) return;
            // After add/delete/duplicate, ensure the active timeline is still valid
            Timeline* active = m_currentProject->sequence(m_currentProject->activeSequenceIndex());
            if (active && active != m_timeline) {
                // Active sequence changed — switch to it
                m_timeline = active;
                if (m_timelineWorkspace) m_timelineWorkspace->setTimeline(active);
                if (m_playbackController) m_playbackController->setTimeline(active);
                if (m_exportPanel) {
                    m_exportPanel->setTimeline(active);
                    m_exportPanel->setProject(m_currentProject.get());
                }
                if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
                    m_timelineWorkspace->timelinePanel()->rebuildTracks();
                if (auto* pm = programMonitor()) pm->refresh();
            }
            // Refresh the bin list and sequence tabs
            if (auto* b = projectBin()) b->refreshSequences();
            if (m_timelineWorkspace)
                m_timelineWorkspace->refreshSequenceTabs();
        });
        connect(bin, &ProjectBin::sequenceSettingsChanged, this, [this]() {
            if (!m_currentProject) return;
            const auto& s = m_currentProject->settings();
            if (auto* pm = programMonitor()) {
                pm->setOutputResolution(s.resolution().width, s.resolution().height);
                pm->requestRefresh();
            }
            if (m_playbackController)
                m_playbackController->setFrameRate(s.frameRate());
            if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
                m_timelineWorkspace->timelinePanel()->setFrameRate(s.frameRate());
            if (auto* b = projectBin()) b->refreshSequences();
            if (m_timelineWorkspace)
                m_timelineWorkspace->refreshSequenceTabs();
        });
        connect(bin, &ProjectBin::nestSequenceRequested,
                this, [this](size_t seqIdx, const QString& seqName) {
            if (!m_currentProject || !m_timeline || !m_timelineWorkspace) return;
            // Prevent circular nesting: don't nest active sequence into itself
            if (seqIdx == m_currentProject->activeSequenceIndex()) return;
            m_timelineWorkspace->nestSequence(seqIdx, seqName);
        });

        // Propagated clip removal from the bin → refresh timeline views and
        // audio/composite caches (keeps playback consistent with the model).
        connect(bin, &ProjectBin::timelineClipsMutated, this, [this]() {
            if (m_timelineWorkspace) {
                if (auto* tp = m_timelineWorkspace->timelinePanel())
                    tp->rebuildTracks();
                m_timelineWorkspace->invalidateAudioSources();
                m_timelineWorkspace->refreshAfterUndoRedo();
            }
            if (auto* pm = programMonitor()) pm->refresh();
        });

        // Double-click (or drag) media items → load in Source Monitor
        connect(bin, &ProjectBin::loadInSourceMonitor,
                this, [this](const std::filesystem::path& /*filePath*/, uint64_t mediaHandle) {
            auto* sm = sourceMonitor();
            if (sm && m_mediaPool)
                sm->loadClip(mediaHandle, m_mediaPool);
        });
    }

    // ── Wire sequence tab bar to sequence switching ─────────────────────
    connect(m_timelineWorkspace, &TimelineWorkspace::sequenceTabChanged,
            this, &MainWindow::switchSequence);

    // ── Wire auto-create project on media drop ──────────────────────────
    connect(m_timelineWorkspace, &TimelineWorkspace::requestNewProjectForMedia,
            this, &MainWindow::onNewProjectForMedia);

    // ── Wire auto-created project from sequence creation ────────────────
    connect(m_timelineWorkspace, &TimelineWorkspace::autoProjectCreated,
            this, [this](Project* project) {
        // Disconnect old timeline from all consumers BEFORE destroying
        // the old project, otherwise dangling-pointer access will crash.
        if (m_timelineWorkspace)
            m_timelineWorkspace->setTimeline(nullptr);
        if (m_playbackController)
            m_playbackController->setTimeline(nullptr);
        if (m_exportPanel)
            m_exportPanel->setTimeline(nullptr);
        m_timeline = nullptr;

        // Save the auto-created project to disk so it survives swaps and
        // can be reopened from the PROJECTS page later.  Without this, an
        // auto-created project (from Project Bin "New Sequence" with no
        // project open) is only kept in memory — if the user switches to
        // another project, the auto-created one is gone forever.
        {
            QString projName = QString::fromStdString(project->name());
            QString projDir = projectsDirectory();
            QString projectFolder = projDir + "/" + projName;
            QDir().mkpath(projectFolder);
            std::filesystem::path savePath =
                (projectFolder + "/" + projName + ".rtp").toStdWString();
            project->setFilePath(savePath);
            ProjectSerializer serializer;
            if (serializer.save(*project, savePath)) {
                spdlog::info("Auto-created project saved to: {}",
                             savePath.string());
                addToRecentFiles(QString::fromStdString(savePath.string()));
            } else {
                spdlog::warn("Failed to save auto-created project '{}'",
                             projName.toStdString());
            }
        }

        // MainWindow takes ownership of the auto-created project
        m_currentProject.reset(project);
        m_lastSavedAudioSyncBlob = {};
        if (m_currentProject->timeline()) {
            m_timeline = m_currentProject->timeline();
            if (m_timelineWorkspace)
                m_timelineWorkspace->setTimeline(m_timeline);
            if (m_playbackController)
                m_playbackController->setTimeline(m_timeline);
            if (m_exportPanel)
                m_exportPanel->setTimeline(m_timeline);
        }
        QString name = QString::fromStdString(m_currentProject->name());
        setWindowTitle(QString("ROUNDTABLE NLE %1 \u2014 %2")
                       .arg(ROUNDTABLE_VERSION).arg(name));
        if (auto* bin = projectBin())
            bin->setProjectName(name);
    });

    // ── Page 4: EXPORT ──────────────────────────────────────────────────
    m_exportPanel = new ExportPanel(this);
    if (m_timeline) m_exportPanel->setTimeline(m_timeline);
    if (m_currentProject) m_exportPanel->setProject(m_currentProject.get());
    if (m_playbackController) m_exportPanel->setPlaybackController(m_playbackController);
    if (m_audioEngine) m_exportPanel->setAudioEngine(m_audioEngine);
    m_exportPanel->setPreviewCallback(
        [this](int64_t tick, uint32_t w, uint32_t h, bool scrub)
            -> std::shared_ptr<CachedFrame> {
            if (m_timelineWorkspace) {
                // Force Full resolution for export preview (characters too).
                // This is always reset on the next call, and the Program/Source
                // Monitors never go through this callback.
                m_timelineWorkspace->setForceFullResolution(true);
                auto result = m_timelineWorkspace->compositeFrame(tick, w, h, scrub);
                m_timelineWorkspace->setForceFullResolution(false);
                return result;
            }
            return nullptr;
        });
    m_pageStack->addWidget(m_exportPanel);

    // ── Wire ProjectPanel signals ────────────────────────────────────
    connect(m_projectPanel, &ProjectPanel::createProject,
            this, &MainWindow::onCreateProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::openProject,
            this, &MainWindow::onOpenProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::deleteProject,
            this, &MainWindow::onDeleteProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::renameProject,
            this, &MainWindow::onRenameProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::duplicateProject,
            this, &MainWindow::onDuplicateProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::revealInExplorer,
            this, &MainWindow::onRevealProjectInExplorer);
    connect(m_projectPanel, &ProjectPanel::openRecentProject,
            this, &MainWindow::onOpenRecentProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::importProject,
            this, &MainWindow::onImportProject);
    connect(m_projectPanel, &ProjectPanel::exportProject,
            this, &MainWindow::onExportProject);
    connect(m_projectPanel, &ProjectPanel::projectsDirChanged,
            this, &MainWindow::onProjectsDirChanged);
    connect(m_projectPanel, &ProjectPanel::openFromFile,
            this, &MainWindow::onOpenProject);
    connect(m_projectPanel, &ProjectPanel::openFilePath,
            this, &MainWindow::onOpenRecentProjectFromPanel);
    connect(m_projectPanel, &ProjectPanel::saveRequested,
            this, &MainWindow::onSaveProject);
    connect(m_projectPanel->refreshButton(), &QPushButton::clicked,
            this, &MainWindow::refreshProjectsList);

    // Give the project panel the projects directory for its file browser
    m_projectPanel->setProjectsDirectory(projectsDirectory());

    // Populate the projects list
    refreshProjectsList();

    // Start on the PROJECTS page for first launch
    setCurrentPage(Page::Projects);

    // F12: Remember last project path but don't auto-open it.
    //       The user should explicitly select a project from the Projects page.
    //       We still save/restore the path for "recent projects" convenience.
    {
        // (auto-load removed — start on Projects page every time)
    }

    // ── Playback resolution shortcuts (Alt+1/2/3/4) ────────────────────
    if (m_shortcutManager && m_timelineWorkspace) {
        auto* pm = m_timelineWorkspace->programMonitor();
        if (pm) {
            m_shortcutManager->registerAction(
                "view.res_full", "Playback Resolution: Full",
                QKeySequence(Qt::ALT | Qt::Key_1),
                [pm]() { pm->setPlaybackResolutionIndex(0); }, "View");
            m_shortcutManager->registerAction(
                "view.res_half", "Playback Resolution: 1/2",
                QKeySequence(Qt::ALT | Qt::Key_2),
                [pm]() { pm->setPlaybackResolutionIndex(1); }, "View");
            m_shortcutManager->registerAction(
                "view.res_quarter", "Playback Resolution: 1/4",
                QKeySequence(Qt::ALT | Qt::Key_3),
                [pm]() { pm->setPlaybackResolutionIndex(2); }, "View");
            m_shortcutManager->registerAction(
                "view.res_eighth", "Playback Resolution: 1/8",
                QKeySequence(Qt::ALT | Qt::Key_4),
                [pm]() { pm->setPlaybackResolutionIndex(3); }, "View");
        }
    }

    m_panelsBuilt = true;
    spdlog::info("MainWindow::buildPanels() — 5 pages created "
                 "({} timeline panels)", m_timelineWorkspace->dockCount());
}

// ═════════════════════════════════════════════════════════════════════════════
// Page navigation
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::setCurrentPage(Page page)
{
    int idx = static_cast<int>(page);
    if (idx >= 0 && idx < 5 && m_navButtons[idx]) {
        m_navButtons[idx]->setChecked(true);
        onPageTabChanged(idx);   // QButtonGroup::idClicked only fires on user clicks
    }
}

Page MainWindow::currentPage() const noexcept
{
    int id = m_navGroup ? m_navGroup->checkedId() : 0;
    return static_cast<Page>(id);
}

void MainWindow::toggleNavRail()
{
    m_navCollapsed = !m_navCollapsed;

    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    // Per-page accent colors for collapsed vertical-text buttons
    static const QColor kPageColors[] = {
        QColor(255, 200, 40),   // PROJECTS  — yellow
        QColor(180, 100, 255),  // CHARACTERS — purple
        QColor(80, 210, 120),   // AUDIO     — green
        QColor(70, 150, 255),   // TIMELINE  — blue
        QColor(240, 70, 70)     // EXPORT    — red
    };
    static const char* kPageLabels[] = {
        "PROJECTS", "CHARACTERS", "AUDIO", "TIMELINE", "EXPORT"
    };

    if (m_navCollapsed) {
        constexpr int kBtnW = 28;

        // Measure the actual expanded entry stride from live button
        // positions.  At this point the buttons are still at their
        // expanded 150×120 size — geometry is valid and fully laid out.
        int kTargetH = m_navButtons[1]->y() - m_navButtons[0]->y();
        if (kTargetH <= 0) {
            // Fallback formula (should never be needed at runtime)
            QFont lblFont;
            lblFont.setPixelSize(16);
            lblFont.setWeight(QFont::ExtraBold);
            int lblH = QFontMetrics(lblFont).height();
            kTargetH = 120 + 2 + lblH + 2 + m.spacingSm + 2 + 1 + 2 + m.spacingSm + 2;
        }
        spdlog::debug("NavRail collapse: measured stride = {}", kTargetH);

        auto* layout = m_navRail->layout();
        auto* vlay = static_cast<QVBoxLayout*>(layout);

        // In compact mode each button takes the full stride height.
        // All spacers, dividers, and labels are hidden — the divider
        // line is drawn as a CSS bottom-border on each button instead.
        // This guarantees pixel-perfect alignment with the sub-rail
        // regardless of spacing arithmetic.
        // In compact mode labels and dividers are hidden.  The
        // remaining gap between consecutive buttons is 2 spacer items
        // (spacingSm each) plus 3 inter-item layout spacings.
        const int kGap     = 3 * vlay->spacing() + 2 * m.spacingSm; // 3×2 + 2×6 = 18
        const int kCompactH = kTargetH - kGap + 2;  // +2 fine-tune

        for (int i = 0; i < 5; ++i) {
            QColor col = kPageColors[i];

            // Render vertical text (rotated 90° CW) into a QPixmap.
            QFont font;
            font.setPixelSize(11);
            font.setWeight(QFont::ExtraBold);
            font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
            QFontMetrics fm(font);

            QString label = QString::fromUtf8(kPageLabels[i]);
            int textW = fm.horizontalAdvance(label);
            int textH = fm.height();

            // Use the compact button height for the rotated text pixmap.
            int pixW = kCompactH;
            int pad  = (pixW - textW) / 2;

            qreal dpr = devicePixelRatioF();
            QPixmap hPix(static_cast<int>(pixW * dpr),
                         static_cast<int>(textH * dpr));
            hPix.setDevicePixelRatio(dpr);
            hPix.fill(Qt::transparent);
            {
                QPainter p(&hPix);
                p.setFont(font);
                p.setPen(col);
                p.drawText(pad, fm.ascent(), label);
            }

            // Rotate 90° clockwise → vertical pixmap
            QTransform rot;
            rot.rotate(90);
            QPixmap vPix = hPix.transformed(rot, Qt::SmoothTransformation);
            vPix.setDevicePixelRatio(dpr);

            m_navButtons[i]->setText(QString());
            m_navButtons[i]->setIcon(QIcon(vPix));
            m_navButtons[i]->setIconSize(QSize(kBtnW, kCompactH));
            m_navButtons[i]->setFixedSize(kBtnW, kCompactH);

            QString css = QStringLiteral(
                "QPushButton { background: rgba(%1,%2,%3,30); border: none;"
                "  border-radius: %4px; padding: 0; }"
                "QPushButton:hover { background: rgba(%1,%2,%3,55); }"
                "QPushButton:pressed { background: rgba(%1,%2,%3,90); }"
                "QPushButton:checked { background: rgba(%1,%2,%3,80); }")
                .arg(col.red()).arg(col.green()).arg(col.blue())
                .arg(m.radiusSm);

            m_navButtons[i]->setStyleSheet(css);
            m_navButtons[i]->show();
        }

        // Hide everything except nav buttons and expand button.
        for (int i = 0; i < layout->count(); ++i) {
            auto* w = layout->itemAt(i)->widget();
            if (!w) continue;
            bool isNavBtn = false;
            for (int j = 0; j < 5; ++j)
                if (w == m_navButtons[j]) { isNavBtn = true; break; }
            if (isNavBtn || w == m_navExpandBtn) continue;
            w->hide();
        }

        // Configure expand button: vertical "EXPAND" text in red
        {
            QColor redCol(240, 70, 70);
            QFont efont;
            efont.setPixelSize(11);
            efont.setWeight(QFont::ExtraBold);
            efont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
            QFontMetrics efm(efont);

            QString elabel = QStringLiteral("EXPAND");
            int etextW = efm.horizontalAdvance(elabel);
            int etextH = efm.height();
            int epixW  = kCompactH;
            int epad   = (epixW - etextW) / 2;

            qreal edpr = devicePixelRatioF();
            QPixmap ehPix(static_cast<int>(epixW * edpr),
                          static_cast<int>(etextH * edpr));
            ehPix.setDevicePixelRatio(edpr);
            ehPix.fill(Qt::transparent);
            {
                QPainter p(&ehPix);
                p.setFont(efont);
                p.setPen(redCol);
                p.drawText(epad, efm.ascent(), elabel);
            }

            QTransform erot;
            erot.rotate(90);
            QPixmap evPix = ehPix.transformed(erot, Qt::SmoothTransformation);
            evPix.setDevicePixelRatio(edpr);

            m_navExpandBtn->setText(QString());
            m_navExpandBtn->setIcon(QIcon(evPix));
            m_navExpandBtn->setIconSize(QSize(kBtnW, kCompactH));
            m_navExpandBtn->setFixedSize(kBtnW, kCompactH);

            QString ecss = QStringLiteral(
                "QPushButton { background: rgba(240,70,70,30); border: none;"
                "  border-radius: %1px; padding: 0; }"
                "QPushButton:hover { background: rgba(240,70,70,55); }"
                "QPushButton:pressed { background: rgba(240,70,70,90); }")
                .arg(m.radiusSm);
            m_navExpandBtn->setStyleSheet(ecss);
        }

        m_navExpandBtn->show();
        m_navRail->setFixedWidth(44);
    } else {
        // (Spacers and layout spacing were never modified in compact mode,
        //  so nothing to restore here — just proceed with button geometry.)

        auto* layout = m_navRail->layout();

        // Restore original emoji + full-size nav buttons
        static const char* kPageIcons[] = {
            "\U0001F4C1", "\U0001F464", "\U0001F3B5",
            "\U0001F39E", "\U0001F4E4"
        };

        QString navBtnStyle = QStringLiteral(
            "QPushButton { background: transparent; border: none;"
            "  border-radius: %1px; color: %2; font-size: 42px;"
            "  padding: 12px 0; }"
            "QPushButton:hover { background: %3; color: %4; }"
            "QPushButton:pressed { background: %5; color: white; }"
            "QPushButton:checked { background: %6; color: %7; }")
            .arg(m.radiusXl)
            .arg(Theme::rgb(c.textTertiary))
            .arg(Theme::rgb(c.surface3))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.accentDim))
            .arg(Theme::rgb(c.accentDim))
            .arg(Theme::rgb(c.accent));

        for (int i = 0; i < 5; ++i) {
            m_navButtons[i]->setIcon(QIcon());   // clear icon
            m_navButtons[i]->setText(QString::fromUtf8(kPageIcons[i]));
            m_navButtons[i]->setFixedSize(128, 84);
            m_navButtons[i]->setStyleSheet(navBtnStyle);
        }

        // Restore collapse button to expanded size
        m_navCollapseBtn->setFixedSize(128, 70);

        // Show everything, hide expand button
        for (int i = 0; i < layout->count(); ++i) {
            auto* w = layout->itemAt(i)->widget();
            if (!w) continue;
            if (w == m_navExpandBtn) continue;
            w->show();
        }
        m_navExpandBtn->hide();
        m_navRail->setFixedWidth(150);
    }
}

void MainWindow::onPageTabChanged(int index)
{
    m_pageStack->setCurrentIndex(index);
    emit pageChanged(index);

    // When switching TO the Timeline page, force the Program Monitor to
    // re-composite so it doesn't show a stale frame from Shots/Export.
    // The VulkanViewport native HWND can retain stale content while hidden,
    // and the ExportPanel shares the same compositeFrame() callback which
    // may have overwritten the GPU compositor output.
    if (index == static_cast<int>(Page::Timeline)) {
        if (auto* pm = programMonitor())
            pm->refresh();
    }

    // Log page switch
    static const char* pageNames[] = {"PROJECTS", "CHARACTERS", "AUDIO", "TIMELINE", "EXPORT"};
    if (index >= 0 && index < 5)
        spdlog::debug("Switched to {} page", pageNames[index]);
}

// ═════════════════════════════════════════════════════════════════════════════
// Panel accessors (delegate through TimelineWorkspace)
// ═════════════════════════════════════════════════════════════════════════════

TimelinePanel*   MainWindow::timelinePanel()   const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->timelinePanel()   : nullptr; }
SourceMonitor*   MainWindow::sourceMonitor()   const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->sourceMonitor()   : nullptr; }
ProgramMonitor*  MainWindow::programMonitor()  const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->programMonitor()  : nullptr; }
ProjectBin*      MainWindow::projectBin()      const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->projectBin()      : nullptr; }
PropertiesPanel* MainWindow::propertiesPanel() const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->propertiesPanel() : nullptr; }
EffectControlsPanel* MainWindow::effectControlsPanel() const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->effectControlsPanel() : nullptr; }
EffectsPanel*    MainWindow::effectsPanel()    const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->effectsPanel()    : nullptr; }
AudioMixer*      MainWindow::audioMixer()      const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->audioMixer()      : nullptr; }
KeyframeEditor*  MainWindow::keyframeEditor()  const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->keyframeEditor()  : nullptr; }
HistoryPanel*    MainWindow::historyPanel()    const noexcept { return m_timelineWorkspace ? m_timelineWorkspace->historyPanel()    : nullptr; }

CharacterBrowser* MainWindow::characterBrowser() const noexcept { return m_characterShotPanel ? m_characterShotPanel->characterBrowser() : nullptr; }
ShotComposer*     MainWindow::shotComposer()     const noexcept { return m_characterShotPanel ? m_characterShotPanel->shotComposer()     : nullptr; }

QDockWidget* MainWindow::dockForPanel(const QString& panelName) const
{
    return m_timelineWorkspace ? m_timelineWorkspace->dockForPanel(panelName) : nullptr;
}

int MainWindow::dockCount() const noexcept
{
    return m_timelineWorkspace ? m_timelineWorkspace->dockCount() : 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// Menu bar methods are in MainWindowMenus.cpp
// ═════════════════════════════════════════════════════════════════════════════

// Workspace
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::applyDefaultLayout()
{
    // Try to restore the last session's layout (window geometry + dock arrangement).
    // Window geometry is applied immediately so the window appears at the correct
    // position/size.
    QSettings settings("Roundtable", "ROUNDTABLE NLE");
    settings.beginGroup("workspace/last_session");

    QByteArray geo = settings.value("geometry").toByteArray();
    bool savedCollapsed = settings.value("navCollapsed", false).toBool();
    int savedPage = static_cast<int>(Page::Projects); // default to Projects if no saved state
    if (!geo.isEmpty()) {
        restoreGeometry(geo);
        // Restore dock layout while still inside the workspace group
        if (m_timelineWorkspace)
            m_timelineWorkspace->restoreDockLayout(settings);
        // Restore the last active page so the user returns to where they left off.
        savedPage = settings.value("activePage",
                                    static_cast<int>(Page::Projects)).toInt();
        if (savedPage < 0 || savedPage > static_cast<int>(Page::Export))
            savedPage = static_cast<int>(Page::Projects);
    }

    settings.endGroup();

    if (geo.isEmpty()) {
        // No saved workspace — try the bundled default layout file.
        // This ships with the installer so new users get the developer's
        // panel arrangement (dock positions, sizes, tab order).
        QString bundledLayout = QCoreApplication::applicationDirPath()
            + QStringLiteral("/assets/default_layout.bin");
        if (QFile::exists(bundledLayout)) {
            spdlog::info("applyDefaultLayout: loading bundled layout from {}",
                         bundledLayout.toStdString());
            restoreWorkspaceFromFile(bundledLayout);
            // Always start on the PROJECTS page regardless of saved state
            setCurrentPage(Page::Projects);
        } else {
            // No saved or bundled layout — reset the timeline workspace to its
            // programmatic default dock arrangement so panels aren't scattered.
            if (m_timelineWorkspace)
                m_timelineWorkspace->resetToDefaultDockLayout();
            // Always start on the PROJECTS tab
            setCurrentPage(Page::Projects);
        }
    } else {
        // Geometry was restored from saved session — always start on Projects
        setCurrentPage(Page::Projects);
    }

    // Defer sidebar collapse until after the window is shown and laid out.
    // toggleNavRail() measures button positions (m_navButtons[1]->y() -
    // m_navButtons[0]->y()) to compute compact button height.  Before the
    // window is shown, geometry is zero/invalid, producing wrong sizes.
    if (savedCollapsed != m_navCollapsed) {
        QTimer::singleShot(0, this, [this]() {
            toggleNavRail();
        });
    }
}

void MainWindow::saveWorkspace(const QString& name)
{
    QSettings settings("Roundtable", "ROUNDTABLE NLE");
    settings.beginGroup("workspace/" + name);
    settings.setValue("geometry", saveGeometry());
    settings.setValue("activePage", static_cast<int>(currentPage()));
    settings.setValue("navCollapsed", m_navCollapsed);

    // Save the Timeline workspace dock layout (panel positions, sizes, tab order)
    if (m_timelineWorkspace)
        m_timelineWorkspace->saveDockLayout(settings);

    settings.endGroup();
    spdlog::info("Workspace '{}' saved", name.toStdString());
}

bool MainWindow::restoreWorkspace(const QString& name)
{
    QSettings settings("Roundtable", "ROUNDTABLE NLE");
    settings.beginGroup("workspace/" + name);

    QByteArray geo = settings.value("geometry").toByteArray();

    if (geo.isEmpty()) {
        settings.endGroup();
        spdlog::warn("No saved workspace '{}'", name.toStdString());
        return false;
    }

    restoreGeometry(geo);
    // Restore the last active page so the user returns to where they left off.
    int savedPage = settings.value("activePage",
                                    static_cast<int>(Page::Projects)).toInt();
    if (savedPage < 0 || savedPage > static_cast<int>(Page::Export))
        savedPage = static_cast<int>(Page::Projects);
    setCurrentPage(static_cast<Page>(savedPage));

    // Restore sidebar collapsed/expanded state
    bool savedCollapsed = settings.value("navCollapsed", false).toBool();
    if (savedCollapsed != m_navCollapsed)
        toggleNavRail();

    // Restore the Timeline workspace dock layout
    if (m_timelineWorkspace)
        m_timelineWorkspace->restoreDockLayout(settings);

    settings.endGroup();
    spdlog::info("Workspace '{}' restored", name.toStdString());
    return true;
}

void MainWindow::saveWorkspaceToFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        spdlog::warn("saveWorkspaceToFile: cannot write {}", filePath.toStdString());
        return;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_5);

    // Serialize the workspace to a temporary QSettings (IniFormat),
    // then write and read back as a single QByteArray blob.
    {
        QTemporaryDir tmpDir;
        QString iniPath = tmpDir.filePath("workspace.ini");
        QSettings tmpSettings(iniPath, QSettings::IniFormat);
        tmpSettings.beginGroup("workspace/last_session");
        tmpSettings.setValue("geometry", saveGeometry());
        tmpSettings.setValue("activePage", static_cast<qint32>(currentPage()));
        tmpSettings.setValue("navCollapsed", m_navCollapsed);
        if (m_timelineWorkspace)
            m_timelineWorkspace->saveDockLayout(tmpSettings);
        tmpSettings.endGroup();
        tmpSettings.sync();

        // Read the ini file back into a byte array
        QFile iniFile(iniPath);
        if (iniFile.open(QIODevice::ReadOnly)) {
            QByteArray fileBytes = iniFile.readAll();
            stream << fileBytes;
        }
    }

    file.close();
    spdlog::info("Workspace layout saved to {}", filePath.toStdString());
}

bool MainWindow::restoreWorkspaceFromFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_5);

    QByteArray iniData;
    stream >> iniData;
    if (iniData.isEmpty()) {
        file.close();
        return false;
    }
    file.close();

    // Write the ini data to a temp file and load via QSettings(IniFormat)
    QTemporaryDir tmpDir;
    QString iniPath = tmpDir.filePath("workspace.ini");
    {
        QFile iniFile(iniPath);
        if (!iniFile.open(QIODevice::WriteOnly)) return false;
        iniFile.write(iniData);
        iniFile.close();
    }

    QSettings fileSettings(iniPath, QSettings::IniFormat);
    fileSettings.beginGroup("workspace/last_session");

    QByteArray geo = fileSettings.value("geometry").toByteArray();
    if (geo.isEmpty()) {
        fileSettings.endGroup();
        return false;
    }

    restoreGeometry(geo);

    // Read the saved active page (default to Projects for backward compat)
    int savedPage = fileSettings.value("activePage",
                                        static_cast<int>(Page::Projects)).toInt();
    setCurrentPage(static_cast<Page>(savedPage));

    bool savedCollapsed = fileSettings.value("navCollapsed", false).toBool();
    if (savedCollapsed != m_navCollapsed)
        toggleNavRail();

    // Restore dock layout (deferred if widget not yet visible)
    if (m_timelineWorkspace)
        m_timelineWorkspace->restoreDockLayout(fileSettings);

    fileSettings.endGroup();
    spdlog::info("Workspace layout restored from {}", filePath.toStdString());
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Status bar
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::setupStatusBar()
{
    auto* sb = statusBar();
    sb->showMessage("Ready");

    // Busy spinner (indeterminate progress bar) — hidden by default
    m_busySpinner = new QProgressBar(sb);
    m_busySpinner->setRange(0, 0);          // indeterminate
    m_busySpinner->setFixedSize(120, 14);
    m_busySpinner->setTextVisible(false);
    m_busySpinner->setVisible(false);

    m_busyLabel = new QLabel(sb);
    m_busyLabel->setStyleSheet("color: palette(text); font-size: 11px;");
    m_busyLabel->setVisible(false);

    sb->addPermanentWidget(m_busyLabel);
    sb->addPermanentWidget(m_busySpinner);
}

void MainWindow::showBusyIndicator(const QString& message)
{
    if (m_busyLabel)   { m_busyLabel->setText(message); m_busyLabel->setVisible(true); }
    if (m_busySpinner) { m_busySpinner->setVisible(true); }
}

void MainWindow::hideBusyIndicator()
{
    if (m_busySpinner) m_busySpinner->setVisible(false);
    if (m_busyLabel)   m_busyLabel->setVisible(false);
    statusBar()->showMessage("Ready");
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
