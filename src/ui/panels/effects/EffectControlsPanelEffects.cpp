/*
 * EffectControlsPanelEffects.cpp - Effect parameter wiring, UltraKey UI, and mask UI.
 * Split from EffectControlsPanelTree.cpp.
 */

#include "panels/effects/EffectControlsPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/OpacityMask.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/EffectCommands.h"
#include "command/commands/KeyframeCmds.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/ChromaKey.h"

#include <QFrame>
#include <QGridLayout>
#include <QMenu>
#include <QColorDialog>
#include <QComboBox>
#include <QPushButton>

namespace rt {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectControlsPanel::wireEffectParam(ScrubbySpinBox* spin,
                                          size_t effectIdx, size_t paramIdx)
{
    // Live preview during scrub
    connect(spin, &ScrubbySpinBox::valueScrubbed,
            this, [this, effectIdx, paramIdx](double val) {
        if (!m_clip || m_updating) return;
        auto& st = m_clip->effects();
        if (effectIdx >= st.effectCount()) return;
        auto& ef = st.effect(effectIdx);
        if (paramIdx >= ef.paramCount()) return;
        ef.param(paramIdx).track.writeValue(clipRelativeTick(), static_cast<float>(val));
        emit propertyChanged();
    });

    // Commit with undo support
    connect(spin, &ScrubbySpinBox::valueCommitted,
            this, [this, effectIdx, paramIdx](double oldVal, double newVal) {
        if (!m_clip || m_updating) return;
        auto& st = m_clip->effects();
        if (effectIdx >= st.effectCount()) return;
        auto& ef = st.effect(effectIdx);
        if (paramIdx >= ef.paramCount()) return;
        auto& trk = ef.param(paramIdx).track;
        int64_t t = clipRelativeTick();
        auto fOld = static_cast<float>(oldVal);
        auto fNew = static_cast<float>(newVal);

        bool createdKF = false;
        if (!trk.isStatic() && trk.keyframeCount() >= 2 && trk.hasKeyframeAt(t)) {
            KeyframeTrack<float> tmp(trk.defaultValue());
            for (const auto& kf : trk.keyframes()) {
                if (kf.time != t) tmp.restoreKeyframe(kf);
            }
            createdKF = (std::abs(tmp.evaluate(t) - fOld) < 0.01f);
        }

        emit propertyChanged();
        if (m_commandStack) {
            auto* stack = &st;
            auto fxId = ef.id();
            auto pi = paramIdx;
            m_commandStack->pushWithoutExecute(
                std::make_unique<LambdaCommand>(
                    "Set Effect Parameter",
                    [stack, fxId, pi, fNew, t]() {
                        if (auto* fx2 = stack->effectById(fxId))
                            if (pi < fx2->paramCount())
                                fx2->param(pi).track.writeValue(t, fNew);
                    },
                    [stack, fxId, pi, fOld, t, createdKF]() {
                        if (auto* fx2 = stack->effectById(fxId))
                            if (pi < fx2->paramCount()) {
                                if (createdKF)
                                    fx2->param(pi).track.removeKeyframeAtTime(t);
                                else
                                    fx2->param(pi).track.writeValue(t, fOld);
                            }
                    }));
        }
    });
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  buildGenericEffectUI â€” flat parameter rows for non-Ultra Key effects
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectControlsPanel::buildGenericEffectUI(Effect& fx, size_t effectIdx,
                                               int& rowIdx)
{
    // Row builder helper (same as the local lambda in buildPropertyTree)
    auto makeRow = [&](const QString& name,
                       KeyframeTrack<float>* track) -> PropertyRow* {
        auto* row = new PropertyRow(name, track, m_propContainer);
        row->setRowIndex(rowIdx++);
        m_propertyRows.push_back(row);
        connect(row, &PropertyRow::addKeyframeRequested,
                this, &EffectControlsPanel::onAddKeyframe);
        connect(row, &PropertyRow::deleteKeyframeRequested,
                this, &EffectControlsPanel::onDeleteKeyframe);
        connect(row, &PropertyRow::goToPrevKeyframe,
                this, &EffectControlsPanel::onGoToPrevKeyframe);
        connect(row, &PropertyRow::goToNextKeyframe,
                this, &EffectControlsPanel::onGoToNextKeyframe);
        return row;
    };

    for (size_t p = 0; p < fx.paramCount(); ++p) {
        auto& param = fx.param(p);
        auto* fxRow = makeRow(QString::fromStdString(param.name), &param.track);
        auto* fxSpin = createScrubby(param.minVal, param.maxVal, 0.01, 2);
        fxSpin->setValue(static_cast<double>(param.track.evaluate(clipRelativeTick())));
        fxRow->addValueWidget(fxSpin);
        m_propLayout->addWidget(fxRow);
        wireEffectParam(fxSpin, effectIdx, p);
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  buildUltraKeyUI â€” grouped sections for Ultra Key effect
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectControlsPanel::buildUltraKeyUI(Effect& fx, size_t effectIdx,
                                          int& rowIdx)
{
    const auto& tc = Theme::colors();

    // Row builder (identical to the one above but captured for this scope)
    auto makeRow = [&](const QString& name,
                       KeyframeTrack<float>* track) -> PropertyRow* {
        auto* row = new PropertyRow(name, track, m_propContainer);
        row->setRowIndex(rowIdx++);
        m_propertyRows.push_back(row);
        connect(row, &PropertyRow::addKeyframeRequested,
                this, &EffectControlsPanel::onAddKeyframe);
        connect(row, &PropertyRow::deleteKeyframeRequested,
                this, &EffectControlsPanel::onDeleteKeyframe);
        connect(row, &PropertyRow::goToPrevKeyframe,
                this, &EffectControlsPanel::onGoToPrevKeyframe);
        connect(row, &PropertyRow::goToNextKeyframe,
                this, &EffectControlsPanel::onGoToNextKeyframe);
        return row;
    };

    // Sub-section header builder (Premiere-style collapsible group)
    auto makeSubHeader = [&](const QString& title) -> QWidget* {
        auto* header = new QWidget(m_propContainer);
        header->setFixedHeight(26);
        header->setCursor(Qt::PointingHandCursor);
        header->setStyleSheet(QStringLiteral(
            "background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2;")
            .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(24, 0, 6, 0);
        hl->setSpacing(6);

        auto* arrow = new QToolButton(header);
        arrow->setText(QStringLiteral("\u25B6"));  // â–¶ collapsed by default
        arrow->setFixedSize(16, 20);
        arrow->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-size: 11px; background: transparent; border: none; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textPrimary)));
        hl->addWidget(arrow);

        auto* lbl = new QLabel(title, header);
        lbl->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 12px; font-weight: bold; background: transparent;")
            .arg(Theme::hex(tc.textPrimary)));
        hl->addWidget(lbl);
        hl->addStretch();

        m_sectionArrows.push_back({header, arrow, {}, title});
        return header;
    };

    // Helper: add a param row and wire it
    auto addParamRow = [&](size_t paramIdx, double step = 1.0, int dec = 1,
                           const QString& suffix = {}) {
        auto& param = fx.param(paramIdx);
        auto* row = makeRow(QString::fromStdString(param.name), &param.track);
        auto* spin = createScrubby(param.minVal, param.maxVal, step, dec, suffix);
        spin->setValue(static_cast<double>(param.track.evaluate(clipRelativeTick())));
        row->addValueWidget(spin);
        m_propLayout->addWidget(row);
        wireEffectParam(spin, effectIdx, paramIdx);
    };

    // â”€â”€ Key Color row with eyedropper button â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        auto* keyColorWidget = new QWidget(m_propContainer);
        keyColorWidget->setFixedHeight(32);
        auto* kcLayout = new QHBoxLayout(keyColorWidget);
        kcLayout->setContentsMargins(20, 2, 6, 2);
        kcLayout->setSpacing(6);

        auto* lbl = new QLabel("Key Color", keyColorWidget);
        lbl->setMinimumWidth(80);
        lbl->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 12px; background: transparent;")
            .arg(Theme::hex(tc.textPrimary)));
        kcLayout->addWidget(lbl);

        // Color swatch (shows current key color)
        auto* swatch = new QPushButton(keyColorWidget);
        swatch->setFixedSize(36, 22);
        float r = fx.param(ChromaKey::KeyColorR).track.evaluate(0);
        float g = fx.param(ChromaKey::KeyColorG).track.evaluate(0);
        float b = fx.param(ChromaKey::KeyColorB).track.evaluate(0);
        QColor keyCol = QColor::fromRgbF(r, g, b);
        swatch->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2; border-radius: 2px; }"
            "QPushButton:hover { border: 1px solid %3; }")
            .arg(keyCol.name(), Theme::hex(tc.border), Theme::hex(tc.accent)));

        // Click swatch â†’ open color dialog
        connect(swatch, &QPushButton::clicked, this, [this, effectIdx, swatch]() {
            if (!m_clip) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            auto& ef = st.effect(effectIdx);
            float cr = ef.param(ChromaKey::KeyColorR).track.evaluate(clipRelativeTick());
            float cg = ef.param(ChromaKey::KeyColorG).track.evaluate(clipRelativeTick());
            float cb = ef.param(ChromaKey::KeyColorB).track.evaluate(clipRelativeTick());
            QColor current = QColor::fromRgbF(cr, cg, cb);
            QColor chosen = QColorDialog::getColor(current, this, "Select Key Color");
            if (chosen.isValid()) {
                int64_t t = clipRelativeTick();
                ef.param(ChromaKey::KeyColorR).track.writeValue(t, static_cast<float>(chosen.redF()));
                ef.param(ChromaKey::KeyColorG).track.writeValue(t, static_cast<float>(chosen.greenF()));
                ef.param(ChromaKey::KeyColorB).track.writeValue(t, static_cast<float>(chosen.blueF()));
                swatch->setStyleSheet(QStringLiteral(
                    "QPushButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                    .arg(chosen.name(), Theme::hex(Theme::colors().border)));
                emit propertyChanged();
            }
        });
        kcLayout->addWidget(swatch);

        // Eyedropper button
        auto* eyedropBtn = new QToolButton(keyColorWidget);
        eyedropBtn->setText(QStringLiteral("\U0001F4A7")); // ðŸ’§ (droplet)
        eyedropBtn->setToolTip(tr("Pick color from Program Monitor"));
        eyedropBtn->setFixedSize(24, 22);
        eyedropBtn->setStyleSheet(QStringLiteral(
            "QToolButton { background: %1; color: %2; border: 1px solid %3; border-radius: 2px; font-size: 12px; }"
            "QToolButton:hover { background: %4; border: 1px solid %5; }")
            .arg(Theme::hex(tc.surface2), Theme::hex(tc.textPrimary),
                 Theme::hex(tc.border), Theme::hex(tc.surface3),
                 Theme::hex(tc.accent)));
        connect(eyedropBtn, &QToolButton::clicked, this, [this, effectIdx]() {
            emit eyedropperRequested(effectIdx);
        });
        kcLayout->addWidget(eyedropBtn);

        kcLayout->addStretch();
        m_propLayout->addWidget(keyColorWidget);
    }

    // â”€â”€ Output Mode dropdown â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        auto* outputWidget = new QWidget(m_propContainer);
        outputWidget->setFixedHeight(28);
        auto* ol = new QHBoxLayout(outputWidget);
        ol->setContentsMargins(20, 2, 6, 2);
        ol->setSpacing(6);

        auto* lbl = new QLabel("Output", outputWidget);
        lbl->setMinimumWidth(80);
        lbl->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;")
            .arg(Theme::hex(tc.textPrimary)));
        ol->addWidget(lbl);

        auto* combo = new QComboBox(outputWidget);
        combo->addItems({"Composite", "Alpha Channel", "Color Channel"});
        combo->setCurrentIndex(static_cast<int>(fx.param(ChromaKey::OutputMode).track.evaluate(0)));
        combo->setFixedHeight(22);
        ol->addWidget(combo, 1);

        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, effectIdx](int idx) {
            if (!m_clip || m_updating) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            st.effect(effectIdx).param(ChromaKey::OutputMode).track
                .writeValue(clipRelativeTick(), static_cast<float>(idx));
            emit propertyChanged();
        });
        m_propLayout->addWidget(outputWidget);
    }

    // â”€â”€ Setting dropdown (Default / Relaxed / Aggressive / Custom) â”€â”€â”€â”€â”€â”€
    {
        auto* settingWidget = new QWidget(m_propContainer);
        settingWidget->setFixedHeight(28);
        auto* sl = new QHBoxLayout(settingWidget);
        sl->setContentsMargins(20, 2, 6, 2);
        sl->setSpacing(6);

        auto* lbl = new QLabel("Setting", settingWidget);
        lbl->setMinimumWidth(80);
        lbl->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;")
            .arg(Theme::hex(tc.textPrimary)));
        sl->addWidget(lbl);

        auto* combo = new QComboBox(settingWidget);
        combo->addItems({"Default", "Relaxed", "Aggressive", "Custom"});
        combo->setCurrentIndex(static_cast<int>(fx.param(ChromaKey::Setting).track.evaluate(0)));
        combo->setFixedHeight(22);
        sl->addWidget(combo, 1);

        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, effectIdx](int idx) {
            if (!m_clip || m_updating) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            auto& ef = st.effect(effectIdx);
            ef.param(ChromaKey::Setting).track
                .writeValue(clipRelativeTick(), static_cast<float>(idx));
            // Apply preset values
            if (auto* ck = dynamic_cast<ChromaKey*>(&ef))
                ck->applyPreset(idx);
            refresh();
            emit propertyChanged();
        });
        m_propLayout->addWidget(settingWidget);
    }

    // â”€â”€ Matte Generation section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* matteHeader = makeSubHeader("Matte Generation");
    m_propLayout->addWidget(matteHeader);
    addParamRow(ChromaKey::Transparency);
    addParamRow(ChromaKey::Highlight);
    addParamRow(ChromaKey::Shadow);
    addParamRow(ChromaKey::Tolerance);
    addParamRow(ChromaKey::Pedestal);

    // â”€â”€ Matte Cleanup section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* cleanupHeader = makeSubHeader("Matte Cleanup");
    m_propLayout->addWidget(cleanupHeader);
    addParamRow(ChromaKey::Choke);
    addParamRow(ChromaKey::Soften);
    addParamRow(ChromaKey::Contrast);
    addParamRow(ChromaKey::MidPoint);

    // â”€â”€ Spill Suppression section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* spillHeader = makeSubHeader("Spill Suppression");
    m_propLayout->addWidget(spillHeader);
    addParamRow(ChromaKey::Desaturate, 1.0, 1);
    addParamRow(ChromaKey::SpillRange);
    addParamRow(ChromaKey::Spill);
    addParamRow(ChromaKey::Luma);

    // â”€â”€ Color Correction section â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* ccHeader = makeSubHeader("Color Correction");
    m_propLayout->addWidget(ccHeader);
    addParamRow(ChromaKey::Saturation);
    addParamRow(ChromaKey::Hue, 1.0, 1, QStringLiteral("\u00B0")); // Â° symbol
    addParamRow(ChromaKey::Luminance);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  addMask â€” create a new mask and rebuild UI
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectControlsPanel::addMask(uint8_t shapeType)
{
    if (!m_clip) return;

    OpacityMask mask;
    mask.shape = static_cast<MaskShape>(shapeType);
    mask.name = "Mask " + std::to_string(m_clip->maskCount() + 1);

    // Default size: ellipse/rect centered at 50% with 50% coverage
    mask.centerX = 0.5f;
    mask.centerY = 0.5f;
    mask.width   = 0.5f;
    mask.height  = 0.5f;

    if (shapeType == 2) {
        // FreeDrawBezier: start with 4 corner vertices forming a rectangle
        mask.vertices = {
            {0.25f, 0.25f, 0, 0, 0, 0},
            {0.75f, 0.25f, 0, 0, 0, 0},
            {0.75f, 0.75f, 0, 0, 0, 0},
            {0.25f, 0.75f, 0, 0, 0, 0}
        };
    }

    Clip* clip = m_clip;
    OpacityMask savedMask = mask;
    size_t insertIdx = m_clip->maskCount();

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Add Mask",
            [clip, savedMask]() {
                clip->addMask(OpacityMask(savedMask));
            },
            [clip, insertIdx]() {
                clip->removeMask(insertIdx);
            }));
    } else {
        m_clip->addMask(std::move(mask));
    }
    refresh();
    emit maskChanged();
    emit propertyChanged();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  buildMaskUI â€” build parameter rows for each mask on the current clip
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectControlsPanel::buildMaskUI(int& /*rowIdx*/)
{
    if (!m_clip || m_clip->maskCount() == 0) return;

    const auto& tc = Theme::colors();

    for (size_t mi = 0; mi < m_clip->maskCount(); ++mi) {
        auto& mask = m_clip->masks()[mi];

        // â”€â”€ Mask sub-section header â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* header = new QWidget(m_propContainer);
        header->setFixedHeight(26);
        header->setCursor(Qt::PointingHandCursor);
        header->setStyleSheet(QStringLiteral(
            "background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2;")
            .arg(Theme::hex(tc.surface2), Theme::hex(tc.border)));
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(24, 0, 6, 0);
        hl->setSpacing(6);

        auto* arrow = new QToolButton(header);
        arrow->setText(QStringLiteral("\u25BC"));
        arrow->setFixedSize(16, 20);
        arrow->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-size: 11px; background: transparent; border: none; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textSecondary), Theme::hex(tc.textPrimary)));
        hl->addWidget(arrow);

        auto* titleLabel = new QLabel(QString::fromStdString(mask.name), header);
        titleLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 12px; font-weight: bold; background: transparent;")
            .arg(Theme::hex(tc.textPrimary)));
        hl->addWidget(titleLabel);
        hl->addStretch();

        // Delete mask button
        auto* deleteBtn = new QToolButton(header);
        deleteBtn->setText(QStringLiteral("\u2715"));
        deleteBtn->setFixedSize(20, 20);
        deleteBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-size: 12px; background: transparent; border: none; padding: 0; }"
            "QToolButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textTertiary), Theme::hex(tc.error)));
        hl->addWidget(deleteBtn);
        connect(deleteBtn, &QToolButton::clicked, this, [this, mi]() {
            if (m_clip) {
                Clip* clip = m_clip;
                OpacityMask savedMask = clip->masks()[mi];
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        "Delete Mask",
                        [clip, mi]() {
                            clip->removeMask(mi);
                        },
                        [clip, mi, savedMask]() {
                            auto& masks = clip->masks();
                            if (mi <= masks.size())
                                masks.insert(masks.begin() + static_cast<ptrdiff_t>(mi), savedMask);
                        }));
                } else {
                    m_clip->removeMask(mi);
                }
                refresh();
                emit maskChanged();
                emit propertyChanged();
            }
        });

        m_sectionArrows.push_back({header, arrow, {}, QString::fromStdString(mask.name)});
        m_propLayout->addWidget(header);

        // Click on mask header â†’ select this mask for editing in Program Monitor
        header->installEventFilter(this);
        header->setProperty("maskIndex", static_cast<int>(mi));

        // â”€â”€ Mask Feather â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        {
            auto* w = new QWidget(m_propContainer);
            w->setFixedHeight(28);
            auto* lay = new QHBoxLayout(w);
            lay->setContentsMargins(36, 2, 6, 2);
            lay->setSpacing(6);
            auto* lbl = new QLabel("Mask Feather", w);
            lbl->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;")
                .arg(Theme::hex(tc.textPrimary)));
            lay->addWidget(lbl);
            auto* spin = createScrubby(0, 500, 0.5, 1, " px");
            spin->setValue(static_cast<double>(mask.feather));
            lay->addWidget(spin, 1);
            connect(spin, &ScrubbySpinBox::valueScrubbed, this, [this, mi](double val) {
                if (m_clip && mi < m_clip->maskCount()) {
                    m_clip->masks()[mi].feather = static_cast<float>(val);
                    emit maskChanged();
                    emit propertyChanged();
                }
            });
            connect(spin, &ScrubbySpinBox::valueCommitted, this, [this, mi](double oldVal, double newVal) {
                if (!m_clip || !m_commandStack || mi >= m_clip->maskCount()) return;
                Clip* clip = m_clip;
                auto fOld = static_cast<float>(oldVal);
                auto fNew = static_cast<float>(newVal);
                m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                    "Mask Feather",
                    [clip, mi, fNew]() { if (mi < clip->maskCount()) clip->masks()[mi].feather = fNew; },
                    [clip, mi, fOld]() { if (mi < clip->maskCount()) clip->masks()[mi].feather = fOld; }));
            });
            m_propLayout->addWidget(w);
        }

        // â”€â”€ Mask Opacity â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        {
            auto* w = new QWidget(m_propContainer);
            w->setFixedHeight(28);
            auto* lay = new QHBoxLayout(w);
            lay->setContentsMargins(36, 2, 6, 2);
            lay->setSpacing(6);
            auto* lbl = new QLabel("Mask Opacity", w);
            lbl->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;")
                .arg(Theme::hex(tc.textPrimary)));
            lay->addWidget(lbl);
            auto* spin = createScrubby(0, 100, 0.5, 1, " %");
            spin->setValue(static_cast<double>(mask.maskOpacity * 100.0f));
            lay->addWidget(spin, 1);
            connect(spin, &ScrubbySpinBox::valueScrubbed, this, [this, mi](double val) {
                if (m_clip && mi < m_clip->maskCount()) {
                    m_clip->masks()[mi].maskOpacity = static_cast<float>(val / 100.0);
                    emit maskChanged();
                    emit propertyChanged();
                }
            });
            connect(spin, &ScrubbySpinBox::valueCommitted, this, [this, mi](double oldVal, double newVal) {
                if (!m_clip || !m_commandStack || mi >= m_clip->maskCount()) return;
                Clip* clip = m_clip;
                auto fOld = static_cast<float>(oldVal / 100.0);
                auto fNew = static_cast<float>(newVal / 100.0);
                m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                    "Mask Opacity",
                    [clip, mi, fNew]() { if (mi < clip->maskCount()) clip->masks()[mi].maskOpacity = fNew; },
                    [clip, mi, fOld]() { if (mi < clip->maskCount()) clip->masks()[mi].maskOpacity = fOld; }));
            });
            m_propLayout->addWidget(w);
        }

        // â”€â”€ Mask Expansion â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        {
            auto* w = new QWidget(m_propContainer);
            w->setFixedHeight(28);
            auto* lay = new QHBoxLayout(w);
            lay->setContentsMargins(36, 2, 6, 2);
            lay->setSpacing(6);
            auto* lbl = new QLabel("Mask Expansion", w);
            lbl->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;")
                .arg(Theme::hex(tc.textPrimary)));
            lay->addWidget(lbl);
            auto* spin = createScrubby(-500, 500, 0.5, 1, " px");
            spin->setValue(static_cast<double>(mask.expansion));
            lay->addWidget(spin, 1);
            connect(spin, &ScrubbySpinBox::valueScrubbed, this, [this, mi](double val) {
                if (m_clip && mi < m_clip->maskCount()) {
                    m_clip->masks()[mi].expansion = static_cast<float>(val);
                    emit maskChanged();
                    emit propertyChanged();
                }
            });
            connect(spin, &ScrubbySpinBox::valueCommitted, this, [this, mi](double oldVal, double newVal) {
                if (!m_clip || !m_commandStack || mi >= m_clip->maskCount()) return;
                Clip* clip = m_clip;
                auto fOld = static_cast<float>(oldVal);
                auto fNew = static_cast<float>(newVal);
                m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                    "Mask Expansion",
                    [clip, mi, fNew]() { if (mi < clip->maskCount()) clip->masks()[mi].expansion = fNew; },
                    [clip, mi, fOld]() { if (mi < clip->maskCount()) clip->masks()[mi].expansion = fOld; }));
            });
            m_propLayout->addWidget(w);
        }

        // â”€â”€ Inverted checkbox â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        {
            auto* chk = new QCheckBox("Inverted", m_propContainer);
            chk->setChecked(mask.inverted);
            chk->setFixedHeight(28);
            chk->setStyleSheet(QStringLiteral(
                "QCheckBox { color: %1; font-size: 12px; padding-left: 36px; background: transparent; }"
                "QCheckBox::indicator { width: 14px; height: 14px; }")
                .arg(Theme::hex(tc.textPrimary)));
            connect(chk, &QCheckBox::toggled, this, [this, mi](bool checked) {
                if (m_clip && mi < m_clip->maskCount()) {
                    Clip* clip = m_clip;
                    bool oldVal = !checked;
                    bool newVal = checked;
                    if (m_commandStack) {
                        m_commandStack->execute(std::make_unique<LambdaCommand>(
                            "Mask Inverted",
                            [clip, mi, newVal]() { if (mi < clip->maskCount()) clip->masks()[mi].inverted = newVal; },
                            [clip, mi, oldVal]() { if (mi < clip->maskCount()) clip->masks()[mi].inverted = oldVal; }));
                    } else {
                        m_clip->masks()[mi].inverted = checked;
                    }
                    emit maskChanged();
                    emit propertyChanged();
                }
            });
            m_propLayout->addWidget(chk);
        }
    }
}

} // namespace rt
