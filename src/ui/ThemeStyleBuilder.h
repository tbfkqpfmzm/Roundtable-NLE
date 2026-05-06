/*
 * ThemeStyleBuilder.h — Modular QSS stylesheet builders.
 *
 * Splits the monolithic ThemeStylesheet.cpp into focused modules:
 *   - coreStyles():     Base widgets, docks, menus, toolbars, scrollbars, tabs, splitters
 *   - controlStyles():  Inputs, buttons, checkboxes, sliders, combos
 *   - viewStyles():     GroupBox, labels, lists, tables, progress bars, tooltips
 *   - editorStyles():   Timeline-specific: tracks, clips, playhead, timecodes
 *
 * Each module receives a StyleContext carrying references to the global
 * ThemeColors, ThemeTypography, and ThemeMetrics singletons.
 */

#pragma once

#include <QString>
#include "Theme.h"

namespace rt::theme_style {

/// Aggregate carrying references to the three theme token singletons.
struct StyleContext
{
    const ThemeColors&     colors;
    const ThemeTypography& typography;
    const ThemeMetrics&    metrics;
};

/// ── Module entry points ─────────────────────────────────────────────
QString coreStyles(const StyleContext& context);
QString controlStyles(const StyleContext& context);
QString viewStyles(const StyleContext& context);
QString editorStyles(const StyleContext& context);

} // namespace rt::theme_style