// The mod was called fnvfov before it became ROWE for Fallout New Vegas. Shared by the plugin and
// the launcher.
#pragma once

// If dir (with a trailing backslash) has no rowe.ini but an fnvfov.ini, writes rowe.ini from it
// (its [fnvfov] section becomes [rowe]; [dev] and [launcher] are kept). The old file stays.
// Returns true if it did.
bool TakeOverOldIni(const char* dir);
