// TimelineWorkspacePanelsWiringMediaDrop.cpp - Media drag-drop signal wiring.
// Extracted from TimelineWorkspacePanelsWiring.cpp for maintainability.

#include <volk.h>

#include <map>
#include <memory>
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

void TimelineWorkspace::wireMediaDropSignals()
{
    // =====================================================================
    //  MEDIA DRAG-DROP -> CREATE CLIP ON TIMELINE
    // =====================================================================
    if (m_timelinePanel && m_timeline) {
        connect(m_timelinePanel, &TimelinePanel::mediaDropped,
                this, [this](const QString& filePath, uint64_t /*mediaHandle*/,
                             int64_t atTick, size_t trackIndex) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline) return;

            // If no project or no sequences exist, prompt to create one
            if (!m_project || m_project->sequenceCount() == 0) {
                // For Spine animation drops, just create a default sequence
                if (filePath.startsWith(QStringLiteral("spine:"))) {
                    emit requestNewProjectForMedia(QString(), atTick, trackIndex);
                    return;
                }
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

            // ── Detect Spine animation drops (from Library CharactersPanel) ──
            // Format: "spine:charName|outfit|stanceInt|animName"
            bool isSpineAnimDrop = filePath.startsWith(QStringLiteral("spine:"));
            std::string spineCharName, spineOutfit, spineAnimName;
            int spineStanceInt = 0;
            if (isSpineAnimDrop) {
                QString payload = filePath.mid(6); // strip "spine:"
                QStringList parts = payload.split('|');
                if (parts.size() >= 4) {
                    spineCharName = parts[0].toStdString();
                    spineOutfit   = parts[1].toStdString();
                    spineStanceInt = parts[2].toInt();
                    spineAnimName = parts[3].toStdString();
                }
                spdlog::info("DIAG-DROP detected Spine animation: {} / {} / stance={} / {}",
                             spineCharName, spineOutfit, spineStanceInt, spineAnimName);
            }

            // Determine if this is an audio file by extension
            QString ext = QFileInfo(filePath).suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            bool isAudio = audioExts.contains(ext);

            // Pre-compute clip properties before the lambda captures
            std::string label = isSpineAnimDrop
                ? spineCharName + " - " + spineAnimName
                : QFileInfo(filePath).baseName().toStdString();
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
                // Cache files live under assets/Converted/{format}/{charName}/{outfit}/{anim}.ext
                if (vcCharName.empty()) {
                    std::string generic = std::filesystem::path(path).generic_string();
                    const std::string marker = "assets/Converted/";
                    auto pos = generic.find(marker);
                    if (pos != std::string::npos) {
                        std::string rest = generic.substr(pos + marker.size());
                        // rest = "{format}/{charName}/{outfit}/{anim}.ext"
                        auto slash1 = rest.find('/');
                        if (slash1 != std::string::npos && slash1 > 0) {
                            std::string fmtName = rest.substr(0, slash1);
                            std::string afterFmt = rest.substr(slash1 + 1);
                            auto slash2 = afterFmt.find('/');
                            if (slash2 != std::string::npos && slash2 > 0) {
                                vcCharName = afterFmt.substr(0, slash2);
                                std::string afterChar = afterFmt.substr(slash2 + 1);
                                auto slash3 = afterChar.find('/');
                                if (slash3 != std::string::npos && slash3 > 0) {
                                    vcOutfit = afterChar.substr(0, slash3);
                                    std::string fileName = afterChar.substr(slash3 + 1);
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
                                        + fmtName + "/" + vcCharName + "/" + vcOutfit + "/";
                                    vcMutePath = prefix + baseStem + extStr;
                                    vcTalkPath = prefix + stem + extStr;
                                } else {
                                    vcAnimName = stem;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + fmtName + "/" + vcCharName + "/" + vcOutfit + "/";
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
                    // Keep the 5-second default for character animation clips so
                    // the animation loops for a full 5 seconds (Premiere Pro behavior).
                    if (info && info->duration > 0 && !isCharacterClip)
                        dur = secondsToTicks(info->duration);
                    if (info && info->hasAudio && !isAudio)
                        mediaHasAudio = true;
                }
            }
            spdlog::info("DIAG-DROP mediaDropped '{}': dur={} ticks ({:.3f}s), mediaHasAudio={}",
                         path, dur, ticksToSeconds(dur), mediaHasAudio);

            // Find the target track (don't create one yet — that happens inside the command)
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
                if (m_destroying.load(std::memory_order_acquire)) return;
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
                auto cmd = std::make_unique<LambdaCommand>(
                    "Add Media to Timeline",
                    /* execute / redo */
                    [this, isAudio, isSpineAnimDrop, mediaHasAudio, path, label, atTick, dur, sourceFps,
                     needsNewTrack, needsNewAudioTrack, forceGhostVideoTrack, forceGhostAudioTrack,
                     clipId, createdTk, tkIdx, overlapCmd,
                     audioClipId, audioCreatedTk, audioTkIdx, audioOverlapCmd,
                     refreshAfter,
                     vcCharName, vcMutePath, vcTalkPath, vcOutfit, vcAnimName,
                     vcPosX, vcPosY, vcScale, vcOpacity, vcIsTalking,
                     spineCharName, spineOutfit, spineStanceInt, spineAnimName]() {
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
                        if (isSpineAnimDrop) {
                            // Create a SpineClip for animation drops from the Library panel
                            auto sc = std::make_unique<SpineClip>(spineCharName, spineOutfit);
                            CharacterStance stance = CharacterStance::Default;
                            if (spineStanceInt == 1) stance = CharacterStance::Aim;
                            else if (spineStanceInt == 2) stance = CharacterStance::Cover;
                            sc->setStance(stance);
                            sc->setAnimationName(spineAnimName);
                            sc->setLooping(true);
                            sc->setTimelineIn(atTick);
                            sc->setDuration(dur);
                            sc->setLabel(label);
                            clip = std::move(sc);
                        } else if (isAudio) {
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
                            // Still images have no real source duration — treat as unbounded.
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
                            } else if (*audioTkIdx != SIZE_MAX) {
                                // Re-validate audio track index — a new video track may have been
                                // inserted at index 0 above, shifting all existing track indices.
                                if (*audioTkIdx >= m_timeline->trackCount() ||
                                    m_timeline->track(*audioTkIdx)->type() != TrackType::Audio) {
                                    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                                        if (m_timeline->track(i)->type() == TrackType::Audio) {
                                            *audioTkIdx = i;
                                            break;
                                        }
                                    }
                                }
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
                    }
                );
                m_commandStack->execute(std::move(cmd));
            } else {
                // No command stack — fall back to direct add
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
                    // Still images have no real source duration — treat as unbounded.
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
            if (m_destroying.load(std::memory_order_acquire)) return;
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
                // Cache files live under assets/Converted/{format}/{charName}/{outfit}/{anim}.ext
                if (vcCharName2.empty()) {
                    std::string generic = std::filesystem::path(path).generic_string();
                    const std::string marker = "assets/Converted/";
                    auto pos = generic.find(marker);
                    if (pos != std::string::npos) {
                        std::string rest = generic.substr(pos + marker.size());
                        // rest = "{format}/{charName}/{outfit}/{anim}.ext"
                        auto slash1 = rest.find('/');
                        if (slash1 != std::string::npos && slash1 > 0) {
                            std::string fmtName = rest.substr(0, slash1);
                            std::string afterFmt = rest.substr(slash1 + 1);
                            auto slash2 = afterFmt.find('/');
                            if (slash2 != std::string::npos && slash2 > 0) {
                                vcCharName2 = afterFmt.substr(0, slash2);
                                label = vcCharName2;
                                std::string afterChar = afterFmt.substr(slash2 + 1);
                                auto slash3 = afterChar.find('/');
                                if (slash3 != std::string::npos && slash3 > 0) {
                                    vcOutfit2 = afterChar.substr(0, slash3);
                                    std::string fileName = afterChar.substr(slash3 + 1);
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
                                        + fmtName + "/" + vcCharName2 + "/" + vcOutfit2 + "/";
                                    vcMutePath2 = prefix + baseStem + extStr;
                                    vcTalkPath2 = prefix + stem + extStr;
                                } else {
                                    vcAnimName2 = stem;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + fmtName + "/" + vcCharName2 + "/" + vcOutfit2 + "/";
                                    vcMutePath2 = prefix + stem + extStr;
                                    vcTalkPath2 = prefix + stem + talkSuffix + extStr;
                                }
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
                if (m_destroying.load(std::memory_order_acquire)) return;
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
                auto cmd = std::make_unique<LambdaCommand>(
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
                            // Still images have no real source duration — treat as unbounded.
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
                            } else if (*audioTkIdx2 != SIZE_MAX) {
                                // Re-validate audio track index — a new video track may have been
                                // inserted at index 0 above, shifting all existing track indices.
                                if (*audioTkIdx2 >= m_timeline->trackCount() ||
                                    m_timeline->track(*audioTkIdx2)->type() != TrackType::Audio) {
                                    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                                        if (m_timeline->track(i)->type() == TrackType::Audio) {
                                            *audioTkIdx2 = i;
                                            break;
                                        }
                                    }
                                }
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
                    }
                );
                m_commandStack->execute(std::move(cmd));
            }

            spdlog::info("Source region dropped on timeline: '{}' at tick {}, sourceIn={} dur={}",
                         path, atTick, sourceIn, dur);
        });

        // External file drop from Windows Explorer — add to bin + timeline
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
}

} // namespace rt
