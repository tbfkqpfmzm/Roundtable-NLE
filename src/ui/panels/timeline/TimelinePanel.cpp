/*
 * TimelinePanel.cpp Ã¢â‚¬â€ Main timeline container.
 * Step 12: Timeline Panel Ã¢â‚¬â€ Core UI
 */

#include "panels/timeline/TimelinePanel.h"
#include "Theme.h"
#include "widgets/TimelineRuler.h"
#include "widgets/TimelineTrackWidget.h"
#include "widgets/NLEScrollBar.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "timeline/EditOperations.h"
#include "timeline/GraphicClip.h"
#include "command/CommandStack.h"
#include "command/commands/TrackCommands.h"
#include "command/CompoundCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"
#include "ShortcutManager.h"
#include "media/AudioFile.h"
#include "media/VideoDecoder.h"

#include <QPixmap>
#include <QFontMetrics>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QCursor>
#include <QInputDialog>
#include <QDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QSplitter>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPointer>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QAction>
#include <QColorDialog>
#include <QPushButton>
#include <QToolTip>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTreeWidget>
#include <QTimer>

#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "timeline/Transition.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <limits>

#include <algorithm>
#include <cmath>
#include <thread>

#include "panels/timeline/TimelinePanelInternal.h"
#include "panels/timeline/PlayheadLineWidget.h"

namespace rt {

};

namespace rt {

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  TimelinePanel
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

TimelinePanel::TimelinePanel(QWidget* parent)
    : QWidget(parent)
{
    setAcceptDrops(true);
    setupLayout();
}

TimelinePanel::~TimelinePanel() = default;

void TimelinePanel::setupLayout()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Top row: header spacer + ruler
    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(0);

    // The spacer must cover both the header scroll area width AND the
    // splitter handle (3px) so the ruler left edge aligns with the track
    // content viewport left edge.  Without this, the ruler and playhead
    // overlay are offset by the handle width.
    constexpr int kHandleW = 3; // must match m_headerSplitter->setHandleWidth()
    m_headerSpacer = new QWidget(this);
    m_headerSpacer->setFixedSize(TrackHeader::kHeaderWidth + kHandleW, TimelineRuler::kRulerHeight);
    m_headerSpacer->setStyleSheet(QStringLiteral("background-color: %1;")
        .arg(Theme::hex(Theme::colors().surface1)));

    m_ruler = new TimelineRuler(this);
    m_ruler->setLayoutEngine(&m_layoutEngine);

    topRow->addWidget(m_headerSpacer);
    topRow->addWidget(m_ruler, 1);
    mainLayout->addLayout(topRow);

    // Middle row: track headers + track content via resizable QSplitter
    m_trackHeaderArea = new QWidget();
    auto* headerLayout = new QVBoxLayout(m_trackHeaderArea);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    m_headerScroll = new QScrollArea(this);
    m_headerScroll->setWidgetResizable(true);
    m_headerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_headerScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_headerScroll->setFrameShape(QFrame::NoFrame);
    // Lock header to kHeaderWidth during initial layout; unlocked on first resize
    m_headerScroll->setFixedWidth(TrackHeader::kHeaderWidth);
    m_headerScroll->setWidget(m_trackHeaderArea);

    // Track content area (inside scroll area)
    m_verticalScroll = new QScrollArea(this);
    m_verticalScroll->setWidgetResizable(true);
    m_verticalScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_verticalScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_verticalScroll->setFrameShape(QFrame::NoFrame);

    m_trackContentArea = new QWidget();
    auto* trackLayout = new QVBoxLayout(m_trackContentArea);
    trackLayout->setContentsMargins(0, 0, 0, 0);
    trackLayout->setSpacing(0);

    m_verticalScroll->setWidget(m_trackContentArea);

    // Install event filter on the vertical scroll area (and its viewport)
    // so wheel events are forwarded to TimelinePanel for zoom/scroll,
    // and mouse events on empty track-area space trigger marquee selection.
    m_verticalScroll->installEventFilter(this);
    m_verticalScroll->viewport()->installEventFilter(this);
    m_trackContentArea->installEventFilter(this);
    m_trackHeaderArea->installEventFilter(this);

    // Enable mouse-tracking on the viewport and track-content widget so
    // cursor feedback (arrow, cross-hair, etc.) works when hovering empty
    // space that is not covered by a TimelineTrackWidget.
    m_verticalScroll->viewport()->setMouseTracking(true);
    m_trackContentArea->setMouseTracking(true);
    setMouseTracking(true);

    // Synchronise vertical scroll positions between header and content areas.
    connect(m_verticalScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            m_headerScroll->verticalScrollBar(), &QScrollBar::setValue);
    connect(m_headerScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            m_verticalScroll->verticalScrollBar(), &QScrollBar::setValue);

    // Resizable splitter between header column and track content
    m_headerSplitter = new QSplitter(Qt::Horizontal, this);
    m_headerSplitter->setChildrenCollapsible(false);
    m_headerSplitter->addWidget(m_headerScroll);
    m_headerSplitter->addWidget(m_verticalScroll);
    m_headerSplitter->setStretchFactor(0, 0); // header side: don't stretch
    m_headerSplitter->setStretchFactor(1, 1); // content side: stretch

    // Style the splitter: dark background matching theme, thin black handle
    m_headerSplitter->setHandleWidth(3);
    m_headerSplitter->setStyleSheet(QStringLiteral(
        "QSplitter { background: #16161e; }"
        "QSplitter::handle { background: #000000; cursor: SplitHCursor; }"));

    // Keep header spacer and scroll spacer in sync with the splitter.
    // Spacer must cover header width + handle so ruler aligns with viewport.
    connect(m_headerSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
        const int hw = m_headerScroll->width();
        const int handle = m_headerSplitter->handleWidth();
        m_headerSpacer->setFixedWidth(hw + handle);
        m_scrollSpacer->setFixedWidth(hw + handle);
        // Re-layout track headers to match new width
        m_trackHeaderArea->setFixedWidth(hw);
        for (auto* hdr : m_trackHeaders)
            hdr->setFixedWidth(hw);
        // Reposition playhead overlay — the track-content viewport changed
        // width so the viewport-relative X coordinate must be recalculated.
        QTimer::singleShot(0, this, [this]() { onScrollChanged(); });
    });

    mainLayout->addWidget(m_headerSplitter, 1);

    // Bottom row: NLE scrollbar
    auto* bottomRow = new QHBoxLayout();
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(0);

    m_scrollSpacer = new QWidget(this);
    m_scrollSpacer->setFixedSize(TrackHeader::kHeaderWidth + kHandleW, NLEScrollBar::kBarHeight);
    m_scrollSpacer->setStyleSheet(QStringLiteral("background-color: %1;")
        .arg(Theme::hex(Theme::colors().surface2)));

    m_scrollBar = new NLEScrollBar(this);
    m_scrollBar->setLayoutEngine(&m_layoutEngine);

    bottomRow->addWidget(m_scrollSpacer);
    bottomRow->addWidget(m_scrollBar, 1);
    mainLayout->addLayout(bottomRow);

    // Connections
    connect(m_ruler, &TimelineRuler::playheadScrubbed,
            this, &TimelinePanel::onRulerScrub);
    connect(m_ruler, &TimelineRuler::markerRequested,
            this, [this](int64_t tick) {
        if (!m_timeline) return;
        bool ok = false;
        QString name = QInputDialog::getText(
            this, "Add Marker",
            "Marker name:",
            QLineEdit::Normal, QString(), &ok);
        if (ok) {
            m_timeline->addMarker(tick, name.toStdString(), 0xFF44FF44);
            m_ruler->setMarkers(&m_timeline->markers());
            m_ruler->update();
            emit contentChanged();
        }
    });
    connect(m_scrollBar, &NLEScrollBar::scrollChanged,
            this, &TimelinePanel::onScrollChanged);

    // Keep the scrollbar range in sync when clips are added/moved/removed.
    connect(this, &TimelinePanel::contentChanged, this, [this]() {
        if (m_timeline)
            m_layoutEngine.setTotalDuration(m_timeline->duration());
        onScrollChanged();
    });

    // Sync selection highlights to track widgets whenever selection changes.
    connect(this, &TimelinePanel::selectionChanged, this, [this]() {
        if (!m_timeline) return;
        bool hasClipSelection = !m_selection.empty();
        for (size_t ti = 0; ti < m_trackWidgets.size(); ++ti) {
            std::vector<size_t> selectedIndices;
            Track* trk = m_timeline->track(ti);
            if (trk) {
                for (const auto& ref : m_selection.clips()) {
                    if (ref.trackIndex == ti) {
                        size_t idx = trk->findClipIndexById(ref.clipId);
                        if (idx < trk->clipCount())
                            selectedIndices.push_back(idx);
                    }
                }
            }
            m_trackWidgets[ti]->setSelectedClips(selectedIndices);
            // Clear transition highlight when clips are selected
            if (hasClipSelection)
                m_trackWidgets[ti]->setSelectedTransition(SIZE_MAX);
        }
        if (hasClipSelection) {
            m_selectedTransitionTrack = SIZE_MAX;
            m_selectedTransitionIndex = SIZE_MAX;
        }
    });

    // Ghost track overlay (not in any layout Ã¢â‚¬â€ positioned absolutely via setGeometry)
    m_ghostOverlay = new GhostTrackOverlay(this);

    // Playhead overlay — a transparent widget parented to the scroll viewport
    // that draws only the thin playhead line.  Repositioning this tiny widget
    // is vastly cheaper than repainting every TimelineTrackWidget (which each
    // redraw 100+ clip rectangles) on every tick.
    m_playheadOverlay = new PlayheadLineWidget(m_verticalScroll->viewport());
    m_playheadOverlay->raise();
}

void TimelinePanel::setTimeline(Timeline* timeline)
{
    ++m_waveformLoadGeneration;
    m_pendingWaveformPaths.clear();
    m_failedWaveformPaths.clear();

    // Clear clip selection when switching to a new timeline so stale
    // clip IDs from the previous project don't persist in m_selection.
    m_selection.clear();
    m_timeline = timeline;
    if (timeline)
    {
        // Ensure at least 1 video track and 1 audio track exist (Premiere Pro default)
        ensureDefaultTracks();

        m_layoutEngine.setTotalDuration(timeline->duration());
        // Reset scroll so stale position from a previous timeline doesn't
        // persist if the deferred zoomToFit below bails out.
        m_layoutEngine.setScrollX(0);

        // Pass markers to ruler for rendering
        m_ruler->setMarkers(&timeline->markers());

        // Deferred initial zoom: set a sensible default view showing ~1 minute.
        // Uses zoomToFit capped at 60 seconds so very long timelines don't
        // start extremely zoomed out.  The user can still manually zoom or
        // use View > Zoom to Fit (which calls zoomToFit() uncapped).
        QTimer::singleShot(0, this, [this]() {
            if (m_timeline) {
                double w = m_ruler->width();
                if (w <= 0) w = width() - headerWidth();
                if (w <= 0) w = 800;
                m_layoutEngine.setViewportWidth(w);

                int64_t dur = m_timeline->duration();
                constexpr int64_t kMaxInitialView = 60; // seconds
                int64_t maxTicks = TimelineLayoutEngine::secondsToTicks(kMaxInitialView);
                if (dur <= 0 || dur > maxTicks) dur = maxTicks;
                m_layoutEngine.zoomToFit(0, dur, w);
                onScrollChanged();

                // Scroll to the saved playhead position so the timeline
                // viewport matches where the user left off (e.g. after
                // crash recovery / project re-open).  Without this, the
                // timeline always opens at scroll=0 regardless of the
                // playhead position stored in the project file.
                setPlayheadPosition(m_timeline->playheadPosition());
            }
        });
    }
    rebuildTracks();
}

void TimelinePanel::ensureDefaultTracks()
{
    if (!m_timeline) return;

    // Count existing video and audio tracks
    bool hasVideo = false;
    bool hasAudio = false;
    for (size_t i = 0; i < m_timeline->trackCount(); ++i)
    {
        const Track* t = m_timeline->track(i);
        if (t->type() == TrackType::Video) hasVideo = true;
        if (t->type() == TrackType::Audio) hasAudio = true;
    }

    // Like Premiere Pro: video tracks go FIRST, then audio tracks.
    // Add V1 before any audio tracks, add A1 at the end.
    if (!hasVideo)
    {
        // Insert V1 at position 0 (top) if no video tracks exist yet
        size_t insertIdx = 0;
        auto vTrack = std::make_unique<Track>(TrackType::Video, "V1");
        m_timeline->insertTrack(insertIdx, std::move(vTrack));
    }

    if (!hasAudio)
        m_timeline->addAudioTrack("A1");
}

void TimelinePanel::setPlayheadPosition(int64_t tick)
{
    m_playheadTick = tick;
    m_ruler->setPlayheadTick(tick);

    // Auto-scroll to keep playhead visible
    double newScroll = m_layoutEngine.scrollXToShow(tick);
    bool scrolled = false;
    if (newScroll != m_layoutEngine.scrollX())
    {
        m_layoutEngine.setScrollX(newScroll);
        onScrollChanged();  // already calls tw->update() for all tracks
        scrolled = true;
    }

    // Store the tick in each track widget (for static rendering when paused)
    // but do NOT trigger a full repaint — the overlay widget handles the
    // playhead line at near-zero cost.
    for (auto* tw : m_trackWidgets)
        tw->setPlayheadTickNoRepaint(tick);

    // Reposition the lightweight playhead overlay
    if (!scrolled)
        updatePlayheadOverlay();
}

void TimelinePanel::rebuildTracks()
{
    spdlog::info("[TimelinePanel] ENTER rebuildTracks: m_timeline={} trackCount={}", (void*)m_timeline, m_timeline ? m_timeline->trackCount() : 0);
    spdlog::info("rebuildTracks: disconnecting and deleting old widgets");
    for (size_t i = 0; i < m_trackHeaders.size(); ++i) {
        auto* header = m_trackHeaders[i];
        if (header) {
            spdlog::info("[LIFECYCLE] About to disable/disconnect TrackHeader {} at {} (type={})", (void*)header, i, header ? header->metaObject()->className() : "null");
            header->setEnabled(false);
            bool disconnected = header->disconnect();
            spdlog::info("[LIFECYCLE] TrackHeader {} disconnect() returned {}", (void*)header, disconnected);
        }
    }
    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        auto* widget = m_trackWidgets[i];
        if (widget) {
            spdlog::info("[LIFECYCLE] About to disable/disconnect TrackWidget {} at {} (type={})", (void*)widget, i, widget ? widget->metaObject()->className() : "null");
            widget->setEnabled(false);
            bool disconnected = widget->disconnect();
            spdlog::info("[LIFECYCLE] TrackWidget {} disconnect() returned {}", (void*)widget, disconnected);
        }
    }
    // NOTE: Do NOT call processEvents() here.  During loading, pending
    // paint/signal events could be delivered to disabled/disconnected
    // widgets and trigger re-entrant calls into the partially torn-down
    // timeline, causing use-after-free crashes.
    // Hide old widgets BEFORE scheduling deletion.  deleteLater() defers
    // destruction to the next event-loop iteration, but paint events can
    // still fire in the meantime.  If the Track object's storage was
    // reallocated (e.g. tracks added/removed from the timeline), the old
    // widget's m_track raw pointer is dangling — accessing it in a paint
    // event causes ACCESS_VIOLATION.  Hiding the widget prevents paint.
    for (size_t i = 0; i < m_trackHeaders.size(); ++i) {
        auto* header = m_trackHeaders[i];
        if (header) {
            header->hide();
            spdlog::info("[LIFECYCLE] Hidden TrackHeader {} at {}", (void*)header, i);
        }
    }
    for (size_t i = 0; i < m_trackHeaders.size(); ++i) {
        auto* header = m_trackHeaders[i];
        if (header) {
            header->deleteLater();
            spdlog::info("[LIFECYCLE] Scheduled deleteLater for TrackHeader {} at {}", (void*)header, i);
        }
    }
    m_trackHeaders.clear();
    spdlog::info("[STEP] Finished deleting all TrackHeaders");

    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        auto* widget = m_trackWidgets[i];
        if (widget) {
            widget->hide();
            spdlog::info("[LIFECYCLE] Hidden TrackWidget {} at {}", (void*)widget, i);
        }
    }
    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        auto* widget = m_trackWidgets[i];
        if (widget) {
            widget->deleteLater();
            spdlog::info("[LIFECYCLE] Scheduled deleteLater for TrackWidget {} at {}", (void*)widget, i);
        }
    }
    m_trackWidgets.clear();
    spdlog::info("[STEP] Finished deleting all TrackWidgets");

    spdlog::info("rebuildTracks: creating new widgets for {} tracks", m_timeline ? m_timeline->trackCount() : 0);

    if (!m_timeline) return;

    // Ã¢â€â‚¬Ã¢â€â‚¬ Auto-rename tracks with Premiere Pro-style numbering Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Video tracks: V1 = lowest (bottom of video section), V2 above, etc.
    // Audio tracks: A1 = highest (top of audio section), A2 below, etc.
    {
        // Collect indices for each type
        std::vector<size_t> videoIndices, audioIndices;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            Track* t = m_timeline->track(i);
            if (t->isDivider()) continue; // Dividers have no name
            if (t->type() == TrackType::Video) videoIndices.push_back(i);
            else                                audioIndices.push_back(i);
        }

        // Video: topmost (lowest index) = highest number
        // e.g., 3 video tracks at indices 0,1,2 Ã¢â€ â€™ V3, V2, V1
        // Always auto-rename so that old-style names like "Video 1" are
        // normalised to the short V#/A# scheme used everywhere else.
        for (size_t vi = 0; vi < videoIndices.size(); ++vi) {
            Track* t = m_timeline->track(videoIndices[vi]);
            int num = static_cast<int>(videoIndices.size() - vi);  // bottom-up
            t->setName("V" + std::to_string(num));
        }

        // Audio: topmost = A1, next = A2, etc.
        // Preserve custom track names (e.g. per-character names from AudioSync export).
        for (size_t ai = 0; ai < audioIndices.size(); ++ai) {
            Track* t = m_timeline->track(audioIndices[ai]);
            const std::string& cur = t->name();
            // Only auto-rename if name is empty or already an auto-name (A1, A2, ...)
            bool isAutoName = cur.empty()
                || (cur.size() >= 2 && cur[0] == 'A'
                    && std::all_of(cur.begin() + 1, cur.end(), ::isdigit));
            if (isAutoName) {
                int num = static_cast<int>(ai + 1);
                t->setName("A" + std::to_string(num));
            }
        }
    }

    auto* headerLayout = m_trackHeaderArea->layout();
    auto* trackLayout  = m_trackContentArea->layout();

    // Remove old items from layouts (headers, track widgets, stretches)
    while (headerLayout->count() > 0)
        headerLayout->takeAt(0);
    while (trackLayout->count() > 0)
        trackLayout->takeAt(0);
    spdlog::info("[STEP] Cleared layouts");

    // Vertically center tracks in both scroll areas (Premiere Pro style).
    // Both scroll areas are the same height (right column has a 24px spacer
    // matching the Add Track button) so stretches expand identically.
    static_cast<QVBoxLayout*>(headerLayout)->addStretch();
    static_cast<QVBoxLayout*>(trackLayout)->addStretch();
    spdlog::info("[STEP] Added stretches to layouts");

    // Determine the current "standard" track height so dividers can size
    // themselves as 1/4 of whatever the user's real tracks are right now.
    // Use the first non-divider track as reference; fall back to 80.
    float refTrackHeight = 80.0f;
    for (size_t ri = 0; ri < m_timeline->trackCount(); ++ri) {
        Track* t = m_timeline->track(ri);
        if (!t || t->isDivider()) continue;
        float rh = t->height();
        if (rh >= 1.0f) { refTrackHeight = rh; break; }
    }
    const float dividerHeight = std::max(8.0f, refTrackHeight * 0.25f);

    spdlog::info("[STEP] About to create headers and widgets for each track");
    for (size_t i = 0; i < m_timeline->trackCount(); ++i)
    {
        Track* track = m_timeline->track(i);
        float h = track->height();
        if (track->isDivider()) {
            h = dividerHeight;
            track->setHeight(h);
        } else if (h < 1.0f) {
            h = 80.0f;
            track->setHeight(h);
        }

        // Header
        spdlog::info("[LIFECYCLE] About to create TrackHeader for track {} (type={})", i, (track ? (int)track->type() : -1));
        auto* header = new TrackHeader(m_trackHeaderArea);
        spdlog::info("[LIFECYCLE] Created TrackHeader {} for track {} (type={})", (void*)header, i, (track ? (int)track->type() : -1));
        spdlog::info("[STEP] TrackHeader created and added to layout for track {}", i);
        header->setTrack(track, i);
        header->setHeight(h);
        header->setFixedWidth(m_headerScroll->width());
        header->setMouseTracking(true);  // Enable hover cursor changes
        headerLayout->addWidget(header);
        m_trackHeaders.push_back(header);

        // Track content
        spdlog::info("[LIFECYCLE] About to create TimelineTrackWidget for track {} (type={})", i, (track ? (int)track->type() : -1));
        auto* tw = new TimelineTrackWidget(m_trackContentArea);
        spdlog::info("[LIFECYCLE] Created TimelineTrackWidget {} for track {} (type={})", (void*)tw, i, (track ? (int)track->type() : -1));
        spdlog::info("[STEP] TimelineTrackWidget created and added to layout for track {}", i);
        tw->setLayoutEngine(&m_layoutEngine);
        tw->setTrack(track, i);
        tw->setFixedHeight(static_cast<int>(h));
        trackLayout->addWidget(tw);
        m_trackWidgets.push_back(tw);

        // Connect track height resize from header drag
        QObject::connect(header, &TrackHeader::heightChanged,
            this, [this](size_t trackIdx, float newHeight) {
            spdlog::debug("[SIGNAL] TrackHeader {} heightChanged for track {} newHeight={}", (void*)sender(), trackIdx, newHeight);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            Track* tk = m_timeline->track(trackIdx);
            tk->setHeight(newHeight);
            // Also resize the corresponding track widget
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(newHeight));
            // If a real (non-divider) track was resized, propagate the new
            // 1/4-height to every divider so they stay proportional.
            if (!tk->isDivider()) {
                float dh = std::max(8.0f, newHeight * 0.25f);
                for (size_t di = 0; di < m_timeline->trackCount(); ++di) {
                    Track* dt = m_timeline->track(di);
                    if (!dt || !dt->isDivider()) continue;
                    dt->setHeight(dh);
                    if (di < m_trackHeaders.size() && m_trackHeaders[di])
                        m_trackHeaders[di]->setHeight(dh);
                    if (di < m_trackWidgets.size() && m_trackWidgets[di])
                        m_trackWidgets[di]->setFixedHeight(static_cast<int>(dh));
                }
            }
            updateMinHeaderWidth();
        });

        // Connect track targeting toggle
        QObject::connect(header, &TrackHeader::targetToggled,
            this, [this](size_t trackIdx, bool targeted) {
            spdlog::debug("[SIGNAL] TrackHeader {} targetToggled for track {} targeted={}", (void*)sender(), trackIdx, targeted);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setTargeted(targeted);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
        });

        // Connect lock/mute/solo/syncLock toggles
        QObject::connect(header, &TrackHeader::lockToggled,
            this, [this](size_t trackIdx, bool locked) {
            spdlog::debug("[SIGNAL] TrackHeader {} lockToggled for track {} locked={}", (void*)sender(), trackIdx, locked);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setLocked(locked);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->update();
        });
        QObject::connect(header, &TrackHeader::muteToggled,
            this, [this](size_t trackIdx, bool muted) {
            spdlog::debug("[SIGNAL] TrackHeader {} muteToggled for track {} muted={}", (void*)sender(), trackIdx, muted);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setMuted(muted);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            emit contentChanged();
        });
        QObject::connect(header, &TrackHeader::soloToggled,
            this, [this](size_t trackIdx, bool soloed) {
            spdlog::debug("[SIGNAL] TrackHeader {} soloToggled for track {} soloed={}", (void*)sender(), trackIdx, soloed);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setSoloed(soloed);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            emit contentChanged();
        });
        QObject::connect(header, &TrackHeader::syncLockToggled,
            this, [this](size_t trackIdx, bool syncLocked) {
            spdlog::debug("[SIGNAL] TrackHeader {} syncLockToggled for track {} syncLocked={}", (void*)sender(), trackIdx, syncLocked);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setSyncLocked(syncLocked);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->update();
        });

        // Connect track collapse/expand toggle
        QObject::connect(header, &TrackHeader::collapseToggled,
            this, [this](size_t trackIdx, bool collapsed) {
            spdlog::debug("[SIGNAL] TrackHeader {} collapseToggled for track {} collapsed={}", (void*)sender(), trackIdx, collapsed);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            static constexpr float kCollapsedHeight = 20.0f;
            Track* t = m_timeline->track(trackIdx);
            t->setCollapsed(collapsed);
            float newH = collapsed ? kCollapsedHeight : 60.0f;
            t->setHeight(newH);
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->setHeight(newH);
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(newH));
            updateMinHeaderWidth();
        });

        // Connect track management signals from header context menu
        QObject::connect(header, &TrackHeader::addTrackRequested,
            this, [this](bool video, bool above, size_t nearIndex) {
            spdlog::debug("[SIGNAL] TrackHeader {} addTrackRequested video={} above={} nearIndex={}", (void*)sender(), video, above, nearIndex);
            if (above)
                emit addTrackAbove(nearIndex, video);
            else
                emit addTrackBelow(nearIndex, video);
        });
        QObject::connect(header, &TrackHeader::deleteTrackRequested,
                this, [this](size_t trackIdx) {
            spdlog::debug("[SIGNAL] TrackHeader {} deleteTrackRequested for track {} (deferred)", (void*)sender(), trackIdx);
            QPointer<TimelinePanel> safeThis(this);
            QTimer::singleShot(0, this, [safeThis, trackIdx]() {
                if (!safeThis) {
                    spdlog::error("[DEFENSIVE] TimelinePanel was deleted before deferred deleteTrack could run!");
                    return;
                }
                spdlog::debug("[DEFENSIVE] Emitting deleteTrack for track {} after deferral", trackIdx);
                emit safeThis->deleteTrack(trackIdx);
            });
        });

        // Insert a divider track relative to this header's track.
        // Defer so we don't delete the TrackHeader that's still running
        // its contextMenu handler on the stack (use-after-free crash).
        QObject::connect(header, &TrackHeader::addDividerRequested,
                this, [this](bool above, size_t nearIdx) {
            spdlog::debug("[SIGNAL] TrackHeader {} addDividerRequested above={} nearIdx={}", (void*)sender(), above, nearIdx);
            if (!m_timeline) return;
            size_t insertAt = above ? nearIdx : nearIdx + 1;
            if (insertAt > m_timeline->trackCount())
                insertAt = m_timeline->trackCount();
            QPointer<TimelinePanel> safeThis(this);
            QTimer::singleShot(0, this, [safeThis, insertAt]() {
                if (!safeThis || !safeThis->m_timeline) {
                    spdlog::error("[DEFENSIVE] TimelinePanel or m_timeline was deleted before deferred addDivider could run!");
                    return;
                }
                spdlog::debug("[DEFENSIVE] Adding divider track at {} after deferral", insertAt);
                safeThis->m_timeline->addDividerTrack(insertAt);
                safeThis->rebuildTracks();
            });
        });

        // Drag-reorder signal wiring
        QObject::connect(header, &TrackHeader::reorderDragStarted,
            this, [this](size_t srcIdx) {
            spdlog::debug("[SIGNAL] TrackHeader {} reorderDragStarted srcIdx={}", (void*)sender(), srcIdx);
            m_reorderSrcIndex = srcIdx;
            if (m_ghostOverlay) {
                m_ghostOverlay->reorderMode = true;
                m_ghostOverlay->raise();
                m_ghostOverlay->show();
            }
        });
        QObject::connect(header, &TrackHeader::reorderDragMoved,
            this, [this](size_t /*srcIdx*/, const QPoint& gp) {
            spdlog::debug("[SIGNAL] TrackHeader {} reorderDragMoved", (void*)sender());
            updateReorderOverlay(gp);
        });
        QObject::connect(header, &TrackHeader::reorderDragFinished,
            this, [this](size_t srcIdx, const QPoint& gp, bool commit) {
            spdlog::debug("[SIGNAL] TrackHeader {} reorderDragFinished srcIdx={} commit={}", (void*)sender(), srcIdx, commit);
            size_t dst = computeReorderInsertionIndex(gp);
            if (m_ghostOverlay) {
                m_ghostOverlay->reorderMode = false;
                m_ghostOverlay->hide();
            }
            m_reorderSrcIndex = SIZE_MAX;
            if (!commit || !m_timeline) return;
            if (dst > srcIdx) --dst; // compensate for removal shift
            if (dst == srcIdx) return;
            if (srcIdx >= m_timeline->trackCount()) return;
            if (dst >= m_timeline->trackCount())
                dst = m_timeline->trackCount() - 1;
            // Defer past the mouseReleaseEvent so we don't delete the
            // TrackHeader that is currently on the stack.
            QTimer::singleShot(0, this, [this, srcIdx, dst]() {
                if (!m_timeline) return;
                m_timeline->moveTrack(srcIdx, dst);
                rebuildTracks();
            });
        });
        QObject::connect(header, &TrackHeader::trackSizePresetRequested,
            this, [this](size_t trackIdx, float height) {
            spdlog::debug("[SIGNAL] TrackHeader {} trackSizePresetRequested trackIdx={} height={}", (void*)sender(), trackIdx, height);
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            m_timeline->track(trackIdx)->setHeight(height);
            if (trackIdx < m_trackWidgets.size())
                m_trackWidgets[trackIdx]->setFixedHeight(static_cast<int>(height));
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->setHeight(height);
            updateMinHeaderWidth();
        });

        // Connect track rename (undoable via command stack)
        QObject::connect(header, &TrackHeader::trackRenamed,
            this, [this](size_t trackIdx, const QString& newName) {
            spdlog::debug("[SIGNAL] TrackHeader {} trackRenamed trackIdx={} newName={}", (void*)sender(), trackIdx, newName.toStdString());
            if (!m_timeline || trackIdx >= m_timeline->trackCount()) return;
            Track* track = m_timeline->track(trackIdx);
            auto cmd = std::make_unique<SetTrackPropertyCommand<std::string>>(
                track, newName.toStdString(),
                +[](const Track& t) -> std::string { return t.name(); },
                +[](Track& t, std::string v) { t.setName(v); },
                "Rename Track",
                static_cast<int>(CommandTypeId::SetTrackName));
            if (m_commandStack) {
                m_commandStack->execute(std::move(cmd));
            } else {
                cmd->execute();
            }
            if (trackIdx < m_trackHeaders.size())
                m_trackHeaders[trackIdx]->update();
            updateMinHeaderWidth();
        });

        // Connect clip selection
        QObject::connect(tw, &TimelineTrackWidget::clipClicked,
            this, [this](size_t trackIndex, size_t clipIndex, bool shiftHeld) {
            spdlog::debug("[SIGNAL] TimelineTrackWidget {} clipClicked trackIndex={} clipIndex={} shiftHeld={}", (void*)sender(), trackIndex, clipIndex, shiftHeld);
            // Update SelectionSet (this was previously just bounced to
            // clipSelected signal without updating m_selection)
            if (!m_timeline) return;
            Track* trk = m_timeline->track(trackIndex);
            if (!trk || clipIndex >= trk->clipCount()) return;
            const Clip* clip = trk->clip(clipIndex);
            if (!clip) return;

            ClipRef ref{trackIndex, clip->id()};
            if (!shiftHeld && !m_selection.isSelected(ref))
                m_selection.clear();
            m_selection.selectClip(ref, shiftHeld);

            // Linked A/V selection: if the clip has a groupId,
            // also select all clips with the same groupId across all tracks.
            uint64_t gid = clip->groupId();
            if (gid != 0) {
                for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                    Track* t = m_timeline->track(ti);
                    for (size_t ci = 0; ci < t->clipCount(); ++ci) {
                        const Clip* c = t->clip(ci);
                        if (c->groupId() == gid && c->id() != clip->id()) {
                            m_selection.selectClip({ti, c->id()}, true);
                            break; // one match per track for linked A/V
                        }
                    }
                }
            }

            emit selectionChanged();
            emit clipSelected(trackIndex, clipIndex);
            update();
        });

        // Install event filter so TimelinePanel gets mouse events for
        // drag/move/trim/marquee even though TimelineTrackWidget is the
        // deepest child that Qt delivers events to.
        tw->installEventFilter(this);
    }

    // Bottom stretch to balance vertical centering.
    static_cast<QVBoxLayout*>(headerLayout)->addStretch();
    static_cast<QVBoxLayout*>(trackLayout)->addStretch();

    spdlog::info("rebuildTracks: created {} headers, {} widgets for {} tracks",
                 m_trackHeaders.size(), m_trackWidgets.size(),
                 m_timeline->trackCount());

    // Diagnostic: log each widget's state
    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        auto* tw = m_trackWidgets[i];
        auto* track = m_timeline->track(i);
        spdlog::info("  widget[{}]: track='{}' size={}x{} visible={} "
                     "fixedH={} clips={}",
                     i, track->name(),
                     tw->width(), tw->height(),
                     tw->isVisible(),
                     tw->minimumHeight(),
                     track->clipCount());
    }

    // Scroll so the video/audio boundary is vertically centered.
    // This ensures all video tracks and the top audio track are visible.
    QTimer::singleShot(0, this, [this]() {
        if (!m_verticalScroll || !m_timeline) return;
        // Find the pixel position of the first audio track (= bottom of video section)
        int videoBottom = 0;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            if (m_timeline->track(i)->type() == TrackType::Audio) break;
            videoBottom += static_cast<int>(m_timeline->track(i)->height());
        }
        // Account for the top stretch: in a centered layout the stretch
        // pushes content down. After layout, widget positions are final.
        // Scroll so that the video/audio boundary sits at ~40% from top.
        int viewH = m_verticalScroll->viewport()->height();
        int contentH = m_trackContentArea->sizeHint().height();
        if (contentH > viewH) {
            // Content overflows — scroll so video/audio boundary is centered
            int target = videoBottom - viewH * 2 / 5;
            if (target < 0) target = 0;
            m_verticalScroll->verticalScrollBar()->setValue(target);
        }
    });

    // Load waveform peaks for audio clips and pass to track widgets
    loadWaveforms();
    for (auto* tw : m_trackWidgets)
        tw->setWaveformCache(&m_waveformPeaks);

    // Load video thumbnails and pass to track widgets
    loadThumbnails();
    for (auto* tw : m_trackWidgets)
        tw->setThumbnailCache(&m_thumbnailCache);

    // Pass animation video cache to track widgets
    for (auto* tw : m_trackWidgets)
        tw->setAnimVideoCache(m_animVideoCache);

    // Push in/out point overlays to track widgets
    if (m_timeline) {
        int64_t inPt  = m_timeline->inPoint();
        int64_t outPt = m_timeline->outPoint();
        for (auto* tw : m_trackWidgets)
            tw->setInOutPoints(inPt, outPt);
    }

    // Re-apply visual selection to the newly created track widgets.
    // m_selection persists across rebuilds but the widget highlight state
    // is lost when old widgets are deleted.
    emit selectionChanged();

    // Update minimum header width based on current track names.
    // Run twice: once now (before first paint) and once deferred (after Qt's
    // layout pass settles widget widths). This ensures newly created track
    // headers reposition their label/buttons based on actual name length and
    // height — matching the behavior existing tracks get on resize.
    updateMinHeaderWidth();
    QTimer::singleShot(0, this, [this]() {
        updateMinHeaderWidth();
        for (auto* hdr : m_trackHeaders)
            if (hdr) hdr->update();
    });
}

// ═══════════════════════════════════════════════════════════════════════════
//  Drag-reorder helpers: phantom insertion line + insertion-index mapping
// ═══════════════════════════════════════════════════════════════════════════

size_t TimelinePanel::computeReorderInsertionIndex(const QPoint& globalMousePos) const
{
    // Map global Y to the track-header column in local coordinates, then find
    // which track-gap the cursor is closest to. Returns index in [0..count].
    if (m_trackHeaders.empty()) return 0;
    if (!m_trackHeaderArea) return 0;

    const QPoint localInArea = m_trackHeaderArea->mapFromGlobal(globalMousePos);
    const int y = localInArea.y();

    // Headers are laid out top-to-bottom inside m_trackHeaderArea with a
    // leading stretch.  Find the closest gap (before/after each header).
    int bestIdx = 0;
    int bestDist = std::numeric_limits<int>::max();
    for (size_t i = 0; i < m_trackHeaders.size(); ++i) {
        auto* hdr = m_trackHeaders[i];
        if (!hdr) continue;
        int top    = hdr->y();
        int bottom = hdr->y() + hdr->height();
        int mid    = (top + bottom) / 2;
        // Candidate "above this track" (insertion index = i)
        int dTop = std::abs(y - top);
        if (dTop < bestDist) { bestDist = dTop; bestIdx = static_cast<int>(i); }
        // Candidate "below this track" (insertion index = i + 1)
        int dBot = std::abs(y - bottom);
        if (dBot < bestDist) { bestDist = dBot; bestIdx = static_cast<int>(i) + 1; }
        // If cursor is strictly inside the upper half, treat as "above"
        if (y >= top && y <= mid) {
            bestIdx = static_cast<int>(i);
            break;
        }
        if (y > mid && y <= bottom) {
            bestIdx = static_cast<int>(i) + 1;
            break;
        }
    }
    if (bestIdx < 0) bestIdx = 0;
    const int maxIdx = static_cast<int>(m_trackHeaders.size());
    if (bestIdx > maxIdx) bestIdx = maxIdx;
    return static_cast<size_t>(bestIdx);
}

void TimelinePanel::updateReorderOverlay(const QPoint& globalMousePos)
{
    if (!m_ghostOverlay) return;
    size_t insertIdx = computeReorderInsertionIndex(globalMousePos);

    // Compute Y of the insertion line in TimelinePanel coords, spanning the
    // full width of the panel so it's visible over both header and tracks.
    int lineY = 0;
    if (m_trackHeaders.empty() || !m_trackHeaderArea) {
        lineY = 0;
    } else if (insertIdx == 0) {
        auto* first = m_trackHeaders.front();
        QPoint tl = first->mapTo(this, QPoint(0, 0));
        lineY = tl.y();
    } else if (insertIdx >= m_trackHeaders.size()) {
        auto* last = m_trackHeaders.back();
        QPoint tl = last->mapTo(this, QPoint(0, last->height()));
        lineY = tl.y();
    } else {
        auto* hdr = m_trackHeaders[insertIdx];
        QPoint tl = hdr->mapTo(this, QPoint(0, 0));
        lineY = tl.y();
    }

    constexpr int kBarH = 3;
    m_ghostOverlay->setGeometry(0, lineY - kBarH / 2, width(), kBarH);
    m_ghostOverlay->reorderMode = true;
    m_ghostOverlay->raise();
    m_ghostOverlay->show();
    m_ghostOverlay->update();
}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Lightweight content refresh Ã¢â‚¬â€ updates caches + repaints, NO widget rebuild
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
void TimelinePanel::refreshTrackContents()
{
    if (!m_timeline) return;

    // Incrementally update waveform/thumbnail caches for any new clip IDs.
    // With the path-based secondary caches this is essentially free for
    // splits (same source file Ã¢â€ â€™ instant copy from m_waveformByPath /
    // m_thumbnailByPath).
    loadWaveforms();
    loadThumbnails();

    // Ensure all track widgets point to the latest caches
    // (pointers are stable but set them on any new widgets).
    for (auto* tw : m_trackWidgets) {
        tw->setWaveformCache(&m_waveformPeaks);
        tw->setThumbnailCache(&m_thumbnailCache);
        tw->setAnimVideoCache(m_animVideoCache);
    }

    // Repaint all track widgets so the new clip geometry is visible.
    onScrollChanged();
}

void TimelinePanel::clearGapSelection()
{
    m_gapSelection.active = false;
    for (auto* tw : m_trackWidgets)
        tw->setGapHighlight(-1, -1);
}

void TimelinePanel::notifyZoomChanged()
{
    onScrollChanged();
}

void TimelinePanel::zoomToFit()
{
    if (!m_timeline) return;
    int64_t dur = m_timeline->duration();
    if (dur <= 0) dur = TimelineLayoutEngine::secondsToTicks(60.0);
    m_layoutEngine.zoomToFit(0, dur, m_ruler->width());
    onScrollChanged();
}

void TimelinePanel::zoomIn()
{
    double playheadPx = m_layoutEngine.timeToPixelX(m_playheadTick);
    m_layoutEngine.zoomAt(playheadPx, 2.0);
    // Center on playhead: adjust scroll so playhead moves to viewport center
    double viewW = std::max(m_ruler->width(), 100);
    double centerPx = viewW / 2.0;
    double newScroll = m_layoutEngine.scrollX() + (playheadPx - centerPx);
    m_layoutEngine.setScrollX(std::max(newScroll, 0.0));
    onScrollChanged();
}

void TimelinePanel::zoomOut()
{
    double playheadPx = m_layoutEngine.timeToPixelX(m_playheadTick);
    m_layoutEngine.zoomAt(playheadPx, 0.5);
    double viewW = std::max(m_ruler->width(), 100);
    double centerPx = viewW / 2.0;
    double newScroll = m_layoutEngine.scrollX() + (playheadPx - centerPx);
    m_layoutEngine.setScrollX(std::max(newScroll, 0.0));
    onScrollChanged();
}

void TimelinePanel::setFrameRate(double fps)
{
    m_layoutEngine.setFrameRate(fps);
    m_ruler->update();
}

int TimelinePanel::headerWidth() const
{
    // Include the splitter handle width so that converting from panel
    // coordinates to viewport-relative coordinates is correct.
    // Without this, all mouse-hit-testing is off by 3px (the handle).
    constexpr int kHandleW = 3;
    return (m_headerScroll ? m_headerScroll->width() : TrackHeader::kHeaderWidth) + kHandleW;
}

void TimelinePanel::updateMinHeaderWidth()
{
    if (!m_timeline || !m_headerScroll) return;

    // Layout constants from TrackHeader::buttonRect / paintEvent
    constexpr int leftPad    = 14;   // label starts at x=14 (past target indicator)
    constexpr int rightPad   = 4;    // right edge padding
    constexpr int bw         = 26;   // button width
    constexpr int gapX       = 3;    // gap between buttons
    constexpr int numButtons = 4;
    constexpr int labelH     = 22;
    constexpr int bh         = 20;
    constexpr int shortThreshold = labelH + bh + 4;  // 46

    // Total button row width (all 4 inline)
    constexpr int totalBtnW  = numButtons * bw + (numButtons - 1) * gapX; // 113

    QFont labelFont("Segoe UI", 10, QFont::Bold);
    QFontMetrics fm(labelFont);

    int minW = TrackHeader::kHeaderWidth; // absolute floor

    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        const Track* track = m_timeline->track(i);
        int nameW = fm.horizontalAdvance(QString::fromStdString(track->name()));
        int trackH = static_cast<int>(track->height());

        int needed;
        if (trackH < shortThreshold) {
            // Short: name and buttons are on the same row
            // Layout: [14px pad][name][2px gap][buttons (113px)][4px pad]
            needed = leftPad + nameW + 2 + totalBtnW + rightPad;
        } else {
            // Tall: name is above buttons, use the wider of the two
            int nameRow = leftPad + nameW + rightPad;
            int btnRow  = leftPad + totalBtnW + rightPad;
            needed = std::max(nameRow, btnRow);
        }
        if (needed > minW)
            minW = needed;
    }

    // Capture the width BEFORE applying the new minimum, because
    // setMinimumWidth() can cause the QSplitter to relayout immediately,
    // which would make the subsequent width() check always pass.
    int prevW = m_headerScroll->width();

    m_headerScroll->setMinimumWidth(minW);

    // If the header needs to grow, explicitly set splitter sizes
    if (prevW < minW && m_headerSplitter) {
        int total = m_headerSplitter->width();
        int hw    = m_headerSplitter->handleWidth();
        m_headerSplitter->setSizes({minW, std::max(1, total - minW - hw)});
        m_trackHeaderArea->setFixedWidth(minW);
        for (auto* hdr : m_trackHeaders)
            hdr->setFixedWidth(minW);
    }

    // Always sync spacers with the actual header width so the ruler and
    // scrollbar stay aligned with the track content viewport.
    if (m_headerSplitter) {
        int actualW = m_headerScroll->width();
        int handle  = m_headerSplitter->handleWidth();
        m_headerSpacer->setFixedWidth(actualW + handle);
        m_scrollSpacer->setFixedWidth(actualW + handle);
        // Defer scroll/playhead update until Qt finishes the layout pass.
        QTimer::singleShot(0, this, [this]() { onScrollChanged(); });
    }
}

void TimelinePanel::setAnimVideoCache(const AnimationVideoCache* cache)
{
    m_animVideoCache = cache;
    for (auto* tw : m_trackWidgets)
        tw->setAnimVideoCache(cache);
}

void TimelinePanel::resizeEvent(QResizeEvent* event)
{
    // Guard against resize → layout → paint → resize recursion during
    // initial dock arrangement.  If the QSplitter triggers another resize
    // while we're already handling one, skip it to avoid infinite loops.
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;

    QWidget::resizeEvent(event);
    if (!m_splitterInitialized && m_headerSplitter && m_headerSplitter->width() > 0) {
        m_splitterInitialized = true;
        // Header was locked at kHeaderWidth via setFixedWidth. Now unlock for
        // user-draggable resizing, and pin the splitter at the correct position.
        int headerW = m_headerScroll->width();
        int total = m_headerSplitter->width();
        int hw = m_headerSplitter->handleWidth();
        m_headerScroll->setMinimumWidth(TrackHeader::kHeaderWidth);
        m_headerScroll->setMaximumWidth(QWIDGETSIZE_MAX);
        m_headerSplitter->setSizes({headerW, std::max(1, total - headerW - hw)});
        // Apply dynamic minimum based on track names
        updateMinHeaderWidth();
    }
    m_layoutEngine.setViewportWidth(m_ruler->width());
    m_scrollBar->update();
    m_ruler->update();

    s_inResize = false;
}

void TimelinePanel::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier)
    {
        // Ctrl+wheel = zoom centered on cursor, then pan to center
        double factor = (event->angleDelta().y() > 0) ? 1.15 : 1.0 / 1.15;
        double px = event->position().x() - headerWidth();
        m_layoutEngine.zoomAt(px, factor);
        double centerPx = std::max(m_ruler->width(), 100) / 2.0;
        double newScroll = m_layoutEngine.scrollX() + (px - centerPx);
        m_layoutEngine.setScrollX(std::max(newScroll, 0.0));
        onScrollChanged();
        event->accept();
    }
    else if (event->modifiers() & Qt::ShiftModifier)
    {
        // Shift+wheel = resize all track heights (Premiere Pro style)
        if (!m_timeline) { event->accept(); return; }
        float delta = (event->angleDelta().y() > 0) ? 10.0f : -10.0f;
        constexpr float kMinHeight = 30.0f;
        constexpr float kMaxHeight = 300.0f;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            Track* t = m_timeline->track(i);
            float h = std::clamp(t->height() + delta, kMinHeight, kMaxHeight);
            t->setHeight(h);
        }
        rebuildTracks();
        event->accept();
    }
    else
    {
        // Regular wheel = horizontal scroll (Premiere Pro style).
        // But if the track content is taller than the viewport (more tracks
        // than fit on screen), allow vertical scrolling too.
        bool needsVerticalScroll = false;
        if (m_verticalScroll && m_verticalScroll->viewport()) {
            int contentH = m_trackContentArea ? m_trackContentArea->sizeHint().height() : 0;
            int viewportH = m_verticalScroll->viewport()->height();
            needsVerticalScroll = (contentH > viewportH);
        }

        if (needsVerticalScroll) {
            // Forward to the vertical scroll area so the user can see
            // tracks that don't fit in the viewport.
            if (m_verticalScroll && m_verticalScroll->verticalScrollBar()) {
                auto* vsb = m_verticalScroll->verticalScrollBar();
                int delta = (event->angleDelta().y() > 0) ? -40 : 40;
                vsb->setValue(vsb->value() + delta);
            }
        } else {
            // All tracks fit — wheel does horizontal scroll
            double delta = (event->angleDelta().y() > 0) ? -50.0 : 50.0;
            m_layoutEngine.setScrollX(m_layoutEngine.scrollX() + delta);
            onScrollChanged();
        }
        event->accept();
    }
}

void TimelinePanel::onRulerScrub(int64_t tick)
{
    m_playheadTick = tick;
    emit playheadMoved(tick);
    // Store tick in track widgets (for static rendering) without repainting.
    // The playhead overlay widget handles the moving line.
    for (auto* tw : m_trackWidgets)
        tw->setPlayheadTickNoRepaint(tick);
    updatePlayheadOverlay();
}

void TimelinePanel::onScrollChanged()
{
    auto scrollT0 = std::chrono::steady_clock::now();

    // Keep total duration in sync so the scrollbar range stays accurate.
    if (m_timeline)
        m_layoutEngine.setTotalDuration(m_timeline->duration());

    m_ruler->update();
    m_scrollBar->update();
    for (auto* tw : m_trackWidgets)
        tw->update();

    double scrollMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - scrollT0).count();
    if (scrollMs > 4.0) {
        spdlog::info("[PERF] onScrollChanged: {:.1f}ms  tracks={}", scrollMs, m_trackWidgets.size());
    }

    // Keep the playhead overlay position in sync after scroll changes.
    updatePlayheadOverlay();
}

void TimelinePanel::updatePlayheadOverlay()
{
    if (!m_playheadOverlay || !m_verticalScroll->viewport()) return;

    // Convert playhead tick to pixel X in the scroll viewport
    double px = m_layoutEngine.timeToPixelX(m_playheadTick);
    int x = static_cast<int>(std::round(px)) - 1; // center the 3px-wide widget
    int vpH = m_verticalScroll->viewport()->height();

    if (x < -3 || x > m_verticalScroll->viewport()->width() + 3) {
        m_playheadOverlay->setVisible(false);
    } else {
        m_playheadOverlay->setGeometry(x, 0, 3, vpH);
        m_playheadOverlay->setVisible(true);
        m_playheadOverlay->raise();
    }
}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Paint event Ã¢â‚¬â€ draws marquee selection box and in/out overlays on top
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void TimelinePanel::paintEvent(QPaintEvent* event)
{
    // ── Paint recursion guard ─────────────────────────────────────────
    // Detect paint → layout → repaint loops caused by QSplitter + dock
    // widget interactions during startup.  If we re-enter paintEvent
    // more than 5 times without returning, bail out.
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        return;
    }

    QWidget::paintEvent(event);

    // Ghost track overlay is now handled by GhostTrackOverlay widget (m_ghostOverlay)
    // which sits on top of the scroll area via raise().

    // In/out point overlays are drawn by each TimelineTrackWidget::paintEvent
    // (so they render ON TOP of clip content, not underneath child widgets).
    // Marquee selection is handled by QRubberBand overlay (not painted here).
    // Effect drag highlight is also rendered by TimelineTrackWidget (z-order).

    --s_paintDepth;
}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Step 13: Editing
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void TimelinePanel::setCommandStack(CommandStack* stack)
{
    m_commandStack = stack;
}

void TimelinePanel::setShortcutManager(ShortcutManager* shortcuts)
{
    m_shortcuts = shortcuts;
    if (m_shortcuts)
        wireShortcuts();
}

void TimelinePanel::setActiveTool(EditTool tool)
{
    m_activeTool = tool;
    updateCursorForTool();

    // Clear razor hover line when switching away from Razor
    if (tool != EditTool::Razor) {
        for (auto* tw : m_trackWidgets)
            tw->setRazorTick(-1);
    }

    emit toolChanged(tool);
}

void TimelinePanel::setSnappingEnabled(bool enabled)
{
    m_snapEngine.setEnabled(enabled);
}

void TimelinePanel::setCaptionTrackVisible(bool visible)
{
    if (!m_timeline) return;
    // Toggle visibility on any track widget whose track name contains "Caption"
    for (auto* tw : m_trackWidgets) {
        size_t idx = tw->trackIndex();
        if (idx < m_timeline->trackCount()) {
            const auto* trk = m_timeline->track(idx);
            if (trk && trk->name().find("Caption") != std::string::npos)
                tw->setVisible(visible);
        }
    }
    update();
}

void TimelinePanel::updateInOutRange()
{
    if (!m_timeline || !m_ruler) return;
    int64_t inPt  = m_timeline->inPoint();
    int64_t outPt = m_timeline->outPoint();
    if (inPt >= 0 || outPt >= 0) {
        m_ruler->setInOutRange(inPt, outPt);
    } else {
        m_ruler->clearInOutRange();
    }
    m_ruler->update();

    // Push in/out points to every track widget so they draw overlays
    for (auto* tw : m_trackWidgets)
        tw->setInOutPoints(inPt, outPt);
}

void TimelinePanel::executeCommand(std::unique_ptr<Command> cmd)
{
    if (cmd && m_commandStack) {
        m_commandStack->execute(std::move(cmd));
        // Refresh all track widgets to reflect the change immediately
        onScrollChanged();
        // Notify workspace so ProgramMonitor re-composites
        emit contentChanged();
    }
}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Event filter Ã¢â‚¬â€ intercepts mouse events on TimelineTrackWidgets so we can
//  handle drag-move, trim, and marquee selection centrally.
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

bool TimelinePanel::eventFilter(QObject* watched, QEvent* event)
{
    // Forward wheel events from the vertical scroll area or any child
    // back to TimelinePanel so Ctrl+wheel zoom, Shift+wheel track resize,
    // and regular-wheel horizontal scroll always work regardless of which
    // child widget the cursor is hovering over.
    if (event->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(event);
        wheelEvent(we);
        return true;  // consumed Ã¢â‚¬â€ don't let QScrollArea scroll vertically
    }

    // Right-click context menu on empty space in the track header area
    if (event->type() == QEvent::ContextMenu && watched == m_trackHeaderArea) {
        auto* ce = static_cast<QContextMenuEvent*>(event);
        QMenu menu(this);
        QAction* addVideo = menu.addAction(QStringLiteral("Add Video Track"));
        QAction* addAudio = menu.addAction(QStringLiteral("Add Audio Track"));
        QAction* chosen = menu.exec(ce->globalPos());
        if (chosen == addVideo)
            emit addTrackAbove(0, true);
        else if (chosen == addAudio)
            emit addTrackBelow(m_timeline ? m_timeline->trackCount() : 0, false);
        event->accept();
        return true;
    }

    // Intercept mouse events on track widgets AND the track content area /
    // scroll-area viewport so that drag-select (marquee) works even when
    // the drag starts in empty space outside any track widget.
    auto* tw = qobject_cast<TimelineTrackWidget*>(watched);
    bool isTrackChild = (tw != nullptr);
    bool isTrackArea = (watched == m_trackContentArea
                     || watched == m_verticalScroll
                     || watched == m_verticalScroll->viewport());
    if (!isTrackChild && !isTrackArea)
        return QWidget::eventFilter(watched, event);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Right-click context menu on clips Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (event->type() == QEvent::ContextMenu && isTrackChild) {
        auto* ce = static_cast<QContextMenuEvent*>(event);
        QPointF panelPos = tw->mapTo(this, ce->pos());
        auto hitRef = hitTestClip(panelPos);
        if (hitRef) {
            showClipContextMenu(ce->globalPos(), *hitRef);
        } else {
            size_t ti = hitTestTrack(panelPos.y());
            showEmptyAreaContextMenu(ce->globalPos(), ti);
        }
        event->accept();
        return true;
    }

    // Right-click context menu on empty area below/between tracks
    if (event->type() == QEvent::ContextMenu && isTrackArea) {
        auto* ce = static_cast<QContextMenuEvent*>(event);
        size_t lastTrack = m_timeline ? m_timeline->trackCount() : 0;
        showEmptyAreaContextMenu(ce->globalPos(), lastTrack > 0 ? lastTrack - 1 : 0);
        event->accept();
        return true;
    }

    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseButtonDblClick)
    {
        auto* me = static_cast<QMouseEvent*>(event);

        // Map the position from the child widget's local coords to
        // TimelinePanel's local coords so our hit-test and drag logic
        // works with the same coordinate space as mousePressEvent etc.
        auto* sourceWidget = qobject_cast<QWidget*>(watched);
        QPointF panelPos = sourceWidget->mapTo(this, me->pos());

        // Create a translated mouse event in panel coords
        QMouseEvent mapped(me->type(), panelPos, me->globalPosition(),
                           me->button(), me->buttons(), me->modifiers());

        switch (event->type())
        {
        case QEvent::MouseButtonDblClick:
            mouseDoubleClickEvent(&mapped);
            break;
        case QEvent::MouseButtonPress:
            // Give keyboard focus to the parent TimelineWorkspace so that
            // single-key shortcuts (I, O, Delete, Space, etc.) fire from
            // its keyPressEvent.
            if (auto* workspace = parentWidget())
                workspace->setFocus(Qt::MouseFocusReason);
            mousePressEvent(&mapped);
            break;
        case QEvent::MouseMove:
            mouseMoveEvent(&mapped);
            break;
        case QEvent::MouseButtonRelease:
            mouseReleaseEvent(&mapped);
            break;
        default:
            break;
        }

        // Accept the original event so the child widget doesn't also
        // process it (prevents double-handling).
        event->accept();
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

void TimelinePanel::keyPressEvent(QKeyEvent* event)
{
    if (m_shortcuts && m_shortcuts->handleKeyPress(event->key(), event->modifiers()))
    {
        event->accept();
        return;
    }
    // Forward unhandled keys to the parent TimelineWorkspace so its
    // keyPressEvent can handle transport (Left/Right arrows with auto-repeat)
    // and other workspace-level shortcuts.  QApplication::sendEvent is used
    // instead of calling keyPressEvent directly because QWidget::keyPressEvent
    // is protected.
    if (auto* parent = parentWidget()) {
        QApplication::sendEvent(parent, event);
        if (event->isAccepted())
            return;
    }
    QWidget::keyPressEvent(event);
}

std::optional<ClipRef> TimelinePanel::hitTestClip(const QPointF& pos) const
{
    if (!m_timeline) return std::nullopt;

    double px = pos.x() - headerWidth();
    int64_t tick = m_layoutEngine.pixelXToTime(px);
    size_t ti = hitTestTrack(pos.y());

    if (ti >= m_timeline->trackCount()) return std::nullopt;

    const Track* track = m_timeline->track(ti);
    for (size_t ci = 0; ci < track->clipCount(); ++ci)
    {
        const Clip* clip = track->clip(ci);
        if (tick >= clip->timelineIn() && tick < clip->timelineOut())
            return ClipRef{ti, clip->id()};
    }
    return std::nullopt;
}

size_t TimelinePanel::hitTestTrack(double y) const
{
    if (!m_timeline) return SIZE_MAX;

    // Use the actual widget positions of the track widgets.
    // The track widgets live inside m_trackContentArea which is inside
    // m_verticalScroll.  We map each widget's position to TimelinePanel
    // coordinates for accurate hit-testing that accounts for scroll offset.
    for (size_t i = 0; i < m_trackWidgets.size(); ++i)
    {
        auto* tw = m_trackWidgets[i];
        QPoint topLeft = tw->mapTo(this, QPoint(0, 0));
        double trackTop    = topLeft.y();
        double trackBottom = trackTop + tw->height();
        if (y >= trackTop && y < trackBottom)
            return i;
    }
    return SIZE_MAX;
}

ClipEdge TimelinePanel::hitTestClipEdge(const QPointF& pos, const ClipRef& ref) const
{
    // Default to head Ã¢â‚¬â€ the actual edge proximity check is done in mousePressEvent
    (void)pos;
    (void)ref;
    return ClipEdge::Head;
}

void TimelinePanel::updateCursorForTool()
{
    // Remove any global override from previous tool
    while (QApplication::overrideCursor()) QApplication::restoreOverrideCursor();

    switch (m_activeTool)
    {
    case EditTool::Selection:
        setCursor(Qt::ArrowCursor);
        break;
    case EditTool::Razor:
        setCursor(Qt::CrossCursor);
        break;
    case EditTool::Rolling:
    case EditTool::Ripple:
        setCursor(Qt::SplitHCursor);
        break;
    case EditTool::Slip:
    case EditTool::Slide:
        setCursor(Qt::SizeHorCursor);
        break;
    case EditTool::Text:
        setCursor(Qt::IBeamCursor);
        break;
    case EditTool::Zoom:
    {
        // Custom magnifying glass cursor — use global override so all
        // child widgets (track headers, content area, scroll viewport)
        // show it regardless of their own cursor settings.
        QPixmap pm(24, 24);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::white, 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(9, 9), 7, 7);
        p.setPen(QPen(Qt::white, 2.5));
        p.drawLine(QPointF(14.5, 14.5), QPointF(21, 21));
        p.setPen(QPen(Qt::white, 1.4));
        p.drawLine(QPointF(6, 9), QPointF(12, 9));
        p.drawLine(QPointF(9, 6), QPointF(9, 12));
        p.end();
        setCursor(QCursor(pm, 9, 9));
        QApplication::setOverrideCursor(QCursor(pm, 9, 9));
        break;
    }
    }
}

void TimelinePanel::wireShortcuts()
{
    if (!m_shortcuts) return;

    // ── Copy: copy selected clips to clipboard + copy attributes ──────
    m_shortcuts->setActionCallback(ShortcutManager::kCopy, [this]() {
        if (!m_timeline) return;
        EditOperations::copySelection(*m_timeline, m_selection, m_clipboard);
        copyAttributesFromSelection();
    });

    // ── Cut: copy + delete selection ──────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kCut, [this]() {
        if (!m_timeline || !m_commandStack) return;
        ClipboardContents cb;
        auto cmd = EditOperations::cutSelection(*m_timeline, m_selection, cb);
        if (cmd) {
            m_selection.clear();
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit selectionChanged();
            emit contentChanged();
        }
    });

    // ── Paste: overwrite at playhead ──────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kPaste, [this]() {
        if (!m_timeline || !m_commandStack || m_clipboard.empty()) return;
        auto cmd = EditOperations::paste(*m_timeline, m_clipboard, m_playheadTick);
        if (cmd) {
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit contentChanged();
        }
    });

    // ── Paste Insert: push clips right, insert at playhead ────────────
    m_shortcuts->setActionCallback(ShortcutManager::kPasteInsert, [this]() {
        if (!m_timeline || !m_commandStack || m_clipboard.empty()) return;
        auto cmd = EditOperations::pasteInsert(*m_timeline, m_clipboard, m_playheadTick);
        if (cmd) {
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit contentChanged();
        }
    });

    // ── Paste Attributes: open dialog to choose which attributes ──────
    m_shortcuts->setActionCallback(ShortcutManager::kPasteAttributes, [this]() {
        showPasteAttributesDialog();
    });

    // ── Delete (Lift): remove selected clips ──────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kDelete, [this]() {
        if (!m_timeline || !m_commandStack) return;
        auto cmd = EditOperations::deleteSelection(*m_timeline, m_selection);
        if (cmd) {
            m_selection.clear();
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit selectionChanged();
            emit contentChanged();
        }
    });

    // ── Ripple Delete: remove clips and close gaps ────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kRippleDel, [this]() {
        if (!m_timeline || !m_commandStack) return;
        auto cmd = EditOperations::rippleDelete(*m_timeline, m_selection);
        if (cmd) {
            m_selection.clear();
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit selectionChanged();
            emit contentChanged();
        }
    });

    // ── Duplicate: paste in-place offset ──────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kDuplicate, [this]() {
        if (!m_timeline || !m_commandStack) return;
        auto cmd = EditOperations::duplicateSelection(*m_timeline, m_selection);
        if (cmd) {
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit contentChanged();
        }
    });

    // ── Select All ────────────────────────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kSelectAll, [this]() {
        if (!m_timeline) return;
        m_selection.selectAll(*m_timeline);
        refreshTrackContents();
        emit selectionChanged();
    });

    // ── Split Selected Clips (F) ────────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kSplitAt, [this]() {
        if (!m_timeline || !m_commandStack || m_selection.empty()) return;
        // Build a compound command that splits each selected clip
        auto compound = std::make_unique<CompoundCommand>("Split Selected Clips");
        for (const auto& ref : m_selection.clips()) {
            if (ref.trackIndex < m_timeline->trackCount()) {
                auto cmd = EditOperations::splitClip(
                    *m_timeline, ref.trackIndex, ref.clipId, m_playheadTick);
                if (cmd)
                    compound->addCommand(std::move(cmd));
            }
        }
        if (compound->size() > 0) {
            m_commandStack->execute(std::move(compound));
            refreshTrackContents();
            emit contentChanged();
        }
    });

    // ── Split All Tracks (Shift+F) ───────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kSplitAll, [this]() {
        if (!m_timeline || !m_commandStack) return;
        auto cmd = EditOperations::splitAllAtPlayhead(*m_timeline, m_playheadTick);
        if (cmd) {
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit contentChanged();
        }
    });
}

void TimelinePanel::setSnapIndicator(int64_t tick)
{
    m_snapIndicatorTick = tick;
    for (auto* tw : m_trackWidgets)
        tw->setSnapIndicatorTick(tick);
}

} // namespace rt
