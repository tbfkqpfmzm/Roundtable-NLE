/*
 * AudioSyncSidePanel.cpp - Side-panel disclosure and rail-panel animation helpers.
 */

#include "panels/audio/AudioSync.h"

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>

namespace rt {
namespace {

constexpr int kAudioSidePanelWidth = 400;

int audioSidePanelWidthForMode(int mode)
{
    switch (mode) {
    case 3: return 180;
    default: return kAudioSidePanelWidth;
    }
}

} // namespace

void AudioSync::toggleSetupPanel()
{
    m_setupExpanded = !m_setupExpanded;

    if (m_setupPanel) m_setupPanel->setVisible(m_setupExpanded);
    if (m_disclosureBtn) {
        m_disclosureBtn->setText(m_setupExpanded
            ? QStringLiteral("\u25BE Setup") : QStringLiteral("\u25B8 Setup"));
    }
}

void AudioSync::showAudioSidePanel(int mode)
{
    if (!m_audioSidePanel || !m_audioSidePanelStack) return;

    const int targetWidth = audioSidePanelWidthForMode(mode);

    m_audioSidePanelMode = mode;
    m_audioSidePanelStack->setCurrentIndex(mode);

    if (mode == 2)
        refreshTranscribeFileList();

    if (m_scriptRailBtn)    m_scriptRailBtn->setChecked(mode == 0);
    if (m_importRailBtn)    m_importRailBtn->setChecked(mode == 1);
    if (m_transcribeRailBtn) m_transcribeRailBtn->setChecked(mode == 2);
    if (m_matchRailBtn)     m_matchRailBtn->setChecked(mode == 3);
    if (m_audioSettingsRailBtn) m_audioSettingsRailBtn->setChecked(mode == 4);

    if (m_audioSidePanel->isVisible() && m_audioSidePanel->width() > 10) {
        const int curWidth = m_audioSidePanel->width();
        if (curWidth != targetWidth) {
            auto* group = new QParallelAnimationGroup(this);

            auto* aMin = new QPropertyAnimation(m_audioSidePanel, "minimumWidth");
            aMin->setDuration(120);
            aMin->setStartValue(curWidth);
            aMin->setEndValue(targetWidth);
            aMin->setEasingCurve(QEasingCurve::OutCubic);
            group->addAnimation(aMin);

            auto* aMax = new QPropertyAnimation(m_audioSidePanel, "maximumWidth");
            aMax->setDuration(120);
            aMax->setStartValue(curWidth);
            aMax->setEndValue(targetWidth);
            aMax->setEasingCurve(QEasingCurve::OutCubic);
            group->addAnimation(aMax);

            connect(group, &QParallelAnimationGroup::finished, this, [this, targetWidth]() {
                m_audioSidePanel->setMinimumWidth(targetWidth);
                m_audioSidePanel->setMaximumWidth(targetWidth);
            });
            group->start(QAbstractAnimation::DeleteWhenStopped);
        }
        return;
    }

    m_audioSidePanel->setMinimumWidth(0);
    m_audioSidePanel->setMaximumWidth(0);
    m_audioSidePanel->setVisible(true);
    m_audioSidePanelStack->setUpdatesEnabled(false);

    auto* group = new QParallelAnimationGroup(this);

    auto* animMin = new QPropertyAnimation(m_audioSidePanel, "minimumWidth");
    animMin->setDuration(150);
    animMin->setStartValue(0);
    animMin->setEndValue(targetWidth);
    animMin->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(animMin);

    auto* animMax = new QPropertyAnimation(m_audioSidePanel, "maximumWidth");
    animMax->setDuration(150);
    animMax->setStartValue(0);
    animMax->setEndValue(targetWidth);
    animMax->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(animMax);

    connect(group, &QParallelAnimationGroup::finished, this, [this, targetWidth]() {
        m_audioSidePanelStack->setUpdatesEnabled(true);
        m_audioSidePanel->setMinimumWidth(targetWidth);
        m_audioSidePanel->setMaximumWidth(targetWidth);
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void AudioSync::hideAudioSidePanel()
{
    if (!m_audioSidePanel) return;
    if (m_audioSidePanelMode == -1) return;

    if (m_scriptRailBtn)      m_scriptRailBtn->setChecked(false);
    if (m_importRailBtn)      m_importRailBtn->setChecked(false);
    if (m_transcribeRailBtn)  m_transcribeRailBtn->setChecked(false);
    if (m_matchRailBtn)       m_matchRailBtn->setChecked(false);
    if (m_audioSettingsRailBtn) m_audioSettingsRailBtn->setChecked(false);

    m_audioSidePanelMode = -1;
    m_audioSidePanelStack->setUpdatesEnabled(false);

    auto* group = new QParallelAnimationGroup(this);

    auto* animMin = new QPropertyAnimation(m_audioSidePanel, "minimumWidth");
    animMin->setDuration(120);
    animMin->setStartValue(m_audioSidePanel->minimumWidth());
    animMin->setEndValue(0);
    animMin->setEasingCurve(QEasingCurve::InCubic);
    group->addAnimation(animMin);

    auto* animMax = new QPropertyAnimation(m_audioSidePanel, "maximumWidth");
    animMax->setDuration(120);
    animMax->setStartValue(m_audioSidePanel->maximumWidth());
    animMax->setEndValue(0);
    animMax->setEasingCurve(QEasingCurve::InCubic);
    group->addAnimation(animMax);

    connect(group, &QParallelAnimationGroup::finished, this, [this]() {
        m_audioSidePanelStack->setUpdatesEnabled(true);
        m_audioSidePanel->setVisible(false);
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void AudioSync::toggleAudioSidePanel(int mode)
{
    if (m_audioSidePanelMode == mode &&
        m_audioSidePanel && m_audioSidePanel->isVisible() &&
        m_audioSidePanel->width() > 10)
    {
        hideAudioSidePanel();
    } else {
        showAudioSidePanel(mode);
    }
}

} // namespace rt