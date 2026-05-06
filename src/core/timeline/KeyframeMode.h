/*
 * KeyframeMode — global "Auto Keyframe" toggle (Premiere-style).
 *
 * When OFF (the default), value writes from the Program Monitor transform
 * overlay and Effect Controls panel never CREATE new keyframes — they only
 * update existing keyframes at the current time, or update the static value
 * if no keyframe exists at that time.
 *
 * When ON, value writes behave like After Effects: any change at the
 * playhead inserts/updates a keyframe on tracks that are already animated.
 *
 * Tracks that are completely static (no keyframes) are NEVER auto-converted
 * to animated regardless of mode — the user must explicitly enable animation
 * on a property (e.g. via the keyframe diamond in EffectControls) before any
 * keyframes are ever created automatically.
 */

#pragma once

namespace rt::KeyframeMode {

[[nodiscard]] bool isAutoEnabled() noexcept;
void setAutoEnabled(bool enabled) noexcept;

} // namespace rt::KeyframeMode
