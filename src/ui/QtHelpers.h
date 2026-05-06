#pragma once
#include <QString>

namespace rt {

/// Custom Qt message handler that suppresses known harmless warnings.
void installQtMessageFilter();

/// Locate the project root directory by walking up from the executable.
QString findProjectRoot();

/// Resolve a writable user data directory (e.g. %LOCALAPPDATA%/ROUNDTABLE/).
/// Created on first call if it doesn't exist. Falls back to project root.
QString userDataDir();

} // namespace rt
