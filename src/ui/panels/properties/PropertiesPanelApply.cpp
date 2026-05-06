/*
 * PropertiesPanelApply.cpp - Data binding & property application extracted from PropertiesPanel.cpp.
 *
 * Contains: refreshEffects, updateShotSection, onShotChanged, populateFromClip,
 * populateFromTransition, and all apply*() methods.
 */


#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/Track.h"
#include "timeline/Timeline.h"
#include "timeline/Transition.h"
#include "spine/ShotPreset.h"
#include "spine/ModelManager.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "command/commands/EffectCommands.h"

#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QComboBox>
#include <QColorDialog>

#include <spdlog/spdlog.h>
#include <chrono>

namespace rt {

void PropertiesPanel::refreshEffects()
{
    if (!m_fxParamsLayout) return;
    const auto& m = Theme::metrics();

    // â”€â”€ Clear previous parameter widgets â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    while (QLayoutItem* item = m_fxParamsLayout->takeAt(0)) {
        if (item->widget()) {
            item->widget()->deleteLater();
        } else if (item->layout()) {
            // Recursively clean up nested layouts
            while (QLayoutItem* child = item->layout()->takeAt(0)) {
                if (child->widget()) child->widget()->deleteLater();
                delete child;
            }
        }
        delete item;
    }

    // Also sync the hidden list (used for remove operations)
    if (m_fxList) m_fxList->clear();

    if (!m_clip) return;

    auto& stack = m_clip->effects();
    if (stack.effectCount() == 0) {
        auto* emptyLabel = new QLabel(tr("No effects applied"), m_effectsSection);
        emptyLabel->setAlignment(Qt::AlignCenter);
        emptyLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; padding: 12px;")
            .arg(Theme::hex(Theme::colors().textTertiary)));
        m_fxParamsLayout->addWidget(emptyLabel);
        return;
    }

    // â”€â”€ Premiere Proâ€“style collapsible header + parameter rows â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Style constants â€” derived from universal Theme
    const auto& tc = Theme::colors();
    const QString kFxHeaderStyle = QStringLiteral(
        "QWidget {"
        "  background: %1;"
        "  border: none;"
        "  border-bottom: 1px solid %2;"
        "}")
    .arg(Theme::hex(tc.surface2))
    .arg(Theme::hex(tc.border));
    const QString kFxParamRowStyle = QStringLiteral(
        "QWidget { background: %1; }")
    .arg(Theme::hex(tc.surface1));
    const QString kFxLabelStyle = QStringLiteral(
        "QLabel { color: %1; font-size: 11px; font-weight: normal; "
        "         padding: 0; background: transparent; }")
    .arg(Theme::hex(tc.textSecondary));
    const QString kFxNameStyle = QStringLiteral(
        "QLabel { color: %1; font-size: 12px; font-weight: bold; "
        "         padding: 0; background: transparent; }")
    .arg(Theme::hex(tc.textPrimary));
    const QString kFxCheckStyle = QStringLiteral(
        "QCheckBox { spacing: 4px; background: transparent; }"
        "QCheckBox::indicator { width: 12px; height: 12px; border-radius: 2px;"
        "  border: 1px solid %1; background: %2; }"
        "QCheckBox::indicator:checked { background: %3; border-color: %4; }")
    .arg(Theme::hex(tc.controlBorder))
    .arg(Theme::hex(tc.controlBg))
    .arg(Theme::hex(tc.accent))
    .arg(Theme::hex(tc.controlBorderFocus));
    const QString kFxRemoveBtnStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none; color: %1;"
        "  font-size: 12px; font-weight: bold; padding: 0; }"
        "QPushButton:hover { color: %2; }")
    .arg(Theme::hex(tc.textTertiary))
    .arg(Theme::hex(tc.error));

    for (size_t i = 0; i < stack.effectCount(); ++i) {
        auto& fx = stack.effect(i);

        // Sync the hidden list
        if (m_fxList) {
            QString text = QString::fromUtf8(fx.name());
            if (!fx.isEnabled()) text += " (disabled)";
            m_fxList->addItem(text);
        }

        // â”€â”€ Container for this effect â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* fxGroup = new QWidget(m_effectsSection);
        auto* fxGroupLayout = new QVBoxLayout(fxGroup);
        fxGroupLayout->setContentsMargins(0, 0, 0, 0);
        fxGroupLayout->setSpacing(0);

        // â”€â”€ Header row: [â–¸] [Enable] [Effect Name] â”€â”€â”€â”€â”€â”€â”€ [âœ•] â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* header = new QWidget(fxGroup);
        header->setFixedHeight(24);
        header->setStyleSheet(kFxHeaderStyle);
        auto* headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(m.spacingSm, 0, m.spacingXs, 0);
        headerLayout->setSpacing(m.spacingXs);

        // Expand/collapse toggle (â–¸ / â–¾)
        auto* toggleBtn = new QPushButton(QStringLiteral("\u25B8"), header);
        toggleBtn->setFixedSize(14, 14);
        toggleBtn->setFlat(true);
        toggleBtn->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; font-size: 11px; background: transparent;"
            "  border: none; padding: 0; }"
            "QPushButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textSecondary))
            .arg(Theme::hex(tc.textBright)));
        headerLayout->addWidget(toggleBtn);

        // Enable checkbox
        auto* enableCheck = new QCheckBox(header);
        enableCheck->setChecked(fx.isEnabled());
        enableCheck->setStyleSheet(kFxCheckStyle);
        enableCheck->setToolTip(tr("Enable / Disable"));
        headerLayout->addWidget(enableCheck);

        // Effect name label
        auto* nameLabel = new QLabel(QString::fromUtf8(fx.name()), header);
        nameLabel->setStyleSheet(kFxNameStyle);
        headerLayout->addWidget(nameLabel, 1);

        // Remove button (âœ•)
        auto* removeBtn = new QPushButton(QStringLiteral("\u2715"), header);
        removeBtn->setFixedSize(16, 16);
        removeBtn->setToolTip(tr("Remove Effect"));
        removeBtn->setStyleSheet(kFxRemoveBtnStyle);
        headerLayout->addWidget(removeBtn);

        fxGroupLayout->addWidget(header);

        // â”€â”€ Parameter rows container (collapsible) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* paramsContainer = new QWidget(fxGroup);
        auto* paramsLayout = new QVBoxLayout(paramsContainer);
        paramsLayout->setContentsMargins(18, m.spacingXs, m.spacingSm, m.spacingSm);
        paramsLayout->setSpacing(3);

        for (size_t pi = 0; pi < fx.paramCount(); ++pi) {
            const auto& param = fx.param(pi);

            auto* row = new QWidget(paramsContainer);
            row->setStyleSheet(kFxParamRowStyle);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 1, 0, 1);
            rowLayout->setSpacing(m.spacingSm);

            // Parameter name label
            auto* paramLabel = new QLabel(QString::fromStdString(param.name), row);
            paramLabel->setFixedWidth(90);
            paramLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            paramLabel->setStyleSheet(kFxLabelStyle);
            rowLayout->addWidget(paramLabel);

            // Scrubby spin box for the parameter value
            auto* spin = createScrubby(
                static_cast<double>(param.minVal),
                static_cast<double>(param.maxVal),
                static_cast<double>((param.maxVal - param.minVal) / 100.0f),
                2);
            spin->setValue(static_cast<double>(param.track.evaluate(0)));
            rowLayout->addWidget(spin, 1);

            // Connect spin to update the effect parameter
            size_t effectIdx = i;
            size_t paramIdx  = pi;

            // Live preview during scrub (no undo)
            connect(spin, &ScrubbySpinBox::valueScrubbed,
                    this, [this, effectIdx, paramIdx](double val) {
                if (!m_clip) return;
                auto& st = m_clip->effects();
                if (effectIdx >= st.effectCount()) return;
                auto& ef = st.effect(effectIdx);
                if (paramIdx >= ef.paramCount()) return;
                ef.param(paramIdx).track.addKeyframe(0, static_cast<float>(val));
                emit propertyChanged();
            });

            // Commit with undo support
            connect(spin, &ScrubbySpinBox::valueCommitted,
                    this, [this, effectIdx, paramIdx](double oldVal, double newVal) {
                if (!m_clip) return;
                auto& st = m_clip->effects();
                if (effectIdx >= st.effectCount()) return;
                auto& ef = st.effect(effectIdx);
                if (paramIdx >= ef.paramCount()) return;
                ef.param(paramIdx).track.addKeyframe(0, static_cast<float>(newVal));
                emit propertyChanged();
                if (m_commandStack) {
                    auto fOld = static_cast<float>(oldVal);
                    auto fNew = static_cast<float>(newVal);
                    auto* stack = &st;
                    auto fxId = ef.id();
                    auto pi = paramIdx;
                    m_commandStack->pushWithoutExecute(
                        std::make_unique<LambdaCommand>(
                            "Set Effect Parameter",
                            [stack, fxId, pi, fNew]() {
                                if (auto* fx = stack->effectById(fxId))
                                    if (pi < fx->paramCount())
                                        fx->param(pi).track.addKeyframe(0, fNew);
                            },
                            [stack, fxId, pi, fOld]() {
                                if (auto* fx = stack->effectById(fxId))
                                    if (pi < fx->paramCount())
                                        fx->param(pi).track.addKeyframe(0, fOld);
                            }));
                }
            });

            paramsLayout->addWidget(row);
        }

        fxGroupLayout->addWidget(paramsContainer);

        // â”€â”€ Toggle collapse â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        connect(toggleBtn, &QPushButton::clicked, this,
                [toggleBtn, paramsContainer]() {
            bool visible = paramsContainer->isVisible();
            paramsContainer->setVisible(!visible);
            toggleBtn->setText(visible ? QStringLiteral("\u25B8")    // â–¸
                                       : QStringLiteral("\u25BE")); // â–¾
        });

        // â”€â”€ Enable/disable â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        size_t effectIdx = i;
        connect(enableCheck, &QCheckBox::toggled, this,
                [this, effectIdx, nameLabel](bool checked) {
            if (!m_clip) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            auto& fx = st.effect(effectIdx);
            if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<SetEffectEnabledCommand>(&st, fx.id(), checked));
            } else {
                fx.setEnabled(checked);
            }
            // Dim the name when disabled
            const auto& c = Theme::colors();
            nameLabel->setStyleSheet(QStringLiteral(
                "QLabel { color: %1; font-size: 12px; font-weight: bold;"
                " padding: 0; background: transparent; }")
                .arg(Theme::hex(checked ? c.textPrimary : c.textDisabled)));
            emit propertyChanged();
        });

        // â”€â”€ Remove effect â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        connect(removeBtn, &QPushButton::clicked, this,
                [this, effectIdx]() {
            if (!m_clip) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<RemoveEffectCommand>(&st, effectIdx));
            } else {
                (void)st.removeEffect(effectIdx);
            }
            refreshEffects();
            emit propertyChanged();
        });

        m_fxParamsLayout->addWidget(fxGroup);
    }
}

void PropertiesPanel::updateShotSection()
{
    if (!m_clip) {
        m_shotSection->setVisible(false);
        return;
    }

    m_shotSection->setVisible(true);

    m_shotInfoLabel->setText(
        QString("Group %1 — Layer: %2")
            .arg(m_clip->groupId())
            .arg(QString::fromStdString(m_clip->layerId())));

    m_shotCombo->blockSignals(true);
    m_shotCombo->clear();

    if (m_shotManager) {
        auto names = m_shotManager->presetNames();
        for (auto& name : names)
            m_shotCombo->addItem(QString::fromStdString(name));

        // Select the current shot if it has a name
        int idx = m_shotCombo->findText(QString::fromStdString(m_clip->shotName()));
        spdlog::info("[SHOT-DROPDOWN] clip groupId={} shotName='{}' findText={} comboCount={}",
                     m_clip->groupId(), m_clip->shotName(), idx, m_shotCombo->count());
        if (idx >= 0) {
            m_shotCombo->setCurrentIndex(idx);
        } else {
            // No matching shot — insert a placeholder so the dropdown doesn't
            // show the first alphabetical item as if it were selected.
            m_shotCombo->insertItem(0, QStringLiteral("-- Choose a shot --"));
            m_shotCombo->setCurrentIndex(0);
        }
        // Otherwise leave selection on the first item but don't force it
    } else {
        if (!m_clip->shotName().empty())
            m_shotCombo->addItem(QString::fromStdString(m_clip->shotName()));
    }

    m_shotCombo->blockSignals(false);
}

void PropertiesPanel::onShotChanged(const std::string& newShotName)
{
    if (!m_clip) return;
    // Placeholder item — ignore
    if (newShotName.empty() || newShotName == "-- Choose a shot --") return;

    uint64_t groupId = m_clip->groupId();
    // Auto-assign a group ID for single clips so the shot switch machinery works.
    // Uses clip ID as a unique group identifier for this operation.
    if (groupId == 0) {
        groupId = m_clip->id();
        m_clip->setGroupId(groupId);
    }

    // Let TimelineWorkspace handle the actual switch with undo support
    emit shotSwitchRequested(groupId, newShotName);
    emit propertyChanged();

    spdlog::info("PropertiesPanel: shot switch requested for group {}", groupId);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Clip binding
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


void PropertiesPanel::populateFromClip()
{
    if (!m_clip) return;

    m_updating = true; // block signals from triggering apply methods

    // Header
    QString typeName;
    switch (m_clip->clipType())
    {
    case ClipType::Spine:      typeName = "Spine";      break;
    case ClipType::Video:      typeName = "Video";      break;
    case ClipType::Audio:      typeName = "Audio";      break;
    case ClipType::Title:      typeName = "Title";      break;
    case ClipType::Adjustment: typeName = "Adjustment"; break;
    case ClipType::Image:      typeName = "Image";      break;
    case ClipType::Graphic:    typeName = "Graphic";    break;
    }
    m_headerLabel->setText(QString::fromStdString(m_clip->label()));
    m_typeLabel->setText(typeName);

    // Colored type pill badge
    QColor badgeColor;
    switch (m_clip->clipType()) {
    case ClipType::Video:      badgeColor = QColor(80, 140, 220);  break;
    case ClipType::Audio:      badgeColor = QColor(80, 180, 100);  break;
    case ClipType::Spine:      badgeColor = QColor(160, 100, 220); break;
    case ClipType::Title:      badgeColor = QColor(220, 180, 60);  break;
    case ClipType::Graphic:    badgeColor = QColor(220, 140, 60);  break;
    case ClipType::Image:      badgeColor = QColor(100, 180, 200); break;
    case ClipType::Adjustment: badgeColor = QColor(180, 180, 180); break;
    default:                   badgeColor = QColor(150, 150, 150); break;
    }
    {
        const auto& bm = Theme::metrics();
        m_typeLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 11px; padding: 1px 6px; "
                           "border-radius: %2px; background: %3; border: none; }")
                .arg(Theme::hex(badgeColor.lighter(180)),
                     QString::number(bm.radiusSm),
                     Theme::hex(badgeColor.darker(200))));
    }

    // Update status bar
    if (m_statusLabel)
        m_statusLabel->setText(typeName + QStringLiteral(" clip"));

    // Identity
    m_labelEdit->setText(QString::fromStdString(m_clip->label()));
    m_enabledCheck->setChecked(m_clip->isEnabled());
    m_speedSpin->setValue(m_clip->speed());

    // Transform (using keyframe at t=0)
    // UI shows percentage for scale (100 = 1.0x) and opacity (100 = 1.0)
    m_posXSpin->setValue(m_clip->positionX().evaluate(0));
    m_posYSpin->setValue(m_clip->positionY().evaluate(0));
    m_scaleXSpin->setValue(m_clip->scaleX().evaluate(0) * 100.0);
    m_scaleYSpin->setValue(m_clip->scaleY().evaluate(0) * 100.0);
    m_rotationSpin->setValue(m_clip->rotation().evaluate(0));
    m_opacitySpin->setValue(m_clip->opacity().evaluate(0) * 100.0);

    // Crop (from SpineClip or VideoClip â€” not on base Clip)
    float cl = 0, cr = 0, ct = 0, cb = 0;
    if (m_clip->clipType() == ClipType::Spine) {
        auto* sc = static_cast<SpineClip*>(m_clip);
        cl = sc->cropLeft(); cr = sc->cropRight();
        ct = sc->cropTop();  cb = sc->cropBottom();
    } else if (m_clip->clipType() == ClipType::Video) {
        auto* vc = static_cast<VideoClip*>(m_clip);
        cl = vc->cropLeft(); cr = vc->cropRight();
        ct = vc->cropTop();  cb = vc->cropBottom();
    }
    m_tfCropLeftSpin->setValue(static_cast<double>(cl));
    m_tfCropRightSpin->setValue(static_cast<double>(cr));
    m_tfCropTopSpin->setValue(static_cast<double>(ct));
    m_tfCropBottomSpin->setValue(static_cast<double>(cb));

    // Type-specific
    if (m_clip->clipType() == ClipType::Spine)
    {
        auto* sc = static_cast<SpineClip*>(m_clip);
        m_spineClip = sc;

        // Restore animation section defaults (may have been hidden for video chars)
        m_animationSection->setTitle("Animation");
        m_loopingCheck->setVisible(true);
        m_animationCombo->setVisible(true);
        m_animSpeedSpin->setVisible(true);
        m_continuityCheck->setVisible(true);
        m_outfitCombo->setVisible(true);
        m_stanceCombo->setVisible(true);
        if (m_characterSection)
            m_characterSection->setTitle("Character");

        // Populate dropdowns (order matters: character → outfit → stance → animation)
        populateCharacterDropdown();
        {
            // Find by folder name stored in item data
            int idx = m_characterCombo->findData(QString::fromStdString(sc->characterName()));
            if (idx >= 0)
                m_characterCombo->setCurrentIndex(idx);
            else
                m_characterCombo->setCurrentText(QString::fromStdString(sc->characterName()));
        }
        populateOutfitDropdown();
        m_outfitCombo->setCurrentText(QString::fromStdString(sc->outfit()));
        populateStanceDropdown();
        m_stanceCombo->setCurrentIndex(static_cast<int>(sc->stance()));
        populateAnimationDropdown();
        m_animationCombo->setCurrentText(QString::fromStdString(sc->animationName()));

        m_loopingCheck->setChecked(sc->isLooping());
        m_talkingCheck->setChecked(sc->isTalking());
        m_animSpeedSpin->setValue(sc->animationSpeed());
        m_continuityCheck->setChecked(sc->useGlobalTime());
    }
    else if (m_clip->clipType() == ClipType::Video)
    {
        auto* vc = static_cast<VideoClip*>(m_clip);
        m_mediaPathLabel->setText(QString::fromStdString(vc->mediaPath()));
        m_volumeSpin->setValue(vc->volume());

        // Video character controls
        if (vc->isVideoCharacter()) {
            // Character section — show name + outfit dropdown
            m_characterCombo->blockSignals(true);
            m_characterCombo->clear();
            m_characterCombo->addItem(QString::fromStdString(vc->characterName()));
            m_characterCombo->setCurrentIndex(0);
            m_characterCombo->blockSignals(false);

            // Populate outfit dropdown from ModelManager
            m_outfitCombo->setVisible(true);
            m_outfitCombo->blockSignals(true);
            m_outfitCombo->clear();
            if (m_modelManager) {
                auto outfits = m_modelManager->getMetadataOutfits(vc->characterName());
                for (const auto& outfit : outfits)
                    m_outfitCombo->addItem(QString::fromStdString(outfit.key));
            }
            if (m_outfitCombo->count() == 0)
                m_outfitCombo->addItem(QString::fromStdString(
                    vc->outfit().empty() ? "default" : vc->outfit()));
            {
                QString cur = QString::fromStdString(
                    vc->outfit().empty() ? "default" : vc->outfit());
                int idx = m_outfitCombo->findText(cur);
                if (idx >= 0) m_outfitCombo->setCurrentIndex(idx);
                else m_outfitCombo->setCurrentIndex(0);
            }
            m_outfitCombo->blockSignals(false);

            m_stanceCombo->setVisible(false);
            if (m_characterSection)
                m_characterSection->setTitle(
                    QString("Character: %1").arg(
                        QString::fromStdString(vc->characterName())));

            // Animation section — talking toggle + animation dropdown
            m_talkingCheck->setChecked(vc->isTalking());
            m_loopingCheck->setVisible(false);
            m_animSpeedSpin->setVisible(false);
            m_continuityCheck->setVisible(false);

            // Populate animation dropdown from video cache directory
            m_animationCombo->setVisible(true);
            m_animationCombo->blockSignals(true);
            m_animationCombo->clear();
            if (m_videoAnimNamesProvider) {
                std::string outfit = vc->outfit().empty() ? "default" : vc->outfit();
                auto anims = m_videoAnimNamesProvider(vc->characterName(), outfit);
                for (const auto& a : anims)
                    m_animationCombo->addItem(QString::fromStdString(a));
            }
            if (m_animationCombo->count() == 0 && !vc->animationName().empty())
                m_animationCombo->addItem(QString::fromStdString(vc->animationName()));
            {
                QString cur = QString::fromStdString(vc->animationName());
                int idx = m_animationCombo->findText(cur);
                if (idx >= 0) m_animationCombo->setCurrentIndex(idx);
                else if (m_animationCombo->count() > 0) m_animationCombo->setCurrentIndex(0);
            }
            m_animationCombo->blockSignals(false);

            m_animationSection->setTitle("Animation");
        }
    }
    else if (m_clip->clipType() == ClipType::Audio)
    {
        auto* ac = static_cast<AudioClip*>(m_clip);
        m_audioVolumeSpin->setValue(ac->volume().evaluate(0));
        m_panSpin->setValue(ac->pan().evaluate(0));
        m_fadeInSpin->setValue(static_cast<double>(ac->fadeInDuration()));
        m_fadeOutSpin->setValue(static_cast<double>(ac->fadeOutDuration()));
    }
    else if (m_clip->clipType() == ClipType::Title)
    {
        auto* tc = static_cast<TitleClip*>(m_clip);
        m_textEdit->setText(QString::fromStdString(tc->text()));
        m_fontFamilyEdit->setText(QString::fromStdString(tc->fontFamily()));
        m_fontSizeSpin->setValue(tc->fontSize());
        m_boldCheck->setChecked(tc->isBold());
        m_italicCheck->setChecked(tc->isItalic());
        m_alignCombo->setCurrentIndex(static_cast<int>(tc->alignment()));
    }
    else if (m_clip->clipType() == ClipType::Graphic)
    {
        auto* gc = static_cast<GraphicClip*>(m_clip);
        // Populate from the first text layer (if any)
        TextLayer* tl = nullptr;
        for (size_t i = 0; i < gc->layerCount(); ++i) {
            if (gc->layer(i)->layerType() == GraphicLayerType::Text) {
                tl = static_cast<TextLayer*>(gc->layer(i));
                break;
            }
        }
        if (tl) {
            m_gfxTextEdit->setText(QString::fromStdString(tl->text()));
            m_gfxFontFamilyEdit->setText(QString::fromStdString(tl->fontFamily()));
            m_gfxFontSizeSpin->setValue(static_cast<double>(tl->fontSize()));
            m_gfxFontWeightSpin->setValue(tl->fontWeight());
            m_gfxItalicCheck->setChecked(tl->isItalic());
            m_gfxAllCapsCheck->setChecked(tl->allCaps());
            m_gfxAlignCombo->setCurrentIndex(static_cast<int>(tl->alignment()));

            const auto& app = tl->appearance();
            // Fill color button
            if (!app.fills.empty()) {
                uint32_t fc = app.fills[0].color;
                QColor fillCol(static_cast<int>((fc>>16)&0xFF),
                               static_cast<int>((fc>>8)&0xFF),
                               static_cast<int>(fc&0xFF));
                m_gfxFillColorBtn->setStyleSheet(
                    QStringLiteral("QPushButton { background: %1; border: 1px solid #555; min-width: 40px; min-height: 18px; }")
                    .arg(fillCol.name()));
            }
            // Stroke
            if (!app.strokes.empty()) {
                m_gfxStrokeCheck->setChecked(app.strokes[0].enabled);
                m_gfxStrokeWidthSpin->setValue(static_cast<double>(app.strokes[0].width));
                uint32_t sc = app.strokes[0].color;
                QColor strokeCol(static_cast<int>((sc>>16)&0xFF),
                                 static_cast<int>((sc>>8)&0xFF),
                                 static_cast<int>(sc&0xFF));
                m_gfxStrokeColorBtn->setStyleSheet(
                    QStringLiteral("QPushButton { background: %1; border: 1px solid #555; min-width: 40px; min-height: 18px; }")
                    .arg(strokeCol.name()));
            }
            // Shadow
            if (!app.shadows.empty()) {
                m_gfxShadowCheck->setChecked(app.shadows[0].enabled);
            }
        }
    }

    m_updating = false;

    // Refresh applied effects list
    refreshEffects();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Apply property changes
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void PropertiesPanel::applyLabel()
{
    if (m_updating || !m_clip) return;
    std::string newLabel = m_labelEdit->text().toStdString();
    if (newLabel == m_clip->label()) return;
    auto oldLabel = m_clip->label();
    auto* clip = m_clip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change label",
            [clip, newLabel, this]() { clip->setLabel(newLabel); populateFromClip(); emit propertyChanged(); },
            [clip, oldLabel, this]() { clip->setLabel(oldLabel); populateFromClip(); emit propertyChanged(); }));
    } else {
        clip->setLabel(newLabel);
        m_headerLabel->setText(m_labelEdit->text());
        emit propertyChanged();
    }
}

void PropertiesPanel::applyEnabled()
{
    if (m_updating || !m_clip) return;
    bool newVal = m_enabledCheck->isChecked();
    if (newVal == m_clip->isEnabled()) return;
    bool oldVal = m_clip->isEnabled();
    auto* clip = m_clip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle enabled",
            [clip, newVal, this]() { clip->setEnabled(newVal); populateFromClip(); emit propertyChanged(); },
            [clip, oldVal, this]() { clip->setEnabled(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        clip->setEnabled(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applySpeed()
{
    if (m_updating || !m_clip) return;
    double newVal = m_speedSpin->value();
    if (newVal == m_clip->speed()) return;
    double oldVal = m_clip->speed();
    auto* clip = m_clip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change speed",
            [clip, newVal, this]() { clip->setSpeed(newVal); populateFromClip(); emit propertyChanged(); },
            [clip, oldVal, this]() { clip->setSpeed(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        clip->setSpeed(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTransformLive()
{
    // Live preview during scrub drag — writes current spinbox values to
    // clip properties without creating undo commands, so the program
    // monitor recomposites on the next poll cycle.
    if (m_updating || !m_clip) return;

    auto* clip = m_clip;
    clip->positionX().writeValue(0, static_cast<float>(m_posXSpin->value()));
    clip->positionY().writeValue(0, static_cast<float>(m_posYSpin->value()));
    clip->scaleX().writeValue(0, static_cast<float>(m_scaleXSpin->value() / 100.0));
    clip->scaleY().writeValue(0, static_cast<float>(m_scaleYSpin->value() / 100.0));
    clip->rotation().writeValue(0, static_cast<float>(m_rotationSpin->value()));
    clip->opacity().writeValue(0, static_cast<float>(m_opacitySpin->value() / 100.0));
    if (clip->clipType() == ClipType::Spine) {
        auto* sc = static_cast<SpineClip*>(clip);
        sc->setCrop(
            static_cast<float>(m_tfCropLeftSpin->value()),
            static_cast<float>(m_tfCropRightSpin->value()),
            static_cast<float>(m_tfCropTopSpin->value()),
            static_cast<float>(m_tfCropBottomSpin->value()));
    } else if (clip->clipType() == ClipType::Video) {
        auto* vc = static_cast<VideoClip*>(clip);
        vc->setCrop(
            static_cast<float>(m_tfCropLeftSpin->value()),
            static_cast<float>(m_tfCropRightSpin->value()),
            static_cast<float>(m_tfCropTopSpin->value()),
            static_cast<float>(m_tfCropBottomSpin->value()));
    }
    emit propertyChanged();
}

void PropertiesPanel::applyTransform()
{
    // Called on scrub-end (valueCommitted) or keyboard entry (editingFinished).
    // applyTransformLive() already wrote the new values during the drag.
    // We create an undo command using the pre-scrub values from each spinbox's
    // scrubStartValue() — these were captured by the spinbox at press time.
    // Use pushWithoutExecute() since values are already applied live.
    if (m_updating || !m_clip) return;
    auto* clip = m_clip;

    // Old values from before the scrub started (captured by ScrubbySpinBox at press)
    float oPX = static_cast<float>(m_posXSpin->scrubStartValue());
    float oPY = static_cast<float>(m_posYSpin->scrubStartValue());
    float oSX = static_cast<float>(m_scaleXSpin->scrubStartValue() / 100.0);
    float oSY = static_cast<float>(m_scaleYSpin->scrubStartValue() / 100.0);
    float oRot = static_cast<float>(m_rotationSpin->scrubStartValue());
    float oOp = static_cast<float>(m_opacitySpin->scrubStartValue() / 100.0);
    float oCL = static_cast<float>(m_tfCropLeftSpin->scrubStartValue());
    float oCR = static_cast<float>(m_tfCropRightSpin->scrubStartValue());
    float oCT = static_cast<float>(m_tfCropTopSpin->scrubStartValue());
    float oCB = static_cast<float>(m_tfCropBottomSpin->scrubStartValue());

    // Current (new) values from UI
    float nPX = static_cast<float>(m_posXSpin->value());
    float nPY = static_cast<float>(m_posYSpin->value());
    float nSX = static_cast<float>(m_scaleXSpin->value() / 100.0);
    float nSY = static_cast<float>(m_scaleYSpin->value() / 100.0);
    float nRot = static_cast<float>(m_rotationSpin->value());
    float nOp = static_cast<float>(m_opacitySpin->value() / 100.0);
    float nCL = static_cast<float>(m_tfCropLeftSpin->value());
    float nCR = static_cast<float>(m_tfCropRightSpin->value());
    float nCT = static_cast<float>(m_tfCropTopSpin->value());
    float nCB = static_cast<float>(m_tfCropBottomSpin->value());

    auto applyVals = [clip, this](float px, float py, float sx, float sy, float rot, float op,
                                   float cl, float cr, float ct, float cb) {
        clip->positionX().addKeyframe(0, px);
        clip->positionY().addKeyframe(0, py);
        clip->scaleX().addKeyframe(0, sx);
        clip->scaleY().addKeyframe(0, sy);
        clip->rotation().addKeyframe(0, rot);
        clip->opacity().addKeyframe(0, op);
        if (clip->clipType() == ClipType::Spine)
            static_cast<SpineClip*>(clip)->setCrop(cl, cr, ct, cb);
        else if (clip->clipType() == ClipType::Video)
            static_cast<VideoClip*>(clip)->setCrop(cl, cr, ct, cb);
        populateFromClip();
        emit propertyChanged();
    };

    if (m_commandStack) {
        // Use pushWithoutExecute — values already applied by applyTransformLive
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Change transform",
            [=]() { applyVals(nPX, nPY, nSX, nSY, nRot, nOp, nCL, nCR, nCT, nCB); },
            [=]() { applyVals(oPX, oPY, oSX, oSY, oRot, oOp, oCL, oCR, oCT, oCB); }));
    }
}

// ── Spine methods are in PropertiesPanelSpine.cpp ───────────────────────────

// â”€â”€ Video â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void PropertiesPanel::applyVideoVolume()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Video) return;
    auto* vc = static_cast<VideoClip*>(m_clip);
    float newVal = static_cast<float>(m_volumeSpin->value());
    if (newVal == vc->volume()) return;
    float oldVal = vc->volume();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change video volume",
            [vc, newVal, this]() { vc->setVolume(newVal); populateFromClip(); emit propertyChanged(); },
            [vc, oldVal, this]() { vc->setVolume(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        vc->setVolume(newVal);
        emit propertyChanged();
    }
}

// â”€â”€ Audio â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void PropertiesPanel::applyAudioVolume()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Audio) return;
    auto* ac = static_cast<AudioClip*>(m_clip);
    float newVal = static_cast<float>(m_audioVolumeSpin->value());
    float oldVal = ac->volume().evaluate(0);
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change audio volume",
            [ac, newVal, this]() { ac->volume().addKeyframe(0, newVal); populateFromClip(); emit propertyChanged(); },
            [ac, oldVal, this]() { ac->volume().addKeyframe(0, oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        ac->volume().addKeyframe(0, newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyAudioPan()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Audio) return;
    auto* ac = static_cast<AudioClip*>(m_clip);
    float newVal = static_cast<float>(m_panSpin->value());
    float oldVal = ac->pan().evaluate(0);
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change audio pan",
            [ac, newVal, this]() { ac->pan().addKeyframe(0, newVal); populateFromClip(); emit propertyChanged(); },
            [ac, oldVal, this]() { ac->pan().addKeyframe(0, oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        ac->pan().addKeyframe(0, newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyAudioFadeIn()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Audio) return;
    auto* ac = static_cast<AudioClip*>(m_clip);
    int64_t newVal = static_cast<int64_t>(m_fadeInSpin->value());
    int64_t oldVal = ac->fadeInDuration();
    if (newVal == oldVal) return;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change fade in",
            [ac, newVal, this]() { ac->setFadeInDuration(newVal); populateFromClip(); emit propertyChanged(); },
            [ac, oldVal, this]() { ac->setFadeInDuration(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        ac->setFadeInDuration(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyAudioFadeOut()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Audio) return;
    auto* ac = static_cast<AudioClip*>(m_clip);
    int64_t newVal = static_cast<int64_t>(m_fadeOutSpin->value());
    int64_t oldVal = ac->fadeOutDuration();
    if (newVal == oldVal) return;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change fade out",
            [ac, newVal, this]() { ac->setFadeOutDuration(newVal); populateFromClip(); emit propertyChanged(); },
            [ac, oldVal, this]() { ac->setFadeOutDuration(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        ac->setFadeOutDuration(newVal);
        emit propertyChanged();
    }
}

// â”€â”€ Title â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void PropertiesPanel::applyTitleText()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    auto newVal = m_textEdit->text().toStdString();
    if (newVal == tc->text()) return;
    auto oldVal = tc->text();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change title text",
            [tc, newVal, this]() { tc->setText(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setText(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setText(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleFontFamily()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    auto newVal = m_fontFamilyEdit->text().toStdString();
    if (newVal == tc->fontFamily()) return;
    auto oldVal = tc->fontFamily();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change font family",
            [tc, newVal, this]() { tc->setFontFamily(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setFontFamily(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setFontFamily(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleFontSize()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    float newVal = static_cast<float>(m_fontSizeSpin->value());
    if (newVal == tc->fontSize()) return;
    float oldVal = tc->fontSize();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change font size",
            [tc, newVal, this]() { tc->setFontSize(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setFontSize(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setFontSize(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleBold()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    bool newVal = m_boldCheck->isChecked();
    if (newVal == tc->isBold()) return;
    bool oldVal = tc->isBold();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle bold",
            [tc, newVal, this]() { tc->setBold(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setBold(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setBold(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleItalic()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    bool newVal = m_italicCheck->isChecked();
    if (newVal == tc->isItalic()) return;
    bool oldVal = tc->isItalic();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle italic",
            [tc, newVal, this]() { tc->setItalic(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setItalic(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setItalic(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applyTitleAlign()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Title) return;
    auto* tc = static_cast<TitleClip*>(m_clip);
    auto newVal = static_cast<TextAlign>(m_alignCombo->currentIndex());
    if (newVal == tc->alignment()) return;
    auto oldVal = tc->alignment();
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change text alignment",
            [tc, newVal, this]() { tc->setAlignment(newVal); populateFromClip(); emit propertyChanged(); },
            [tc, oldVal, this]() { tc->setAlignment(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        tc->setAlignment(newVal);
        emit propertyChanged();
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Graphic property changes
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

static TextLayer* firstTextLayer(GraphicClip* gc)
{
    for (size_t i = 0; i < gc->layerCount(); ++i)
        if (gc->layer(i)->layerType() == GraphicLayerType::Text)
            return static_cast<TextLayer*>(gc->layer(i));
    return nullptr;
}

void PropertiesPanel::applyGfxText()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    std::string val = m_gfxTextEdit->text().toStdString();
    if (val == tl->text()) return;
    tl->setText(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFontFamily()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    std::string val = m_gfxFontFamilyEdit->text().toStdString();
    if (val == tl->fontFamily()) return;
    tl->setFontFamily(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFontSize()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    float val = static_cast<float>(m_gfxFontSizeSpin->value());
    if (std::abs(val - tl->fontSize()) < 0.01f) return;
    tl->setFontSize(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFontWeight()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    int val = static_cast<int>(m_gfxFontWeightSpin->value());
    if (val == tl->fontWeight()) return;
    tl->setFontWeight(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxItalic()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    tl->setItalic(m_gfxItalicCheck->isChecked());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxAllCaps()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    tl->setAllCaps(m_gfxAllCapsCheck->isChecked());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxAlign()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    auto val = static_cast<GTextAlign>(m_gfxAlignCombo->currentIndex());
    if (val == tl->alignment()) return;
    tl->setAlignment(val);
    emit propertyChanged();
}

void PropertiesPanel::applyGfxFillColor()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;

    // Get current color
    uint32_t current = 0xFFFFFFFF;
    if (!tl->appearance().fills.empty())
        current = tl->appearance().fills[0].color;
    QColor curCol(static_cast<int>((current>>16)&0xFF),
                  static_cast<int>((current>>8)&0xFF),
                  static_cast<int>(current&0xFF),
                  static_cast<int>((current>>24)&0xFF));

    QColor chosen = QColorDialog::getColor(curCol, this, "Fill Color",
        QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;

    uint32_t packed = (static_cast<uint32_t>(chosen.alpha()) << 24)
                    | (static_cast<uint32_t>(chosen.red())   << 16)
                    | (static_cast<uint32_t>(chosen.green()) << 8)
                    |  static_cast<uint32_t>(chosen.blue());

    if (tl->appearance().fills.empty()) {
        tl->appearance().fills.push_back({packed, true});
    } else {
        tl->appearance().fills[0].color = packed;
        tl->appearance().fills[0].enabled = true;
    }

    m_gfxFillColorBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; border: 1px solid #555; min-width: 40px; min-height: 18px; }")
        .arg(chosen.name()));
    emit propertyChanged();
}

void PropertiesPanel::applyGfxStrokeEnabled()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    bool enabled = m_gfxStrokeCheck->isChecked();
    if (tl->appearance().strokes.empty()) {
        if (enabled)
            tl->appearance().strokes.push_back({0xFF000000, 2.0f, StrokePosition::Outer, true});
    } else {
        tl->appearance().strokes[0].enabled = enabled;
    }
    emit propertyChanged();
}

void PropertiesPanel::applyGfxStrokeWidth()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl || tl->appearance().strokes.empty()) return;
    tl->appearance().strokes[0].width = static_cast<float>(m_gfxStrokeWidthSpin->value());
    emit propertyChanged();
}

void PropertiesPanel::applyGfxStrokeColor()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;

    uint32_t current = 0xFF000000;
    if (!tl->appearance().strokes.empty())
        current = tl->appearance().strokes[0].color;
    QColor curCol(static_cast<int>((current>>16)&0xFF),
                  static_cast<int>((current>>8)&0xFF),
                  static_cast<int>(current&0xFF));

    QColor chosen = QColorDialog::getColor(curCol, this, "Stroke Color");
    if (!chosen.isValid()) return;

    uint32_t packed = 0xFF000000
                    | (static_cast<uint32_t>(chosen.red()) << 16)
                    | (static_cast<uint32_t>(chosen.green()) << 8)
                    |  static_cast<uint32_t>(chosen.blue());

    if (tl->appearance().strokes.empty())
        tl->appearance().strokes.push_back({packed, 2.0f, StrokePosition::Outer, true});
    else
        tl->appearance().strokes[0].color = packed;

    m_gfxStrokeColorBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; border: 1px solid #555; }")
        .arg(chosen.name()));
    emit propertyChanged();
}

void PropertiesPanel::applyGfxShadowEnabled()
{
    if (m_updating || !m_clip || m_clip->clipType() != ClipType::Graphic) return;
    auto* tl = firstTextLayer(static_cast<GraphicClip*>(m_clip));
    if (!tl) return;
    bool enabled = m_gfxShadowCheck->isChecked();
    if (tl->appearance().shadows.empty()) {
        if (enabled)
            tl->appearance().shadows.push_back({0x80000000, 135.0f, 4.0f, 0.0f, 0.6f, true});
    } else {
        tl->appearance().shadows[0].enabled = enabled;
    }
    emit propertyChanged();
}

} // namespace rt

