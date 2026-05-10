/*
 * Settings.h — Centralized QSettings factory.
 *
 * Dev builds (ROUNDTABLE_DEV_BUILD) use ("ROUNDTABLE", "NLE-Dev") so they
 * don't contaminate the installed version's settings (recent files, workspace
 * layout, last project path, etc.).  Release builds use ("ROUNDTABLE", "NLE").
 */

#pragma once

#include <QSettings>

namespace rt {

/// Returns a QSettings instance scoped to the correct application key.
/// Dev builds use "NLE-Dev"; installed releases use "NLE".
inline QSettings appSettings()
{
#ifdef ROUNDTABLE_DEV_BUILD
    return QSettings("ROUNDTABLE", "NLE-Dev");
#else
    return QSettings("ROUNDTABLE", "NLE");
#endif
}

} // namespace rt
