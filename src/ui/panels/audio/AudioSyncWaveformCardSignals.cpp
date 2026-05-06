/*
 * AudioSyncWaveformCardSignals.cpp - Matched-card waveform signal wiring.
 * Split from AudioSyncCardSections.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "media/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <memory>
namespace rt {
void AudioSync::connectMatchedClipWaveform(MiniWaveformWidget* waveform, size_t clipIdx)
{
    connect(waveform, &MiniWaveformWidget::trimChanged,
                            this, [this, clipIdx, waveform](double inPt, double outPt) {
                        if (clipIdx < m_clips.size()) {
                            // Capture old state on first drag move
                            if (!m_trimDebounceTimer->isActive()) {
                                m_preTrimStart = m_clips[clipIdx].start;
                                m_preTrimEnd   = m_clips[clipIdx].end;
                                m_preTrimMatchState = m_clips[clipIdx].matchState;
                                m_trimDebounceClipIdx = clipIdx;
                            }
                            m_clips[clipIdx].start = inPt;
                            m_clips[clipIdx].end   = outPt;
                            if (m_clips[clipIdx].matchState == 2)
                                m_clips[clipIdx].matchState = 1;
                            if (waveform->isPlayheadVisible()) {
                                double ph = waveform->playhead();
                                if (ph < inPt) waveform->setPlayhead(inPt);
                                else if (ph >= outPt) waveform->setPlayhead(outPt - 0.001);
                            }
                            m_trimDebounceTimer->start();
                            // Rebuild playback buffer so trim changes are heard immediately
                            if (m_playingClipIdx == static_cast<int>(clipIdx))
                                playClip(clipIdx);
                        }
                    });
                    connect(waveform, &MiniWaveformWidget::seekRequested,
                            this, [this, clipIdx, waveform](double t) {
                        if (clipIdx < m_clips.size())
                            t = std::clamp(t, m_clips[clipIdx].start, m_clips[clipIdx].end - 0.001);
                        waveform->setPlayhead(t);
                        waveform->setPlayheadVisible(true);
                        m_selectedClipIdx = static_cast<int>(clipIdx);
                        if (m_playingClipIdx == static_cast<int>(clipIdx))
                            seekPlayingClip(t);
                        else
                            scrubClipAt(clipIdx, t);
                    });
                    connect(waveform, &MiniWaveformWidget::playToggleRequested,
                            this, [this, clipIdx]() {
                        m_selectedClipIdx = static_cast<int>(clipIdx);
                        togglePlayClip(clipIdx);
                    });
                    connect(waveform, &MiniWaveformWidget::shuttleSpeedChanged,
                            this, [this, clipIdx](double speed) {
                        if (clipIdx >= m_clips.size()) {
                            spdlog::warn("AudioSync shuttle: clipIdx {} >= clips.size() {}", clipIdx, m_clips.size());
                            return;
                        }
                        if (!m_audioEngine) {
                            spdlog::warn("AudioSync shuttle: no audio engine");
                            return;
                        }
                        spdlog::info("AudioSync shuttle: clip={} speed={:.2f} playing={} engineState={}",
                                     clipIdx, speed, m_playingClipIdx,
                                     static_cast<int>(m_audioEngine->transportState()));
                        m_selectedClipIdx = static_cast<int>(clipIdx);
                        if (speed == 0.0) {
                            pausePlayback();
                        } else {
                            if (m_playingClipIdx != static_cast<int>(clipIdx)) {
                                playClip(clipIdx);
                                // playClip may fail silently (e.g. no audio data).
                                // If it didn't start, don't try to shuttle.
                                if (m_playingClipIdx != static_cast<int>(clipIdx)) {
                                    spdlog::warn("AudioSync shuttle: playClip failed for clip {}", clipIdx);
                                    return;
                                }
                            }
                            m_audioEngine->setPlaybackSpeed(speed);
                            if (m_audioEngine->transportState() != TransportState::Playing)
                                m_audioEngine->play();
                            if (m_playheadTimer) m_playheadTimer->start();
                        }
                    });
                    connect(waveform, &MiniWaveformWidget::inPointSet,
                            this, [this, clipIdx, waveform](double t) {
                        if (clipIdx < m_clips.size()) {
                            // trimChanged already fired from the same I-key press and
                            // modified start/end/matchState.  Use saved pre-trim state.
                            double oldStart;
                            int oldMS;
                            if (m_trimDebounceTimer->isActive() && m_trimDebounceClipIdx == clipIdx) {
                                m_trimDebounceTimer->stop();
                                oldStart = m_preTrimStart;
                                oldMS    = m_preTrimMatchState;
                            } else {
                                oldStart = m_clips[clipIdx].start;
                                oldMS    = m_clips[clipIdx].matchState;
                                m_clips[clipIdx].start = t;
                                if (m_clips[clipIdx].matchState == 2)
                                    m_clips[clipIdx].matchState = 1;
                            }
                            int newMS = m_clips[clipIdx].matchState;
                            if (m_commandStack) {
                                m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                                    "Set in point",
                                    [this, clipIdx, t, newMS]() {
                                        if (clipIdx < m_clips.size()) {
                                            m_clips[clipIdx].start = t;
                                            m_clips[clipIdx].matchState = newMS;
                                            if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                                wf->setTrimRange(t, m_clips[clipIdx].end);
                                            populateLeftList();
                                            updateCardMatchStyle(clipIdx);
                                            updateSmartBar();
                                        }
                                    },
                                    [this, clipIdx, oldStart, oldMS]() {
                                        if (clipIdx < m_clips.size()) {
                                            m_clips[clipIdx].start = oldStart;
                                            m_clips[clipIdx].matchState = oldMS;
                                            if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                                wf->setTrimRange(oldStart, m_clips[clipIdx].end);
                                            populateLeftList();
                                            updateCardMatchStyle(clipIdx);
                                            updateSmartBar();
                                        }
                                    }
                                ));
                            }
                            populateLeftList();
                            updateCardMatchStyle(clipIdx);
                            updateSmartBar();
                            // Restart playback from the new in-point so the user
                            // always hears the result of their trim.
                            waveform->setPlayhead(t);
                            waveform->setPlayheadVisible(true);
                            playClip(clipIdx);
                        }
                    });
                    connect(waveform, &MiniWaveformWidget::outPointSet,
                            this, [this, clipIdx, waveform](double t) {
                        if (clipIdx < m_clips.size()) {
                            // trimChanged already fired from the same O-key press.
                            double oldEnd;
                            int oldMS;
                            if (m_trimDebounceTimer->isActive() && m_trimDebounceClipIdx == clipIdx) {
                                m_trimDebounceTimer->stop();
                                oldEnd = m_preTrimEnd;
                                oldMS  = m_preTrimMatchState;
                            } else {
                                oldEnd = m_clips[clipIdx].end;
                                oldMS  = m_clips[clipIdx].matchState;
                                m_clips[clipIdx].end = t;
                                if (m_clips[clipIdx].matchState == 2)
                                    m_clips[clipIdx].matchState = 1;
                            }
                            int newMS = m_clips[clipIdx].matchState;
                            if (m_commandStack) {
                                m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                                    "Set out point",
                                    [this, clipIdx, t, newMS]() {
                                        if (clipIdx < m_clips.size()) {
                                            m_clips[clipIdx].end = t;
                                            m_clips[clipIdx].matchState = newMS;
                                            if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                                wf->setTrimRange(m_clips[clipIdx].start, t);
                                            populateLeftList();
                                            updateCardMatchStyle(clipIdx);
                                            updateSmartBar();
                                        }
                                    },
                                    [this, clipIdx, oldEnd, oldMS]() {
                                        if (clipIdx < m_clips.size()) {
                                            m_clips[clipIdx].end = oldEnd;
                                            m_clips[clipIdx].matchState = oldMS;
                                            if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                                wf->setTrimRange(m_clips[clipIdx].start, oldEnd);
                                            populateLeftList();
                                            updateCardMatchStyle(clipIdx);
                                            updateSmartBar();
                                        }
                                    }
                                ));
                            }
                            populateLeftList();
                            updateCardMatchStyle(clipIdx);
                            updateSmartBar();
                            // Rebuild playback buffer so out-point change is heard immediately
                            if (m_playingClipIdx == static_cast<int>(clipIdx))
                                playClip(clipIdx);
                        }
                    });
                    connect(waveform, &MiniWaveformWidget::deleteRegionRequested,
                            this, [this, clipIdx](double s, double e) {
                        if (clipIdx < m_clips.size()) {
                            int oldMS = m_clips[clipIdx].matchState;
                            m_clips[clipIdx].deletedRegions.emplace_back(s, e);
                            if (m_clips[clipIdx].matchState == 2)
                                m_clips[clipIdx].matchState = 1;
                            int newMS = m_clips[clipIdx].matchState;
                            if (m_commandStack) {
                                m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                                    "Delete audio region",
                                    [this, clipIdx, s, e, newMS]() {
                                        if (clipIdx < m_clips.size()) {
                                            m_clips[clipIdx].deletedRegions.emplace_back(s, e);
                                            m_clips[clipIdx].matchState = newMS;
                                            if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                                wf->setDeletedRegions(m_clips[clipIdx].deletedRegions);
                                            populateLeftList();
                                            updateCardMatchStyle(clipIdx);
                                            updateSmartBar();
                                        }
                                    },
                                    [this, clipIdx, oldMS]() {
                                        if (clipIdx < m_clips.size() && !m_clips[clipIdx].deletedRegions.empty()) {
                                            m_clips[clipIdx].deletedRegions.pop_back();
                                            m_clips[clipIdx].matchState = oldMS;
                                            if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                                wf->setDeletedRegions(m_clips[clipIdx].deletedRegions);
                                            populateLeftList();
                                            updateCardMatchStyle(clipIdx);
                                            updateSmartBar();
                                        }
                                    }
                                ));
                            }
                        }
                    });
                    connect(waveform, &MiniWaveformWidget::deletedRegionsChanged,
                            this, [this, clipIdx, waveform]() {
                        if (clipIdx < m_clips.size()) {
                            auto oldRegions = m_clips[clipIdx].deletedRegions;
                            int oldMS = m_clips[clipIdx].matchState;
                            m_clips[clipIdx].deletedRegions = waveform->deletedRegions();
                            if (m_clips[clipIdx].matchState == 2)
                                m_clips[clipIdx].matchState = 1;
                            int newMS = m_clips[clipIdx].matchState;
                            if (m_playingClipIdx == static_cast<int>(clipIdx))
                                playClip(clipIdx);
                            populateLeftList();
                            updateCardMatchStyle(clipIdx);
                            updateSmartBar();
                            if (m_commandStack) {
                                auto newRegions = m_clips[clipIdx].deletedRegions;
                                m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                                    "Edit deleted regions",
                                    [this, clipIdx, newRegions, newMS]() {
                                        if (clipIdx < m_clips.size()) {
                                            m_clips[clipIdx].deletedRegions = newRegions;
                                            m_clips[clipIdx].matchState = newMS;
                                            if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                                wf->setDeletedRegions(newRegions);
                                            populateLeftList();
                                            updateCardMatchStyle(clipIdx);
                                            updateSmartBar();
                                        }
                                    },
                                    [this, clipIdx, oldRegions, oldMS]() {
                                        if (clipIdx < m_clips.size()) {
                                            m_clips[clipIdx].deletedRegions = oldRegions;
                                            m_clips[clipIdx].matchState = oldMS;
                                            if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                                wf->setDeletedRegions(oldRegions);
                                            populateLeftList();
                                            updateCardMatchStyle(clipIdx);
                                            updateSmartBar();
                                        }
                                    }
                                ));
                            }
                        }
                    });
}
} // namespace rt