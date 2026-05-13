/*
 * TimelineWorkspacePanelsShortcuts.cpp — Keyboard shortcut registration
 * extracted from TimelineWorkspacePanels.cpp::buildPanels().
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "command/CommandStack.h"
#include "command/commands/TransitionCmds.h"
#include "MainWindow.h"
#include "media/PlaybackController.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/project/ProjectBin.h"
#include "panels/timeline/TimelinePanel.h"
#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"

#include <QApplication>
#include <QKeySequence>
#include <QLineEdit>
#include <QShortcut>
#include <QWidget>

namespace rt {

void TimelineWorkspace::registerKeyboardShortcuts()
{
    setFocusPolicy(Qt::StrongFocus);
    for (auto* btn : m_toolButtons)
        if (btn) btn->setFocusPolicy(Qt::NoFocus);

    auto addShortcut = [this](const QKeySequence& key, auto&& fn) {
        auto* sc = new QShortcut(key, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, std::forward<decltype(fn)>(fn));
    };

    // Home / End: go to start / end of timeline
    addShortcut(Qt::Key_Home, [this]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw)) return;
        if (m_playbackController) m_playbackController->goToStart();
    });
    addShortcut(Qt::Key_End, [this]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw)) return;
        if (m_playbackController) m_playbackController->goToEnd();
    });

    // Shift+I / Shift+O: go to in/out point
    addShortcut(Qt::SHIFT | Qt::Key_I, [this]() {
        if (m_playbackController) m_playbackController->goToInPoint();
    });
    addShortcut(Qt::SHIFT | Qt::Key_O, [this]() {
        if (m_playbackController) m_playbackController->goToOutPoint();
    });
    // Alt+X: clear in/out
    addShortcut(Qt::ALT | Qt::Key_X, [this]() {
        if (m_timeline) {
            EditOperations::clearInOutPoints(*m_timeline);
            if (m_timelinePanel) m_timelinePanel->updateInOutRange();
            syncProgramMonitorInOut();
        }
    });
    // Ctrl+Shift+X: clear in/out points
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_X, [this]() {
        if (m_timeline) {
            EditOperations::clearInOutPoints(*m_timeline);
            if (m_timelinePanel) m_timelinePanel->updateInOutRange();
            syncProgramMonitorInOut();
        }
    });
    // Ctrl+Shift+V: Paste Attributes dialog
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_V, [this]() {
        if (m_timelinePanel) m_timelinePanel->showPasteAttributesDialog();
    });
    // Ctrl+Shift+C: Paste Insert
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_C, [this]() {
        if (m_timeline && m_timelinePanel && m_commandStack && !m_timelinePanel->clipboard().empty()) {
            auto cmd = EditOperations::pasteInsert(
                *m_timeline, m_timelinePanel->clipboard(),
                m_playbackController ? m_playbackController->currentTick() : 0);
            if (cmd) {
                m_commandStack->execute(std::move(cmd));
                if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
                invalidateAudioSources();
                invalidateCompositeCache();
                updateTransformOverlay();
                if (m_programMonitor) m_programMonitor->requestRefresh();
                schedulePostEditWork();
            }
        }
    });
    // Ctrl+V: paste at playhead (or paste layer if Essential Graphics focused)
    addShortcut(Qt::CTRL | Qt::Key_V, [this]() {
        auto* fw = QApplication::focusWidget();
        bool egFocused = m_GraphicsEditorPanel && m_GraphicsEditorPanel->isAncestorOf(fw);
        bool pmFocused = m_programMonitor && m_programMonitor->isAncestorOf(fw);
        if (m_GraphicsEditorPanel && (egFocused || (pmFocused && m_selectedGraphicLayerIdx >= 0))) {
            m_GraphicsEditorPanel->pasteLayer();
            invalidateCompositeCache();
            scheduleOverlayRefresh();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            return;
        }
        if (m_timeline && m_timelinePanel && m_commandStack && !m_timelinePanel->clipboard().empty()) {
            auto cmd = EditOperations::paste(
                *m_timeline, m_timelinePanel->clipboard(),
                m_playbackController ? m_playbackController->currentTick() : 0);
            if (cmd) {
                m_commandStack->execute(std::move(cmd));
                if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
                invalidateAudioSources();
                invalidateCompositeCache();
                updateTransformOverlay();
                if (m_programMonitor) m_programMonitor->requestRefresh();
                schedulePostEditWork();
            }
        }
    });
    // Ctrl+X: cut
    addShortcut(Qt::CTRL | Qt::Key_X, [this]() {
        if (!m_timeline || !m_timelinePanel || !m_commandStack) return;
        auto& cb = m_timelinePanel->mutableClipboard();
        auto cmd = EditOperations::cutSelection(*m_timeline,
            m_timelinePanel->selection(), cb);
        if (cmd) {
            m_timelinePanel->selection().clear();
            m_commandStack->execute(std::move(cmd));
            m_timelinePanel->refreshTrackContents();
            invalidateAudioSources();
            invalidateCompositeCache();
            updateTransformOverlay();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        }
    });
    // Ctrl+C: copy (or copy layer if Essential Graphics focused)
    addShortcut(Qt::CTRL | Qt::Key_C, [this]() {
        auto* fw = QApplication::focusWidget();
        bool egFocused = m_GraphicsEditorPanel && m_GraphicsEditorPanel->isAncestorOf(fw);
        bool pmFocused = m_programMonitor && m_programMonitor->isAncestorOf(fw);
        if (m_GraphicsEditorPanel && (egFocused || (pmFocused && m_selectedGraphicLayerIdx >= 0))) {
            m_GraphicsEditorPanel->copySelectedLayer();
            return;
        }
        if (!m_timeline || !m_timelinePanel) return;
        EditOperations::copySelection(*m_timeline,
            m_timelinePanel->selection(),
            m_timelinePanel->mutableClipboard());
        m_timelinePanel->copyAttributesFromSelection();
    });
    // Shift+Delete / Shift+Backspace: extract (ripple delete)
    addShortcut(Qt::SHIFT | Qt::Key_Delete, [this]() {
        if (!m_timeline || !m_timelinePanel || !m_commandStack) return;
        auto cmd = EditOperations::rippleDelete(*m_timeline,
            m_timelinePanel->selection());
        if (cmd) {
            m_timelinePanel->selection().clear();
            m_commandStack->execute(std::move(cmd));
            m_timelinePanel->refreshTrackContents();
            invalidateAudioSources();
            invalidateCompositeCache();
            updateTransformOverlay();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            schedulePostEditWork();
        }
    });
    addShortcut(Qt::SHIFT | Qt::Key_Backspace, [this]() {
        if (!m_timeline || !m_timelinePanel || !m_commandStack) return;
        auto cmd = EditOperations::rippleDelete(*m_timeline,
            m_timelinePanel->selection());
        if (cmd) {
            m_timelinePanel->selection().clear();
            m_commandStack->execute(std::move(cmd));
            m_timelinePanel->refreshTrackContents();
            invalidateAudioSources();
            invalidateCompositeCache();
            updateTransformOverlay();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            schedulePostEditWork();
        }
    });
    // Ctrl+A: select all
    addShortcut(Qt::CTRL | Qt::Key_A, [this]() {
        if (m_projectBin && m_projectBin->isAncestorOf(
                QApplication::focusWidget())) {
            m_projectBin->selectAllItems();
            return;
        }
        if (!m_timeline || !m_timelinePanel) return;
        m_timelinePanel->selection().selectAll(*m_timeline);
        emit m_timelinePanel->selectionChanged();
    });
    // Ctrl+Shift+A: deselect all
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_A, [this]() {
        if (!m_timelinePanel) return;
        m_timelinePanel->selection().clear();
        emit m_timelinePanel->selectionChanged();
    });

    // Ctrl+B: New Bin when Project Bin is focused
    addShortcut(Qt::CTRL | Qt::Key_B, [this]() {
        if (m_projectBin && m_projectBin->isAncestorOf(
                QApplication::focusWidget())) {
            m_projectBin->createNewBin();
            return;
        }
    });

    // Ctrl+T: add default transition
    addShortcut(Qt::CTRL | Qt::Key_T, [this]() {
        if (!m_timeline || !m_commandStack || !m_timelinePanel) return;
        auto edge = m_timelinePanel->lastClickedEdge();
        if (!edge.valid) return;

        Track* track = m_timeline->track(edge.clipRef.trackIndex);
        if (!track) return;

        size_t clipIdx = track->findClipIndexById(edge.clipRef.clipId);
        if (clipIdx >= track->clipCount()) return;

        const Clip* clip = track->clip(clipIdx);
        Transition trans;
        trans.duration = kDefaultTransitionDuration;

        if (edge.edge == ClipEdge::Head) {
            const Clip* leftClip = nullptr;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                if (ci == clipIdx) continue;
                const Clip* c = track->clip(ci);
                int64_t gap = std::abs(c->timelineOut() - clip->timelineIn());
                if (gap <= 1600) {
                    leftClip = c;
                    break;
                }
            }
            if (leftClip) {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = leftClip->id();
                trans.rightClipId = clip->id();
                trans.editPointTick = clip->timelineIn();
            } else {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = 0;
                trans.rightClipId = clip->id();
                trans.editPointTick = clip->timelineIn();
            }
        } else {
            const Clip* rightClip = nullptr;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                if (ci == clipIdx) continue;
                const Clip* c = track->clip(ci);
                int64_t gap = std::abs(c->timelineIn() - clip->timelineOut());
                if (gap <= 1600) {
                    rightClip = c;
                    break;
                }
            }
            if (rightClip) {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = clip->id();
                trans.rightClipId = rightClip->id();
                trans.editPointTick = clip->timelineOut();
            } else {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = clip->id();
                trans.rightClipId = 0;
                trans.editPointTick = clip->timelineOut();
            }
        }

        bool alreadyExists = false;
        for (size_t ti2 = 0; ti2 < track->transitionCount(); ++ti2) {
            const Transition* existing = track->transition(ti2);
            if (existing && existing->editPointTick == trans.editPointTick) {
                alreadyExists = true;
                break;
            }
        }
        if (alreadyExists) return;

        auto cmd = std::make_unique<AddTransitionCommand>(
            track, clipIdx, clipIdx, trans);
        m_commandStack->execute(std::move(cmd));

        invalidateCompositeCache();
        if (m_timelinePanel) m_timelinePanel->rebuildTracks();
        if (m_programMonitor) m_programMonitor->requestRefresh();
    });

    // Ctrl+=: zoom in
    addShortcut(Qt::CTRL | Qt::Key_Equal, [this]() {
        if (m_timelinePanel) {
            auto& engine = m_timelinePanel->layoutEngine();
            double anchorPx = engine.viewportWidth() * 0.5;
            if (m_playbackController) {
                double playheadPx = engine.timeToPixelX(m_playbackController->currentTick());
                if (playheadPx >= 0.0 && playheadPx <= engine.viewportWidth())
                    anchorPx = playheadPx;
            }
            engine.zoomAt(anchorPx, 1.3);
            m_timelinePanel->notifyZoomChanged();
        }
    });
    // Ctrl+-: zoom out
    addShortcut(Qt::CTRL | Qt::Key_Minus, [this]() {
        if (m_timelinePanel) {
            auto& engine = m_timelinePanel->layoutEngine();
            double anchorPx = engine.viewportWidth() * 0.5;
            if (m_playbackController) {
                double playheadPx = engine.timeToPixelX(m_playbackController->currentTick());
                if (playheadPx >= 0.0 && playheadPx <= engine.viewportWidth())
                    anchorPx = playheadPx;
            }
            engine.zoomAt(anchorPx, 1.0 / 1.3);
            m_timelinePanel->notifyZoomChanged();
        }
    });

    // Ctrl+E: switch to Export tab
    addShortcut(Qt::CTRL | Qt::Key_E, [this]() {
        for (QWidget* w = parentWidget(); w; w = w->parentWidget()) {
            if (auto* mw = qobject_cast<MainWindow*>(w)) {
                mw->setCurrentPage(Page::Export);
                break;
            }
        }
    });

    // Give this widget focus so shortcuts work immediately
    setFocus();
}

} // namespace rt
