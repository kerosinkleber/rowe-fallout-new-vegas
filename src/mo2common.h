// Mod Organizer 2 helpers shared by the plugin (mo2setup.cpp) and rowe_mo2setup.exe (mo2helper.cpp):
// paths, the instance's ini (QSettings text: @ByteArray(...), doubled backslashes) and finding a file
// in its mods folder.
#pragma once
#include <string>
#include <vector>

bool Mo2Exists(const std::string& path);
std::string Mo2Dir(const std::string& path);   // with the trailing backslash
std::string Mo2Norm(std::string path);         // for comparing paths
std::string Mo2IniFor(const std::string& moExe);  // the instance's ModOrganizer.ini, or ""

struct Mo2IniText {
    std::vector<std::string> lines;
    std::string eol = "\r\n";
};
bool Mo2ReadIni(const std::string& path, Mo2IniText* ini);
bool Mo2WriteIni(const std::string& path, const Mo2IniText& ini);
int Mo2Section(const Mo2IniText& ini, const char* name, int* end);  // first line, -1 if missing
std::string Mo2Get(const Mo2IniText& ini, const char* section, const std::string& key);
void Mo2Set(Mo2IniText* ini, const char* section, const std::string& key, const std::string& value);

// <mods folder>\<mod>\<relative> for the mod enabled in the selected profile (else the first mod
// that has it), or "".
std::string Mo2FindInMods(const Mo2IniText& ini, const std::string& iniPath, const char* relative);
