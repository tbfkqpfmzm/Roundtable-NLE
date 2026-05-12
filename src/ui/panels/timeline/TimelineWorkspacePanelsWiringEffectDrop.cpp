// TimelineWorkspacePanelsWiringEffectDrop.cpp - Effect/transition drop signal wiring.
// Extracted from TimelineWorkspacePanelsWiring.cpp for maintainability.

#include <volk.h>

#include <map>
#include <set>

#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/ClipRenderers.h"
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Theme.h"

#include "panels/audio/AudioMixer.h"
// ShotPanel removed — character/shot controls merged into PropertiesPanel
#include "panels/effects/EffectsPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/project/ProjectBin.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/ColorGradingPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

#include "widgets/MiniTimeline.h"
#include "widgets/DockTitleBar.h"
#include "widgets/VUMeter.h"
#include "viewport/Viewport.h"
#include "viewport/TransformOverlayWidget.h"

#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/MarkerCommands.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/EffectCommands.h"
#include "project/Project.h"
#include "MainWindow.h"
#include "media/AudioEngine.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include "timeline/AudioClip.h"
#include "timeline/EditOperations.h"
#include "timeline/ImageClip.h"
#include "timeline/OpacityMask.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/VideoClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include "effects/ChromaKey.h"
#include "media/FrameCache.h"
#include "media/AudioPlaybackService.h"

#include "panels/characters/ShotComposerInternal.h"
#include "spine/ShotPreset.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#endif

#include <QDockWidget>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QPainter>
#include <QTimer>
#include <spdlog/spdlog.h>

namespace rt {

void TimelineWorkspace::wireEffectDropSignals()
{
    // =====================================================================
    //  EFFECT DRAG-DROP -> ADD EFFECT TO CLIP
    // =====================================================================
    if (m_timelinePanel) {
        connect(m_timelinePanel, &TimelinePanel::effectDroppedOnClip,
                this, [this](size_t trackIdx, uint64_t clipId, int effectType) {
            if (!m_timeline) return;
            auto* track = m_timeline->track(trackIdx);
            if (!track) return;
            size_t clipIdx = track->findClipIndexById(clipId);
            if (clipIdx == SIZE_MAX) return;
            auto* clip = track->clip(clipIdx);
            if (!clip) return;

            auto type = static_cast<EffectType>(effectType);
            auto& stack = clip->effects();

            if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<AddEffectCommand>(&stack, type));
            } else {
                stack.addEffect(createEffect(type));
            }

            // Select the clip and refresh Effect Controls + Program Monitor
            if (m_propertiesPanel) {
                m_propertiesPanel->setClip(clip, track);
                m_propertiesPanel->refreshEffects();
            }
            if (m_effectControlsPanel) {
                m_effectControlsPanel->setClip(clip, track);
                m_effectControlsPanel->refresh();
            }
            m_selectedClip = clip;
            m_selectedTrackIdx = trackIdx;
            m_selectedClipIdx = clipIdx;
            m_selectedGraphicLayerIdx = -1;

            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();

            spdlog::info("Effect '{}' added to clip '{}' via drag-drop",
                         effectTypeName(type), clip->label());
        });
    }

    // =====================================================================
    //  TRANSITION DRAG-DROP -> ADD TRANSITION AT CLIP EDGE
    // =====================================================================
    if (m_timelinePanel) {
        connect(m_timelinePanel, &TimelinePanel::transitionDroppedAtEdge,
                this, [this](size_t trackIdx, uint64_t leftClipId,
                             uint64_t rightClipId, int64_t editPointTick,
                             int transitionType) {
            if (!m_timeline) return;
            auto* track = m_timeline->track(trackIdx);
            if (!track) return;

            // Check if a transition already exists at this edit point
            for (size_t ti = 0; ti < track->transitionCount(); ++ti) {
                const Transition* existing = track->transition(ti);
                if (existing && existing->editPointTick == editPointTick)
                    return; // already exists
            }

            // Find clip indices
            size_t clipIdxA = SIZE_MAX;
            size_t clipIdxB = SIZE_MAX;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                const Clip* c = track->clip(ci);
                if (!c) continue;
                if (c->id() == leftClipId)  clipIdxA = ci;
                if (c->id() == rightClipId) clipIdxB = ci;
            }
            // Need at least one valid clip
            if (clipIdxA == SIZE_MAX && clipIdxB == SIZE_MAX) return;

            // Use whichever index is valid as both params if one is missing
            if (clipIdxA == SIZE_MAX) clipIdxA = clipIdxB;
            if (clipIdxB == SIZE_MAX) clipIdxB = clipIdxA;

            Transition trans;
            trans.type = static_cast<TransitionType>(transitionType);
            trans.duration = kDefaultTransitionDuration;
            trans.leftClipId = leftClipId;
            trans.rightClipId = rightClipId;
            trans.editPointTick = editPointTick;

            if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<AddTransitionCommand>(track, clipIdxA, clipIdxB, trans));
            } else {
                track->addTransition(trans);
            }

            invalidateCompositeCache();
            if (m_timelinePanel) m_timelinePanel->rebuildTracks();
            if (m_programMonitor) m_programMonitor->requestRefresh();

            spdlog::info("Transition type {} added via drag-drop at edit point {}",
                         transitionType, editPointTick);
        });
    }
}

} // namespace rt
