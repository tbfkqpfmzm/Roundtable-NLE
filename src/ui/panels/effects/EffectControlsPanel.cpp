/*
 * EffectControlsPanel.cpp -- clip binding, transform, and keyframe ops.
 *
 * UI construction  --> EffectControlsPanelUI.cpp
 * Property tree    --> EffectControlsPanelTree.cpp
 */

#include "panels/effects/EffectControlsPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/Track.h"
#include "timeline/Timeline.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/KeyframeMode.h"
#include "media/PlaybackController.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/EffectCommands.h"
#include "command/commands/KeyframeCmds.h"

#include <cmath>

namespace rt {

namespace {
// Audio-volume spin is displayed in dB. Keyframe track stores linear gain.
// dB range used by the UI: [-60 .. +12]. Values at the min are treated as
// effectively muted (1e-3 gain) for a smooth ramp.
constexpr float kAudioVolumeMinDb = -60.0f;
constexpr float kAudioVolumeMaxDb = 12.0f;

inline float dbToGain(float db) noexcept {
    if (db <= kAudioVolumeMinDb) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}
inline float gainToDb(float gain) noexcept {
    if (gain <= 0.0f) return kAudioVolumeMinDb;
    float db = 20.0f * std::log10(gain);
    if (db < kAudioVolumeMinDb) db = kAudioVolumeMinDb;
    if (db > kAudioVolumeMaxDb) db = kAudioVolumeMaxDb;
    return db;
}
} // namespace

void EffectControlsPanel::setClip(Clip* clip, Track* track)
{
    if (m_clip == clip) return;
    m_clip = clip;
    m_track = track;
    m_updating = true;

    clearPropertyTree();

    if (clip) {
        m_clipNameLabel->setText(QString::fromStdString(clip->label()));

        const char* typeStr = "Video";
        QColor badgeColor;
        const auto& tc = Theme::colors();
        switch (clip->clipType()) {
        case ClipType::Spine:      typeStr = "Spine";      badgeColor = QColor(200, 170, 255); break;
        case ClipType::Video:      typeStr = "Video";      badgeColor = tc.accent; break;
        case ClipType::Audio:      typeStr = "Audio";      badgeColor = tc.success; break;
        case ClipType::Title:      typeStr = "Title";      badgeColor = QColor(230, 180, 80); break;
        case ClipType::Adjustment: typeStr = "Adjustment"; badgeColor = QColor(180, 180, 180); break;
        case ClipType::Image:      typeStr = "Image";      badgeColor = tc.accent; break;
        case ClipType::Graphic:    typeStr = "Graphic";    badgeColor = QColor(230, 130, 80); break;
        case ClipType::Sequence:   typeStr = "Sequence";   badgeColor = QColor(180, 130, 230); break;
        }
        m_clipTypeLabel->setText(typeStr);
        m_clipTypeLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; font-size: 11px; font-weight: bold; "
            "background: %2; border-radius: 3px; padding: 1px 6px; }")
            .arg(Theme::hex(tc.textPrimary),
                 Theme::hex(badgeColor.darker(200))));

        buildPropertyTree();
        populateFromClip();

        m_kfTimeline->setClip(clip);
        m_kfTimeline->setPropertyRows(m_propertyRows);

        // Show property tree, hide empty state
        m_splitter->show();
        m_emptyLabel->hide();
    } else {
        m_clipNameLabel->setText("No clip selected");
        m_clipTypeLabel->clear();
        m_clipTypeLabel->setStyleSheet(QStringLiteral(
            "QLabel { background: transparent; border: none; }"));
        m_kfTimeline->setClip(nullptr);

        // Hide property tree, show empty state
        m_splitter->hide();
        m_emptyLabel->show();
    }

    m_updating = false;
    emit clipChanged(clip);
}

void EffectControlsPanel::clearClip()
{
    setClip(nullptr);
}

void EffectControlsPanel::refresh()
{
    if (!m_clip) return;
    m_updating = true;
    clearPropertyTree();
    buildPropertyTree();
    populateFromClip();
    m_kfTimeline->setPropertyRows(m_propertyRows);
    m_updating = false;

    // Update property-row button states (diamond add/remove, prev/next)
    // and force the mini-timeline to repaint with current keyframe data.
    int64_t relTick = (m_playheadTick >= m_clip->timelineIn())
                    ? (m_playheadTick - m_clip->timelineIn()) : 0;
    for (auto* row : m_propertyRows)
        row->updateForTime(relTick);
    m_kfTimeline->update();
}


void EffectControlsPanel::setPlayheadTick(int64_t tick)
{
    m_playheadTick = tick;
    m_kfTimeline->setPlayheadTick(tick);

    // Update keyframe nav buttons for current time
    int64_t relTick = m_clip ? (tick - m_clip->timelineIn()) : tick;
    for (auto* row : m_propertyRows) {
        row->updateForTime(relTick);
    }

    // Update footer timecode (assume 24fps if no clip frame rate available)
    if (m_footerTimecodeLabel) {
        double fps = 24.0;
        auto tc = tickToTimecode(tick, fps);
        m_footerTimecodeLabel->setText(QString::fromStdString(tc.toString()));
    }

    // Update spin box values to reflect evaluated keyframes at current time
    if (m_clip && !m_updating) {
        m_updating = true;
        populateFromClip();
        m_updating = false;
    }
}

int64_t EffectControlsPanel::clipRelativeTick() const noexcept
{
    if (!m_clip) return 0;
    return m_playheadTick - m_clip->timelineIn();
}


EffectControlsPanel::TransformState EffectControlsPanel::captureTransformState() const
{
    if (!m_clip) return {};
    int64_t t = clipRelativeTick();
    TransformState s{
        m_clip->positionX().evaluate(t),
        m_clip->positionY().evaluate(t),
        m_clip->scaleX().evaluate(t),
        m_clip->scaleY().evaluate(t),
        m_clip->rotation().evaluate(t),
        m_clip->opacity().evaluate(t),
        m_clip->speed()
    };
    if (auto* ac = dynamic_cast<AudioClip*>(m_clip)) {
        s.pan    = ac->pan().evaluate(t);
        s.volume = ac->volume().evaluate(t);
    }
    return s;
}

void EffectControlsPanel::restoreTransformState(const TransformState& s)
{
    if (!m_clip) return;
    int64_t t = clipRelativeTick();
    m_clip->positionX().writeValue(t, s.posX);
    m_clip->positionY().writeValue(t, s.posY);
    m_clip->scaleX().writeValue(t, s.scaleX);
    m_clip->scaleY().writeValue(t, s.scaleY);
    m_clip->rotation().writeValue(t, s.rotation);
    m_clip->opacity().writeValue(t, s.opacity);
    m_clip->setSpeed(s.speed);
    if (auto* ac = dynamic_cast<AudioClip*>(m_clip)) {
        ac->pan().writeValue(t, s.pan);
        ac->volume().writeValue(t, s.volume);
    }
}


void EffectControlsPanel::applyTransformLive()
{
    // Called during scrub drag — update ONLY the property being scrubbed.
    // Non-animated tracks (no keyframes) update the default value.
    // Animated tracks (has keyframes) write at the current playhead time.
    if (!m_clip || m_updating) return;

    auto* spin = qobject_cast<ScrubbySpinBox*>(sender());
    if (!spin) return;

    // Helper: write to a track — animated tracks get a keyframe, static tracks update default
    auto writeTrack = [this](KeyframeTrack<float>& track, float val) {
        track.writeValue(clipRelativeTick(), val);
    };

    if (spin == m_posXSpin) {
        writeTrack(m_clip->positionX(), static_cast<float>(spin->value()));
    } else if (spin == m_posYSpin) {
        writeTrack(m_clip->positionY(), static_cast<float>(spin->value()));
    } else if (spin == m_scaleSpin) {
        float s = static_cast<float>(spin->value() / 100.0);
        writeTrack(m_clip->scaleX(), s);
        if (m_uniformScaleCheck && m_uniformScaleCheck->isChecked())
            writeTrack(m_clip->scaleY(), s);
    } else if (spin == m_scaleWSpin) {
        writeTrack(m_clip->scaleY(), static_cast<float>(spin->value() / 100.0));
    } else if (spin == m_rotationSpin) {
        writeTrack(m_clip->rotation(), static_cast<float>(spin->value()));
    } else if (spin == m_opacitySpin) {
        writeTrack(m_clip->opacity(), static_cast<float>(spin->value() / 100.0));
    } else if (spin == m_speedSpin) {
        m_clip->setSpeed(spin->value() / 100.0);
    } else if (auto* ac = dynamic_cast<AudioClip*>(m_clip)) {
        if (spin == m_panSpin) {
            float newPan = static_cast<float>(spin->value() / 100.0);
            writeTrack(ac->pan(), newPan);
            emit audioLevelsChanged(ac->id(), ac->volume().evaluate(clipRelativeTick()), newPan);
        }
        else if (spin == m_audioVolumeSpin) {
            // Spin displays dB; keyframe track stores linear gain.
            float newGain = dbToGain(static_cast<float>(spin->value()));
            writeTrack(ac->volume(), newGain);
            emit audioLevelsChanged(ac->id(), newGain, ac->pan().evaluate(clipRelativeTick()));
        }
    }

    emit propertyChanged();
}


void EffectControlsPanel::commitTransform(double /*oldVal*/, double /*newVal*/)
{
    // Called at end of scrub — push per-property undo command.
    if (!m_clip || m_updating) return;

    auto* spin = qobject_cast<ScrubbySpinBox*>(sender());
    if (!spin) return;

    // Identify which track this spin operates on
    KeyframeTrack<float>* track = nullptr;
    double factor = 1.0;  // spin-display-value = track-value * factor
    bool uniformScale = false;

    if      (spin == m_posXSpin)     { track = &m_clip->positionX(); }
    else if (spin == m_posYSpin)     { track = &m_clip->positionY(); }
    else if (spin == m_scaleSpin)    { track = &m_clip->scaleX(); factor = 100.0;
                                       uniformScale = m_uniformScaleCheck && m_uniformScaleCheck->isChecked(); }
    else if (spin == m_scaleWSpin)   { track = &m_clip->scaleY(); factor = 100.0; }
    else if (spin == m_rotationSpin) { track = &m_clip->rotation(); }
    else if (spin == m_opacitySpin)  { track = &m_clip->opacity(); factor = 100.0; }
    else if (spin == m_speedSpin) {
        // Speed is not a keyframe track
        double oldSpd = spin->scrubStartValue() / 100.0;
        double newSpd = m_clip->speed();
        Clip* c = m_clip; auto* p = this;
        if (m_commandStack)
            m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                "Set Speed",
                [c, newSpd, p]() { c->setSpeed(newSpd); p->populateFromClip(); emit p->propertyChanged(); },
                [c, oldSpd, p]() { c->setSpeed(oldSpd); p->populateFromClip(); emit p->propertyChanged(); }));
        return;
    } else if (auto* ac = dynamic_cast<AudioClip*>(m_clip)) {
        if      (spin == m_panSpin)         { track = &ac->pan(); factor = 100.0; }
        else if (spin == m_audioVolumeSpin) { track = &ac->volume(); /* dB conversion handled below */ }
    }

    if (!track) return;  // crop / anchor / anti-flicker — no track yet

    float oldF;
    float newF;
    if (spin == m_audioVolumeSpin) {
        oldF = dbToGain(static_cast<float>(spin->scrubStartValue()));
        newF = dbToGain(static_cast<float>(spin->value()));
    } else {
        oldF = static_cast<float>(spin->scrubStartValue() / factor);
        newF = static_cast<float>(spin->value() / factor);
    }
    int64_t t = (track->keyframeCount() > 0) ? clipRelativeTick() : 0;

    auto* trk = track;
    auto* panel = this;

    // Detect if the scrub created a NEW keyframe (vs updating an existing one).
    // If so, undo must remove it rather than just restoring the old value.
    auto kfWasCreated = [](const KeyframeTrack<float>& tk, int64_t time, float oldVal) -> bool {
        if (tk.isStatic() || tk.keyframeCount() < 2) return false;
        if (!tk.hasKeyframeAt(time)) return false;
        // Evaluate without the KF at time — if result matches oldVal, the KF
        // was created by the scrub (the old value came from interpolation).
        KeyframeTrack<float> tmp(tk.defaultValue());
        for (const auto& kf : tk.keyframes()) {
            if (kf.time != time) tmp.restoreKeyframe(kf);
        }
        return std::abs(tmp.evaluate(time) - oldVal) < 0.01f;
    };

    bool createdKF = kfWasCreated(*trk, t, oldF);
    // Auto-keyframe gate: with global Auto-Keyframe OFF (default), spinbox edits
    // only update an existing keyframe at the playhead time on already-animated
    // tracks. They do not create new keyframes mid-edit (Premiere-style).
    const bool autoKf = KeyframeMode::isAutoEnabled();
    bool wasAnimated = (trk->keyframeCount() > 0) && (autoKf || trk->hasKeyframeAt(t));

    // Also handle uniform scale (mirror to scaleY)
    KeyframeTrack<float>* trkY = uniformScale ? &m_clip->scaleY() : nullptr;
    int64_t tY = trkY ? ((trkY->keyframeCount() > 0) ? clipRelativeTick() : 0) : 0;
    bool createdKFY = trkY ? kfWasCreated(*trkY, tY, oldF) : false;
    bool wasAnimatedY = trkY && (trkY->keyframeCount() > 0) && (autoKf || trkY->hasKeyframeAt(tY));

    if (m_commandStack) {
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Transform",
            [trk, t, newF, wasAnimated, trkY, tY, wasAnimatedY, panel]() {
                if (wasAnimated) trk->addKeyframe(t, newF);
                else trk->setDefaultValue(newF);
                if (trkY) {
                    if (wasAnimatedY) trkY->addKeyframe(tY, newF);
                    else trkY->setDefaultValue(newF);
                }
                panel->populateFromClip();
                emit panel->propertyChanged();
            },
            [trk, t, oldF, createdKF, trkY, tY, createdKFY, panel]() {
                if (createdKF)
                    trk->removeKeyframeAtTime(t);
                else
                    trk->writeValue(t, oldF);
                if (trkY) {
                    if (createdKFY) trkY->removeKeyframeAtTime(tY);
                    else trkY->writeValue(tY, oldF);
                }
                panel->populateFromClip();
                emit panel->propertyChanged();
            }));
    }
}

void EffectControlsPanel::applyTransform()
{
    // Keyboard entry path — delegates to the same per-property logic
    if (!m_clip || m_updating) return;
    applyTransformLive();
}

// ── Keyframe operations ─────────────────────────────────────────────────────



void EffectControlsPanel::onAddKeyframe(KeyframeTrack<float>* track, int64_t time)
{
    if (!track) return;
    float val = track->evaluate(time);
    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<AddKeyframeCommand>(track, time, val));
    } else {
        track->addKeyframe(time, val);
    }
    // Refresh button states so the diamond switches to "delete" mode
    for (auto* row : m_propertyRows)
        row->updateForTime(time);
    m_kfTimeline->update();
    emit propertyChanged();
}

void EffectControlsPanel::onDeleteKeyframe(KeyframeTrack<float>* track, int64_t time)
{
    if (!track) return;
    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<RemoveKeyframeCommand>(track, time));
    } else {
        track->removeKeyframeAtTime(time);
    }
    // Refresh button states so the diamond switches to "add" mode
    for (auto* row : m_propertyRows)
        row->updateForTime(time);
    m_kfTimeline->update();
    emit propertyChanged();
}

void EffectControlsPanel::onGoToPrevKeyframe(KeyframeTrack<float>* track)
{
    if (!track || !m_clip) return;
    int64_t relTick = m_playheadTick - m_clip->timelineIn();
    int64_t prevTime = -1;
    for (size_t i = 0; i < track->keyframeCount(); ++i) {
        int64_t t = track->keyframe(i).time;
        if (t < relTick) prevTime = t;
    }
    if (prevTime >= 0) {
        int64_t absTick = prevTime + m_clip->timelineIn();
        m_kfTimeline->setPlayheadTick(absTick);
        emit seekRequested(absTick);
    }
}

void EffectControlsPanel::onGoToNextKeyframe(KeyframeTrack<float>* track)
{
    if (!track || !m_clip) return;
    int64_t relTick = m_playheadTick - m_clip->timelineIn();
    for (size_t i = 0; i < track->keyframeCount(); ++i) {
        int64_t t = track->keyframe(i).time;
        if (t > relTick) {
            int64_t absTick = t + m_clip->timelineIn();
            m_kfTimeline->setPlayheadTick(absTick);
            emit seekRequested(absTick);
            return;
        }
    }
}

// ── Effect deletion ─────────────────────────────────────────────────────

void EffectControlsPanel::deleteEffect(size_t index)
{
    if (!m_clip || index >= m_clip->effects().effectCount()) return;
    if (m_commandStack) {
        m_commandStack->execute(
            std::make_unique<RemoveEffectCommand>(&m_clip->effects(), index));
    }
    m_selectedEffectIndex = -1;
    refresh();
    emit propertyChanged();
}

void EffectControlsPanel::deleteSelectedEffect()
{
    if (m_selectedEffectIndex >= 0)
        deleteEffect(static_cast<size_t>(m_selectedEffectIndex));
}

void EffectControlsPanel::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        && m_selectedEffectIndex >= 0) {
        deleteEffect(static_cast<size_t>(m_selectedEffectIndex));
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool EffectControlsPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto* w = qobject_cast<QWidget*>(watched);
        if (w) {
            const auto& tc = Theme::colors();

            // ── Check if a mask header was clicked ──────────────────────
            QVariant maskIdxVar = w->property("maskIndex");
            if (maskIdxVar.isValid()) {
                int clickedMask = maskIdxVar.toInt();
                emit maskSelected(clickedMask);
                // Highlight selected mask header, dim others
                for (auto& sec : m_sectionArrows) {
                    QVariant v = sec.header->property("maskIndex");
                    if (v.isValid()) {
                        bool selected = (v.toInt() == clickedMask);
                        sec.header->setStyleSheet(QStringLiteral(
                            "background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2;")
                            .arg(Theme::hex(selected ? tc.accentDim : tc.surface2),
                                 Theme::hex(tc.border)));
                    }
                }
                return false; // let event propagate for collapse toggle
            }

            // Find which effect header was clicked
            for (size_t i = 0; i < m_effectHeaders.size(); ++i) {
                // Reset all headers to default style
                m_effectHeaders[i]->setStyleSheet(QStringLiteral(
                    "background: %1; border-bottom: 1px solid %2;")
                    .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
            }
            for (size_t i = 0; i < m_effectHeaders.size(); ++i) {
                if (m_effectHeaders[i] == w) {
                    m_selectedEffectIndex = static_cast<int>(i);
                    // Highlight selected header
                    w->setStyleSheet(QStringLiteral(
                        "background: %1; border-bottom: 1px solid %2;")
                        .arg(Theme::hex(tc.accentDim), Theme::hex(tc.border)));
                    setFocus(); // Ensure we get key events
                    break;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace rt
