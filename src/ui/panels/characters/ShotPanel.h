/*
 * ShotPanel — Dedicated dock panel for shot/character properties.
 *
 * Moved from PropertiesPanel's Spine+Shot sections into a standalone tab
 * that sits next to Audio Mixer (tabified with Effect Controls/Effects/
 * Keyframes/History/Audio Mixer).
 *
 * Provides proper dropdowns for character, animation, outfit, and stance
 * instead of free-text fields.  Populated from ModelManager (characters/
 * outfits) and SpineAnimation (available animations in the loaded skeleton).
 *
 * Layout:
 *   ┌─────────────────────────────────────────────┐
 *   │  Shots — [Clip Name]                        │
 *   ├─────────────────────────────────────────────┤
 *   │  ▸ Character                                 │
 *   │    Character: [▼ dropdown       ]            │
 *   │    Outfit:    [▼ dropdown       ]            │
 *   │    Stance:    [▼ Default / Aim / Cover ]     │
 *   ├─────────────────────────────────────────────┤
 *   │  ▸ Animation                                 │
 *   │    Animation: [▼ dropdown       ]            │
 *   │    Loop:      ☑                              │
 *   │    Talking:   ☐                              │
 *   │    Speed:     [←─ 1.000 ─→]                  │
 *   │    Continuity: ☑  (seamless across cuts)     │
 *   ├─────────────────────────────────────────────┤
 *   │  ▸ Shot Group                                │
 *   │    Group:     Group 1 — Layer: V1            │
 *   │    Shot:      [▼ preset dropdown ]           │
 *   └─────────────────────────────────────────────┘
 */

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rt {

class Clip;
class SpineClip;
class Track;
class CommandStack;
class ModelManager;
class ShotPresetManager;
class Timeline;
class ScrubbySpinBox;

/// Shots panel — dedicated dock for character + animation + shot properties.
class ShotPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ShotPanel(QWidget* parent = nullptr);
    ~ShotPanel() override;

    // ── Clip binding ────────────────────────────────────────────────────

    /// Set the clip to display/edit. Pass nullptr to clear.
    void setClip(Clip* clip, Track* track = nullptr);

    /// Set multiple selected clips. Shows shot group UI if all share the same group.
    void setMultiSelection(const std::vector<Clip*>& clips);

    /// Refresh all widgets from current clip.
    void refresh();

    /// Clear the panel.
    void clearClip();

    // ── Dependencies ────────────────────────────────────────────────────

    void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }
    void setModelManager(ModelManager* mgr) noexcept { m_modelManager = mgr; }
    void setShotPresetManager(ShotPresetManager* mgr) noexcept { m_shotManager = mgr; }
    void setTimeline(Timeline* tl) noexcept { m_timeline = tl; }

    /// Callback that returns animation names for a character identity.
    /// TimelineWorkspace sets this to query the spine shared cache.
    using AnimNamesProvider = std::function<std::vector<std::string>(
        const std::string& characterName, const std::string& outfit, int stance)>;
    void setAnimationNamesProvider(AnimNamesProvider provider) {
        m_animNamesProvider = std::move(provider);
    }

    // ── Accessors ───────────────────────────────────────────────────────

    [[nodiscard]] QComboBox*    characterCombo()  const noexcept { return m_characterCombo; }
    [[nodiscard]] QComboBox*    outfitCombo()     const noexcept { return m_outfitCombo; }
    [[nodiscard]] QComboBox*    stanceCombo()     const noexcept { return m_stanceCombo; }
    [[nodiscard]] QComboBox*    animationCombo()  const noexcept { return m_animationCombo; }
    [[nodiscard]] QCheckBox*    loopingCheck()    const noexcept { return m_loopingCheck; }
    [[nodiscard]] QCheckBox*    talkingCheck()    const noexcept { return m_talkingCheck; }
    [[nodiscard]] ScrubbySpinBox* animSpeedSpin() const noexcept { return m_animSpeedSpin; }
    [[nodiscard]] QCheckBox*    continuityCheck() const noexcept { return m_continuityCheck; }

    QSize sizeHint() const override;

signals:
    /// Emitted when any property changes.
    void propertyChanged();

    /// Emitted when a shot switch is needed.
    void shotSwitchRequested(uint64_t groupId, const std::string& newShotName);

private:
    void setupUI();
    void setupCharacterSection(QWidget* container);
    void setupAnimationSection(QWidget* container);
    void setupShotGroupSection(QWidget* container);

    void showSectionsForType();
    void populateFromClip();
    void populateCharacterDropdown();
    void populateOutfitDropdown();
    void populateStanceDropdown();
    void populateAnimationDropdown();
    void updateShotSection();

    // ── Apply changes ───────────────────────────────────────────────────
    void applyCharacter();
    void applyOutfit();
    void applyStance();
    void applyAnimation();
    void applyLooping();
    void applyTalking();
    void applyAnimSpeed();
    void applyContinuity();
    void onShotChanged(const std::string& newShotName);

    // State
    Clip*              m_clip{nullptr};
    SpineClip*         m_spineClip{nullptr};  // cached dynamic_cast
    Track*             m_track{nullptr};
    CommandStack*      m_commandStack{nullptr};
    ModelManager*      m_modelManager{nullptr};
    ShotPresetManager* m_shotManager{nullptr};
    Timeline*          m_timeline{nullptr};
    bool               m_updating{false};
    AnimNamesProvider  m_animNamesProvider;

    // Header
    QLabel*            m_headerLabel{nullptr};

    // Character section
    QWidget*           m_characterSection{nullptr};
    QComboBox*         m_characterCombo{nullptr};
    QComboBox*         m_outfitCombo{nullptr};
    QComboBox*         m_stanceCombo{nullptr};

    // Animation section
    QWidget*           m_animationSection{nullptr};
    QComboBox*         m_animationCombo{nullptr};
    QCheckBox*         m_loopingCheck{nullptr};
    QCheckBox*         m_talkingCheck{nullptr};
    ScrubbySpinBox*    m_animSpeedSpin{nullptr};
    QCheckBox*         m_continuityCheck{nullptr};

    // Shot group section
    QWidget*           m_shotGroupSection{nullptr};
    QLabel*            m_shotInfoLabel{nullptr};
    QComboBox*         m_shotCombo{nullptr};

    // Empty state label
    QLabel*            m_emptyLabel{nullptr};
};

} // namespace rt
