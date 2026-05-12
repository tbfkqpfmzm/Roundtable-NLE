/*
 * AudioSyncEvents.cpp - Mouse and keyboard event routing for AudioSync cards.
 */

#include "panels/audio/AudioSync.h"

#include "Theme.h"
#include "command/CommandStack.h"
#include "widgets/MiniWaveformWidget.h"

#include <spdlog/spdlog.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>

namespace rt {

int AudioSync::clipRowForWidget(QWidget* widget) const
{
    if (!widget) return -1;

    QWidget* current = widget;
    while (current) {
        auto widgets = m_cardWidgets;  // local copy — protect against vector modification
        for (size_t i = 0; i < widgets.size(); ++i) {
            if (widgets[i] == current && i < m_cardClipIndices.size()) {
                return m_cardClipIndices[i];
            }
        }
        current = current->parentWidget();
    }

    return -1;
}

MiniWaveformWidget* AudioSync::waveformForClip(int clipIdx) const
{
    if (clipIdx < 0) return nullptr;

    for (size_t i = 0; i < m_cardClipIndices.size(); ++i) {
        if (m_cardClipIndices[i] == clipIdx && i < m_cardWaveforms.size()) {
            return m_cardWaveforms[i];
        }
    }

    return nullptr;
}

bool AudioSync::eventFilter(QObject* watched, QEvent* event)
{
    if (m_manualMatchOpen)
        return QWidget::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            auto* widget = qobject_cast<QWidget*>(watched);
            if (widget) {
                QWidget* card = widget;
                while (card && !card->objectName().startsWith("scriptCard_"))
                    card = card->parentWidget();

                if (card) {
                    auto widgets = m_cardWidgets;  // local copy — protect against vector modification
                    for (size_t i = 0; i < widgets.size(); ++i) {
                        if (widgets[i] == card) {
                            if (i < m_cardClipIndices.size() && m_cardClipIndices[i] >= 0)
                                m_selectedClipIdx = m_cardClipIndices[i];

                            if (i < m_cardScriptLineNums.size()) {
                                if (m_highlightedCard) {
                                    m_highlightedCard->setGraphicsEffect(nullptr);
                                    m_highlightedCard = nullptr;
                                }

                                auto* cardFrame = qobject_cast<QFrame*>(card);
                                if (cardFrame) {
                                    m_highlightedCard = cardFrame;
                                    auto* glow = new QGraphicsDropShadowEffect(cardFrame);
                                    glow->setBlurRadius(18);
                                    glow->setColor(Theme::colors().accent);
                                    glow->setOffset(0, 0);
                                    cardFrame->setGraphicsEffect(glow);
                                }
                            }

                            if (m_leftScriptList && i < m_cardScriptLineNums.size()) {
                                auto scriptLineNum = m_cardScriptLineNums[i];
                                for (int row = 0; row < m_leftScriptList->count(); ++row) {
                                    auto* item = m_leftScriptList->item(row);
                                    if (item &&
                                        item->data(Qt::UserRole).toInt() == scriptLineNum) {
                                        m_leftScriptList->blockSignals(true);
                                        m_leftScriptList->setCurrentRow(row);
                                        m_leftScriptList->blockSignals(false);
                                        break;
                                    }
                                }
                            }

                            break;
                        }
                    }
                }
            }
        }
    }

    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = keyEvent->key();

        if (auto* widget = qobject_cast<QWidget*>(watched)) {
            if (qobject_cast<QLineEdit*>(widget) || qobject_cast<QComboBox*>(widget))
                return QWidget::eventFilter(watched, event);
        }

        auto resolveRow = [&]() -> int {
            if (m_selectedClipIdx >= 0 &&
                static_cast<size_t>(m_selectedClipIdx) < m_clips.size())
                return m_selectedClipIdx;

            int row = clipRowForWidget(qobject_cast<QWidget*>(watched));
            if (row >= 0) return row;

            return -1;
        };

        if (key == Qt::Key_Space) {
            int row = resolveRow();
            if (row >= 0 && static_cast<size_t>(row) < m_clips.size()) {
                spdlog::debug("AudioSync::eventFilter Space -> clip {}", row);
                m_selectedClipIdx = row;
                togglePlayClip(static_cast<size_t>(row));
                return true;
            }
            return QWidget::eventFilter(watched, event);
        }

        if (key == Qt::Key_J || key == Qt::Key_K || key == Qt::Key_L ||
            key == Qt::Key_I || key == Qt::Key_O) {
            if (m_forwardingKey)
                return false;

            int row = resolveRow();
            if (row < 0 || static_cast<size_t>(row) >= m_clips.size())
                return QWidget::eventFilter(watched, event);

            auto* waveform = waveformForClip(row);
            if (waveform) {
                m_forwardingKey = true;
                QCoreApplication::sendEvent(waveform, keyEvent);
                m_forwardingKey = false;
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void AudioSync::keyPressEvent(QKeyEvent* event)
{
    if (m_manualMatchOpen) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Z && (event->modifiers() & Qt::ControlModifier)) {
        if (m_commandStack) {
            if (event->modifiers() & Qt::ShiftModifier)
                m_commandStack->redo();
            else
                m_commandStack->undo();
            event->accept();
            return;
        }
    }

    if (event->key() == Qt::Key_Y && (event->modifiers() & Qt::ControlModifier)) {
        if (m_commandStack) {
            m_commandStack->redo();
            event->accept();
            return;
        }
    }

    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        int row = (m_selectedClipIdx >= 0 &&
                   static_cast<size_t>(m_selectedClipIdx) < m_clips.size())
            ? m_selectedClipIdx
            : -1;
        if (row >= 0 && static_cast<size_t>(row) < m_clips.size()) {
            m_selectedClipIdx = row;
            togglePlayClip(static_cast<size_t>(row));
            event->accept();
            return;
        }
    }

    QWidget::keyPressEvent(event);
}

} // namespace rt