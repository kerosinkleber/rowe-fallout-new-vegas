// The launcher's view of the game: where it is, its INI files, saves and archives.
#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct GamePaths {
    std::string game;      // the game folder (FalloutNV.exe), with a trailing backslash
    std::string myGames;   // Documents\My Games\FalloutNV\ (under MO2 its virtual file system maps the profile's files there)
    std::string saves;     // the save folder
    std::string modIni;    // Data\NVSE\Plugins\rowe.ini
    std::string mo2;       // the Mod Organizer 2 instance the launcher runs in ("" if none)
    std::string mo2Profile;
    std::string how;       // how the game folder was found
};

bool FindGame(GamePaths* p);
bool GameRunning();

// ---- INI files: the game reads Fallout.ini, then FalloutPrefs.ini (and plugins apply
// FalloutCustom.ini over both). A setting is read from the last file that has it and written to
// FalloutCustom.ini if that file sets it, else to FalloutPrefs.ini.
enum IniFile { kIniDefault, kIniPrefs, kIniCustom, kIniNone };

struct Ini {
    std::string fallout, prefs, custom;
    IniFile Where(const char* section, const char* key) const;
    bool Has(const std::string& file, const char* section, const char* key) const;
    std::string Get(const char* section, const char* key, const char* def) const;
    int GetInt(const char* section, const char* key, int def) const;
    float GetFloat(const char* section, const char* key, float def) const;
    bool Set(const char* section, const char* key, const char* value);
    bool SetInt(const char* section, const char* key, int value);
    bool SetFloat(const char* section, const char* key, float value);
    void BackupOnce();  // copies FalloutPrefs.ini (and FalloutCustom.ini) once before the first change
    bool backedUp = false;
};

// ---- saves
struct SaveInfo {
    std::string file;      // file name without .fos (what `load` takes)
    std::string player, karma, location, playTime;
    int level = 0, number = 0;
    FILETIME written = {};
    int shotW = 0, shotH = 0;
    long long shotOffset = 0;  // RGB screenshot in the file
};
std::vector<SaveInfo> ListSaves(const std::string& dir);
bool LoadSaveShot(const std::string& dir, const SaveInfo& s, std::vector<unsigned char>* rgba);  // shotW x shotH, BGRA

// ---- archives and the UI font
bool ReadGameFile(const GamePaths& p, const Ini& ini, const char* path, std::vector<unsigned char>* out);
// The files of one Data folder (e.g. "textures\\interface\\loading") whose lower-case names `want`
// accepts, each name once: loose ones first, then from the archives (archives without textures are
// skipped for a textures folder). `got` returns false to stop; so does `stop`, checked between archives.
void ReadGameFolder(const GamePaths& p, const Ini& ini, const char* folder, bool (*want)(const char* name),
                    const std::function<bool(const std::string& name, std::vector<unsigned char>& data)>& got,
                    const std::atomic<bool>* stop = nullptr);
bool LoadGameFont(const GamePaths& p, const Ini& ini, std::string* used);  // hands the font to GameFontSet

// Launch: writes the plugin's start file (load `save`, or stay in the main menu if empty) and
// starts the game the way the plugin's restart does.
bool LaunchGame(const GamePaths& p, const std::string& save, std::string* error);
