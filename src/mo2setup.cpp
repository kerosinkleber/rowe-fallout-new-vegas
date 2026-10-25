// Mod Organizer 2 setup after an install with the installer's options (fomod: "make the ROWE
// launcher MO2's start option", "desktop shortcut"). Each chosen option installs
// rowe_mo2setup.exe and a marker file next to the DLL (rowe_setup_start.txt,
// rowe_setup_shortcut.txt). On the first game start under Mod Organizer the plugin writes the job
// (rowe_mo2setup_request.ini) next to the helper in its mod folder and has Explorer start the
// helper from there, so it runs outside Mod Organizer's virtual file system (Mod Organizer would
// otherwise wait for it before closing); the helper waits until Mod Organizer closes and then
// edits its ini (mo2helper.cpp). The markers are then removed, so this happens once per install.
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string>
#include "mo2common.h"
#include "mo2setup.h"

// Mod Organizer among this process's ancestors (the game may be started by nvse_loader or the
// ROWE launcher, which MO2 started): its image path, or "".
static std::string Mo2Ancestor()
{
    std::string found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    DWORD pid = GetCurrentProcessId();
    for (int depth = 0; depth < 6 && found.empty(); ++depth) {
        DWORD parent = 0;
        PROCESSENTRY32 pe = {sizeof(pe)};
        for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
            if (pe.th32ProcessID == pid) {
                parent = pe.th32ParentProcessID;
                break;
            }
        if (!parent) break;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
        if (!h) break;
        char path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameA(h, 0, path, &size)) {
            const char* name = strrchr(path, '\\');
            if (!_stricmp(name ? name + 1 : path, "ModOrganizer.exe")) found = path;
        }
        CloseHandle(h);
        pid = parent;
    }
    CloseHandle(snap);
    return found;
}

void Mo2SetupStart(const char* pluginDir, Mo2SetupLog log)
{
    std::string dir = pluginDir;
    if (!dir.empty() && dir.back() != '\\') dir += '\\';
    std::string startMark = dir + "rowe_setup_start.txt", shortcutMark = dir + "rowe_setup_shortcut.txt";
    bool start = Mo2Exists(startMark), shortcut = Mo2Exists(shortcutMark);
    if (!start && !shortcut) return;
    std::string mo = Mo2Ancestor();
    if (mo.empty()) {
        log("mo2 setup: options chosen (start option %d, shortcut %d), but not running under Mod Organizer 2; left for later", start, shortcut);
        return;
    }
    std::string iniPath = Mo2IniFor(mo);
    Mo2IniText ini;
    if (iniPath.empty() || !Mo2ReadIni(iniPath, &ini)) {
        log("mo2 setup: ModOrganizer.ini not found for %s", mo.c_str());
        return;
    }
    // The helper as it really is in its mod folder (Explorer does not see the virtual Data folder).
    std::string helper = Mo2FindInMods(ini, iniPath, "NVSE\\Plugins\\rowe_mo2setup.exe");
    if (helper.empty()) {
        log("mo2 setup: rowe_mo2setup.exe not found in Mod Organizer's mods folder");
        return;
    }
    std::string request = Mo2Dir(helper) + "rowe_mo2setup_request.ini";
    WritePrivateProfileStringA("setup", "ini", iniPath.c_str(), request.c_str());
    WritePrivateProfileStringA("setup", "modorganizer", mo.c_str(), request.c_str());
    WritePrivateProfileStringA("setup", "start", start ? "1" : "0", request.c_str());
    WritePrivateProfileStringA("setup", "shortcut", shortcut ? "1" : "0", request.c_str());
    char windir[MAX_PATH];
    GetWindowsDirectoryA(windir, MAX_PATH);
    std::string cmd = std::string("\"") + windir + "\\explorer.exe\" \"" + helper + "\"";
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        log("mo2 setup: could not start Explorer (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DeleteFileA(startMark.c_str());
    DeleteFileA(shortcutMark.c_str());
    log("mo2 setup: started %s (start option %d, shortcut %d); it applies when Mod Organizer 2 closes (log: rowe_mo2setup.log in its "
        "logs folder)",
        helper.c_str(), start, shortcut);
}
