// Mod Organizer 2 setup from the installer's options (mo2setup.cpp), shared by rowe.dll and
// rowe_launcher.exe.
#pragma once

typedef void (*Mo2SetupLog)(const char* fmt, ...);

// pluginDir: the folder with rowe_launcher.exe and the installer's marker files (Data\NVSE\Plugins
// as the process sees it). Does nothing without markers; under Mod Organizer it starts the
// launcher's setup mode and removes the markers.
void Mo2SetupStart(const char* pluginDir, Mo2SetupLog log);
