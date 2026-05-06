/*
 * TimelineWorkspaceOverlay.cpp - Shot switching and transform overlay for TimelineWorkspace.
 * Split from TimelineWorkspace.cpp for maintainability.
 */
#include "panels/timeline/TimelineWorkspace.h"
#include "CompositeService.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/properties/PropertiesPanel.h"
#include "viewport/Viewport.h"
#include "viewport/TransformOverlayWidget.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SpineClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include <QFileInfo>

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ShotPreset.h"
#endif

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QTimer>

#include <spdlog/spdlog.h>

#include <filesystem>
namespace rt {

// Transform overlay ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â update the Program Monitor handle overlay for the
// currently selected clip so the user can drag-to-move / drag-to-scale.
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â

// â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”
// applyShotSwitch â€” shared undo-aware shot switch used by both
// PropertiesPanel and ShotPanel signal handlers.
// â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”

void TimelineWorkspace::applyShotSwitch(uint64_t groupId, const std::string& newShotName)
{
    if (!m_timeline || !m_shotPresetManager || groupId == 0) {
        if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
        if (m_programMonitor) m_programMonitor->requestRefresh();
        return;
    }

    auto presetOpt = m_shotPresetManager->load(newShotName);
    if (!presetOpt) {
        spdlog::warn("TimelineWorkspace: shot preset '{}' not found", newShotName);
        if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
        if (m_programMonitor) m_programMonitor->requestRefresh();
        return;
    }

    const auto& preset = *presetOpt;
    spdlog::info("TimelineWorkspace: applying shot '{}' to group {}", newShotName, groupId);

    // â”€â”€ Snapshot old clips for undo â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    struct ClipSnapshot {
        size_t trackIndex;
        std::unique_ptr<Clip> clip;
    };
    auto oldClips = std::make_shared<std::vector<ClipSnapshot>>();
    auto oldShotName = std::make_shared<std::string>();

    // Find existing group's time position/duration
    int64_t groupStart = 0;
    int64_t groupDuration = 48000; // 1 second fallback
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        Track* trk = m_timeline->track(ti);
        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            Clip* c = trk->clip(ci);
            if (c && c->groupId() == groupId) {
                groupStart = c->timelineIn();
                groupDuration = c->duration();
                goto foundGroupApply;
            }
        }
    }
    foundGroupApply:

    // Clone old visual clips in this group (for undo)
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        Track* trk = m_timeline->track(ti);
        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            Clip* c = trk->clip(ci);
            if (c && c->groupId() == groupId && c->clipType() != ClipType::Audio) {
                if (oldShotName->empty() && !c->shotName().empty())
                    *oldShotName = c->shotName();
                oldClips->push_back({ti, c->clone()});
            }
        }
    }

    // â”€â”€ Build list of new clips â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto newClips = std::make_shared<std::vector<ClipSnapshot>>();

    const auto& order = preset.layerOrder();

    // â”€â”€ Ensure enough video tracks exist â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Count visible layers to determine the required track count.
    size_t neededTracks = 0;
    for (size_t li = 0; li < order.size(); ++li) {
        const auto& lr = order[li];
        bool vis = false;
        if (lr.type == LayerType::Background) {
            auto* bg = preset.background(lr.index);
            vis = (bg && bg->visible);
        } else {
            auto* ch = preset.character(lr.index);
            vis = (ch && ch->visible);
        }
        if (vis) ++neededTracks;
    }

    // Collect current video track indices
    std::vector<size_t> videoIndices;
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti)
        if (m_timeline->track(ti)->type() == TrackType::Video)
            videoIndices.push_back(ti);

    // Add tracks if needed â€” new tracks are inserted before audio,
    // so they append to the video-track range.
    while (videoIndices.size() < neededTracks) {
        (void)m_timeline->addVideoTrack("");
        // addVideoTrack inserts before the first audio track,
        // so the new index is one past the last video index.
        size_t newIdx = videoIndices.empty() ? 0 : videoIndices.back() + 1;
        videoIndices.push_back(newIdx);
    }

    // â”€â”€ Map layers to tracks by position â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // layerOrder[0] = FRONT (top of UI â†’ lowest index â†’ V3)
    // layerOrder[last] = BACK  (bottom of UI â†’ highest index â†’ V1)
    // Iterate back-to-front (oi from lastâ†’0), assign track positions
    // so that:
    //   back layer (BG)    â†’ highest video index â†’ V1 (bottom)
    //   front layer (char) â†’ lowest video index  â†’ VN (top)

    int layerIdx = 0;
    for (int oi = static_cast<int>(order.size()) - 1; oi >= 0; --oi) {
        const auto& ref = order[static_cast<size_t>(oi)];

        // Assign track by POSITION: layerIdx 0 (back/BG) â†’ last video
        // track index, layerIdx N-1 (front) â†’ first video track index.
        size_t trackPos = videoIndices.size() - 1 - static_cast<size_t>(layerIdx);
        size_t targetTrack = (trackPos < videoIndices.size())
            ? videoIndices[trackPos] : videoIndices.back();

        if (ref.type == LayerType::Background) {
            auto* bg = preset.background(ref.index);
            if (!bg || !bg->visible) continue;

            auto vc = std::make_unique<VideoClip>();
            vc->setMediaPath(bg->path);
            vc->setTimelineIn(groupStart);
            vc->setDuration(groupDuration);
            // Use original filename (no extension) as label
            QString bgFile = QString::fromStdString(bg->path);
            QString bgLabel = QFileInfo(bgFile).baseName();
            vc->setLabel(bgLabel.toStdString());
            vc->setGroupId(groupId);
            vc->setLayerId("background_" + std::to_string(ref.index));
            vc->setShotName(newShotName);
            constexpr float outW2 = 1920.0f, outH2 = 1080.0f;
            vc->positionX().addKeyframe(0, (bg->posX - 0.5f) * outW2);
            vc->positionY().addKeyframe(0, (bg->posY - 0.5f) * outH2);
            vc->scaleX().addKeyframe(0, bg->scale);
            vc->scaleY().addKeyframe(0, bg->scale);
            vc->opacity().addKeyframe(0, bg->opacity);
            if (bg->cropLeft > 0 || bg->cropRight > 0 || bg->cropTop > 0 || bg->cropBottom > 0)
                vc->setCrop(bg->cropLeft, bg->cropRight, bg->cropTop, bg->cropBottom);
            newClips->push_back({targetTrack, std::move(vc)});
        } else { // Character
            auto* ch = preset.character(ref.index);
            if (!ch || !ch->visible) continue;

            if (ch->isVideoCharacter()) {
                const std::string& videoPath = ch->activeVideoPath();
                if (videoPath.empty()) { ++layerIdx; continue; }
                auto vc = std::make_unique<VideoClip>();
                vc->setMediaPath(videoPath);
                vc->setTimelineIn(groupStart);
                vc->setDuration(groupDuration);
                // Use "CHARACTER - ANIMATION" as label
                std::string animLabel = ch->characterName;
                if (!ch->animation.empty())
                    animLabel += " - " + ch->animation;
                vc->setLabel(animLabel);
                vc->setShotName(newShotName);
                vc->setGroupId(groupId);
                vc->setLayerId("char_" + std::to_string(ref.index));
                // Character metadata for Properties panel controls
                vc->setCharacterName(ch->characterName);
                vc->setTalking(ch->isTalking);
                vc->setVideoMutePath(ch->videoMutePath);
                vc->setVideoTalkPath(ch->videoTalkPath);
                constexpr float cW = 1920.0f, cH = 1080.0f;
                vc->positionX().addKeyframe(0, (ch->posX - 0.5f) * cW);
                vc->positionY().addKeyframe(0, (ch->posY - 0.5f) * cH);
                vc->scaleX().addKeyframe(0, ch->scale);
                vc->scaleY().addKeyframe(0, ch->scale);
                vc->opacity().addKeyframe(0, ch->opacity);
                if (ch->cropLeft > 0 || ch->cropRight > 0 || ch->cropTop > 0 || ch->cropBottom > 0)
                    vc->setCrop(ch->cropLeft, ch->cropRight, ch->cropTop, ch->cropBottom);
                newClips->push_back({targetTrack, std::move(vc)});
            } else {
                auto sc = std::make_unique<SpineClip>();
                sc->setCharacterName(ch->characterName);
                sc->setOutfit(ch->outfit);
                sc->setStance(ch->stance);
                sc->setAnimationName(ch->animation);
                sc->setTalking(ch->isTalking);
                sc->setTimelineIn(groupStart);
                sc->setDuration(groupDuration);
                // Use "CHARACTER - ANIMATION" as label
                std::string animLabel = ch->characterName;
                if (!ch->animation.empty())
                    animLabel += " - " + ch->animation;
                sc->setLabel(animLabel);
                sc->setShotName(newShotName);
                sc->setGroupId(groupId);
                sc->setLayerId("char_" + std::to_string(ref.index));
                constexpr float sW = 1920.0f, sH = 1080.0f;
                // The COMPOSE 0.85 base-fit factor and the 0.9 pre-render
                // padding compensation are applied dynamically in the
                // compositor (compositeFrame) via *0.85/0.9 so that ALL
                // existing clips benefit without re-creation.
                sc->positionX().addKeyframe(0, (ch->posX - 0.5f) * sW);
                sc->positionY().addKeyframe(0, (ch->posY - 0.5f) * sH);
                sc->scaleX().addKeyframe(0, ch->scale);
                sc->scaleY().addKeyframe(0, ch->scale);
                sc->opacity().addKeyframe(0, ch->opacity);
                if (ch->cropLeft > 0 || ch->cropRight > 0 || ch->cropTop > 0 || ch->cropBottom > 0)
                    sc->setCrop(ch->cropLeft, ch->cropRight, ch->cropTop, ch->cropBottom);
                newClips->push_back({targetTrack, std::move(sc)});
            }
        }
        ++layerIdx;
    }

    // â”€â”€ Helper: replace group clips on timeline â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto replaceGroupClips = [this, groupId](
            const std::vector<ClipSnapshot>& source) {
        // Remove existing visual clips for this group
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            Track* trk = m_timeline->track(ti);
            for (int ci = static_cast<int>(trk->clipCount()) - 1; ci >= 0; --ci) {
                Clip* c = trk->clip(static_cast<size_t>(ci));
                if (c && c->groupId() == groupId && c->clipType() != ClipType::Audio)
                    trk->removeClip(static_cast<size_t>(ci));
            }
        }

        // Insert clones of the source clips.  When track indices are stale
        // (undo after track layout changed), validate the target is a video
        // track.  If it's audio, find the nearest video track instead.
        for (const auto& snap : source) {
            Track* trk = nullptr;
            size_t idx = snap.trackIndex;
            // Ensure the target is a video track
            if (idx < m_timeline->trackCount()) {
                trk = m_timeline->track(idx);
                if (trk && trk->type() != TrackType::Video)
                    trk = nullptr;
            }
            // Fallback: find the video track closest to the target index
            if (!trk) {
                // Search upward from target index for a video track
                for (size_t si = idx; si < m_timeline->trackCount(); ++si) {
                    Track* t = m_timeline->track(si);
                    if (t && t->type() == TrackType::Video) {
                        trk = t; break;
                    }
                }
                // Search downward if not found
                if (!trk) {
                    for (size_t si = idx; si > 0; --si) {
                        Track* t = m_timeline->track(si - 1);
                        if (t && t->type() == TrackType::Video) {
                            trk = t; break;
                        }
                    }
                }
                // Still not found — create a new video track
                if (!trk)
                    trk = m_timeline->addVideoTrack("");
            }
            if (trk && snap.clip)
                trk->addClip(snap.clip->clone());
        }

        // Update shot names on remaining group clips
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            Track* trk = m_timeline->track(ti);
            for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                Clip* c = trk->clip(ci);
                if (c && c->groupId() == groupId && !source.empty()) {
                    c->setShotName(source[0].clip->shotName());
                }
            }
        }

        // Invalidate cache first so compositor doesn't render stale data.
        // Then rebuild tracks and request fresh composite.
        invalidateCompositeCache();
        if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
        if (m_programMonitor) m_programMonitor->requestRefresh();
        // Auto-select first group clip so PropertiesPanel shows correct shot name
        if (m_propertiesPanel && !source.empty()) {
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                Track* trk = m_timeline->track(ti);
                for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                    Clip* c = trk->clip(ci);
                    if (c && c->groupId() == groupId && c->clipType() != ClipType::Audio) {
                        m_propertiesPanel->setClip(c);
                        goto selectDone;
                    }
                }
            }
            selectDone:;
        }
    };

    // â”€â”€ Execute via undo command â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (m_commandStack) {
        auto cmd = std::make_unique<LambdaCommand>(
            "Switch Shot to " + newShotName,
            [this, newClips, replaceGroupClips]() {
                replaceGroupClips(*newClips);
            },
            [this, oldClips, oldShotName, replaceGroupClips]() {
                for (auto& snap : *oldClips) {
                    if (snap.clip && !oldShotName->empty())
                        snap.clip->setShotName(*oldShotName);
                }
                replaceGroupClips(*oldClips);
            });
        cmd->execute();
        m_commandStack->pushWithoutExecute(std::move(cmd));
    } else {
        replaceGroupClips(*newClips);
    }

    spdlog::info("TimelineWorkspace: shot switch complete");
}

void TimelineWorkspace::scheduleOverlayRefresh()
{
    // Bump generation so stale timer callbacks are ignored.
    uint32_t gen = ++m_overlayRefreshGen;

    // Also force a recomposite so the displayed frame matches compositeWidth.
    invalidateCompositeCache();
    if (m_programMonitor) m_programMonitor->requestRefresh();

    // Schedule deferred overlay updates at increasing intervals.
    // By the time the later callbacks fire, the pipeline will have
    // composited and displayed the new frame, so srcWidth() is current.
    for (int delayMs : {0, 50, 150}) {
        QTimer::singleShot(delayMs, this, [this, gen]() {
            if (gen == m_overlayRefreshGen)
                updateTransformOverlay();
        });
    }
}

void TimelineWorkspace::updateTransformOverlay()
{
    if (!m_programMonitor || !m_programMonitor->viewport()) return;
    auto* vp = m_programMonitor->viewport();

    if (!m_selectedClip || !m_timeline) {
        vp->clearTransformOverlay();
        if (m_programMonitor->transformOverlay())
            m_programMonitor->transformOverlay()->clearTransformOverlay();
        return;
    }

    // Hide overlay when playhead is outside the clip's time range.
    int64_t clipEnd = m_selectedClip->timelineOut();
    int64_t clipIn = m_selectedClip->timelineIn();
    if (m_playbackController) {
        int64_t curTick = m_playbackController->currentTick();
        if (curTick < clipIn || curTick > clipEnd) {
            vp->clearTransformOverlay();
            if (m_programMonitor->transformOverlay())
                m_programMonitor->transformOverlay()->clearTransformOverlay();
            return;
        }
    }

    TransformOverlayInfo info;
    info.visible  = true;

    // Evaluate at the current playhead position relative to clip start
    const int64_t relTick = m_playbackController
        ? std::max(int64_t{0}, m_playbackController->currentTick() - m_selectedClip->timelineIn())
        : int64_t{0};

    // Per-layer transform for GraphicClip: when a specific layer is selected
    // in Essential Graphics, show the overlay sized around that layer only.
    if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
        auto* gc = static_cast<GraphicClip*>(m_selectedClip);
        if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
            auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
            const auto& xf = layer->transform();
            info.posX     = xf.posX.evaluate(relTick);
            info.posY     = xf.posY.evaluate(relTick);
            info.scaleX   = xf.scaleX.evaluate(relTick);
            info.scaleY   = xf.scaleY.evaluate(relTick);
            info.rotation = xf.rotation.evaluate(relTick);

            // Content-rect mode: the overlay will apply the EXACT same
            // QPainter transform that renderGraphicClip() uses, so the
            // bounding box matches the rendered content pixel-perfectly.
            // posX/posY/scale/rotation are the layer transform values;
            // contentL/T/R/B are the pre-transform canvas-space bounds.
            info.useContentRect = true;

            if (layer->layerType() == GraphicLayerType::Text) {
                auto* tl = static_cast<TextLayer*>(layer);
                QFont font(QString::fromStdString(tl->fontFamily()),
                           static_cast<int>(tl->fontSize()));
                font.setWeight(static_cast<QFont::Weight>(tl->fontWeight()));
                font.setItalic(tl->isItalic());
                float tracking = tl->tracking().evaluate(relTick);
                font.setLetterSpacing(QFont::AbsoluteSpacing, static_cast<qreal>(tracking));

                QString text = QString::fromStdString(tl->text());
                if (tl->allCaps()) text = text.toUpper();

                // Output dimensions — use the project output resolution.
                // The renderer always renders GraphicClip at full project res
                // then downscales, so text proportions are identical at all
                // display resolutions.  The overlay measures at the same
                // reference resolution to match.
                uint32_t outW = m_programMonitor ? m_programMonitor->outputWidth()  : 1920;
                uint32_t outH = m_programMonitor ? m_programMonitor->outputHeight() : 1080;
                if (outW == 0) outW = 1920;
                if (outH == 0) outH = 1080;

                // Use a very large rect so text is never clipped/wrapped by canvas bounds
                double bigW = static_cast<double>(outW) * 10.0;
                double bigH = static_cast<double>(outH) * 10.0;
                QRectF textRect(-bigW * 0.5 + static_cast<double>(outW) * 0.5,
                                -bigH * 0.5 + static_cast<double>(outH) * 0.5,
                                bigW, bigH);

                // Same alignment as renderGraphicClip
                int hAlign = Qt::AlignHCenter;
                switch (tl->alignment()) {
                    case GTextAlign::Left:    hAlign = Qt::AlignLeft;    break;
                    case GTextAlign::Center:  hAlign = Qt::AlignHCenter; break;
                    case GTextAlign::Right:   hAlign = Qt::AlignRight;   break;
                    case GTextAlign::Justify: hAlign = Qt::AlignJustify; break;
                }
                int vAlign = Qt::AlignVCenter;
                switch (tl->vAlignment()) {
                    case GTextVAlign::Top:    vAlign = Qt::AlignTop;     break;
                    case GTextVAlign::Middle: vAlign = Qt::AlignVCenter; break;
                    case GTextVAlign::Bottom: vAlign = Qt::AlignBottom;  break;
                }

                // Measure text bounds using QPainter::drawText — this is the
                // exact same code path as renderGraphicClip(), guaranteeing
                // pixel-perfect bounding box matching.
                QImage metricsCanvas(static_cast<int>(outW), static_cast<int>(outH),
                                     QImage::Format_ARGB32_Premultiplied);
                QPainter metricsP(&metricsCanvas);
                metricsP.setRenderHint(QPainter::Antialiasing, true);
                metricsP.setRenderHint(QPainter::TextAntialiasing, true);
                metricsP.setFont(font);
                QRectF textBounds;
                metricsP.drawText(textRect, hAlign | vAlign | Qt::TextWordWrap,
                                  text, &textBounds);
                metricsP.end();

                float pad = tl->fontSize() * 0.3f;
                info.contentL = static_cast<float>(textBounds.left())   - pad;
                info.contentT = static_cast<float>(textBounds.top())    - pad;
                info.contentR = static_cast<float>(textBounds.right())  + pad;
                info.contentB = static_cast<float>(textBounds.bottom()) + pad;

                spdlog::info("[Overlay] text='{}' outW={} outH={} textBounds=({:.1f},{:.1f},{:.1f},{:.1f}) "
                             "content=({:.1f},{:.1f},{:.1f},{:.1f}) posX={:.1f} posY={:.1f} scX={:.2f} scY={:.2f}",
                             tl->text(), outW, outH,
                             textBounds.left(), textBounds.top(), textBounds.width(), textBounds.height(),
                             info.contentL, info.contentT, info.contentR, info.contentB,
                             info.posX, info.posY, info.scaleX, info.scaleY);

                info.contentCanvasW = static_cast<float>(outW);
                info.contentCanvasH = static_cast<float>(outH);
            } else {
                auto* sl = static_cast<ShapeLayer*>(layer);
                float sw = sl->shapeWidth();
                float sh = sl->shapeHeight();
                // Use project output resolution for shapes (resolution-independent)
                uint32_t outW = m_programMonitor ? m_programMonitor->outputWidth()  : 1920;
                uint32_t outH = m_programMonitor ? m_programMonitor->outputHeight() : 1080;
                if (outW == 0) outW = 1920;
                if (outH == 0) outH = 1080;
                // Shapes are centered in the canvas
                float cx = static_cast<float>(outW) * 0.5f;
                float cy = static_cast<float>(outH) * 0.5f;
                info.contentL = cx - sw * 0.5f;
                info.contentT = cy - sh * 0.5f;
                info.contentR = cx + sw * 0.5f;
                info.contentB = cy + sh * 0.5f;
                info.contentCanvasW = static_cast<float>(outW);
                info.contentCanvasH = static_cast<float>(outH);
            }

            // Clip-level (outer) transform — applied by compositor on top of layer transform.
            info.clipPosX     = m_selectedClip->positionX().evaluate(relTick);
            info.clipPosY     = m_selectedClip->positionY().evaluate(relTick);
            info.clipScaleX   = m_selectedClip->scaleX().evaluate(relTick);
            info.clipScaleY   = m_selectedClip->scaleY().evaluate(relTick);
            info.clipRotation = m_selectedClip->rotation().evaluate(relTick);
        }
    } else {
        info.posX     = m_selectedClip->positionX().evaluate(relTick);
        info.posY     = m_selectedClip->positionY().evaluate(relTick);
        info.scaleX   = m_selectedClip->scaleX().evaluate(relTick);
        info.scaleY   = m_selectedClip->scaleY().evaluate(relTick);
        info.rotation = m_selectedClip->rotation().evaluate(relTick);
    }

    // Determine source dimensions for the bounding box.
    // For VideoClip / SpineClipÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢video fallback, look up the media info.
    // For GraphicClip, use the output resolution (graphics fill the canvas).
    // For GraphicClip without per-layer bounding box, use output resolution
    if (m_selectedClip->clipType() == ClipType::Graphic && info.srcW == 0 && info.srcH == 0) {
        info.srcW = m_programMonitor->outputWidth();
        info.srcH = m_programMonitor->outputHeight();
    }

    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<ImageClip*>(m_selectedClip)) {
        auto* imageClip = dynamic_cast<ImageClip*>(m_selectedClip);
        // Use stored source dimensions first
        if (imageClip->sourceWidth() > 0 && imageClip->sourceHeight() > 0) {
            info.srcW = imageClip->sourceWidth();
            info.srcH = imageClip->sourceHeight();
        } else if (m_mediaPool) {
            uint64_t handle = m_compositeService->findMediaHandle(imageClip->mediaPath());
            if (handle == 0) {
                handle = m_mediaPool->open(imageClip->mediaPath());
                if (handle != 0)
                    m_compositeService->registerMediaHandle(imageClip->mediaPath(), handle);
            }
            if (handle != 0) {
                const auto* mi = m_mediaPool->getInfo(handle);
                if (mi) {
                    info.srcW = mi->width;
                    info.srcH = mi->height;
                }
            }
        }
    }

    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<VideoClip*>(m_selectedClip)) {
        auto* videoClip = dynamic_cast<VideoClip*>(m_selectedClip);
        if (m_mediaPool) {
            // Try cached handle first
            uint64_t handle = m_compositeService->findMediaHandle(videoClip->mediaPath());
            if (handle == 0) {
                // Media not opened yet ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â open it now so we get correct dimensions.
                // compositeFrame() will find this handle on its next pass.
                handle = m_mediaPool->open(videoClip->mediaPath());
                if (handle == 0) {
                    // Try alternate extension (.webm ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬Â .mov)
                    namespace fs = std::filesystem;
                    fs::path vidPath(videoClip->mediaPath());
                    fs::path alt = vidPath;
                    if (vidPath.extension() == ".webm") alt.replace_extension(".mov");
                    else if (vidPath.extension() == ".mov") alt.replace_extension(".webm");
                    if (alt != vidPath) {
                        handle = m_mediaPool->open(alt);
                        if (handle == 0) {
                            fs::path candidate = fs::path("assets") / "videos" / alt.filename();
                            if (fs::exists(candidate))
                                handle = m_mediaPool->open(candidate);
                        }
                    }
                    if (handle == 0) {
                        fs::path candidate = fs::path("assets") / "videos" / vidPath.filename();
                        if (fs::exists(candidate))
                            handle = m_mediaPool->open(candidate);
                    }
                }
                if (handle != 0)
                    m_compositeService->registerMediaHandle(videoClip->mediaPath(), handle);
            }
            if (handle != 0) {
                const auto* mi = m_mediaPool->getInfo(handle);
                if (mi) {
                    info.srcW = mi->width;
                    info.srcH = mi->height;
                }
            }
        }
    }
#ifdef ROUNDTABLE_HAS_SPINE
    // For SpineClip: use shared spine bounds for the overlay size.
    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<SpineClip*>(m_selectedClip)) {
        auto* spineClip = static_cast<SpineClip*>(m_selectedClip);
        if (m_compositeService) {
            const auto* shared = m_compositeService->getSpineSharedDataForOverlay(
                spineClip->characterName(), spineClip->outfit(),
                static_cast<int>(spineClip->stance()));
            if (shared && shared->stableBoundsW > 1.0f && shared->stableBoundsH > 1.0f) {
                // Scale: same as compositor: fit height with 0.9 padding
                float refH = 1080.0f;
                float fitZoom = (refH / shared->stableBoundsH) * 0.9f;
                info.srcW = static_cast<uint32_t>(shared->stableBoundsW * fitZoom);
                info.srcH = static_cast<uint32_t>(shared->stableBoundsH * fitZoom);
            }
        }
    }
#endif

    // Fallback: if we still don't have dimensions, use the viewport's
    // frame dimensions (the composite output).  This at least makes the
    // bounding box match the visible canvas rather than an arbitrary 16:9.
    if (info.srcW == 0 || info.srcH == 0) {
        if (vp->frameWidth() > 0 && vp->frameHeight() > 0) {
            info.srcW = vp->frameWidth();
            info.srcH = vp->frameHeight();
        } else {
            info.srcW = 1920;
            info.srcH = 1080;
        }
    }

    vp->setTransformOverlay(info);

    // Packed-alpha: visible area is top half (same as compositor).
    // Characters use cover-fit like normal videos — no containFit override.
    if (info.srcW > 0 && info.srcH > 0 && !info.directSize && !info.useContentRect) {
        if (auto* vc = dynamic_cast<VideoClip*>(m_selectedClip)) {
            if (m_mediaPool) {
                uint64_t h = m_compositeService->findMediaHandle(vc->mediaPath());
                if (h != 0) {
                    const auto* mi2 = m_mediaPool->getInfo(h);
                    if (mi2 && mi2->packedAlpha) {
                        if (info.srcH > 1) info.srcH /= 2;
                    }
                }
            }
        }
    }

    // Also update the GPU overlay widget (TransformOverlayWidget)
    if (auto* overlay = m_programMonitor->transformOverlay()) {
        overlay->setTransformOverlay(info);

        // Pass mask data for overlay drawing
        if (m_selectedClip && m_selectedClip->maskCount() > 0)
            overlay->setMasks(&m_selectedClip->masks());
        else
            overlay->setMasks(nullptr);
    }
}

// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
// TitleClip CPU rendering ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â draw text to a BGRA CachedFrame using QPainter
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â


} // namespace rt
