// TimelineWorkspacePanelsWiring.cpp - Signal wiring for TimelineWorkspace.
// Split from TimelineWorkspacePanels.cpp for maintainability.

#include <volk.h>

#include <map>
#include <set>

#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/ClipRenderers.h"
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Theme.h"

#include "panels/audio/AudioMixer.h"
// ShotPanel removed � character/shot controls merged into PropertiesPanel
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

// Returns true for still-image media. Such files have no real "source duration",
// so trim operations should treat them as unbounded (Premiere-style infinite extension).
static bool isStillImagePath(const std::string& path)
{
    QString ext = QFileInfo(QString::fromStdString(path)).suffix().toLower();
    static const QStringList kImageExts = {
        "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff", "webp", "tga", "dds"
    };
    return kImageExts.contains(ext);
}

void TimelineWorkspace::wirePanelSignals()
{
    // =====================================================================
    //  VU METER POLLING -- feed AudioEngine::meter() -> timeline VU meter
    // =====================================================================
    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(33); // ~30 Hz
    connect(m_meterTimer, &QTimer::timeout, this, [this]() {
        if (!m_audioEngine || !m_timelineVUMeter) return;
        auto state = m_audioEngine->transportState();
        if (state != TransportState::Playing && state != TransportState::Scrubbing) {
            // Decay to silence when not playing
            float curL = m_timelineVUMeter->level(0);
            float curR = m_timelineVUMeter->level(1);
            if (curL < 0.001f && curR < 0.001f) {
                // Fully decayed � stop polling until next play/scrub
                m_timelineVUMeter->setLevel(0, 0.0f);
                m_timelineVUMeter->setLevel(1, 0.0f);
                m_meterTimer->stop();
                return;
            }
            constexpr float decay = 0.85f;
            m_timelineVUMeter->setLevel(0, curL * decay < 0.001f ? 0.0f : curL * decay);
            m_timelineVUMeter->setLevel(1, curR * decay < 0.001f ? 0.0f : curR * decay);
            return;
        }
        auto m = m_audioEngine->meter();
        m_timelineVUMeter->setLevel(0, m.peakL);
        m_timelineVUMeter->setLevel(1, m.peakR);
    });
    // Don't start yet � started on play/scrub via onStateChanged

    // =====================================================================
    wireClipSelectionSignals();
    // -- ShotPanel wiring removed: character/shot controls merged into PropertiesPanel --

    
    // =====================================================================
    //  MEDIA DRAG-DROP -> CREATE CLIP ON TIMELINE
    // =====================================================================
    // =====================================================================
    //  MEDIA DRAG-DROP -> CREATE CLIP ON TIMELINE
    // =====================================================================
    if (m_timelinePanel && m_timeline) {
        connect(m_timelinePanel, &TimelinePanel::mediaDropped,
                this, [this](const QString& filePath, uint64_t /*mediaHandle*/,
                             int64_t atTick, size_t trackIndex) {
            if (!m_timeline) return;

            // If no project or no sequences exist, prompt to create one
            if (!m_project || m_project->sequenceCount() == 0) {
                uint32_t fileW = 0, fileH = 0;
                double fileFps = 30.0;
                if (m_mediaPool) {
                    uint64_t h = m_mediaPool->open(filePath.toStdString());
                    if (h != 0) {
                        const auto* info = m_mediaPool->getInfo(h);
                        if (info) {
                            fileW = info->width;
                            fileH = info->height;
                            if (info->fps > 0.0) fileFps = info->fps;
                        }
                    }
                }
                QString resolutionStr = (fileW > 0 && fileH > 0)
                    ? QString("%1 x %2").arg(fileW).arg(fileH)
                    : QString("Unknown");
                QString fpsStr = QString::number(fileFps, 'f', 2);
                auto result = QMessageBox::question(
                    m_timelinePanel, "Create Sequence",
                    QString("No sequence is open.\n\n"
                            "Do you want to create a new sequence with this media?\n\n"
                            "File: %1\n"
                            "Resolution: %2\n"
                            "Frame rate: %3 fps\n\n"
                            "A new project will be created automatically.")
                        .arg(QFileInfo(filePath).fileName())
                        .arg(resolutionStr).arg(fpsStr),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
                if (result == QMessageBox::Yes)
                    emit requestNewProjectForMedia(filePath, atTick, trackIndex);
                return;
            }

            // Determine if this is an audio file by extension
            QString ext = QFileInfo(filePath).suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            bool isAudio = audioExts.contains(ext);

            // Pre-compute clip properties before the lambda captures
            std::string label = QFileInfo(filePath).baseName().toStdString();
            std::string path  = filePath.toStdString();

            // Detect video character files so the clip gets full metadata
            std::string vcCharName, vcMutePath, vcTalkPath;
            std::string vcOutfit, vcAnimName;
            float vcPosX = 0.0f, vcPosY = 0.0f, vcScale = 1.0f, vcOpacity = 1.0f;
            bool vcIsTalking = false;
            bool isCharacterClip = false;
            {
                std::string lowerName = QFileInfo(filePath).fileName().toLower().toStdString();
                const auto& vcTable = videoCharacterFiles();
                auto vcIt = vcTable.find(lowerName);
                if (vcIt != vcTable.end()) {
                    vcCharName = vcIt->second.charName;
                    vcMutePath = vcIt->second.mutePath;
                    vcTalkPath = vcIt->second.talkPath;
                    // For character/animation clips, label = "CHARACTER - ANIMATION"
                    label = vcCharName;
                    isCharacterClip = true;
                }

                // Fallback: detect character from AnimationVideoCache path.
                // Cache files live under assets/cache/animations/<charName>/<outfit>/<anim>.ext
                if (vcCharName.empty()) {
                    std::string generic = std::filesystem::path(path).generic_string();
                    const std::string marker = "assets/cache/animations/";
                    auto pos = generic.find(marker);
                    if (pos != std::string::npos) {
                        std::string rest = generic.substr(pos + marker.size());
                        // rest = "<charName>/<outfit>/<anim>.ext"
                        auto slash1 = rest.find('/');
                        if (slash1 != std::string::npos && slash1 > 0) {
                            vcCharName = rest.substr(0, slash1);
                            std::string afterChar = rest.substr(slash1 + 1);
                            auto slash2 = afterChar.find('/');
                            if (slash2 != std::string::npos && slash2 > 0) {
                                vcOutfit = afterChar.substr(0, slash2);
                                std::string fileName = afterChar.substr(slash2 + 1);
                                // Remove extension to get animation name
                                auto dotPos = fileName.rfind('.');
                                std::string stem = (dotPos != std::string::npos)
                                    ? fileName.substr(0, dotPos) : fileName;
                                std::string extStr = (dotPos != std::string::npos)
                                    ? fileName.substr(dotPos) : "";
                                // Detect _talk suffix
                                const std::string talkSuffix = "_talk";
                                if (stem.size() > talkSuffix.size() &&
                                    stem.compare(stem.size() - talkSuffix.size(),
                                                 talkSuffix.size(), talkSuffix) == 0) {
                                    // Dragged file is the talk variant
                                    std::string baseStem = stem.substr(0, stem.size() - talkSuffix.size());
                                    vcAnimName = baseStem;
                                    vcIsTalking = true;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + vcCharName + "/" + vcOutfit + "/";
                                    vcMutePath = prefix + baseStem + extStr;
                                    vcTalkPath = prefix + stem + extStr;
                                } else {
                                    vcAnimName = stem;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + vcCharName + "/" + vcOutfit + "/";
                                    vcMutePath = prefix + stem + extStr;
                                    vcTalkPath = prefix + stem + talkSuffix + extStr;
                                }
                                // For character/animation clips, label = "CHARACTER - ANIMATION"
                                label = vcCharName + " - " + vcAnimName;
                                isCharacterClip = true;
                            }
                        }
                    }
                }

                // Load the default shot preset to get character transform
                if (!vcCharName.empty() && m_shotPresetManager) {
                    std::string presetName = vcCharName + " (Default)";
                    auto preset = m_shotPresetManager->load(presetName);
                    if (preset) {
                        for (int ci2 = 0; ci2 < preset->characterCount(); ++ci2) {
                            auto* ch = preset->character(ci2);
                            if (!ch || ch->characterName != vcCharName || !ch->isVideoCharacter())
                                continue;
                            constexpr float cW = 1920.0f, cH = 1080.0f;
                            vcPosX    = (ch->posX - 0.5f) * cW;
                            vcPosY    = (ch->posY - 0.5f) * cH;
                            vcScale   = ch->scale;
                            vcOpacity = ch->opacity;
                            vcIsTalking = ch->isTalking;
                            break;
                        }
                    }
                }
            }

            // For all other (non-character) clips, label is always the original filename (no extension)
            if (!isCharacterClip) {
                label = QFileInfo(QString::fromStdString(path)).baseName().toStdString();
            }
            int64_t dur = secondsToTicks(5.0);
            double sourceFps = 0.0;
            bool mediaHasAudio = false;
            if (m_mediaPool) {
                auto handle = m_mediaPool->open(path);
                if (handle != InvalidMedia) {
                    const auto* info = m_mediaPool->getInfo(handle);
                    if (info) {
                        spdlog::info("DIAG-DROP mediaDropped '{}': info->duration={:.3f}s, "
                                     "frameCount={}, fps={:.2f}, hasAudio={}, videoIdx={}, audioIdx={}",
                                     path, info->duration, info->frameCount, info->fps,
                                     info->hasAudio, info->videoStreamIndex, info->audioStreamIndex);
                        sourceFps = info->fps;
                    }
                    if (info && info->duration > 0)
                        dur = secondsToTicks(info->duration);
                    if (info && info->hasAudio && !isAudio)
                        mediaHasAudio = true;
                }
            }
            spdlog::info("DIAG-DROP mediaDropped '{}': dur={} ticks ({:.3f}s), mediaHasAudio={}",
                         path, dur, ticksToSeconds(dur), mediaHasAudio);

            // Find the target track (don't create one yet â€” that happens inside the command)
            size_t targetTrackIdx = SIZE_MAX;
            bool needsNewTrack = false;
            const bool forceGhostVideoTrack = (trackIndex == (SIZE_MAX - 1));
            const bool forceGhostAudioTrack = (trackIndex == (SIZE_MAX - 2));

            if (isAudio && forceGhostAudioTrack) {
                needsNewTrack = true;
            } else if (!isAudio && forceGhostVideoTrack) {
                needsNewTrack = true;
            } else if (isAudio) {
                if (trackIndex < m_timeline->trackCount() &&
                    m_timeline->track(trackIndex)->type() == TrackType::Audio)
                    targetTrackIdx = trackIndex;
                if (targetTrackIdx == SIZE_MAX) {
                    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                        if (m_timeline->track(i)->type() == TrackType::Audio) {
                            targetTrackIdx = i;
                            break;
                        }
                    }
                }
                if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;
            } else {
                if (trackIndex < m_timeline->trackCount() &&
                    m_timeline->track(trackIndex)->type() == TrackType::Video)
                    targetTrackIdx = trackIndex;
                if (targetTrackIdx == SIZE_MAX) {
                    for (size_t i = m_timeline->trackCount(); i > 0; --i) {
                        if (m_timeline->track(i - 1)->type() == TrackType::Video) {
                            targetTrackIdx = i - 1;
                            break;
                        }
                    }
                }
                if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;
            }

            spdlog::info("DIAG-DROP mediaDropped routing: dropTrackIdx={} isAudio={} forceGhostVideo={} forceGhostAudio={} targetTrackIdx={} needsNewTrack={}",
                         trackIndex, isAudio, forceGhostVideoTrack, forceGhostAudioTrack,
                         targetTrackIdx, needsNewTrack);

            // Shared state for undo/redo: track the clip ID and whether we created a track
            auto clipId    = std::make_shared<uint64_t>(0);
            auto createdTk = std::make_shared<bool>(false);
            auto tkIdx     = std::make_shared<size_t>(targetTrackIdx);
            // Track overlap resolution state for undo
            auto overlapCmd = std::make_shared<std::unique_ptr<Command>>(nullptr);

            // Audio-companion state for video files that contain audio.
            // If the user dropped directly on an audio track, route the audio
            // companion there (mirrors the "drop video higher to pick upper
            // video track" behavior).  Otherwise fall back to the first audio
            // track.
            size_t audioTargetIdx = SIZE_MAX;
            bool needsNewAudioTrack = false;
            if (mediaHasAudio) {
                if (trackIndex < m_timeline->trackCount() &&
                    m_timeline->track(trackIndex)->type() == TrackType::Audio) {
                    audioTargetIdx = trackIndex;
                }
                if (audioTargetIdx == SIZE_MAX) {
                    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                        if (m_timeline->track(i)->type() == TrackType::Audio) {
                            audioTargetIdx = i;
                            break;
                        }
                    }
                }
                if (audioTargetIdx == SIZE_MAX) needsNewAudioTrack = true;
            }
            auto audioClipId      = std::make_shared<uint64_t>(0);
            auto audioCreatedTk   = std::make_shared<bool>(false);
            auto audioTkIdx       = std::make_shared<size_t>(audioTargetIdx);
            auto audioOverlapCmd  = std::make_shared<std::unique_ptr<Command>>(nullptr);

            auto refreshAfter = [this](bool trackStructureChanged = false) {
                if (trackStructureChanged)
                    m_timelinePanel->rebuildTracks();
                else
                    m_timelinePanel->refreshTrackContents();
                invalidateAudioSources();
                invalidateCompositeCache();
                warmAudioCacheAsync();
                if (m_programMonitor) m_programMonitor->requestRefresh();
            };

            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Add Media to Timeline",
                    /* execute / redo */
                    [this, isAudio, mediaHasAudio, path, label, atTick, dur, sourceFps,
                     needsNewTrack, needsNewAudioTrack, forceGhostVideoTrack, forceGhostAudioTrack,
                     clipId, createdTk, tkIdx, overlapCmd,
                     audioClipId, audioCreatedTk, audioTkIdx, audioOverlapCmd,
                     refreshAfter,
                     vcCharName, vcMutePath, vcTalkPath, vcOutfit, vcAnimName,
                     vcPosX, vcPosY, vcScale, vcOpacity, vcIsTalking]() {
                        // Create track if needed
                        if (needsNewTrack && *tkIdx == SIZE_MAX) {
                            Track* t = nullptr;
                            if (!isAudio && forceGhostVideoTrack) {
                                auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                                t = m_timeline->insertTrack(0, std::move(newTrack));
                            } else if (isAudio && forceGhostAudioTrack) {
                                t = m_timeline->addAudioTrack("A1");
                            } else {
                                t = isAudio ? m_timeline->addAudioTrack("A1")
                                            : m_timeline->addVideoTrack("V1");
                            }
                            // Find the index of the newly added track
                            for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                                if (m_timeline->track(i) == t) { *tkIdx = i; break; }
                            }
                            *createdTk = true;
                        }
                        Track* track = m_timeline->track(*tkIdx);
                        if (!track) return;
                        spdlog::info("DIAG-DROP mediaDropped execute: resolved track idx={} name='{}' type={}",
                                     *tkIdx, track->name(),
                                     track->type() == TrackType::Video ? "video" : "audio");

                        std::unique_ptr<Clip> clip;
                        if (isAudio) {
                            auto ac = std::make_unique<AudioClip>(path);
                            ac->setTimelineIn(atTick);
                            ac->setDuration(dur);
                            ac->setSourceDuration(dur);
                            ac->setLabel(label);
                            clip = std::move(ac);
                        } else {
                            auto vc = std::make_unique<VideoClip>(path);
                            vc->setTimelineIn(atTick);
                            vc->setDuration(dur);
                            // Still images have no real source duration ? treat as unbounded.
                            vc->setSourceDuration(isStillImagePath(path) ? 0 : dur);
                            vc->setSourceFps(sourceFps);
                            vc->setLabel(label);
                            if (!vcCharName.empty()) {
                                vc->setCharacterName(vcCharName);
                                vc->setVideoMutePath(vcMutePath);
                                vc->setVideoTalkPath(vcTalkPath);
                                vc->setOutfit(vcOutfit);
                                vc->setAnimationName(vcAnimName);
                                vc->setTalking(vcIsTalking);
                                vc->positionX().setDefaultValue(vcPosX);
                                vc->positionY().setDefaultValue(vcPosY);
                                vc->scaleX().setDefaultValue(vcScale);
                                vc->scaleY().setDefaultValue(vcScale);
                                vc->opacity().setDefaultValue(vcOpacity);
                            }
                            clip = std::move(vc);
                        }
                        *clipId = clip->id();
                        spdlog::info("DIAG-DROP mediaDropped clip id={} type={} "
                                     "timelineIn={} dur={} ({:.3f}s) sourceIn={} srcDur={}",
                                     *clipId, isAudio ? "audio" : "video",
                                     atTick, dur, dur/48000.0, 0, dur);
                        track->addClip(std::move(clip));

                        // Resolve overlaps (overwrite like Premiere Pro)
                        *overlapCmd = EditOperations::resolveOverlaps(
                            *m_timeline, *tkIdx, *clipId);
                        if (*overlapCmd) (*overlapCmd)->execute();

                        // Verify clip state after overlap resolution
                        {
                            size_t vi = track->findClipIndexById(*clipId);
                            if (vi < track->clipCount()) {
                                const auto* c = track->clip(vi);
                                spdlog::info("DIAG-DROP mediaDropped VERIFY clip id={} "
                                             "in={} dur={} ({:.3f}s) srcIn={} out={}",
                                             c->id(), c->timelineIn(), c->duration(),
                                             c->duration()/48000.0, c->sourceIn(),
                                             c->timelineOut());
                            }
                        }

                        // -- Create companion AudioClip for video+audio media --
                        if (mediaHasAudio) {
                            if (needsNewAudioTrack && *audioTkIdx == SIZE_MAX) {
                                Track* at = m_timeline->addAudioTrack("A1");
                                for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                                    if (m_timeline->track(i) == at) { *audioTkIdx = i; break; }
                                }
                                *audioCreatedTk = true;
                            }
                            Track* audioTrack = m_timeline->track(*audioTkIdx);
                            if (audioTrack) {
                                auto ac = std::make_unique<AudioClip>(path);
                                ac->setTimelineIn(atTick);
                                ac->setDuration(dur);
                                ac->setSourceDuration(dur);
                                ac->setLabel(label);
                                *audioClipId = ac->id();
                                spdlog::info("DIAG-DROP mediaDropped audioCompanion id={} "
                                             "in={} dur={} ({:.3f}s)",
                                             *audioClipId, atTick, dur, dur/48000.0);
                                audioTrack->addClip(std::move(ac));
                                *audioOverlapCmd = EditOperations::resolveOverlaps(
                                    *m_timeline, *audioTkIdx, *audioClipId);
                                if (*audioOverlapCmd) (*audioOverlapCmd)->execute();
                            }
                        }

                        const bool trackStructureChanged = (*createdTk || *audioCreatedTk);
                        refreshAfter(trackStructureChanged);
                    },
                    /* undo */
                    [this, clipId, createdTk, tkIdx, overlapCmd,
                     mediaHasAudio, audioClipId, audioCreatedTk, audioTkIdx, audioOverlapCmd,
                            refreshAfter]() {
                                const bool trackStructureChanged = (*createdTk || *audioCreatedTk);
                        // Undo audio companion first
                        if (mediaHasAudio) {
                            if (*audioOverlapCmd) (*audioOverlapCmd)->undo();
                            if (*audioTkIdx < m_timeline->trackCount()) {
                                Track* at = m_timeline->track(*audioTkIdx);
                                if (at) at->removeClipById(*audioClipId);
                            }
                            if (*audioCreatedTk) {
                                m_timeline->removeTrack(*audioTkIdx);
                                *audioTkIdx = SIZE_MAX;
                                *audioCreatedTk = false;
                            }
                        }

                        // Undo overlap resolution first
                        if (*overlapCmd) (*overlapCmd)->undo();

                        if (*tkIdx < m_timeline->trackCount()) {
                            Track* track = m_timeline->track(*tkIdx);
                            if (track) track->removeClipById(*clipId);
                        }
                        if (*createdTk) {
                            m_timeline->removeTrack(*tkIdx);
                            *tkIdx = SIZE_MAX;
                            *createdTk = false;
                        }
                        refreshAfter(trackStructureChanged);
                    }));
            } else {
                // No command stack â€” fall back to direct add
                Track* track = nullptr;
                bool trackStructureChanged = false;
                if (needsNewTrack) {
                    if (!isAudio && forceGhostVideoTrack) {
                        auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                        track = m_timeline->insertTrack(0, std::move(newTrack));
                        trackStructureChanged = true;
                    } else if (isAudio && forceGhostAudioTrack) {
                        track = m_timeline->addAudioTrack("A1");
                        trackStructureChanged = true;
                    } else {
                        track = isAudio ? m_timeline->addAudioTrack("A1")
                                        : m_timeline->addVideoTrack("V1");
                        trackStructureChanged = true;
                    }
                }
                else
                    track = m_timeline->track(targetTrackIdx);
                if (!track) return;

                std::unique_ptr<Clip> clip;
                if (isAudio) {
                    auto ac = std::make_unique<AudioClip>(path);
                    ac->setTimelineIn(atTick);
                    ac->setDuration(dur);
                    ac->setSourceDuration(dur);
                    ac->setLabel(label);
                    clip = std::move(ac);
                } else {
                    auto vc = std::make_unique<VideoClip>(path);
                    vc->setTimelineIn(atTick);
                    vc->setDuration(dur);
                    // Still images have no real source duration ? treat as unbounded.
                    vc->setSourceDuration(isStillImagePath(path) ? 0 : dur);
                    vc->setSourceFps(sourceFps);
                    vc->setLabel(label);
                    if (!vcCharName.empty()) {
                        vc->setCharacterName(vcCharName);
                        vc->setVideoMutePath(vcMutePath);
                        vc->setVideoTalkPath(vcTalkPath);
                        vc->setOutfit(vcOutfit);
                        vc->setAnimationName(vcAnimName);
                        vc->setTalking(vcIsTalking);
                        vc->positionX().setDefaultValue(vcPosX);
                        vc->positionY().setDefaultValue(vcPosY);
                        vc->scaleX().setDefaultValue(vcScale);
                        vc->scaleY().setDefaultValue(vcScale);
                        vc->opacity().setDefaultValue(vcOpacity);
                    }
                    clip = std::move(vc);
                }
                uint64_t cid = clip->id();
                track->addClip(std::move(clip));

                // Resolve overlaps in fallback path
                size_t fbTkIdx = SIZE_MAX;
                for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                    if (m_timeline->track(i) == track) { fbTkIdx = i; break; }
                }
                if (fbTkIdx != SIZE_MAX) {
                    auto cmd = EditOperations::resolveOverlaps(*m_timeline, fbTkIdx, cid);
                    if (cmd) cmd->execute();
                }

                // -- Create companion AudioClip for video+audio media (fallback path) --
                if (mediaHasAudio) {
                    Track* audioTrack = nullptr;
                    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                        if (m_timeline->track(i)->type() == TrackType::Audio) {
                            audioTrack = m_timeline->track(i);
                            break;
                        }
                    }
                    if (!audioTrack) {
                        audioTrack = m_timeline->addAudioTrack("A1");
                        trackStructureChanged = true;
                    }
                    if (audioTrack) {
                        auto ac = std::make_unique<AudioClip>(path);
                        ac->setTimelineIn(atTick);
                        ac->setDuration(dur);
                        ac->setSourceDuration(dur);
                        ac->setLabel(label);
                        uint64_t acid = ac->id();
                        audioTrack->addClip(std::move(ac));
                        size_t fbAudioIdx = SIZE_MAX;
                        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                            if (m_timeline->track(i) == audioTrack) { fbAudioIdx = i; break; }
                        }
                        if (fbAudioIdx != SIZE_MAX) {
                            auto acmd = EditOperations::resolveOverlaps(*m_timeline, fbAudioIdx, acid);
                            if (acmd) acmd->execute();
                        }
                    }
                }

                refreshAfter(trackStructureChanged);
            }

            spdlog::info("Media dropped on timeline: '{}' at tick {}",
                         path, atTick);
        });

        // Source Monitor drag with in/out region
        connect(m_timelinePanel, &TimelinePanel::mediaDroppedWithRegion,
                this, [this](const QString& filePath, uint64_t /*mediaHandle*/,
                             int64_t atTick, size_t trackIndex,
                             int64_t sourceIn, int64_t sourceOut) {
            if (!m_timeline) return;

            QString ext = QFileInfo(filePath).suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            bool isAudio = audioExts.contains(ext);

            std::string label = QFileInfo(filePath).baseName().toStdString();
            std::string path  = filePath.toStdString();
            int64_t dur = sourceOut - sourceIn;
            if (dur <= 0) dur = secondsToTicks(5.0);

            spdlog::info("DIAG-DROP mediaDroppedWithRegion '{}': sourceIn={} ({:.3f}s) "
                         "sourceOut={} ({:.3f}s) dur={} ({:.3f}s)",
                         path, sourceIn, sourceIn/48000.0,
                         sourceOut, sourceOut/48000.0, dur, dur/48000.0);

            // Query full media duration for sourceDuration (extent limit)
            int64_t sourceDur = dur;  // fallback: region length
            double sourceFps = 0.0;
            bool mediaHasAudio = false;
            if (m_mediaPool) {
                auto handle = m_mediaPool->open(path);
                if (handle != InvalidMedia) {
                    const auto* info = m_mediaPool->getInfo(handle);
                    if (info) {
                        spdlog::info("DIAG-DROP mediaDroppedWithRegion '{}': info->duration={:.3f}s, "
                                     "frameCount={}, fps={:.2f}, hasAudio={}",
                                     path, info->duration, info->frameCount, info->fps, info->hasAudio);
                        sourceFps = info->fps;
                    }
                    if (info && info->duration > 0)
                        sourceDur = secondsToTicks(info->duration);
                    if (info && info->hasAudio && !isAudio)
                        mediaHasAudio = true;
                }
            }
            spdlog::info("DIAG-DROP mediaDroppedWithRegion '{}': final dur={} ({:.3f}s) "
                         "sourceDur={} ({:.3f}s) mediaHasAudio={}",
                         path, dur, dur/48000.0, sourceDur, sourceDur/48000.0, mediaHasAudio);

            // Detect video character files
            std::string vcCharName2, vcMutePath2, vcTalkPath2;
            std::string vcOutfit2, vcAnimName2;
            float vcPosX2 = 0.0f, vcPosY2 = 0.0f, vcScale2 = 1.0f, vcOpacity2 = 1.0f;
            bool vcIsTalking2 = false;
            {
                std::string lowerName = QFileInfo(filePath).fileName().toLower().toStdString();
                const auto& vcTable = videoCharacterFiles();
                auto vcIt = vcTable.find(lowerName);
                if (vcIt != vcTable.end()) {
                    vcCharName2 = vcIt->second.charName;
                    vcMutePath2 = vcIt->second.mutePath;
                    vcTalkPath2 = vcIt->second.talkPath;
                    label = vcCharName2;
                }

                // Fallback: detect character from AnimationVideoCache path.
                // Cache files live under assets/cache/animations/<charName>/<outfit>/<anim>.ext
                if (vcCharName2.empty()) {
                    std::string generic = std::filesystem::path(path).generic_string();
                    const std::string marker = "assets/cache/animations/";
                    auto pos = generic.find(marker);
                    if (pos != std::string::npos) {
                        std::string rest = generic.substr(pos + marker.size());
                        auto slash1 = rest.find('/');
                        if (slash1 != std::string::npos && slash1 > 0) {
                            vcCharName2 = rest.substr(0, slash1);
                            label = vcCharName2;
                            std::string afterChar = rest.substr(slash1 + 1);
                            auto slash2 = afterChar.find('/');
                            if (slash2 != std::string::npos && slash2 > 0) {
                                vcOutfit2 = afterChar.substr(0, slash2);
                                std::string fileName = afterChar.substr(slash2 + 1);
                                auto dotPos = fileName.rfind('.');
                                std::string stem = (dotPos != std::string::npos)
                                    ? fileName.substr(0, dotPos) : fileName;
                                std::string extStr = (dotPos != std::string::npos)
                                    ? fileName.substr(dotPos) : "";
                                const std::string talkSuffix = "_talk";
                                if (stem.size() > talkSuffix.size() &&
                                    stem.compare(stem.size() - talkSuffix.size(),
                                                 talkSuffix.size(), talkSuffix) == 0) {
                                    std::string baseStem = stem.substr(0, stem.size() - talkSuffix.size());
                                    vcAnimName2 = baseStem;
                                    vcIsTalking2 = true;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + vcCharName2 + "/" + vcOutfit2 + "/";
                                    vcMutePath2 = prefix + baseStem + extStr;
                                    vcTalkPath2 = prefix + stem + extStr;
                                } else {
                                    vcAnimName2 = stem;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + vcCharName2 + "/" + vcOutfit2 + "/";
                                    vcMutePath2 = prefix + stem + extStr;
                                    vcTalkPath2 = prefix + stem + talkSuffix + extStr;
                                }
                            }
                        }
                    }
                }

                if (!vcCharName2.empty() && m_shotPresetManager) {
                    std::string presetName = vcCharName2 + " (Default)";
                    auto preset = m_shotPresetManager->load(presetName);
                    if (preset) {
                        for (int ci2 = 0; ci2 < preset->characterCount(); ++ci2) {
                            auto* ch = preset->character(ci2);
                            if (!ch || ch->characterName != vcCharName2 || !ch->isVideoCharacter())
                                continue;
                            constexpr float cW = 1920.0f, cH = 1080.0f;
                            vcPosX2    = (ch->posX - 0.5f) * cW;
                            vcPosY2    = (ch->posY - 0.5f) * cH;
                            vcScale2   = ch->scale;
                            vcOpacity2 = ch->opacity;
                            vcIsTalking2 = ch->isTalking;
                            break;
                        }
                    }
                }
            }

            size_t targetTrackIdx = SIZE_MAX;
            bool needsNewTrack = false;
            const bool forceGhostVideoTrack = (trackIndex == (SIZE_MAX - 1));
            const bool forceGhostAudioTrack = (trackIndex == (SIZE_MAX - 2));

            if (isAudio && forceGhostAudioTrack) {
                needsNewTrack = true;
            } else if (!isAudio && forceGhostVideoTrack) {
                needsNewTrack = true;
            } else if (isAudio) {
                if (trackIndex < m_timeline->trackCount() &&
                    m_timeline->track(trackIndex)->type() == TrackType::Audio)
                    targetTrackIdx = trackIndex;
                if (targetTrackIdx == SIZE_MAX) {
                    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                        if (m_timeline->track(i)->type() == TrackType::Audio) {
                            targetTrackIdx = i; break;
                        }
                    }
                }
                if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;
            } else {
                if (trackIndex < m_timeline->trackCount() &&
                    m_timeline->track(trackIndex)->type() == TrackType::Video)
                    targetTrackIdx = trackIndex;
                if (targetTrackIdx == SIZE_MAX) {
                    for (size_t i = m_timeline->trackCount(); i > 0; --i) {
                        if (m_timeline->track(i - 1)->type() == TrackType::Video) {
                            targetTrackIdx = i - 1; break;
                        }
                    }
                }
                if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;
            }

            spdlog::info("DIAG-DROP mediaDroppedWithRegion routing: dropTrackIdx={} isAudio={} forceGhostVideo={} forceGhostAudio={} targetTrackIdx={} needsNewTrack={}",
                         trackIndex, isAudio, forceGhostVideoTrack, forceGhostAudioTrack,
                         targetTrackIdx, needsNewTrack);

            auto clipId    = std::make_shared<uint64_t>(0);
            auto createdTk = std::make_shared<bool>(false);
            auto tkIdx     = std::make_shared<size_t>(targetTrackIdx);
            auto overlapCmd2 = std::make_shared<std::unique_ptr<Command>>(nullptr);

            // Audio-companion state for video+audio media
            size_t audioTargetIdx2 = SIZE_MAX;
            bool needsNewAudioTrack2 = false;
            if (mediaHasAudio) {
                for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                    if (m_timeline->track(i)->type() == TrackType::Audio) {
                        audioTargetIdx2 = i; break;
                    }
                }
                if (audioTargetIdx2 == SIZE_MAX) needsNewAudioTrack2 = true;
            }
            auto audioClipId2      = std::make_shared<uint64_t>(0);
            auto audioCreatedTk2   = std::make_shared<bool>(false);
            auto audioTkIdx2       = std::make_shared<size_t>(audioTargetIdx2);
            auto audioOverlapCmd2  = std::make_shared<std::unique_ptr<Command>>(nullptr);

            auto refreshAfter = [this](bool trackStructureChanged = false) {
                if (trackStructureChanged)
                    m_timelinePanel->rebuildTracks();
                else
                    m_timelinePanel->refreshTrackContents();
                invalidateAudioSources();
                invalidateCompositeCache();
                warmAudioCacheAsync();
                if (m_programMonitor) m_programMonitor->requestRefresh();
            };

            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Add Source Region to Timeline",
                    /* execute / redo */
                    [this, isAudio, mediaHasAudio, path, label, atTick, dur, sourceDur, sourceIn, sourceFps,
                     needsNewTrack, needsNewAudioTrack2, forceGhostVideoTrack, forceGhostAudioTrack,
                     clipId, createdTk, tkIdx, overlapCmd2,
                     audioClipId2, audioCreatedTk2, audioTkIdx2, audioOverlapCmd2,
                     refreshAfter,
                     vcCharName2, vcMutePath2, vcTalkPath2, vcOutfit2, vcAnimName2,
                     vcPosX2, vcPosY2, vcScale2, vcOpacity2, vcIsTalking2]() {
                        if (needsNewTrack && *tkIdx == SIZE_MAX) {
                            Track* t = nullptr;
                            if (!isAudio && forceGhostVideoTrack) {
                                auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                                t = m_timeline->insertTrack(0, std::move(newTrack));
                            } else if (isAudio && forceGhostAudioTrack) {
                                t = m_timeline->addAudioTrack("A1");
                            } else {
                                t = isAudio ? m_timeline->addAudioTrack("A1")
                                            : m_timeline->addVideoTrack("V1");
                            }
                            for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                                if (m_timeline->track(i) == t) { *tkIdx = i; break; }
                            }
                            *createdTk = true;
                        }
                        Track* track = m_timeline->track(*tkIdx);
                        if (!track) return;
                        spdlog::info("DIAG-DROP mediaDroppedWithRegion execute: resolved track idx={} name='{}' type={}",
                                     *tkIdx, track->name(),
                                     track->type() == TrackType::Video ? "video" : "audio");

                        std::unique_ptr<Clip> clip;
                        if (isAudio) {
                            auto ac = std::make_unique<AudioClip>(path);
                            ac->setTimelineIn(atTick);
                            ac->setDuration(dur);
                            ac->setSourceDuration(sourceDur);
                            ac->setSourceIn(sourceIn);
                            ac->setLabel(label);
                            clip = std::move(ac);
                        } else {
                            auto vc = std::make_unique<VideoClip>(path);
                            vc->setTimelineIn(atTick);
                            vc->setDuration(dur);
                            // Still images have no real source duration ? treat as unbounded.
                            vc->setSourceDuration(isStillImagePath(path) ? 0 : sourceDur);
                            vc->setSourceIn(sourceIn);
                            vc->setSourceFps(sourceFps);
                            vc->setLabel(label);
                            if (!vcCharName2.empty()) {
                                vc->setCharacterName(vcCharName2);
                                vc->setVideoMutePath(vcMutePath2);
                                vc->setVideoTalkPath(vcTalkPath2);
                                vc->setOutfit(vcOutfit2);
                                vc->setAnimationName(vcAnimName2);
                                vc->setTalking(vcIsTalking2);
                                vc->positionX().setDefaultValue(vcPosX2);
                                vc->positionY().setDefaultValue(vcPosY2);
                                vc->scaleX().setDefaultValue(vcScale2);
                                vc->scaleY().setDefaultValue(vcScale2);
                                vc->opacity().setDefaultValue(vcOpacity2);
                            }
                            clip = std::move(vc);
                        }
                        *clipId = clip->id();
                        spdlog::info("DIAG-DROP mediaDroppedWithRegion clip id={} type={} "
                                     "timelineIn={} dur={} ({:.3f}s) sourceIn={} srcDur={} ({:.3f}s)",
                                     *clipId, isAudio ? "audio" : "video",
                                     atTick, dur, dur/48000.0, sourceIn, sourceDur, sourceDur/48000.0);
                        track->addClip(std::move(clip));

                        // Resolve overlaps (overwrite like Premiere Pro)
                        *overlapCmd2 = EditOperations::resolveOverlaps(
                            *m_timeline, *tkIdx, *clipId);
                        if (*overlapCmd2) (*overlapCmd2)->execute();

                        // Verify clip state after overlap resolution
                        {
                            size_t vi = track->findClipIndexById(*clipId);
                            if (vi < track->clipCount()) {
                                const auto* c = track->clip(vi);
                                spdlog::info("DIAG-DROP mediaDroppedWithRegion VERIFY clip id={} "
                                             "in={} dur={} ({:.3f}s) srcIn={} out={}",
                                             c->id(), c->timelineIn(), c->duration(),
                                             c->duration()/48000.0, c->sourceIn(),
                                             c->timelineOut());
                            }
                        }

                        // -- Create companion AudioClip for video+audio media --
                        if (mediaHasAudio) {
                            if (needsNewAudioTrack2 && *audioTkIdx2 == SIZE_MAX) {
                                Track* at = m_timeline->addAudioTrack("A1");
                                for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                                    if (m_timeline->track(i) == at) { *audioTkIdx2 = i; break; }
                                }
                                *audioCreatedTk2 = true;
                            }
                            Track* audioTrack = m_timeline->track(*audioTkIdx2);
                            if (audioTrack) {
                                auto ac = std::make_unique<AudioClip>(path);
                                ac->setTimelineIn(atTick);
                                ac->setDuration(dur);
                                ac->setSourceDuration(sourceDur);
                                ac->setSourceIn(sourceIn);
                                ac->setLabel(label);
                                *audioClipId2 = ac->id();
                                audioTrack->addClip(std::move(ac));
                                *audioOverlapCmd2 = EditOperations::resolveOverlaps(
                                    *m_timeline, *audioTkIdx2, *audioClipId2);
                                if (*audioOverlapCmd2) (*audioOverlapCmd2)->execute();
                            }
                        }

                        const bool trackStructureChanged = (*createdTk || *audioCreatedTk2);
                        refreshAfter(trackStructureChanged);
                    },
                    /* undo */
                    [this, clipId, createdTk, tkIdx, overlapCmd2,
                     mediaHasAudio, audioClipId2, audioCreatedTk2, audioTkIdx2, audioOverlapCmd2,
                            refreshAfter]() {
                                const bool trackStructureChanged = (*createdTk || *audioCreatedTk2);
                        // Undo audio companion first
                        if (mediaHasAudio) {
                            if (*audioOverlapCmd2) (*audioOverlapCmd2)->undo();
                            if (*audioTkIdx2 < m_timeline->trackCount()) {
                                Track* at = m_timeline->track(*audioTkIdx2);
                                if (at) at->removeClipById(*audioClipId2);
                            }
                            if (*audioCreatedTk2) {
                                m_timeline->removeTrack(*audioTkIdx2);
                                *audioTkIdx2 = SIZE_MAX;
                                *audioCreatedTk2 = false;
                            }
                        }

                        // Undo overlap resolution
                        if (*overlapCmd2) (*overlapCmd2)->undo();

                        if (*tkIdx < m_timeline->trackCount()) {
                            Track* track = m_timeline->track(*tkIdx);
                            if (track) track->removeClipById(*clipId);
                        }
                        if (*createdTk) {
                            m_timeline->removeTrack(*tkIdx);
                            *tkIdx = SIZE_MAX;
                            *createdTk = false;
                        }
                        refreshAfter(trackStructureChanged);
                    }));
            }

            spdlog::info("Source region dropped on timeline: '{}' at tick {}, sourceIn={} dur={}",
                         path, atTick, sourceIn, dur);
        });

        // External file drop from Windows Explorer ? add to bin + timeline
        connect(m_timelinePanel, &TimelinePanel::externalFileDropped,
                this, [this](const QString& filePath,
                             int64_t atTick, size_t trackIndex) {
            if (!m_timeline) return;

            // If no project or no sequences exist, prompt to create one
            if (!m_project || m_project->sequenceCount() == 0) {
                uint32_t fileW = 0, fileH = 0;
                double fileFps = 30.0;
                if (m_mediaPool) {
                    uint64_t h = m_mediaPool->open(filePath.toStdString());
                    if (h != 0) {
                        const auto* info = m_mediaPool->getInfo(h);
                        if (info) {
                            fileW = info->width;
                            fileH = info->height;
                            if (info->fps > 0.0) fileFps = info->fps;
                        }
                    }
                }
                QString resolutionStr = (fileW > 0 && fileH > 0)
                    ? QString("%1 x %2").arg(fileW).arg(fileH)
                    : QString("Unknown");
                QString fpsStr = QString::number(fileFps, 'f', 2);
                auto result = QMessageBox::question(
                    m_timelinePanel, "Create Sequence",
                    QString("No sequence is open.\n\n"
                            "Do you want to create a new sequence with this media?\n\n"
                            "File: %1\n"
                            "Resolution: %2\n"
                            "Frame rate: %3 fps\n\n"
                            "A new project will be created automatically.")
                        .arg(QFileInfo(filePath).fileName())
                        .arg(resolutionStr).arg(fpsStr),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
                if (result == QMessageBox::Yes)
                    emit requestNewProjectForMedia(filePath, atTick, trackIndex);
                return;
            }

            // Auto-add to the Project Bin (skip if already present)
            if (m_projectBin) {
                namespace fs = std::filesystem;
                m_projectBin->addFiles({ fs::path(filePath.toStdWString()) });
            }

            // Open in MediaPool and forward to the normal media-drop path
            uint64_t handle = 0;
            if (m_mediaPool)
                handle = m_mediaPool->open(filePath.toStdString());

            emit m_timelinePanel->mediaDropped(filePath, handle, atTick, trackIndex);
        });
    }

    // =====================================================================
    //  NEST SELECTED CLIPS -> CREATE NESTED SEQUENCE
    // =====================================================================
    if (m_timelinePanel && m_timeline) {
        connect(m_timelinePanel, &TimelinePanel::nestSelectedClips,
                this, [this](const std::vector<ClipRef>& clips, const QString& nestName) {
            if (!m_timeline || !m_project || !m_commandStack || clips.empty()) return;

            // Find the time range spanned by selected clips
            int64_t minTick = std::numeric_limits<int64_t>::max();
            int64_t maxTick = std::numeric_limits<int64_t>::min();
            size_t targetTrackIdx = SIZE_MAX;

            for (const auto& cr : clips) {
                auto* trk = m_timeline->track(cr.trackIndex);
                if (!trk) continue;
                size_t ci = trk->findClipIndexById(cr.clipId);
                if (ci >= trk->clipCount()) continue;
                auto* c = trk->clip(ci);
                if (!c) continue;
                minTick = std::min(minTick, c->timelineIn());
                maxTick = std::max(maxTick, c->timelineOut());
                if (targetTrackIdx == SIZE_MAX ||
                    cr.trackIndex < targetTrackIdx)
                    targetTrackIdx = cr.trackIndex;
            }
            if (minTick >= maxTick || targetTrackIdx == SIZE_MAX) return;

            // Save clones of the original clips BEFORE the operation so
            // undo can restore them precisely (including original IDs).
            struct SavedClip {
                size_t trackIndex;
                uint64_t clipId;
                std::shared_ptr<Clip> clonedClip;  // shared for lambda capture
            };
            auto savedClips = std::make_shared<std::vector<SavedClip>>();
            for (const auto& cr : clips) {
                auto* trk = m_timeline->track(cr.trackIndex);
                if (!trk) continue;
                size_t ci = trk->findClipIndexById(cr.clipId);
                if (ci >= trk->clipCount()) continue;
                auto* srcClip = trk->clip(ci);
                if (!srcClip) continue;
                SavedClip sc;
                sc.trackIndex = cr.trackIndex;
                sc.clipId     = cr.clipId;
                sc.clonedClip = std::shared_ptr<Clip>(srcClip->clone().release());
                savedClips->push_back(std::move(sc));
            }

            // Shared state for execute / undo
            auto seqIdx     = std::make_shared<size_t>(SIZE_MAX);
            auto seqClipId  = std::make_shared<uint64_t>(0);
            auto targetTk   = std::make_shared<size_t>(targetTrackIdx);
            auto savedMin   = minTick;
            auto savedMax   = maxTick;
            auto name       = nestName.toStdString();

            auto refreshAfter = [this]() {
                m_selectedClip = nullptr;
                m_selectedGraphicLayerIdx = -1;
                m_timelinePanel->selection().clear();
                if (m_effectControlsPanel) m_effectControlsPanel->clearClip();
                if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->clearClip();
                if (m_ColorGradingPanel) m_ColorGradingPanel->clearClip();
                if (m_propertiesPanel) m_propertiesPanel->clearClip();
                if (m_programMonitor && m_programMonitor->viewport())
                    m_programMonitor->viewport()->clearTransformOverlay();
                if (m_programMonitor && m_programMonitor->transformOverlay())
                    m_programMonitor->transformOverlay()->clearTransformOverlay();
                m_timelinePanel->refreshTrackContents();
                emit m_timelinePanel->selectionChanged();
                invalidateCompositeCache();
                if (m_programMonitor) m_programMonitor->requestRefresh();
                if (m_projectBin) m_projectBin->refreshSequences();
            };

            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Nest Selected Clips",
                /* execute / redo */
                [this, savedClips, seqIdx, seqClipId, targetTk, savedMin, savedMax, name, refreshAfter]() {
                    // Create a new sequence for the nested content
                    *seqIdx = m_project->sequenceCount();
                    auto* nestedTimeline = m_project->addSequence(name);
                    if (!nestedTimeline) return;

                    // Strip the default V1+A1 tracks
                    while (nestedTimeline->trackCount() > 0)
                        nestedTimeline->removeTrack(0);

                    // Collect source track indices
                    std::set<size_t> usedTrackIndices;
                    for (const auto& sc : *savedClips)
                        usedTrackIndices.insert(sc.trackIndex);

                    // Mirror tracks: video first, then audio
                    std::map<size_t, size_t> trackMap;
                    for (size_t si : usedTrackIndices) {
                        auto* srcTrack = m_timeline->track(si);
                        if (!srcTrack || srcTrack->type() != TrackType::Video) continue;
                        size_t ni = nestedTimeline->trackCount();
                        nestedTimeline->addVideoTrack(srcTrack->name());
                        trackMap[si] = ni;
                    }
                    for (size_t si : usedTrackIndices) {
                        auto* srcTrack = m_timeline->track(si);
                        if (!srcTrack || srcTrack->type() != TrackType::Audio) continue;
                        size_t ni = nestedTimeline->trackCount();
                        nestedTimeline->addAudioTrack(srcTrack->name());
                        trackMap[si] = ni;
                    }

                    // Clone saved clips into the nested timeline
                    for (const auto& sc : *savedClips) {
                        auto mapIt = trackMap.find(sc.trackIndex);
                        if (mapIt == trackMap.end()) continue;
                        auto* dstTrack = nestedTimeline->track(mapIt->second);
                        if (!dstTrack) continue;
                        auto cloned = sc.clonedClip->clone();
                        cloned->setTimelineIn(sc.clonedClip->timelineIn() - savedMin);
                        cloned->setDuration(sc.clonedClip->duration());
                        cloned->setSourceIn(sc.clonedClip->sourceIn());
                        dstTrack->addClip(std::move(cloned));
                    }

                    // Remove the original clips from the current timeline
                    for (const auto& sc : *savedClips) {
                        auto* trk = m_timeline->track(sc.trackIndex);
                        if (trk) trk->removeClipById(sc.clipId);
                    }

                    // Insert a SequenceClip in their place
                    auto* targetTrack = m_timeline->track(*targetTk);
                    if (targetTrack && targetTrack->type() == TrackType::Video) {
                        auto seqClip = std::make_unique<SequenceClip>();
                        seqClip->setSequenceIndex(*seqIdx);
                        seqClip->setSequenceName(name);
                        seqClip->setLabel(name);
                        seqClip->setTimelineIn(savedMin);
                        seqClip->setDuration(savedMax - savedMin);
                        *seqClipId = seqClip->id();
                        targetTrack->addClip(std::move(seqClip));
                    }

                    refreshAfter();
                },
                /* undo */
                [this, savedClips, seqIdx, seqClipId, targetTk, refreshAfter]() {
                    // Remove the SequenceClip from the target track
                    if (*targetTk < m_timeline->trackCount()) {
                        auto* trk = m_timeline->track(*targetTk);
                        if (trk) trk->removeClipById(*seqClipId);
                    }

                    // Restore the original clips
                    for (const auto& sc : *savedClips) {
                        auto* trk = m_timeline->track(sc.trackIndex);
                        if (!trk) continue;
                        auto restored = sc.clonedClip->clone();
                        trk->addClip(std::move(restored));
                    }

                    // Remove the created nested sequence
                    if (*seqIdx < m_project->sequenceCount())
                        m_project->extractSequence(*seqIdx);

                    refreshAfter();
                }));

            spdlog::info("Nested {} clips into sequence '{}' (index {})",
                         clips.size(), name, *seqIdx);
        });

        // -- Sequence dropped from project bin or Source Monitor --------
        connect(m_timelinePanel, &TimelinePanel::sequenceDropped,
                this, [this](size_t sequenceIndex, int64_t atTick, size_t trackIndex,
                             int64_t sourceIn, int64_t sourceOut) {
            if (!m_timeline || !m_project || !m_commandStack) return;
            if (sequenceIndex >= m_project->sequenceCount()) return;

            auto* nestedTimeline = m_project->sequence(sequenceIndex);
            if (!nestedTimeline) return;

            // Prevent dropping a sequence into itself (infinite recursion)
            if (nestedTimeline == m_timeline) {
                spdlog::warn("Cannot nest a sequence into itself");
                return;
            }

            // Compute nested sequence duration from its content
            int64_t dur = 0;
            for (size_t ti = 0; ti < nestedTimeline->trackCount(); ++ti) {
                auto* trk = nestedTimeline->track(ti);
                if (!trk) continue;
                for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                    auto* c = trk->clip(ci);
                    if (c) dur = std::max(dur, c->timelineOut());
                }
            }
            if (dur <= 0) dur = static_cast<int64_t>(5.0 * 48000.0); // fallback 5s

            // Find a video track to place the SequenceClip
            size_t targetTrackIdx = SIZE_MAX;
            bool needsNewTrack = false;
            const bool forceGhostVideoTrack = (trackIndex == (SIZE_MAX - 1));
            if (forceGhostVideoTrack)
                needsNewTrack = true;
            if (trackIndex < m_timeline->trackCount() &&
                m_timeline->track(trackIndex)->type() == TrackType::Video)
                targetTrackIdx = trackIndex;
            if (targetTrackIdx == SIZE_MAX) {
                for (size_t i = m_timeline->trackCount(); i > 0; --i) {
                    if (m_timeline->track(i - 1)->type() == TrackType::Video) {
                        targetTrackIdx = i - 1;
                        break;
                    }
                }
            }
            if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;

            auto clipId     = std::make_shared<uint64_t>(0);
            auto createdTk  = std::make_shared<bool>(false);
            auto tkIdx      = std::make_shared<size_t>(targetTrackIdx);
            auto overlapCmd2 = std::make_shared<std::unique_ptr<Command>>(nullptr);
            std::string seqName = nestedTimeline->name();

            auto refreshAfter = [this](bool trackStructureChanged = false) {
                if (trackStructureChanged)
                    m_timelinePanel->rebuildTracks();
                else
                    m_timelinePanel->refreshTrackContents();
                invalidateCompositeCache();
                if (m_programMonitor) m_programMonitor->requestRefresh();
            };

            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Add Sequence to Timeline",
                /* execute / redo */
                [this, sequenceIndex, atTick, sourceIn, sourceOut, dur,
                 needsNewTrack, forceGhostVideoTrack,
                 clipId, createdTk, tkIdx, overlapCmd2, seqName,
                 refreshAfter]() {
                    if (needsNewTrack && *tkIdx == SIZE_MAX) {
                        Track* t = nullptr;
                        if (forceGhostVideoTrack) {
                            auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                            t = m_timeline->insertTrack(0, std::move(newTrack));
                        } else {
                            t = m_timeline->addVideoTrack("V1");
                        }
                        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                            if (m_timeline->track(i) == t) {
                                *tkIdx = i; break;
                            }
                        }
                        *createdTk = true;
                    }
                    auto* track = m_timeline->track(*tkIdx);
                    if (!track) return;

                    auto seqClip = std::make_unique<SequenceClip>();
                    seqClip->setSequenceIndex(sequenceIndex);
                    seqClip->setSequenceName(seqName);
                    seqClip->setLabel(seqName);
                    seqClip->setTimelineIn(atTick);

                    if (sourceIn >= 0 && sourceOut > sourceIn) {
                        seqClip->setSourceIn(sourceIn);
                        seqClip->setDuration(sourceOut - sourceIn);
                    } else {
                        seqClip->setDuration(dur);
                    }
                    *clipId = seqClip->id();
                    track->addClip(std::move(seqClip));

                    *overlapCmd2 = EditOperations::resolveOverlaps(
                        *m_timeline, *tkIdx, *clipId);
                    if (*overlapCmd2) (*overlapCmd2)->execute();

                    refreshAfter(*createdTk);
                },
                /* undo */
                [this, clipId, createdTk, tkIdx, overlapCmd2, refreshAfter]() {
                    const bool trackStructureChanged = *createdTk;
                    if (*overlapCmd2) (*overlapCmd2)->undo();

                    if (*tkIdx < m_timeline->trackCount()) {
                        auto* track = m_timeline->track(*tkIdx);
                        if (track) track->removeClipById(*clipId);
                    }
                    if (*createdTk) {
                        m_timeline->removeTrack(*tkIdx);
                        *tkIdx = SIZE_MAX;
                        *createdTk = false;
                    }
                    refreshAfter(trackStructureChanged);
                }));

            spdlog::info("Sequence '{}' (index {}) dropped on timeline at tick {}",
                         seqName, sequenceIndex, atTick);
        });

        // -- Open nested sequence (from context menu) --------------------
        connect(m_timelinePanel, &TimelinePanel::openNestedSequence,
                this, [this](size_t sequenceIndex) {
            if (!m_project || sequenceIndex >= m_project->sequenceCount()) return;
            auto* mw = qobject_cast<MainWindow*>(window());
            if (mw) mw->switchSequence(sequenceIndex);
        });

        // -- Reveal in Project Bin (from clip context menu) --------------
        connect(m_timelinePanel, &TimelinePanel::revealInProjectBin,
                this, [this](const QString& filePath) {
            if (m_projectBin) {
                m_projectBin->revealByPath(filePath);
                // Raise the Project Bin dock so the user sees the selection
                if (auto* dock = dockForPanel(QStringLiteral("Project")))  {
                    dock->setVisible(true);
                    dock->raise();
                }
            }
        });
    }

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

// =====================================================================
    //  TRACK MANAGEMENT -- add/delete tracks from context menus
    // =====================================================================
    if (m_timelinePanel && m_timeline) {
        connect(m_timelinePanel, &TimelinePanel::addTrackAbove,
                this, [this](size_t nearIndex, bool video) {
            if (!m_timeline) return;
            auto t = std::make_unique<Track>(video ? TrackType::Video : TrackType::Audio, "");
            size_t idx = nearIndex < m_timeline->trackCount() ? nearIndex : 0;
            m_timeline->insertTrack(idx, std::move(t));
            m_timelinePanel->rebuildTracks();
            invalidateAudioSources();
        });
        connect(m_timelinePanel, &TimelinePanel::addTrackBelow,
                this, [this](size_t nearIndex, bool video) {
            if (!m_timeline) return;
            auto t = std::make_unique<Track>(video ? TrackType::Video : TrackType::Audio, "");
            size_t idx = nearIndex < m_timeline->trackCount() ? nearIndex + 1 : m_timeline->trackCount();
            m_timeline->insertTrack(idx, std::move(t));
            m_timelinePanel->rebuildTracks();
            invalidateAudioSources();
        });
        connect(m_timelinePanel, &TimelinePanel::deleteTrack,
                this, [this](size_t trackIndex) {
            if (!m_timeline || m_timeline->trackCount() <= 1) return;
            if (trackIndex >= m_timeline->trackCount()) return;

            // Clear selection state BEFORE removing the track so we don't
            // hold a dangling Clip* that belonged to the deleted track.
            m_selectedClip = nullptr;
            m_selectedTrackIdx = 0;
            m_selectedClipIdx  = 0;
            m_selectedGraphicLayerIdx = -1;
            if (m_propertiesPanel) m_propertiesPanel->clearClip();

            m_timeline->removeTrack(trackIndex);
            m_timelinePanel->rebuildTracks();
            invalidateAudioSources();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });

        connect(m_timelinePanel, &TimelinePanel::clipContextAction,
                this, [this](const QString& action, size_t trackIndex, uint64_t clipId) {
            Q_UNUSED(action); Q_UNUSED(trackIndex); Q_UNUSED(clipId);
        });
    }

    // =====================================================================
    //  EFFECTS PANEL -> REFRESH EFFECT CONTROLS WHEN EFFECTS CHANGE
    // =====================================================================
    if (m_effectsPanel && m_effectControlsPanel) {
        auto refreshAfterEffectChange = [this]() {
            if (m_effectControlsPanel) m_effectControlsPanel->refresh();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        };
        connect(m_effectsPanel, &EffectsPanel::effectAdded,
                this, refreshAfterEffectChange);
        connect(m_effectsPanel, &EffectsPanel::effectRemoved,
                this, refreshAfterEffectChange);
        connect(m_effectsPanel, &EffectsPanel::effectMoved,
                this, refreshAfterEffectChange);
    }

    m_panelsBuilt = true;
}

} // namespace rt

