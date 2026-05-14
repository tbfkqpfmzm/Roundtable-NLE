/*
 * SourceMonitor.cpp -- media loading, playback, events, audio.
 *
 * Constructor/setupUI --> SourceMonitorUI.cpp
 */

#include "panels/monitors/SourceMonitor.h"
#include "panels/monitors/WaveformDisplayWidget.h"

#include "Theme.h"
#include "media/FrameCache.h"

#include "viewport/Viewport.h"
#include "widgets/MiniTimeline.h"
#include "widgets/TransportButton.h"
#include "media/PlaybackController.h"
#include "media/MediaPool.h"
#include "media/MediaSourceService.h"
#include "media/AudioFile.h"
#include "media/AudioEngine.h"
#include "media/AVSyncClock.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "CompositeService.h"
#include "timeline/SpineClip.h"
#endif

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStackedLayout>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <thread>


namespace rt {

namespace {

constexpr int64_t kSourceScrubPreRollFrames  = 4096;
constexpr int64_t kSourceScrubPostRollFrames = 8192;
constexpr int64_t kSourceScrubMinFrames      = 4096;

bool loadResampledAudioFile(const std::filesystem::path& filePath,
                            std::vector<float>& samples,
                            uint16_t& channels)
{
    AudioFile file;
    if (!file.open(filePath)) {
        return false;
    }

    samples = file.readAllResampled(48000);
    if (samples.empty()) {
        return false;
    }

    channels = file.info().channels;
    if (channels == 0) {
        channels = 1;
    }
    return true;
}

} // namespace


void SourceMonitor::loadClip(uint64_t mediaHandle, MediaPool* pool)
{
    clearClip();

    if (!pool || !pool->isValid(mediaHandle))
        return;

    m_pool        = pool;
    m_mediaHandle = mediaHandle;
    m_hasClip     = true;
    m_audioOnly   = false;

    // Get clip info
    const auto* info = pool->getInfo(mediaHandle);
    if (info)
    {
        m_audioOnly = (info->videoStreamIndex < 0);
        m_fps          = info->fps > 0.0 ? info->fps : 24.0;
        m_frameCount   = info->frameCount;
        m_clipDuration = static_cast<int64_t>(info->duration * 48000.0);

        m_controller->setFrameRate(m_fps);
        m_controller->setStandaloneDuration(m_clipDuration);
        m_miniTimeline->setDuration(m_clipDuration);
        m_miniTimeline->setFrameRate(m_fps);

        // Set clip name from file path
        auto path = pool->getPath(mediaHandle);
        setClipName(QString::fromStdString(path.filename().string()));
    }

    // Show first frame (skip for audio-only — no video to decode)
    m_controller->seekTo(0);
    if (!m_audioOnly) {
        m_viewStack->setCurrentIndex(0); // video viewport
        updateFrameDisplay();
    } else {
        m_viewStack->setCurrentIndex(1); // waveform display
        m_waveformWidget->clear();
        updateTimecodeDisplay();
    }

    // Leave source audio fully lazy. Only audio-only clips need waveform data
    // on open because the waveform view is immediately visible.
    m_audioSamples.reset();
    m_audioChannels = 0;
    m_audioLoadFailed = false;
    m_audioLoadInFlight = false;
    ++m_audioLoadGeneration;
    m_scrubAudioSamples.reset();
    m_scrubAudioChannels = 0;
    m_scrubAudioStartFrame = 0;
    ++m_waveformLoadGeneration;
    if (m_audioOnly) {
        loadWaveformAsync();
    }
}

void SourceMonitor::loadSequence(size_t sequenceIndex, const QString& name,
                                 int64_t durationTicks, double fps,
                                 SequenceFrameProvider frameProvider)
{
    clearClip();

    m_isSequence      = true;
    m_sequenceIndex   = sequenceIndex;
    m_seqFrameProvider = std::move(frameProvider);
    m_hasClip         = true;
    m_audioOnly       = false;
    m_fps             = fps > 0.0 ? fps : 24.0;
    m_clipDuration    = durationTicks;
    m_frameCount      = static_cast<int64_t>(
        (static_cast<double>(durationTicks) / 48000.0) * m_fps);

    m_controller->setFrameRate(m_fps);
    m_controller->setStandaloneDuration(m_clipDuration);
    m_miniTimeline->setDuration(m_clipDuration);
    m_miniTimeline->setFrameRate(m_fps);
    setClipName(name);

    m_controller->seekTo(0);
    m_viewStack->setCurrentIndex(0); // video viewport
    updateFrameDisplay();
}

#ifdef ROUNDTABLE_HAS_SPINE
void SourceMonitor::loadSpineClip(SpineClip* spineClip, CompositeService* compositeService)
{
    if (!spineClip || !compositeService) return;
    
    // Clear any previous clip
    clearClip();
    
    // Render first frame of the animation at preview size
    auto frame = compositeService->renderSpineClip(spineClip, 0, 960, 540);
    if (!frame || !frame->ensurePixels()) return;
    
    m_isSequence = true;
    m_hasClip = true;
    m_audioOnly = false;
    m_fps = 30.0;
    m_frameCount = 150;
    m_clipDuration = secondsToTicks(5.0);
    
    m_controller->setFrameRate(m_fps);
    m_controller->setStandaloneDuration(m_clipDuration);
    m_miniTimeline->setDuration(m_clipDuration);
    m_miniTimeline->setFrameRate(m_fps);
    setClipName(QString::fromStdString(spineClip->label()));
    
    m_controller->seekTo(0);
    m_viewStack->setCurrentIndex(0);
    
    // Display the rendered frame directly
    if (m_viewport) {
        m_viewport->displayFrame(*frame);
        updateTimecodeDisplay();
    }
}
#endif // ROUNDTABLE_HAS_SPINE

void SourceMonitor::clearClip()
{
    m_pollTimer->stop();

    // Stop audio before stopping the controller (stopSourceAudio
    // restores the timeline's sync clock ownership of the AudioEngine).
    stopSourceAudio();
    m_audioSamples.reset();
    m_audioChannels = 0;
    m_audioLoadFailed = false;
    m_audioLoadInFlight = false;
    ++m_audioLoadGeneration;
    m_scrubAudioSamples.reset();
    m_scrubAudioChannels = 0;
    m_scrubAudioStartFrame = 0;
    ++m_waveformLoadGeneration;

    if (m_hasClip && m_controller)
        m_controller->stop();

    m_pool         = nullptr;
    m_mediaHandle  = 0;
    m_hasClip      = false;
    m_audioOnly    = false;
    m_isSequence   = false;
    m_sequenceIndex = 0;
    m_seqFrameProvider = nullptr;
    m_frameCount   = 0;
    m_clipDuration = 0;
    m_fps          = 24.0;

    m_viewport->clearFrame();
    m_waveformWidget->clear();
    m_viewStack->setCurrentIndex(0);
    m_miniTimeline->setDuration(0);
    m_miniTimeline->clearInOutPoints();
    m_clipLabel->setText(tr("No clip loaded"));
    m_timecodeLabel->setText(QStringLiteral("00:00:00:00"));
}

// ═════════════════════════════════════════════════════════════════════════════
//  In / Out points
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::markIn()
{
    if (!m_hasClip) return;
    m_miniTimeline->setInPoint(currentTick());
    emit inOutChanged();
}

void SourceMonitor::markOut()
{
    if (!m_hasClip) return;
    m_miniTimeline->setOutPoint(currentTick());
    emit inOutChanged();
}

void SourceMonitor::clearInOut()
{
    m_miniTimeline->clearInOutPoints();
    emit inOutChanged();
}

int64_t SourceMonitor::inPoint() const noexcept
{
    return m_miniTimeline ? m_miniTimeline->inPoint() : -1;
}

int64_t SourceMonitor::outPoint() const noexcept
{
    return m_miniTimeline ? m_miniTimeline->outPoint() : -1;
}

bool SourceMonitor::hasInPoint() const noexcept
{
    return m_miniTimeline && m_miniTimeline->hasInPoint();
}

bool SourceMonitor::hasOutPoint() const noexcept
{
    return m_miniTimeline && m_miniTimeline->hasOutPoint();
}

int64_t SourceMonitor::selectedDuration() const noexcept
{
    return m_miniTimeline ? m_miniTimeline->selectedDuration() : 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Transport
// ═════════════════════════════════════════════════════════════════════════════

int64_t SourceMonitor::currentTick() const noexcept
{
    return m_controller ? m_controller->currentTick() : 0;
}

void SourceMonitor::scrubTo(int64_t tick)
{
    if (!m_hasClip || !m_controller) return;
    m_controller->seekTo(tick);
    updateFrameDisplay();
}

void SourceMonitor::scrubToFrame(int64_t frameNumber)
{
    if (!m_hasClip || m_fps <= 0.0) return;
    int64_t tick = static_cast<int64_t>((static_cast<double>(frameNumber) / m_fps) * 48000.0);
    scrubTo(tick);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Source region
// ═════════════════════════════════════════════════════════════════════════════

SourceMonitor::SourceRegion SourceMonitor::sourceRegion() const
{
    SourceRegion region;
    region.mediaHandle = m_mediaHandle;
    region.fps         = m_fps;

    if (hasInPoint())
        region.sourceIn = inPoint();
    else
        region.sourceIn = 0;

    if (hasOutPoint())
        region.sourceOut = outPoint();
    else
        region.sourceOut = m_clipDuration;

    region.duration = region.sourceOut - region.sourceIn;
    if (region.duration < 0) region.duration = 0;

    return region;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Display
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::setClipName(const QString& name)
{
    m_clipLabel->setText(name);
}

QSize SourceMonitor::sizeHint() const
{
    return QSize(480, 400);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Slots
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::onScrub(int64_t tick)
{
    if (!m_hasClip) return;

    // Coalesce: just record the latest scrub target.  The poll timer
    // (16 ms) will pick it up and do a single seek+decode for the most
    // recent position, instead of one full-resolution precise-seek per
    // mouse-move event.  On long-GOP H.264 sources each precise seek is
    // 100-300 ms; without coalescing, dragging the playhead generates a
    // huge backlog and feels frozen.
    m_pendingScrubTick = tick;
    m_scrubPending     = true;

    // Start settle polling so late prefetch deliveries update the display
    // after the user stops scrubbing (poll timer is normally off when paused).
    m_scrubSettleCounter = 10; // ~160ms at 16ms interval
    if (!m_pollTimer->isActive())
        m_pollTimer->start();
}

void SourceMonitor::onPollTimer()
{
    if (!m_hasClip || !m_controller) return;

    // Drain any pending scrub: a single seek+decode for the latest
    // mouse-move target, regardless of how many scrub events arrived
    // since the last poll.
    if (m_scrubPending) {
        m_scrubPending = false;
        m_controller->seekTo(m_pendingScrubTick);
        scrubAudioAt(m_pendingScrubTick);
        emit playheadChanged(m_pendingScrubTick);
    }

    (void)m_controller->pollPosition();
    updateFrameDisplay();

    // Stop polling if playback ended (but honour scrub settle window)
    if (!m_controller->isPlaying()) {
        if (m_scrubSettleCounter > 0) {
            --m_scrubSettleCounter;
        } else {
            m_pollTimer->stop();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Internal helpers
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::updateFrameDisplay()
{
    if (!m_hasClip || !m_controller) return;

    int64_t tick = m_controller->currentTick();
    m_miniTimeline->setPlayhead(tick);

    if (m_audioOnly) {
        // Update waveform playhead position
        if (m_clipDuration > 0)
            m_waveformWidget->setPlayheadRatio(
                static_cast<double>(tick) / static_cast<double>(m_clipDuration));
    }
    else if (m_isSequence && m_seqFrameProvider)
    {
        auto frame = m_seqFrameProvider(
            tick,
            static_cast<uint32_t>(m_viewport->width()),
            static_cast<uint32_t>(m_viewport->height()),
            false);
        if (frame)
            m_viewport->displayFrame(frame);
    }
    else if (m_pool && m_pool->isValid(m_mediaHandle))
    {
        // Convert tick → frame number
        int64_t frameNum = 0;
        if (m_fps > 0.0)
            frameNum = static_cast<int64_t>(
                (static_cast<double>(tick) / 48000.0) * m_fps);
        frameNum = std::clamp(frameNum, int64_t(0), std::max(int64_t(0), m_frameCount - 1));

        const bool playing = m_controller->isPlaying();
        // When scrubbing/paused, decode at Half tier — the source-monitor
        // viewport is usually <= ~720px wide, so Full-tier 1080p+ decode
        // is wasted work that turns each precise seek into a 100-300 ms
        // stall.  Playback path (tryGetFrame) already uses Half.
        std::shared_ptr<CachedFrame> frame;
        if (m_mediaSources) {
            auto result = m_mediaSources->requestFrame({
                m_mediaHandle, frameNum,
                RenderRequestType::SourceMonitor,
                RenderQuality::Auto,
                playing ? RenderExactness::BestEffortAllowed : RenderExactness::ExactRequired,
                ResolutionTier::Half
            });
            frame = result.frame;
        } else if (m_pool) {
            frame = playing
                ? m_pool->tryGetFrame(m_mediaHandle, frameNum, ResolutionTier::Half)
                : m_pool->getFrame(m_mediaHandle, frameNum, ResolutionTier::Half, /*scrubMode=*/true);
        }
        if (frame) {
            frame->ensurePixels();
            m_viewport->displayFrame(*frame);
        }
    }

    updateTimecodeDisplay();

    // Update shuttle speed display
    if (m_shuttleSpeedLabel && m_controller) {
        double speed = m_controller->shuttleSpeed();
        if (m_controller->isPlaying() && speed != 0.0 && speed != 1.0) {
            QString speedText;
            if (speed == static_cast<int>(speed))
                speedText = QStringLiteral("%1x").arg(static_cast<int>(speed));
            else
                speedText = QStringLiteral("%1x").arg(speed, 0, 'g', 2);
            m_shuttleSpeedLabel->setText(speedText);
            m_shuttleSpeedLabel->show();
        } else {
            m_shuttleSpeedLabel->hide();
        }
    }

    emit playheadChanged(tick);
}

void SourceMonitor::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // Mirror of ProgramMonitor::resizeEvent — see that file for the full
    // explanation of why repaint() + updateFrameDisplay() + a deferred
    // update() are all needed (Windows modal WM_SIZE loop suppresses
    // queued paint events while the dock still receives stale WM_PAINTs).
    auto forceRepaint = [this]() {
        repaint();
        if (auto* lay = layout()) {
            for (int i = 0; i < lay->count(); ++i) {
                auto* item = lay->itemAt(i);
                auto* w = item ? item->widget() : nullptr;
                if (!w || !w->isVisible()) continue;
                if (m_viewport && w == m_viewport->parentWidget()) continue;
                w->repaint();
            }
        }
    };
    forceRepaint();

    // Produce a fresh decoded frame at the new widget size so the viewport
    // content is correct during/after resize even when paused.  Without
    // this, the poll timer stops (when not playing) and the old stretched
    // frame remains — the "echo" effect.
    updateFrameDisplay();
    // Keep the poll timer alive briefly after resize to settle, so any
    // late decodes or paint corrections take effect.
    if (!m_controller->isPlaying()) {
        m_scrubSettleCounter = 10;
        if (!m_pollTimer->isActive())
            m_pollTimer->start();
    }

    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        update();
        if (auto* lay = layout()) {
            for (int i = 0; i < lay->count(); ++i) {
                auto* item = lay->itemAt(i);
                auto* w = item ? item->widget() : nullptr;
                if (!w || !w->isVisible()) continue;
                if (m_viewport && w == m_viewport->parentWidget()) continue;
                w->update();
            }
        }
    });
}

void SourceMonitor::updateTimecodeDisplay()
{
    // Update duration timecode
    if (m_durationLabel) {
        auto dur = tickToTimecode(m_controller->durationTicks(),
                                  m_controller->frameRate());
        m_durationLabel->setText(QString::fromStdString(dur.toString()));
    }
}

bool SourceMonitor::eventFilter(QObject* watched, QEvent* event)
{
    // Grab keyboard focus when user clicks anywhere in the Source Monitor
    // (viewport, mini-timeline, waveform) so JKL/Space route here.
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonDblClick) {
        // Timecode label click → show editable timecode entry
        if (watched == m_timecodeLabel && event->type() == QEvent::MouseButtonPress) {
            m_timecodeLabel->hide();
            m_timecodeEdit->setText(m_timecodeLabel->text());
            m_timecodeEdit->show();
            m_timecodeEdit->setFocus();
            m_timecodeEdit->selectAll();
            return true;
        }
        setFocus();
    }

    // Forward drag events from the viewport child to our own handlers
    if (watched == m_viewport) {
        if (event->type() == QEvent::DragEnter) {
            dragEnterEvent(static_cast<QDragEnterEvent*>(event));
            return event->isAccepted();
        }
        if (event->type() == QEvent::DragMove) {
            dragMoveEvent(static_cast<QDragMoveEvent*>(event));
            return event->isAccepted();
        }
        if (event->type() == QEvent::Drop) {
            dropEvent(static_cast<QDropEvent*>(event));
            return event->isAccepted();
        }

        // Drag-out: click and drag from the viewport to the timeline
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && m_hasClip)
                m_dragStartPos = me->pos();
        }
        if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if ((me->buttons() & Qt::LeftButton) && m_hasClip
                && !m_dragStartPos.isNull()
                && (me->pos() - m_dragStartPos).manhattanLength()
                       >= QApplication::startDragDistance()) {

                auto region = sourceRegion();
                auto* mimeData = new QMimeData;

                if (m_isSequence) {
                    // Sequence drag-out: encode sequence index + in/out
                    mimeData->setData("application/x-roundtable-sequence",
                                      QByteArray::number(qulonglong(m_sequenceIndex)));
                    mimeData->setData("application/x-roundtable-sequence-duration",
                                      QByteArray::number(qlonglong(m_clipDuration)));
                    mimeData->setData("application/x-roundtable-source-in",
                                      QByteArray::number(qlonglong(region.sourceIn)));
                    mimeData->setData("application/x-roundtable-source-out",
                                      QByteArray::number(qlonglong(region.sourceOut)));
                } else {
                    auto filePath = m_mediaSources
                        ? m_mediaSources->sourceInfo(m_mediaHandle).value().path
                        : (m_pool ? m_pool->getPath(m_mediaHandle) : std::filesystem::path());
                    if (filePath.empty()) { m_dragStartPos = QPoint(); return false; }

                    mimeData->setData("application/x-roundtable-media",
                                      QByteArray::number(qulonglong(m_mediaHandle)));
                    mimeData->setUrls({QUrl::fromLocalFile(
                        QString::fromStdString(filePath.string()))});

                    // Attach source in/out so the timeline can trim the clip
                    mimeData->setData("application/x-roundtable-source-in",
                                      QByteArray::number(qlonglong(region.sourceIn)));
                    mimeData->setData("application/x-roundtable-source-out",
                                      QByteArray::number(qlonglong(region.sourceOut)));
                }

                // Create a clean drag pixmap (Premiere-style pill)
                QString label = m_clipLabel ? m_clipLabel->text()
                                            : QStringLiteral("Source");
                QFontMetrics fm(font());
                int textW = fm.horizontalAdvance(label);
                int pillW = qBound(120, textW + 24, 300);
                int pillH = 28;
                QPixmap pix(pillW, pillH);
                pix.fill(Qt::transparent);
                {
                    const auto& tc = Theme::colors();
                    QPainter p(&pix);
                    p.setRenderHint(QPainter::Antialiasing);
                    p.setBrush(QColor(tc.surface2));
                    p.setPen(Qt::NoPen);
                    p.drawRoundedRect(0, 0, pillW, pillH, 4, 4);
                    // accent stripe – purple for sequences, blue for media
                    p.setBrush(m_isSequence ? QColor("#7B68EE")
                                            : QColor(tc.accent));
                    p.drawRoundedRect(0, 0, 4, pillH, 2, 2);
                    // text
                    p.setPen(QColor(tc.text));
                    p.drawText(QRect(10, 0, pillW - 14, pillH),
                               Qt::AlignVCenter | Qt::AlignLeft,
                               fm.elidedText(label, Qt::ElideRight, pillW - 18));
                }

                auto* drag = new QDrag(this);
                drag->setMimeData(mimeData);
                drag->setPixmap(pix);
                drag->setHotSpot(QPoint(pillW / 2, pillH / 2));
                drag->exec(Qt::CopyAction);

                m_dragStartPos = QPoint();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SourceMonitor::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat("application/x-roundtable-media") ||
        event->mimeData()->hasFormat("application/x-roundtable-sequence") ||
        qobject_cast<QTreeWidget*>(event->source()))
        event->acceptProposedAction();
}

void SourceMonitor::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasFormat("application/x-roundtable-media") ||
        event->mimeData()->hasFormat("application/x-roundtable-sequence") ||
        qobject_cast<QTreeWidget*>(event->source()))
        event->acceptProposedAction();
}

void SourceMonitor::dropEvent(QDropEvent* event)
{
    // Try custom mime type first (from ThumbnailGrid / BinTreeWidget)
    if (event->mimeData()->hasFormat("application/x-roundtable-media")) {
        bool ok = false;
        uint64_t handle = event->mimeData()->data("application/x-roundtable-media")
                              .toULongLong(&ok);
        if (!ok || handle == 0) return;

        if (m_pool)
            loadClip(handle, m_pool);
        else
            emit dropReceived(handle);

        event->acceptProposedAction();
        return;
    }

    // Sequence via custom MIME (drag from another source)
    if (event->mimeData()->hasFormat("application/x-roundtable-sequence")) {
        bool ok = false;
        size_t seqIdx = event->mimeData()->data("application/x-roundtable-sequence")
                            .toULongLong(&ok);
        if (ok) {
            emit sequenceDropReceived(seqIdx);
            event->acceptProposedAction();
        }
        return;
    }

    // Fallback: QTreeWidget default drag provides model data but no custom mime.
    // The source QTreeWidget item has media handle in UserRole+1.
    if (auto* tree = qobject_cast<QTreeWidget*>(event->source())) {
        auto* item = tree->currentItem();
        if (!item) return;
        // Skip bins
        if (item->data(0, Qt::UserRole + 2).toBool()) return;

        // Handle sequences from the tree
        if (item->data(0, Qt::UserRole + 3).toBool()) {
            size_t seqIdx = item->data(0, Qt::UserRole + 4).toULongLong();
            emit sequenceDropReceived(seqIdx);
            event->acceptProposedAction();
            return;
        }

        uint64_t handle = item->data(0, Qt::UserRole + 1).toULongLong();
        if (handle == 0) return;

        if (m_pool)
            loadClip(handle, m_pool);
        else
            emit dropReceived(handle);

        event->acceptProposedAction();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Waveform loading
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::loadWaveformAsync()
{
    if (!m_pool || !m_pool->isValid(m_mediaHandle)) return;

    auto filePath = m_mediaSources
        ? m_mediaSources->sourceInfo(m_mediaHandle).value().path
        : m_pool->getPath(m_mediaHandle);
    if (filePath.empty()) return;

    auto pathStr = filePath;
    QPointer<SourceMonitor> self(this);
    QPointer<WaveformDisplayWidget> widget(m_waveformWidget);
    bool audioOnly = m_audioOnly;
    const uint64_t generation = m_waveformLoadGeneration;

    // Load on a background thread and stream a peak envelope so opening a
    // source clip does not require a full decoded playback buffer.
    std::thread([self, widget, pathStr, audioOnly, generation]() {
        AudioFile file;
        if (!file.open(pathStr)) return;

        const auto& info = file.info();
        uint16_t ch = info.channels;
        if (ch == 0) ch = 1;
        constexpr int kPeakWindow = 256;
        std::vector<float> peaks;
        if (file.buildPeakEnvelopeResampled(48000, kPeakWindow, peaks) == 0 ||
            peaks.empty()) {
            return;
        }

        // Deliver to main thread
        if (!self || !widget) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, widget, ch, audioOnly, generation,
                                         peaks = std::move(peaks)]() mutable {
            if (!self || !widget || self->m_waveformLoadGeneration != generation) {
                return;
            }
            if (audioOnly)
                widget->setPeaks(std::move(const_cast<std::vector<float>&>(peaks)), ch);
        }, Qt::QueuedConnection);
    }).detach();
}

bool SourceMonitor::ensureSourceAudioLoaded()
{
    if (m_audioSamples && !m_audioSamples->empty() && m_audioChannels > 0) {
        return true;
    }
    if (m_audioLoadFailed || !m_pool || !m_pool->isValid(m_mediaHandle)) {
        return false;
    }

    requestSourceAudioLoadAsync();
    return false;
}

void SourceMonitor::requestSourceAudioLoadAsync()
{
    if (m_audioLoadInFlight || m_audioLoadFailed || !m_pool || !m_pool->isValid(m_mediaHandle)) {
        return;
    }

    const auto filePath = m_mediaSources
        ? m_mediaSources->sourceInfo(m_mediaHandle).value().path
        : m_pool->getPath(m_mediaHandle);
    if (filePath.empty()) {
        m_audioLoadFailed = true;
        return;
    }

    m_audioLoadInFlight = true;
    const uint64_t generation = m_audioLoadGeneration;
    QPointer<SourceMonitor> self(this);

    std::thread([self, generation, filePath]() {
        std::vector<float> samples;
        uint16_t channels = 0;
        const bool ok = loadResampledAudioFile(filePath, samples, channels);

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, generation, ok, channels,
                                         samples = std::move(samples)]() mutable {
            if (!self || self->m_audioLoadGeneration != generation) {
                return;
            }

            self->m_audioLoadInFlight = false;
            if (!ok || samples.empty() || channels == 0) {
                self->m_audioLoadFailed = true;
                return;
            }

            self->m_audioSamples = std::make_shared<std::vector<float>>(std::move(samples));
            self->m_audioChannels = channels;
            self->m_audioLoadFailed = false;

            if (self->m_controller && self->m_controller->isPlaying() &&
                self->m_audioEngine && !self->m_sourceAudioActive) {
                self->startSourceAudio();
            }
        }, Qt::QueuedConnection);
    }).detach();
}

bool SourceMonitor::ensureScrubAudioLoaded(int64_t frame, int64_t durationFrames)
{
    if (m_audioSamples && !m_audioSamples->empty() && m_audioChannels > 0) {
        return true;
    }

    if (!m_pool || !m_pool->isValid(m_mediaHandle) || m_audioLoadFailed) {
        return false;
    }

    const int64_t neededStart = std::max<int64_t>(0, frame);
    if (m_scrubAudioSamples && !m_scrubAudioSamples->empty() && m_scrubAudioChannels > 0) {
        const int64_t cachedFrames = static_cast<int64_t>(m_scrubAudioSamples->size() / m_scrubAudioChannels);
        const int64_t cachedEnd = m_scrubAudioStartFrame + cachedFrames;
        if (neededStart >= m_scrubAudioStartFrame &&
            (neededStart + durationFrames) <= cachedEnd) {
            return true;
        }
    }

    const auto filePath = m_mediaSources
        ? m_mediaSources->sourceInfo(m_mediaHandle).value().path
        : m_pool->getPath(m_mediaHandle);
    if (filePath.empty()) {
        m_audioLoadFailed = true;
        return false;
    }

    AudioFile file;
    if (!file.open(filePath)) {
        m_audioLoadFailed = true;
        return false;
    }

    uint16_t channels = file.info().channels;
    if (channels == 0) {
        channels = 1;
    }

    const int64_t windowStart = std::max<int64_t>(0, frame - kSourceScrubPreRollFrames);
    const int64_t windowFrames = std::max<int64_t>(
        durationFrames + kSourceScrubPreRollFrames + kSourceScrubPostRollFrames,
        kSourceScrubMinFrames);

    std::vector<float> samples;
    const int64_t framesRead = file.readRegionResampled(windowStart, windowFrames, 48000, samples);
    if (framesRead <= 0 || samples.empty()) {
        return false;
    }

    m_scrubAudioSamples = std::make_shared<std::vector<float>>(std::move(samples));
    m_scrubAudioChannels = channels;
    m_scrubAudioStartFrame = windowStart;
    requestSourceAudioLoadAsync();
    return true;
}

void SourceMonitor::startSourceAudio()
{
    if (!m_audioEngine) return;
    if (m_sourceAudioActive) return;
    if (!ensureSourceAudioLoaded()) return;

    // Detach the timeline's sync clock so audioEngine->play() won't
    // start/stop/reset the timeline position.
    m_savedSyncClock = m_audioEngine->syncClock();
    m_audioEngine->setSyncClock(nullptr);
    m_sourceAudioActive = true;

    emit playbackStarted();

    // Load source clip audio into the engine
    loadSourceAudio();

    // Match audio speed to controller shuttle speed
    double speed = m_controller->shuttleSpeed();
    if (speed == 0.0) speed = 1.0;
    m_audioEngine->setPlaybackSpeed(speed);

    // Seek audio to current playhead
    int64_t tick = m_controller->currentTick();
    int64_t frame = static_cast<int64_t>(
        static_cast<double>(tick) / 48000.0 * m_audioEngine->sampleRate());
    m_audioEngine->seekToFrame(frame);
    m_audioEngine->play();
}

void SourceMonitor::stopSourceAudio()
{
    if (!m_audioEngine || !m_sourceAudioActive) return;

    m_audioEngine->pause();
    m_audioEngine->setPlaybackSpeed(1.0);  // Reset to normal speed
    m_audioEngine->clearTrackSources();
    m_audioEngine->resetStretchers();

    // Restore the timeline's sync clock
    m_audioEngine->setSyncClock(m_savedSyncClock);
    m_savedSyncClock = nullptr;
    m_sourceAudioActive = false;
}

void SourceMonitor::loadSourceAudio()
{
    if (!m_audioEngine || !ensureSourceAudioLoaded())
        return;

    AudioTrackSource src;
    src.trackId     = 9999;
    src.samples     = m_audioSamples->data();
    src.totalFrames = static_cast<int64_t>(m_audioSamples->size() / m_audioChannels);
    src.startFrame  = 0;
    src.channels    = m_audioChannels;
    src.sampleRate  = 48000;
    src.volume      = 1.0f;
    src.pan         = 0.0f;
    src.muted       = false;
    src.solo        = false;

    m_audioEngine->setTrackSources({src});
}

void SourceMonitor::scrubAudioAt(int64_t tick)
{
    if (!m_audioEngine)
        return;

    const int64_t frame = static_cast<int64_t>(
        static_cast<double>(tick) / 48000.0 * m_audioEngine->sampleRate());
    constexpr int64_t scrubDurationFrames = 2048;

    if (!ensureScrubAudioLoaded(frame, scrubDurationFrames)) {
        return;
    }

    AudioTrackSource src;
    src.trackId = 9999;
    src.startFrame = 0;
    src.sampleRate = 48000;
    src.volume = 1.0f;
    src.pan = 0.0f;
    src.muted = false;
    src.solo = false;

    int64_t localFrame = frame;
    if (m_audioSamples && !m_audioSamples->empty() && m_audioChannels > 0) {
        src.samples = m_audioSamples->data();
        src.totalFrames = static_cast<int64_t>(m_audioSamples->size() / m_audioChannels);
        src.channels = m_audioChannels;
    } else {
        src.samples = m_scrubAudioSamples ? m_scrubAudioSamples->data() : nullptr;
        src.totalFrames = (m_scrubAudioSamples && m_scrubAudioChannels > 0)
            ? static_cast<int64_t>(m_scrubAudioSamples->size() / m_scrubAudioChannels)
            : 0;
        src.channels = m_scrubAudioChannels;
        localFrame = std::max<int64_t>(0, frame - m_scrubAudioStartFrame);
    }

    if (!src.samples || src.totalFrames <= 0 || src.channels == 0) {
        return;
    }

    // Temporarily detach sync clock, load sources, scrub, then restore
    if (!m_sourceAudioActive) {
        m_savedSyncClock = m_audioEngine->syncClock();
        m_audioEngine->setSyncClock(nullptr);
        m_audioEngine->setTrackSources({src});
    }

    m_audioEngine->scrub(localFrame, scrubDurationFrames);

    if (!m_sourceAudioActive) {
        // Restore after scrub burst finishes — scrub is async so we just
        // restore the clock pointer; the callback will stop on its own.
        m_audioEngine->setSyncClock(m_savedSyncClock);
        m_savedSyncClock = nullptr;
    }
}

} // namespace rt
