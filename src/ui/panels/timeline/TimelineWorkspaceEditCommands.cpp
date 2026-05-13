/*
 * TimelineWorkspaceEditCommands.cpp — Editing commands extracted from
 * TimelineWorkspace.cpp.
 *
 * Contains: setInPoint(), setOutPoint(), clearInOut(),
 * syncProgramMonitorInOut(), refreshSequenceTabs(),
 * nestSequence().
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"

#include "panels/monitors/ProgramMonitor.h"
#include "widgets/MiniTimeline.h"
#include "panels/timeline/TimelinePanel.h"

#include "command/CommandStack.h"
#include "command/commands/ClipCommands.h"
#include "project/Project.h"
#include "media/PlaybackController.h"
#include "timeline/EditOperations.h"
#include "timeline/SequenceClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <QString>

#include <spdlog/spdlog.h>

#include <memory>

namespace rt {

void TimelineWorkspace::setInPoint()
{
    if (!m_timeline || !m_playbackController) return;
    EditOperations::setInPoint(*m_timeline, m_playbackController->currentTick());
    if (m_timelinePanel) m_timelinePanel->updateInOutRange();
    syncProgramMonitorInOut();
}

void TimelineWorkspace::setOutPoint()
{
    if (!m_timeline || !m_playbackController) return;
    EditOperations::setOutPoint(*m_timeline, m_playbackController->currentTick());
    if (m_timelinePanel) m_timelinePanel->updateInOutRange();
    syncProgramMonitorInOut();
}

void TimelineWorkspace::clearInOut()
{
    if (!m_timeline) return;
    EditOperations::clearInOutPoints(*m_timeline);
    if (m_timelinePanel) m_timelinePanel->updateInOutRange();
    syncProgramMonitorInOut();
}

void TimelineWorkspace::syncProgramMonitorInOut()
{
    if (!m_programMonitor || !m_programMonitor->miniTimeline())
        return;
    auto* mt = m_programMonitor->miniTimeline();
    if (m_timeline) {
        int64_t inPt  = m_timeline->inPoint();
        int64_t outPt = m_timeline->outPoint();
        if (inPt < 0 && outPt < 0) {
            mt->clearInOutPoints();
        } else {
            mt->setInPoint(inPt);
            mt->setOutPoint(outPt);
        }
    } else {
        mt->clearInOutPoints();
    }
}

void TimelineWorkspace::refreshSequenceTabs()
{
    if (!m_sequenceTabBar || !m_project) return;
    m_suppressTabChange = true;
    while (m_sequenceTabBar->count() > 0)
        m_sequenceTabBar->removeTab(0);
    for (size_t i = 0; i < m_project->sequenceCount(); ++i) {
        const Timeline* seq = m_project->sequence(i);
        m_sequenceTabBar->addTab(seq ? QString::fromStdString(seq->name())
                                     : QStringLiteral("Sequence"));
    }
    int activeIdx = static_cast<int>(m_project->activeSequenceIndex());
    if (activeIdx < m_sequenceTabBar->count())
        m_sequenceTabBar->setCurrentIndex(activeIdx);
    m_suppressTabChange = false;
}

void TimelineWorkspace::nestSequence(size_t sequenceIndex, const QString& sequenceName)
{
    if (!m_timeline || !m_project || !m_commandStack) return;

    // Find first targeted video track
    Track* targetTrack = nullptr;
    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        Track* t = m_timeline->track(i);
        if (t && t->type() == TrackType::Video && t->isTargeted()) {
            targetTrack = t;
            break;
        }
    }
    if (!targetTrack) return;

    // Get playhead position
    int64_t playhead = m_playbackController ? m_playbackController->currentTick() : 0;

    // Get the nested sequence's duration (fallback to 5 seconds)
    const Timeline* nestedSeq = m_project->sequence(sequenceIndex);
    int64_t dur = nestedSeq ? nestedSeq->duration() : (48000 * 5);
    if (dur <= 0) dur = 48000 * 5;

    auto clip = std::make_unique<SequenceClip>();
    clip->setSequenceIndex(sequenceIndex);
    clip->setSequenceName(sequenceName.toStdString());
    clip->setLabel(sequenceName.toStdString());
    clip->setTimelineIn(playhead);
    clip->setSourceIn(0);
    clip->setDuration(dur);

    auto cmd = std::make_unique<AddClipCommand>(targetTrack, std::move(clip));
    m_commandStack->execute(std::move(cmd));

    if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
    invalidateAudioSources();
    invalidateCompositeCache();
    updateTransformOverlay();
    if (m_programMonitor) m_programMonitor->requestRefresh();
    schedulePostEditWork();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
