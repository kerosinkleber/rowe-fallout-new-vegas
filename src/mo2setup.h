// Mod Organizer 2 setup from the installer's options (mo2setup.cpp), run by the plugin at DeferredInit.
#pragma once

typedef void (*Mo2SetupLog)(const char* fmt, ...);

// pluginDir: Data\NVSE\Plugins as the game sees it (the installer's marker files). Does nothing
// without markers; under Mod Organizer it starts rowe_mo2setup.exe and removes the markers.
void Mo2SetupStart(const char* pluginDir, Mo2SetupLog log);
