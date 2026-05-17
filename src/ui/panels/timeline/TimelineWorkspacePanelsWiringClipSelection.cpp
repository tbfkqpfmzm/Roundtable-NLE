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
#include <QPainter>
#include <QTimer>
#include <spdlog/spdlog.h>

namespace rt {

// Returns true for still-image media. Such files have no real "source duration",

void TimelineWorkspace::wireClipSelectionSignals() {
    //  CLIP SELECTION -> EFFECT CONTROLS / PROPERTIES PANEL
    // =====================================================================
    if (m_timelinePanel) {
        connect(m_timelinePanel, &TimelinePanel::clipSelected,
                this, [this](size_t trackIdx, size_t clipIdx) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline) return;
            // NOTE: Do NOT early-return when selection count > 1.
            // Linked clips (groupId) cause multiple clips to be selected,
            // but we still want to populate panels with the clicked clip.
            auto* track = m_timeline->track(trackIdx);
            if (!track) return;
            auto* clip = track->clip(clipIdx);
            if (clip) {
                auto cs0 = std::chrono::steady_clock::now();
                // Set m_selectedClip BEFORE setClip, because setClip's
                // layerSelected signal triggers updateTransformOverlay()
                // which reads m_selectedClip.
                // Only reset m_selectedGraphicLayerIdx when the clip
                // actually changes â€” if same clip, setClip returns early
                // and layerSelected won't fire to re-establish the index.
                if (clip != m_selectedClip)
                    m_selectedGraphicLayerIdx = -1;
                m_selectedClip = clip;
                m_selectedTrackIdx = trackIdx;
                m_selectedClipIdx = clipIdx;
                if (m_effectControlsPanel) {
                    auto* dock = dockForPanel(QStringLiteral("Effect Controls"));
                    if (!dock || dock->isVisible())
                        m_effectControlsPanel->setClip(clip, track);
                }
                if (m_GraphicsEditorPanel) {
                    // Always call setClip for Graphic clips � the layerSelected
                    // signal is required to set m_selectedGraphicLayerIdx for
                    // per-layer transform overlay mode.
                    bool isGraphic = (clip->clipType() == ClipType::Graphic);
                    auto* dock = dockForPanel(QStringLiteral("Essential Graphics"));
                    if (!dock || dock->isVisible() || isGraphic)
                        m_GraphicsEditorPanel->setClip(clip, track);
                }
                if (m_ColorGradingPanel) {
                    auto* dock = dockForPanel(QStringLiteral("Color Grading"));
                    if (!dock || dock->isVisible())
                        m_ColorGradingPanel->setClip(clip, track);
                }
                if (m_propertiesPanel) {
                    auto* dock = dockForPanel(QStringLiteral("Properties"));
                    if (!dock || dock->isVisible())
                        m_propertiesPanel->setClip(clip, track);
                }
                auto cs1 = std::chrono::steady_clock::now();
                scheduleOverlayRefresh();
                auto cs2 = std::chrono::steady_clock::now();

                // Auto-raise the appropriate dock tab
                if (clip->clipType() == ClipType::Spine) {
                    if (auto* dock = dockForPanel(QStringLiteral("Properties"))) {
                        dock->setVisible(true);
                        dock->raise();
                    }
                } else if (clip->clipType() == ClipType::Video) {
                    auto* vc = static_cast<VideoClip*>(clip);
                    if (vc->isVideoCharacter()) {
                        if (auto* dock = dockForPanel(QStringLiteral("Properties"))) {
                            dock->setVisible(true);
                            dock->raise();
                        }
                    } else {
                        if (auto* dock = dockForPanel(QStringLiteral("Effect Controls"))) {
                            dock->setVisible(true);
                            dock->raise();
                        }
                    }
                } else if (clip->clipType() == ClipType::Graphic) {
                    if (auto* dock = dockForPanel(QStringLiteral("Essential Graphics"))) {
                        dock->setVisible(true);
                        dock->raise();
                    }
                } else {
                    if (auto* dock = dockForPanel(QStringLiteral("Effect Controls"))) {
                        dock->setVisible(true);
                        dock->raise();
                    }
                }
                auto cs3 = std::chrono::steady_clock::now();
                spdlog::info("clipSelected  props={:.1f}ms  overlay={:.1f}ms  dockRaise={:.1f}ms  total={:.1f}ms",
                    std::chrono::duration<double, std::milli>(cs1 - cs0).count(),
                    std::chrono::duration<double, std::milli>(cs2 - cs1).count(),
                    std::chrono::duration<double, std::milli>(cs3 - cs2).count(),
                    std::chrono::duration<double, std::milli>(cs3 - cs0).count());
            }
        });
        connect(m_timelinePanel, &TimelinePanel::clipDoubleClicked,
                this, [this](size_t trackIdx, size_t clipIdx) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline || !m_sourceMonitor || !m_mediaPool) return;
            auto* track = m_timeline->track(trackIdx);
            if (!track) return;
            auto* clip = track->clip(clipIdx);
            if (!clip) return;

            // SequenceClip: open the nested sequence
            if (clip->clipType() == ClipType::Sequence) {
                auto* seqClip = static_cast<SequenceClip*>(clip);
                if (m_project && seqClip->sequenceIndex() < m_project->sequenceCount()) {
                    auto* mw = qobject_cast<MainWindow*>(window());
                    if (mw) mw->switchSequence(seqClip->sequenceIndex());
                }
                return;
            }

            // SpineClip: try to open the pre-rendered video from AnimationVideoCache.
            // If no cached video exists, fall back to live Spine rendering in the Source Monitor.
            if (clip->clipType() == ClipType::Spine) {
                auto* spineClip = static_cast<SpineClip*>(clip);
                if (m_compositeService) {
                    if (m_compositeService->animVideoCache()) {
                        uint64_t handle = m_compositeService->animVideoCache()->getMediaHandle(
                            spineClip->characterName(),
                            spineClip->outfit(),
                            spineClip->animationName());
                        if (handle != 0) {
                            m_sourceMonitor->loadClip(handle, m_mediaPool);
                            return;
                        }
                    }
                    // No cached video — render live from the Spine skeleton
                    m_sourceMonitor->loadSpineClip(spineClip, m_compositeService.get());
                }
                return;
            }

            std::string mediaPath;
            if (clip->clipType() == ClipType::Video) {
                mediaPath = static_cast<VideoClip*>(clip)->mediaPath();
            } else if (clip->clipType() == ClipType::Audio) {
                mediaPath = static_cast<AudioClip*>(clip)->mediaPath();
            } else if (clip->clipType() == ClipType::Image) {
                mediaPath = static_cast<ImageClip*>(clip)->mediaPath();
            }
            if (mediaPath.empty()) return;

            uint64_t handle = 0;
            if (m_compositeService)
                handle = m_compositeService->findMediaHandle(mediaPath);
            if (handle == 0) {
                handle = m_mediaPool->open(std::filesystem::path(mediaPath));
                if (handle != 0 && m_compositeService)
                    m_compositeService->registerMediaHandle(mediaPath, handle);
            }
            if (handle != 0)
                m_sourceMonitor->loadClip(handle, m_mediaPool);
        });
        connect(m_timelinePanel, &TimelinePanel::transitionSelected,
                this, [this](size_t trackIdx, size_t transIdx) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline) return;
            auto* track = m_timeline->track(trackIdx);
            if (!track || transIdx >= track->transitionCount()) return;
            m_selectedClip = nullptr;
            if (m_propertiesPanel) {
                m_propertiesPanel->setTransition(track, transIdx);
                if (auto* dock = dockForPanel(QStringLiteral("Properties"))) {
                    dock->setVisible(true);
                    dock->raise();
                }
            }
        });
        connect(m_timelinePanel, &TimelinePanel::selectionChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            const auto& sel = m_timelinePanel->selection();
            if (sel.empty()) {
                if (m_effectControlsPanel) m_effectControlsPanel->clearClip();
                if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->clearClip();
                if (m_ColorGradingPanel) m_ColorGradingPanel->clearClip();
                if (m_propertiesPanel) m_propertiesPanel->clearClip();
                m_selectedClip = nullptr;
                m_selectedGraphicLayerIdx = -1;
                if (m_programMonitor && m_programMonitor->viewport())
                    m_programMonitor->viewport()->clearTransformOverlay();
                if (m_programMonitor && m_programMonitor->transformOverlay())
                    m_programMonitor->transformOverlay()->clearTransformOverlay();
            } else if (sel.count() == 1) {
                // Single clip selected (e.g. via drag/marquee) â€” populate
                // all panels the same way clipSelected does.
                const auto& ref = sel.clips().front();
                auto* trk = m_timeline->track(ref.trackIndex);
                if (trk) {
                    size_t idx = trk->findClipIndexById(ref.clipId);
                    if (idx < trk->clipCount()) {
                        auto* clip = trk->clip(idx);
                        // Set clip state BEFORE setClip so layerSelected
                        // handler can see the correct m_selectedClip.
                        if (clip != m_selectedClip)
                            m_selectedGraphicLayerIdx = -1;
                        m_selectedClip = clip;
                        m_selectedTrackIdx = ref.trackIndex;
                        m_selectedClipIdx = idx;
                        if (m_effectControlsPanel) m_effectControlsPanel->setClip(clip, trk);
                        if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->setClip(clip, trk);
                        if (m_ColorGradingPanel) m_ColorGradingPanel->setClip(clip, trk);
                        if (m_propertiesPanel) m_propertiesPanel->setClip(clip, trk);
                        scheduleOverlayRefresh();

                        // Auto-raise the appropriate dock tab
                        if (clip->clipType() == ClipType::Graphic) {
                            if (auto* dock = dockForPanel(QStringLiteral("Essential Graphics"))) {
                                dock->setVisible(true);
                                dock->raise();
                            }
                        } else if (clip->clipType() == ClipType::Spine) {
                            if (auto* dock = dockForPanel(QStringLiteral("Properties"))) {
                                dock->setVisible(true);
                                dock->raise();
                            }
                        } else if (clip->clipType() == ClipType::Video) {
                            auto* vc = static_cast<VideoClip*>(clip);
                            if (vc->isVideoCharacter()) {
                                if (auto* dock = dockForPanel(QStringLiteral("Properties"))) {
                                    dock->setVisible(true);
                                    dock->raise();
                                }
                            } else {
                                if (auto* dock = dockForPanel(QStringLiteral("Effect Controls"))) {
                                    dock->setVisible(true);
                                    dock->raise();
                                }
                            }
                        } else {
                            if (auto* dock = dockForPanel(QStringLiteral("Effect Controls"))) {
                                dock->setVisible(true);
                                dock->raise();
                            }
                        }
                    }
                }
            } else if (sel.count() > 1) {
                std::vector<Clip*> clips;
                clips.reserve(sel.count());
                for (const auto& ref : sel.clips()) {
                    if (auto* trk = m_timeline->track(ref.trackIndex)) {
                        size_t idx = trk->findClipIndexById(ref.clipId);
                        if (idx < trk->clipCount())
                            clips.push_back(trk->clip(idx));
                    }
                }
                m_propertiesPanel->setMultiSelection(clips);
            }
        });
    }

    // -- Transform overlay: connect Viewport signals to update clip props --
    if (m_programMonitor && m_programMonitor->viewport()) {
        auto* vp = m_programMonitor->viewport();
        connect(vp, &Viewport::transformPositionChanged,
                this, [this](float posX, float posY) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            // Per-layer: update layer transform if a layer is selected
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    layer->transform().posX.writeValue(relTick, posX);
                    layer->transform().posY.writeValue(relTick, posY);
                }
            } else {
                m_selectedClip->positionX().writeValue(relTick, posX);
                m_selectedClip->positionY().writeValue(relTick, posY);
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(vp, &Viewport::transformScaleChanged,
                this, [this](float scX, float scY) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    if (!m_scaleDragActive) {
                        m_scaleDragActive = true;
                        m_scaleXWasStaticAtDragStart = layer->transform().scaleX.isStatic();
                        m_scaleYWasStaticAtDragStart = layer->transform().scaleY.isStatic();
                    }
                    if (!layer->transform().scaleX.isStatic() || !layer->transform().scaleY.isStatic()) {
                        layer->transform().scaleX.addKeyframe(relTick, scX);
                        layer->transform().scaleY.addKeyframe(relTick, scY);
                    } else {
                        layer->transform().scaleX.setDefaultValue(scX);
                        layer->transform().scaleY.setDefaultValue(scY);
                    }
                }
            } else {
                if (!m_scaleDragActive) {
                    m_scaleDragActive = true;
                    m_scaleXWasStaticAtDragStart = m_selectedClip->scaleX().isStatic();
                    m_scaleYWasStaticAtDragStart = m_selectedClip->scaleY().isStatic();
                }
                if (!m_selectedClip->scaleX().isStatic() || !m_selectedClip->scaleY().isStatic()) {
                    m_selectedClip->scaleX().addKeyframe(relTick, scX);
                    m_selectedClip->scaleY().addKeyframe(relTick, scY);
                } else {
                    m_selectedClip->scaleX().setDefaultValue(scX);
                    m_selectedClip->scaleY().setDefaultValue(scY);
                }
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(vp, &Viewport::transformRotationChanged,
                this, [this](float rot) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    layer->transform().rotation.writeValue(relTick, rot);
                }
            } else {
                m_selectedClip->rotation().writeValue(relTick, rot);
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(vp, &Viewport::transformDragFinished,
                this, [this](float oldPosX, float oldPosY, float oldScX, float oldScY, float oldRot,
                             float newPosX, float newPosY, float newScX, float newScY, float newRot) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            // Capture pre-drag static state before resetting
            bool sxWasStatic = m_scaleXWasStaticAtDragStart;
            bool syWasStatic = m_scaleYWasStaticAtDragStart;
            m_scaleDragActive = false;
            updateTransformOverlay();
            if (m_selectedClip) {
                auto* track = m_timeline ? m_timeline->track(m_selectedTrackIdx) : nullptr;
                if (m_propertiesPanel) m_propertiesPanel->setClip(m_selectedClip, track);
                if (m_effectControlsPanel) m_effectControlsPanel->setClip(m_selectedClip, track);
                if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
            }
            if (m_selectedClip && m_commandStack) {
                const int64_t relTick = m_playbackController
                    ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                    : 0;
                bool posChanged = (std::abs(oldPosX - newPosX) > 0.01f ||
                                   std::abs(oldPosY - newPosY) > 0.01f);
                bool scaleChanged = (std::abs(oldScX - newScX) > 0.001f ||
                                    std::abs(oldScY - newScY) > 0.001f);
                bool rotChanged = (std::abs(oldRot - newRot) > 0.01f);

                // Detect which tracks had KFs newly created (vs pre-existing)
                auto kfCreated = [relTick](const KeyframeTrack<float>& tk, float oldVal) -> bool {
                    if (tk.isStatic() || tk.keyframeCount() < 2) return false;
                    if (!tk.hasKeyframeAt(relTick)) return false;
                    KeyframeTrack<float> tmp(tk.defaultValue());
                    for (const auto& kf : tk.keyframes()) {
                        if (kf.time != relTick) tmp.restoreKeyframe(kf);
                    }
                    return std::abs(tmp.evaluate(relTick) - oldVal) < 0.01f;
                };

                if (posChanged || scaleChanged || rotChanged) {
                    if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                        auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                        int layerIdx = m_selectedGraphicLayerIdx;
                        if (layerIdx < static_cast<int>(gc->layerCount())) {
                            auto* layer = gc->layer(static_cast<size_t>(layerIdx));
                            bool pxC = posChanged && kfCreated(layer->transform().posX, oldPosX);
                            bool pyC = posChanged && kfCreated(layer->transform().posY, oldPosY);
                            bool sxC = scaleChanged && kfCreated(layer->transform().scaleX, oldScX);
                            bool syC = scaleChanged && kfCreated(layer->transform().scaleY, oldScY);
                            bool rtC = rotChanged && kfCreated(layer->transform().rotation, oldRot);
                            bool pxA = posChanged && !layer->transform().posX.isStatic();
                            bool pyA = posChanged && !layer->transform().posY.isStatic();
                            bool sxA = scaleChanged && !layer->transform().scaleX.isStatic();
                            bool syA = scaleChanged && !layer->transform().scaleY.isStatic();
                            bool rtA = rotChanged && !layer->transform().rotation.isStatic();
                            auto cmd = std::make_unique<LambdaCommand>(
                                "Transform Layer",
                                [gc, layerIdx, newPosX, newPosY, newScX, newScY, newRot,
                                 relTick, posChanged, scaleChanged, rotChanged,
                                 pxA, pyA, sxA, syA, rtA]() {
                                    auto* l = gc->layer(static_cast<size_t>(layerIdx));
                                    if (posChanged) {
                                        if (pxA) l->transform().posX.addKeyframe(relTick, newPosX);
                                        else l->transform().posX.setDefaultValue(newPosX);
                                        if (pyA) l->transform().posY.addKeyframe(relTick, newPosY);
                                        else l->transform().posY.setDefaultValue(newPosY);
                                    }
                                    if (scaleChanged) {
                                        if (sxA) l->transform().scaleX.addKeyframe(relTick, newScX);
                                        else l->transform().scaleX.setDefaultValue(newScX);
                                        if (syA) l->transform().scaleY.addKeyframe(relTick, newScY);
                                        else l->transform().scaleY.setDefaultValue(newScY);
                                    }
                                    if (rotChanged) {
                                        if (rtA) l->transform().rotation.addKeyframe(relTick, newRot);
                                        else l->transform().rotation.setDefaultValue(newRot);
                                    }
                                },
                                [gc, layerIdx, oldPosX, oldPosY, oldScX, oldScY, oldRot,
                                 relTick, posChanged, scaleChanged, rotChanged,
                                 pxC, pyC, sxC, syC, rtC, sxWasStatic, syWasStatic]() {
                                    auto* l = gc->layer(static_cast<size_t>(layerIdx));
                                    if (posChanged) {
                                        if (pxC) l->transform().posX.removeKeyframeAtTime(relTick);
                                        else l->transform().posX.writeValue(relTick, oldPosX);
                                        if (pyC) l->transform().posY.removeKeyframeAtTime(relTick);
                                        else l->transform().posY.writeValue(relTick, oldPosY);
                                    }
                                    if (scaleChanged) {
                                        if (sxWasStatic) { l->transform().scaleX.removeKeyframeAtTime(relTick); l->transform().scaleX.setDefaultValue(oldScX); }
                                        else if (sxC) l->transform().scaleX.removeKeyframeAtTime(relTick);
                                        else l->transform().scaleX.writeValue(relTick, oldScX);
                                        if (syWasStatic) { l->transform().scaleY.removeKeyframeAtTime(relTick); l->transform().scaleY.setDefaultValue(oldScY); }
                                        else if (syC) l->transform().scaleY.removeKeyframeAtTime(relTick);
                                        else l->transform().scaleY.writeValue(relTick, oldScY);
                                    }
                                    if (rotChanged) {
                                        if (rtC) l->transform().rotation.removeKeyframeAtTime(relTick);
                                        else l->transform().rotation.writeValue(relTick, oldRot);
                                    }
                                });
                            m_commandStack->pushWithoutExecute(std::move(cmd));
                        }
                    } else {
                        Clip* clip = m_selectedClip;
                        bool pxC = posChanged && kfCreated(clip->positionX(), oldPosX);
                        bool pyC = posChanged && kfCreated(clip->positionY(), oldPosY);
                        bool sxC = scaleChanged && kfCreated(clip->scaleX(), oldScX);
                        bool syC = scaleChanged && kfCreated(clip->scaleY(), oldScY);
                        bool rtC = rotChanged && kfCreated(clip->rotation(), oldRot);
                        bool pxA = posChanged && !clip->positionX().isStatic();
                        bool pyA = posChanged && !clip->positionY().isStatic();
                        bool sxA = scaleChanged && !clip->scaleX().isStatic();
                        bool syA = scaleChanged && !clip->scaleY().isStatic();
                        bool rtA = rotChanged && !clip->rotation().isStatic();
                        auto cmd = std::make_unique<LambdaCommand>(
                            "Transform Clip",
                            [clip, relTick, newPosX, newPosY, newScX, newScY, newRot,
                             posChanged, scaleChanged, rotChanged,
                             pxA, pyA, sxA, syA, rtA]() {
                                if (posChanged) {
                                    if (pxA) clip->positionX().addKeyframe(relTick, newPosX);
                                    else clip->positionX().setDefaultValue(newPosX);
                                    if (pyA) clip->positionY().addKeyframe(relTick, newPosY);
                                    else clip->positionY().setDefaultValue(newPosY);
                                }
                                if (scaleChanged) {
                                    if (sxA) clip->scaleX().addKeyframe(relTick, newScX);
                                    else clip->scaleX().setDefaultValue(newScX);
                                    if (syA) clip->scaleY().addKeyframe(relTick, newScY);
                                    else clip->scaleY().setDefaultValue(newScY);
                                }
                                if (rotChanged) {
                                    if (rtA) clip->rotation().addKeyframe(relTick, newRot);
                                    else clip->rotation().setDefaultValue(newRot);
                                }
                            },
                            [clip, relTick, oldPosX, oldPosY, oldScX, oldScY, oldRot,
                             posChanged, scaleChanged, rotChanged,
                             pxC, pyC, sxC, syC, rtC, sxWasStatic, syWasStatic]() {
                                if (posChanged) {
                                    if (pxC) clip->positionX().removeKeyframeAtTime(relTick);
                                    else clip->positionX().writeValue(relTick, oldPosX);
                                    if (pyC) clip->positionY().removeKeyframeAtTime(relTick);
                                    else clip->positionY().writeValue(relTick, oldPosY);
                                }
                                if (scaleChanged) {
                                    if (sxWasStatic) { clip->scaleX().removeKeyframeAtTime(relTick); clip->scaleX().setDefaultValue(oldScX); }
                                    else if (sxC) clip->scaleX().removeKeyframeAtTime(relTick);
                                    else clip->scaleX().writeValue(relTick, oldScX);
                                    if (syWasStatic) { clip->scaleY().removeKeyframeAtTime(relTick); clip->scaleY().setDefaultValue(oldScY); }
                                    else if (syC) clip->scaleY().removeKeyframeAtTime(relTick);
                                    else clip->scaleY().writeValue(relTick, oldScY);
                                }
                                if (rotChanged) {
                                    if (rtC) clip->rotation().removeKeyframeAtTime(relTick);
                                    else clip->rotation().writeValue(relTick, oldRot);
                                }
                            });
                        m_commandStack->pushWithoutExecute(std::move(cmd));
                    }
                }
            }
        });
    }

    // -- GPU TransformOverlayWidget: same signals but for the GPU viewport --
    if (m_programMonitor && m_programMonitor->transformOverlay()) {
        auto* ov = m_programMonitor->transformOverlay();
        connect(ov, &TransformOverlayWidget::transformPositionChanged,
                this, [this](float posX, float posY) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    layer->transform().posX.writeValue(relTick, posX);
                    layer->transform().posY.writeValue(relTick, posY);
                }
            } else {
                m_selectedClip->positionX().writeValue(relTick, posX);
                m_selectedClip->positionY().writeValue(relTick, posY);
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(ov, &TransformOverlayWidget::transformScaleChanged,
                this, [this](float scX, float scY) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    if (!m_scaleDragActive) {
                        m_scaleDragActive = true;
                        m_scaleXWasStaticAtDragStart = layer->transform().scaleX.isStatic();
                        m_scaleYWasStaticAtDragStart = layer->transform().scaleY.isStatic();
                    }
                    if (!layer->transform().scaleX.isStatic() || !layer->transform().scaleY.isStatic()) {
                        layer->transform().scaleX.addKeyframe(relTick, scX);
                        layer->transform().scaleY.addKeyframe(relTick, scY);
                    } else {
                        layer->transform().scaleX.setDefaultValue(scX);
                        layer->transform().scaleY.setDefaultValue(scY);
                    }
                }
            } else {
                if (!m_scaleDragActive) {
                    m_scaleDragActive = true;
                    m_scaleXWasStaticAtDragStart = m_selectedClip->scaleX().isStatic();
                    m_scaleYWasStaticAtDragStart = m_selectedClip->scaleY().isStatic();
                }
                if (!m_selectedClip->scaleX().isStatic() || !m_selectedClip->scaleY().isStatic()) {
                    m_selectedClip->scaleX().addKeyframe(relTick, scX);
                    m_selectedClip->scaleY().addKeyframe(relTick, scY);
                } else {
                    m_selectedClip->scaleX().setDefaultValue(scX);
                    m_selectedClip->scaleY().setDefaultValue(scY);
                }
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(ov, &TransformOverlayWidget::transformRotationChanged,
                this, [this](float rot) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    layer->transform().rotation.writeValue(relTick, rot);
                }
            } else {
                m_selectedClip->rotation().writeValue(relTick, rot);
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(ov, &TransformOverlayWidget::transformDragFinished,
                this, [this](float oldPosX, float oldPosY, float oldScX, float oldScY, float oldRot,
                             float newPosX, float newPosY, float newScX, float newScY, float newRot) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            // Capture pre-drag static state before resetting
            bool sxWasStatic = m_scaleXWasStaticAtDragStart;
            bool syWasStatic = m_scaleYWasStaticAtDragStart;
            m_scaleDragActive = false;
            updateTransformOverlay();
            if (m_selectedClip) {
                auto* track = m_timeline ? m_timeline->track(m_selectedTrackIdx) : nullptr;
                if (m_propertiesPanel) m_propertiesPanel->setClip(m_selectedClip, track);
                if (m_effectControlsPanel) m_effectControlsPanel->setClip(m_selectedClip, track);
                if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
            }
            if (m_selectedClip && m_commandStack) {
                const int64_t relTick = m_playbackController
                    ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                    : 0;
                bool posChanged = (std::abs(oldPosX - newPosX) > 0.01f ||
                                   std::abs(oldPosY - newPosY) > 0.01f);
                bool scaleChanged = (std::abs(oldScX - newScX) > 0.001f ||
                                    std::abs(oldScY - newScY) > 0.001f);
                bool rotChanged = (std::abs(oldRot - newRot) > 0.01f);

                // Detect which tracks had KFs newly created (vs pre-existing)
                auto kfCreated = [relTick](const KeyframeTrack<float>& tk, float oldVal) -> bool {
                    if (tk.isStatic() || tk.keyframeCount() < 2) return false;
                    if (!tk.hasKeyframeAt(relTick)) return false;
                    KeyframeTrack<float> tmp(tk.defaultValue());
                    for (const auto& kf : tk.keyframes()) {
                        if (kf.time != relTick) tmp.restoreKeyframe(kf);
                    }
                    return std::abs(tmp.evaluate(relTick) - oldVal) < 0.01f;
                };

                if (posChanged || scaleChanged || rotChanged) {
                    if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                        auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                        int layerIdx = m_selectedGraphicLayerIdx;
                        if (layerIdx < static_cast<int>(gc->layerCount())) {
                            auto* layer = gc->layer(static_cast<size_t>(layerIdx));
                            bool pxC = posChanged && kfCreated(layer->transform().posX, oldPosX);
                            bool pyC = posChanged && kfCreated(layer->transform().posY, oldPosY);
                            bool sxC = scaleChanged && kfCreated(layer->transform().scaleX, oldScX);
                            bool syC = scaleChanged && kfCreated(layer->transform().scaleY, oldScY);
                            bool rtC = rotChanged && kfCreated(layer->transform().rotation, oldRot);
                            bool pxA = posChanged && !layer->transform().posX.isStatic();
                            bool pyA = posChanged && !layer->transform().posY.isStatic();
                            bool sxA = scaleChanged && !layer->transform().scaleX.isStatic();
                            bool syA = scaleChanged && !layer->transform().scaleY.isStatic();
                            bool rtA = rotChanged && !layer->transform().rotation.isStatic();
                            auto cmd = std::make_unique<LambdaCommand>(
                                "Transform Layer",
                                [gc, layerIdx, newPosX, newPosY, newScX, newScY, newRot,
                                 relTick, posChanged, scaleChanged, rotChanged,
                                 pxA, pyA, sxA, syA, rtA]() {
                                    auto* l = gc->layer(static_cast<size_t>(layerIdx));
                                    if (posChanged) {
                                        if (pxA) l->transform().posX.addKeyframe(relTick, newPosX);
                                        else l->transform().posX.setDefaultValue(newPosX);
                                        if (pyA) l->transform().posY.addKeyframe(relTick, newPosY);
                                        else l->transform().posY.setDefaultValue(newPosY);
                                    }
                                    if (scaleChanged) {
                                        if (sxA) l->transform().scaleX.addKeyframe(relTick, newScX);
                                        else l->transform().scaleX.setDefaultValue(newScX);
                                        if (syA) l->transform().scaleY.addKeyframe(relTick, newScY);
                                        else l->transform().scaleY.setDefaultValue(newScY);
                                    }
                                    if (rotChanged) {
                                        if (rtA) l->transform().rotation.addKeyframe(relTick, newRot);
                                        else l->transform().rotation.setDefaultValue(newRot);
                                    }
                                },
                                [gc, layerIdx, oldPosX, oldPosY, oldScX, oldScY, oldRot,
                                 relTick, posChanged, scaleChanged, rotChanged,
                                 pxC, pyC, sxC, syC, rtC, sxWasStatic, syWasStatic]() {
                                    auto* l = gc->layer(static_cast<size_t>(layerIdx));
                                    if (posChanged) {
                                        if (pxC) l->transform().posX.removeKeyframeAtTime(relTick);
                                        else l->transform().posX.writeValue(relTick, oldPosX);
                                        if (pyC) l->transform().posY.removeKeyframeAtTime(relTick);
                                        else l->transform().posY.writeValue(relTick, oldPosY);
                                    }
                                    if (scaleChanged) {
                                        if (sxWasStatic) { l->transform().scaleX.removeKeyframeAtTime(relTick); l->transform().scaleX.setDefaultValue(oldScX); }
                                        else if (sxC) l->transform().scaleX.removeKeyframeAtTime(relTick);
                                        else l->transform().scaleX.writeValue(relTick, oldScX);
                                        if (syWasStatic) { l->transform().scaleY.removeKeyframeAtTime(relTick); l->transform().scaleY.setDefaultValue(oldScY); }
                                        else if (syC) l->transform().scaleY.removeKeyframeAtTime(relTick);
                                        else l->transform().scaleY.writeValue(relTick, oldScY);
                                    }
                                    if (rotChanged) {
                                        if (rtC) l->transform().rotation.removeKeyframeAtTime(relTick);
                                        else l->transform().rotation.writeValue(relTick, oldRot);
                                    }
                                });
                            m_commandStack->pushWithoutExecute(std::move(cmd));
                        }
                    } else {
                        Clip* clip = m_selectedClip;
                        bool pxC = posChanged && kfCreated(clip->positionX(), oldPosX);
                        bool pyC = posChanged && kfCreated(clip->positionY(), oldPosY);
                        bool sxC = scaleChanged && kfCreated(clip->scaleX(), oldScX);
                        bool syC = scaleChanged && kfCreated(clip->scaleY(), oldScY);
                        bool rtC = rotChanged && kfCreated(clip->rotation(), oldRot);
                        bool pxA = posChanged && !clip->positionX().isStatic();
                        bool pyA = posChanged && !clip->positionY().isStatic();
                        bool sxA = scaleChanged && !clip->scaleX().isStatic();
                        bool syA = scaleChanged && !clip->scaleY().isStatic();
                        bool rtA = rotChanged && !clip->rotation().isStatic();
                        auto cmd = std::make_unique<LambdaCommand>(
                            "Transform Clip",
                            [clip, relTick, newPosX, newPosY, newScX, newScY, newRot,
                             posChanged, scaleChanged, rotChanged,
                             pxA, pyA, sxA, syA, rtA]() {
                                if (posChanged) {
                                    if (pxA) clip->positionX().addKeyframe(relTick, newPosX);
                                    else clip->positionX().setDefaultValue(newPosX);
                                    if (pyA) clip->positionY().addKeyframe(relTick, newPosY);
                                    else clip->positionY().setDefaultValue(newPosY);
                                }
                                if (scaleChanged) {
                                    if (sxA) clip->scaleX().addKeyframe(relTick, newScX);
                                    else clip->scaleX().setDefaultValue(newScX);
                                    if (syA) clip->scaleY().addKeyframe(relTick, newScY);
                                    else clip->scaleY().setDefaultValue(newScY);
                                }
                                if (rotChanged) {
                                    if (rtA) clip->rotation().addKeyframe(relTick, newRot);
                                    else clip->rotation().setDefaultValue(newRot);
                                }
                            },
                            [clip, relTick, oldPosX, oldPosY, oldScX, oldScY, oldRot,
                             posChanged, scaleChanged, rotChanged,
                             pxC, pyC, sxC, syC, rtC, sxWasStatic, syWasStatic]() {
                                if (posChanged) {
                                    if (pxC) clip->positionX().removeKeyframeAtTime(relTick);
                                    else clip->positionX().writeValue(relTick, oldPosX);
                                    if (pyC) clip->positionY().removeKeyframeAtTime(relTick);
                                    else clip->positionY().writeValue(relTick, oldPosY);
                                }
                                if (scaleChanged) {
                                    if (sxWasStatic) { clip->scaleX().removeKeyframeAtTime(relTick); clip->scaleX().setDefaultValue(oldScX); }
                                    else if (sxC) clip->scaleX().removeKeyframeAtTime(relTick);
                                    else clip->scaleX().writeValue(relTick, oldScX);
                                    if (syWasStatic) { clip->scaleY().removeKeyframeAtTime(relTick); clip->scaleY().setDefaultValue(oldScY); }
                                    else if (syC) clip->scaleY().removeKeyframeAtTime(relTick);
                                    else clip->scaleY().writeValue(relTick, oldScY);
                                }
                                if (rotChanged) {
                                    if (rtC) clip->rotation().removeKeyframeAtTime(relTick);
                                    else clip->rotation().writeValue(relTick, oldRot);
                                }
                            });
                        m_commandStack->pushWithoutExecute(std::move(cmd));
                    }
                }
            }
        });

        // -- Mask live update: refresh composite during drag --
        connect(ov, &TransformOverlayWidget::maskLiveUpdate,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });

        // -- Motion-path handle drag: refresh composite + monitor while dragging --
        connect(ov, &TransformOverlayWidget::motionPathLiveUpdate,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });

        // -- Mask drag finished: undo support for mask manipulation in Program Monitor --
        connect(ov, &TransformOverlayWidget::maskDragFinished,
                this, [this](int maskIndex, OpacityMask oldMask, OpacityMask newMask) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip || !m_commandStack) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            if (m_effectControlsPanel) {
                auto* track = m_timeline ? m_timeline->track(m_selectedTrackIdx) : nullptr;
                m_effectControlsPanel->setClip(m_selectedClip, track);
            }
            Clip* clip = m_selectedClip;
            int mi = maskIndex;
            auto cmd = std::make_unique<LambdaCommand>(
                "Move Mask",
                [clip, mi, newMask]() {
                    if (mi >= 0 && static_cast<size_t>(mi) < clip->masks().size())
                        clip->masks()[static_cast<size_t>(mi)] = newMask;
                },
                [clip, mi, oldMask]() {
                    if (mi >= 0 && static_cast<size_t>(mi) < clip->masks().size())
                        clip->masks()[static_cast<size_t>(mi)] = oldMask;
                });
            m_commandStack->pushWithoutExecute(std::move(cmd));
        });

        // -- Click on empty area: layer selection (Selection tool) or text creation (Text tool) --
        connect(ov, &TransformOverlayWidget::emptyAreaClicked,
                this, [this](float frameX, float frameY) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timelinePanel || !m_timeline) return;

            // -- Selection/Text tool: hit-test layers across all GraphicClips at playhead --
            if (m_timelinePanel->activeTool() == EditTool::Selection ||
                m_timelinePanel->activeTool() == EditTool::Text)
            {
                if (!m_GraphicsEditorPanel) goto skipHitTest;

                {
                uint32_t outW = m_programMonitor ? m_programMonitor->outputWidth()  : 1920u;
                uint32_t outH = m_programMonitor ? m_programMonitor->outputHeight() : 1080u;
                if (outW == 0) outW = 1920;
                if (outH == 0) outH = 1080;

                // frameX/frameY are in viewport source (composite) resolution.
                // Scale to output (project) resolution to match text layout space.
                float hitX = frameX;
                float hitY = frameY;
                if (m_programMonitor) {
                    uint32_t cW = m_programMonitor->compositeWidth();
                    uint32_t cH = m_programMonitor->compositeHeight();
                    if (cW > 0 && cH > 0) {
                        hitX = frameX * static_cast<float>(outW) / static_cast<float>(cW);
                        hitY = frameY * static_cast<float>(outH) / static_cast<float>(cH);
                    }
                }

                int64_t playheadTick = m_playbackController
                    ? m_playbackController->currentTick() : 0;

                // Lambda: hit-test all layers in a GraphicClip, returns layer index or -1
                // skipIdx: if >= 0, skip that layer (used for cycling)
                auto hitTestGraphicClip = [&](GraphicClip* gc, int skipIdx = -1) -> int {
                    for (int i = static_cast<int>(gc->layerCount()) - 1; i >= 0; --i) {
                        if (i == skipIdx) continue;
                        auto* layer = gc->layer(static_cast<size_t>(i));
                        if (!layer || !layer->isVisible()) continue;

                        int64_t localTick = playheadTick - gc->timelineIn();
                        const auto& xf = layer->transform();
                        float posX   = xf.posX.evaluate(localTick);
                        float posY   = xf.posY.evaluate(localTick);
                        float scaleX = xf.scaleX.evaluate(localTick);
                        float scaleY = xf.scaleY.evaluate(localTick);

                        float cL = 0, cT = 0, cR = 0, cB = 0;
                        if (layer->layerType() == GraphicLayerType::Text) {
                            auto* tl = static_cast<TextLayer*>(layer);
                            QFont font(QString::fromStdString(tl->fontFamily()),
                                       static_cast<int>(tl->fontSize()));
                            font.setWeight(static_cast<QFont::Weight>(tl->fontWeight()));
                            font.setItalic(tl->isItalic());
                            float tracking = tl->tracking().evaluate(localTick);
                            font.setLetterSpacing(QFont::AbsoluteSpacing, static_cast<qreal>(tracking));
                            QString txt = QString::fromStdString(tl->text());
                            if (tl->allCaps()) txt = txt.toUpper();
                            double bigW = static_cast<double>(outW) * 10.0;
                            double bigH = static_cast<double>(outH) * 10.0;
                            QRectF textRect(-bigW * 0.5 + static_cast<double>(outW) * 0.5,
                                            -bigH * 0.5 + static_cast<double>(outH) * 0.5,
                                            bigW, bigH);
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
                            // Use QPainter::drawText to measure � matches renderer
                            QImage metricsImg(static_cast<int>(outW), static_cast<int>(outH), QImage::Format_ARGB32_Premultiplied);
                            QPainter mp(&metricsImg);
                            mp.setFont(font);
                            QRectF tb;
                            mp.drawText(textRect,
                                hAlign | vAlign | Qt::TextWordWrap, txt, &tb);
                            mp.end();
                            float pad = tl->fontSize() * 0.3f;
                            cL = static_cast<float>(tb.left())   - pad;
                            cT = static_cast<float>(tb.top())    - pad;
                            cR = static_cast<float>(tb.right())  + pad;
                            cB = static_cast<float>(tb.bottom()) + pad;
                        } else {
                            auto* sl = static_cast<ShapeLayer*>(layer);
                            float sw = sl->shapeWidth();
                            float sh = sl->shapeHeight();
                            float cx = static_cast<float>(outW) * 0.5f;
                            float cy = static_cast<float>(outH) * 0.5f;
                            cL = cx - sw * 0.5f;
                            cT = cy - sh * 0.5f;
                            cR = cx + sw * 0.5f;
                            cB = cy + sh * 0.5f;
                        }

                        // Match renderer pivot transform:
                        // final = (pt - canvas_center) * scale + canvas_center + pos
                        float canvasCX = static_cast<float>(outW) * 0.5f;
                        float canvasCY = static_cast<float>(outH) * 0.5f;
                        float tL = (cL - canvasCX) * scaleX + canvasCX + posX;
                        float tR = (cR - canvasCX) * scaleX + canvasCX + posX;
                        float tT = (cT - canvasCY) * scaleY + canvasCY + posY;
                        float tB = (cB - canvasCY) * scaleY + canvasCY + posY;

                        if (hitX >= tL && hitX <= tR &&
                            hitY >= tT && hitY <= tB)
                            return i;
                    }
                    return -1;
                };

                // 1) First try the already-selected clip (lowest latency)
                if (m_selectedClip && m_selectedClip->clipType() == ClipType::Graphic) {
                    auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                    int hitIdx = hitTestGraphicClip(gc);
                    if (hitIdx >= 0) {
                        if (hitIdx != m_selectedGraphicLayerIdx) {
                            m_GraphicsEditorPanel->selectLayerByStackIndex(hitIdx);
                        } else {
                            // Already-selected layer was hit � cycle to next
                            // layer underneath by skipping the current one
                            int cycleIdx = hitTestGraphicClip(gc, hitIdx);
                            if (cycleIdx >= 0)
                                m_GraphicsEditorPanel->selectLayerByStackIndex(cycleIdx);
                        }
                        return;
                    }
                }

                // 2) Search all other GraphicClips at the playhead (top tracks first)
                for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                    auto* trk = m_timeline->track(ti);
                    if (!trk || trk->type() != TrackType::Video) continue;
                    for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                        auto* clip = trk->clip(ci);
                        if (!clip || clip->clipType() != ClipType::Graphic) continue;
                        if (clip == m_selectedClip) continue;
                        if (playheadTick < clip->timelineIn() || playheadTick >= clip->timelineOut()) continue;

                        auto* gc = static_cast<GraphicClip*>(clip);
                        int hitIdx = hitTestGraphicClip(gc);
                        if (hitIdx >= 0) {
                            // Switch to this clip and select the hit layer
                            m_selectedClip = clip;
                            m_selectedTrackIdx = ti;
                            m_selectedClipIdx = ci;
                            m_selectedGraphicLayerIdx = -1;
                            if (m_effectControlsPanel) m_effectControlsPanel->setClip(clip, trk);
                            if (m_GraphicsEditorPanel) {
                                m_GraphicsEditorPanel->setClip(clip, trk);
                                m_GraphicsEditorPanel->selectLayerByStackIndex(hitIdx);
                            }
                            if (m_ColorGradingPanel) m_ColorGradingPanel->setClip(clip, trk);
                            if (m_propertiesPanel) m_propertiesPanel->setClip(clip, trk);
                            if (m_timelinePanel) m_timelinePanel->selection().selectClip(ClipRef{ti, clip->id()}, false);
                            scheduleOverlayRefresh();
                            return;
                        }
                    }
                }
                } // end scope for outW/outH/playheadTick
            }
            skipHitTest:
            if (m_timelinePanel->activeTool() != EditTool::Text) return;

            // Find the topmost video track
            Track* targetTrack = nullptr;
            size_t targetTrackIdx = 0;
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                auto* trk = m_timeline->track(ti);
                if (trk && trk->type() == TrackType::Video) {
                    targetTrack = trk;
                    targetTrackIdx = ti;
                    break;
                }
            }
            if (!targetTrack) return;

            // Get playhead position
            int64_t tick = 0;
            if (m_playbackController)
                tick = m_playbackController->currentTick();

            // Check if track is occupied at this position
            for (size_t ci = 0; ci < targetTrack->clipCount(); ++ci) {
                const Clip* c = targetTrack->clip(ci);
                if (tick >= c->timelineIn() && tick < c->timelineOut())
                    return; // occupied
            }

            // Create a 5-second GraphicClip with a TextLayer
            int64_t duration = kTicksPerSecond * 5;
            auto gc = std::make_unique<GraphicClip>();
            gc->setTimelineIn(tick);
            gc->setDuration(duration);
            gc->setSourceIn(0);
            gc->setLabel("Text");

            // Position the text layer at the click position (convert from output coords to reference 1920x1080)
            auto* tl = gc->addTextLayer("Text");
            tl->transform().posX = KeyframeTrack<float>(frameX * (1920.0f / static_cast<float>(m_programMonitor->outputWidth())) - 960.0f);
            tl->transform().posY = KeyframeTrack<float>(frameY * (1080.0f / static_cast<float>(m_programMonitor->outputHeight())) - 540.0f);

            // Route through the command stack so Ctrl+Z undoes the new
            // text layer (previously addClip() was called directly, leaving
            // nothing on the undo stack).
            const uint64_t newClipId = gc->id();
            if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<AddClipCommand>(targetTrack, std::move(gc)));
            } else {
                targetTrack->addClip(std::move(gc));
            }

            // Re-fetch the clip from the track — AddClipCommand owns the
            // unique_ptr until execute(), so the raw pointer must come from
            // the track to stay valid across undo/redo.
            size_t clipIdx = targetTrack->findClipIndexById(newClipId);
            if (clipIdx >= targetTrack->clipCount()) return;
            Clip* ptr = targetTrack->clip(clipIdx);
            if (!ptr) return;
            m_selectedClip = ptr;
            m_selectedTrackIdx = targetTrackIdx;
            m_selectedClipIdx = clipIdx;
            m_selectedGraphicLayerIdx = -1;

            // Refresh everything
            if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();

            // Select in panels â€” setClip triggers layerSelected which sets
            // m_selectedGraphicLayerIdx and calls updateTransformOverlay().
            // We must NOT call updateTransformOverlay() before setClip,
            // otherwise it runs with layerIdx == -1 â†’ full-canvas overlay.
            if (m_effectControlsPanel) m_effectControlsPanel->setClip(ptr, targetTrack);
            if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->setClip(ptr, targetTrack);
            if (m_ColorGradingPanel) m_ColorGradingPanel->setClip(ptr, targetTrack);
            if (m_propertiesPanel) m_propertiesPanel->setClip(ptr, targetTrack);
            scheduleOverlayRefresh();

            // Raise Essential Graphics for the new graphic clip
            if (auto* dock = dockForPanel(QStringLiteral("Essential Graphics"))) {
                dock->setVisible(true);
                dock->raise();
            }

            spdlog::info("Text tool: created GraphicClip at frame ({}, {}), track {}", frameX, frameY, targetTrackIdx);
        });
    }

    // -- Forward tool changes to TransformOverlayWidget -------------------
    if (m_timelinePanel && m_programMonitor && m_programMonitor->transformOverlay()) {
        auto* ov2 = m_programMonitor->transformOverlay();
        connect(m_timelinePanel, &TimelinePanel::toolChanged,
                this, [ov2](EditTool tool) {
            ov2->setEditTool(static_cast<uint8_t>(tool));
        });

        // Double-click a text layer in the Program Monitor → drop an
        // editable text box right on the layer (Premiere Pro). The single
        // click that precedes the double-click already selected the layer
        // and bound it to the panels; the Essential Graphics panel's
        // selectedLayer() is the authoritative source for which layer
        // we're editing.
        auto currentTextLayer = [this]() -> TextLayer* {
            if (!m_GraphicsEditorPanel) return nullptr;
            GraphicLayer* gl = m_GraphicsEditorPanel->selectedLayer();
            if (!gl || gl->layerType() != GraphicLayerType::Text) return nullptr;
            return static_cast<TextLayer*>(gl);
        };

        connect(ov2, &TransformOverlayWidget::textEditRequested,
                this, [this, ov2, currentTextLayer](float, float) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            TextLayer* tl = currentTextLayer();
            if (!tl) return;

            // Pick the first fill colour for the editor text colour.
            QColor textColor(Qt::white);
            const auto& fills = tl->appearance().fills;
            if (!fills.empty())
                textColor = QColor::fromRgba(fills.front().color);

            // Snapshot the current text and clear the layer's text so the
            // rendered text doesn't show through behind the editor box
            // while typing (Premiere Pro). Restored or replaced on commit.
            m_preEditOriginalText = tl->text();
            m_inlineTextEditActive = true;
            tl->setText(std::string{});
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            scheduleOverlayRefresh();

            // Pass the layer's font in REFERENCE units; the overlay scales
            // it to its on-screen content rect so the editor's text matches
            // the rendered text size exactly.
            ov2->beginInlineTextEdit(
                QString::fromStdString(m_preEditOriginalText),
                QString::fromStdString(tl->fontFamily()),
                tl->fontSize(),
                tl->fontWeight(),
                tl->isItalic(),
                textColor);
        });

        connect(ov2, &TransformOverlayWidget::inlineTextCommitted,
                this, [this, currentTextLayer](const QString& newText) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            TextLayer* tl = currentTextLayer();
            if (!tl) {
                m_inlineTextEditActive = false;
                return;
            }
            const std::string newVal = newText.toStdString();
            const std::string oldVal = m_preEditOriginalText;
            const bool wasActive = m_inlineTextEditActive;
            m_inlineTextEditActive = false;
            m_preEditOriginalText.clear();

            auto refresh = [this]() {
                if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
                invalidateCompositeCache();
                if (m_programMonitor) m_programMonitor->requestRefresh();
                scheduleOverlayRefresh();
            };

            // If nothing changed (e.g. cancel/commit unchanged), restore
            // the original text without making an undo entry.
            if (newVal == oldVal) {
                if (wasActive) {
                    tl->setText(oldVal);
                    refresh();
                }
                return;
            }

            // Route through the command stack so Ctrl+Z reverts to the
            // pre-edit text. The layer is currently "" (cleared on begin),
            // so execute() sets it to newVal and undo restores oldVal.
            if (m_commandStack) {
                TextLayer* target = tl;
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Edit Text",
                    [target, newVal, refresh]() {
                        target->setText(newVal);
                        refresh();
                    },
                    [target, oldVal, refresh]() {
                        target->setText(oldVal);
                        refresh();
                    }));
            } else {
                tl->setText(newVal);
                refresh();
            }
        });
    }

    // Refresh Program Monitor whenever timeline content changes
    if (m_timelinePanel && m_programMonitor) {
        connect(m_timelinePanel, &TimelinePanel::contentChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            // Defer spine warm-up + audio reload to next event-loop iteration
            // so razor splits don't block the UI thread.
            invalidateAudioSources();
            schedulePostEditWork();
        });

        // Also refresh when a new clip is created via tools (Text tool, etc.)
        connect(m_timelinePanel, &TimelinePanel::clipCreated,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
    }

    // Refresh Program Monitor when its dock becomes visible again
    // (e.g. after switching tabs away and back). Without this the
    // viewport can show stale content from whatever was rendered at
    // that screen location while tabs were switched.
    auto* dockProgramMonitor = dockForPanel(QStringLiteral("Program Monitor"));
    if (dockProgramMonitor && m_programMonitor) {
        connect(dockProgramMonitor, &QDockWidget::visibilityChanged,
                this, [this](bool visible) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (visible && m_programMonitor) {
                // Flush composite cache so we re-render from current state
                invalidateCompositeCache();
                m_programMonitor->requestRefresh();
            }
        });
    }

    // Refresh Program Monitor when clip properties change
    if (m_propertiesPanel && m_programMonitor) {
        connect(m_propertiesPanel, &PropertiesPanel::propertyChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
#ifdef ROUNDTABLE_HAS_SPINE
            // Refresh timeline track widgets so clip label changes are visible
            if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
            // Spine character/outfit changes may require reloading the engine
            if (m_compositeService && m_selectedClip && m_selectedClip->clipType() == ClipType::Spine) {
                auto* spineClip = dynamic_cast<SpineClip*>(m_selectedClip);
                if (spineClip) {
                    const uint64_t clipId = spineClip->id();
                    m_compositeService->evictSpineState(clipId);

                    const std::string key = m_compositeService->spineCharKey(*spineClip);
                    auto shared = m_compositeService->findSpineSharedData(key);
                    if (shared && !shared->skelBytes.empty()) {
                        m_compositeService->getOrCreateSpineState(spineClip);
                    } else {
                        std::string assetsDir = "assets";
                        if (m_modelManager) assetsDir = m_modelManager->assetsDir();
                        scheduleSpineSharedLoad(
                            spineClip->characterName(), spineClip->outfit(),
                            static_cast<int>(spineClip->stance()), assetsDir);
                    }
                }
            }
#endif
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(m_propertiesPanel, &PropertiesPanel::shotSwitchRequested,
                this, [this](uint64_t groupId, const std::string& newShotName) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            applyShotSwitch(groupId, newShotName);
        });
    }

    // Refresh Program Monitor when Effect Controls change
    if (m_effectControlsPanel && m_programMonitor) {
        connect(m_effectControlsPanel, &EffectControlsPanel::propertyChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            scheduleOverlayRefresh();
        });
        connect(m_effectControlsPanel, &EffectControlsPanel::maskChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            scheduleOverlayRefresh();
        });
        connect(m_effectControlsPanel, &EffectControlsPanel::maskSelected,
                this, [this](int maskIndex) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_programMonitor) {
                auto* ov = m_programMonitor->transformOverlay();
                if (ov) ov->setActiveMaskIndex(maskIndex);
            }
        });
    }

    // Push live volume/pan changes from Effect Controls straight to the
    // AudioEngine mixer so playback reflects edits without rebuilding the
    // source list (which would glitch during scrubbing).
    if (m_effectControlsPanel) {
        connect(m_effectControlsPanel, &EffectControlsPanel::audioLevelsChanged,
                this, [this](uint64_t clipId, float vol, float pan) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline) return;
            // Resolve track mute state so the mixer sees the effective
            // muted flag (otherwise updateSourceLevels would re-enable
            // a muted clip).
            bool muted = false;
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                auto* tr = m_timeline->track(ti);
                if (!tr) continue;
                for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
                    if (tr->clip(ci) && tr->clip(ci)->id() == clipId) {
                        muted = tr->isMuted();
                        break;
                    }
                }
            }
            if (m_audioPlayback)
                m_audioPlayback->updateClipLevels(clipId, vol, pan, muted);
        });
    }

    // Re-sync transform overlay whenever a new frame is displayed.
    // The overlay must track the current tick (properties may be keyframed)
    // and the current compositor resolution (scrub vs settled).
    if (m_programMonitor) {
        connect(m_programMonitor, &ProgramMonitor::frameDisplayed,
                this, [this](int64_t) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_selectedClip && m_selectedGraphicLayerIdx >= 0)
                updateTransformOverlay();
        });
    }

    // Effect Controls seek (mini-timeline scrub and keyframe navigation).
    // Mirrors the main timeline-ruler scrub path so the Program Monitor
    // actually renders the new frame instead of staying on the old one.
    if (m_effectControlsPanel && m_playbackController) {
        connect(m_effectControlsPanel, &EffectControlsPanel::seekRequested,
                this, [this](int64_t tick) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_playbackController->isPlaying())
                m_playbackController->pause();
            m_playbackController->seekTo(tick);
            ensureAudioSourcesLoaded();
            if (m_audioEngine && !m_playbackController->isPlaying()) {
                const uint32_t sr = m_audioEngine->sampleRate();
                const int64_t frame = static_cast<int64_t>(
                    static_cast<double>(tick) / 48000.0 * sr);
                m_audioEngine->scrub(frame);
            }
            // Force the Program Monitor to repaint with the frame at the new
            // playhead — without this it stays on whatever was last rendered.
            if (m_programMonitor) {
                m_programMonitor->notifyScrub();
                m_programMonitor->requestRefresh();
            }
        });
    }

    // -- Eyedropper: switch to pick mode when UI requests it -------------
    if (m_effectControlsPanel && m_programMonitor) {
        auto* ov = m_programMonitor->transformOverlay();
        if (ov) {
            connect(m_effectControlsPanel, &EffectControlsPanel::eyedropperRequested,
                    this, [this, ov](size_t effectIdx) {
                m_eyedropperEffectIdx = effectIdx;
                m_savedEditToolBeforeEyedropper = static_cast<uint8_t>(
                    m_timelinePanel ? m_timelinePanel->activeTool() : EditTool::Selection);
                ov->setEditTool(8); // Eyedropper
            });

            connect(ov, &TransformOverlayWidget::colorPicked,
                    this, [this, ov](float frameX, float frameY) {
                // Restore previous tool
                ov->setEditTool(m_savedEditToolBeforeEyedropper);

                auto frame = m_programMonitor->lastDisplayedFrame();
                if (!frame || !frame->ensurePixels() || frame->pixels.empty())
                    return;

                int px = std::clamp(static_cast<int>(frameX), 0,
                                    static_cast<int>(frame->width) - 1);
                int py = std::clamp(static_cast<int>(frameY), 0,
                                    static_cast<int>(frame->height) - 1);
                const uint8_t* p = frame->pixels.data()
                                   + py * frame->stride + px * 4;
                float r = p[2] / 255.0f;  // BGRA ? R at offset 2
                float g = p[1] / 255.0f;
                float b = p[0] / 255.0f;

                // Write into the Ultra Key effect's KeyColor params
                if (m_selectedClip) {
                    auto& st = m_selectedClip->effects();
                    if (m_eyedropperEffectIdx < st.effectCount()) {
                        auto& fx = st.effect(m_eyedropperEffectIdx);
                        int64_t t = m_playbackController
                            ? m_playbackController->currentTick() - m_selectedClip->timelineIn()
                            : 0;
                        fx.param(ChromaKey::KeyColorR).track.writeValue(t, r);
                        fx.param(ChromaKey::KeyColorG).track.writeValue(t, g);
                        fx.param(ChromaKey::KeyColorB).track.writeValue(t, b);
                        invalidateCompositeCache();
                        m_programMonitor->requestRefresh();
                        m_effectControlsPanel->refresh();
                    }
                }
            });
        }
    }

    // Refresh Program Monitor when Essential Graphics change
    if (m_GraphicsEditorPanel && m_programMonitor) {
        connect(m_GraphicsEditorPanel, &GraphicsEditorPanel::propertyChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            scheduleOverlayRefresh();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        // Track which graphic layer is selected for per-layer transform overlay
        connect(m_GraphicsEditorPanel, &GraphicsEditorPanel::layerSelected,
                this, [this](GraphicLayer* /*layer*/, int layerIdx) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            m_selectedGraphicLayerIdx = layerIdx;
            scheduleOverlayRefresh();
        });
        // (Double-clicking a layer row in Essential Graphics now focuses
        // the in-panel text box directly — handled inside the panel — so
        // it no longer swaps to the Properties panel.)
    }

    // -- Wire ColorGradingPanel property changes to refresh the monitor ----
    if (m_ColorGradingPanel && m_programMonitor) {
        connect(m_ColorGradingPanel, &ColorGradingPanel::propertyChanged,
                this, [this]() {
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
    }

}

}