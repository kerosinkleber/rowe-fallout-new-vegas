// rowe_mo2setup.exe: Mod Organizer 2 setup from the installer's options (installed only with them).
// Started by the plugin through Explorer (mo2setup.cpp), it reads its job from
// rowe_mo2setup_request.ini next to itself (and removes it); for tests the job can be given as
// "rowe_mo2setup.exe <ModOrganizer.ini> <ModOrganizer.exe> [start] [shortcut]". No window. It
// waits until that Mod Organizer has closed (it keeps its executables and the selected one in
// memory and writes the selection back when it closes, so a change made while it runs would be
// lost), then:
// - start: makes the ROWE launcher MO2's start option: an entry under [customExecutables] (the one
//   whose binary is this mod's rowe_launcher.exe if there is one, else a new "ROWE Launcher") and
//   [Widgets] MainWindow_executablesListBox_index pointing at it (the list shows "<Edit...>" first,
//   then the entries that are not hidden);
// - shortcut: a desktop shortcut that starts that entry through Mod Organizer
//   (ModOrganizer.exe "moshortcut://<instance>:<title>", the instance empty for a portable one).
// ModOrganizer.ini is copied to ModOrganizer.ini.rowe-backup first. The steps are logged to
// rowe_mo2setup.log in the instance's logs folder.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include "mo2common.h"

static FILE* s_log;

static void Say(const char* fmt, ...)
{
    if (!s_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(s_log, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfprintf(s_log, fmt, a);
    va_end(a);
    fprintf(s_log, "\n");
    fflush(s_log);
}

static std::string Forward(std::string p)
{
    for (char& c : p)
        if (c == '\\') c = '/';
    return p;
}

// Is a ModOrganizer.exe with this image path running?
static bool Running(const std::string& exe)
{
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe = {sizeof(pe)};
    for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, "ModOrganizer.exe")) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        char path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameA(h, 0, path, &size) && Mo2Norm(path) == Mo2Norm(exe)) found = true;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

static bool CreateShortcut(const std::string& lnk, const std::string& target, const std::string& args, const std::string& workDir,
                           const std::string& icon, const std::string& description)
{
    IShellLinkA* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkA, (void**)&link))) return false;
    link->SetPath(target.c_str());
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    link->SetDescription(description.c_str());
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file))) {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, lnk.c_str(), -1, w, MAX_PATH);
        ok = SUCCEEDED(file->Save(w, TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    std::string iniPath, moExe;
    bool start = false, shortcut = false;
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string request = Mo2Dir(self) + "rowe_mo2setup_request.ini";
    if (__argc >= 3) {
        iniPath = __argv[1];
        moExe = __argv[2];
        for (int i = 3; i < __argc; ++i) {
            if (!_stricmp(__argv[i], "start")) start = true;
            if (!_stricmp(__argv[i], "shortcut")) shortcut = true;
        }
    } else if (Mo2Exists(request)) {
        char v[MAX_PATH];
        GetPrivateProfileStringA("setup", "ini", "", v, MAX_PATH, request.c_str());
        iniPath = v;
        GetPrivateProfileStringA("setup", "modorganizer", "", v, MAX_PATH, request.c_str());
        moExe = v;
        start = GetPrivateProfileIntA("setup", "start", 0, request.c_str()) != 0;
        shortcut = GetPrivateProfileIntA("setup", "shortcut", 0, request.c_str()) != 0;
        DeleteFileA(request.c_str());
    }
    if (iniPath.empty() || moExe.empty()) return 2;  // nothing asked
    std::string instanceDir = Mo2Dir(iniPath);
    std::string logDir = Mo2Exists(instanceDir + "logs") ? instanceDir + "logs\\" : instanceDir;
    s_log = fopen((logDir + "rowe_mo2setup.log").c_str(), "a");
    Say("ROWE Mod Organizer 2 setup: ini %s, Mod Organizer %s, start option %d, desktop shortcut %d", iniPath.c_str(), moExe.c_str(), start,
        shortcut);
    HANDLE once = CreateMutexA(nullptr, TRUE, "ROWE_MO2_SETUP");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Say("another setup is already waiting; this one ends");
        return 0;
    }
    if (Running(moExe)) {
        Say("waiting for Mod Organizer 2 to close (it would overwrite the change)");
        while (Running(moExe)) Sleep(2000);
        Sleep(1500);  // let it finish writing its ini
    }
    Mo2IniText ini;
    if (!Mo2ReadIni(iniPath, &ini)) {
        Say("cannot read %s", iniPath.c_str());
        return 1;
    }
    std::string launcher = Mo2FindInMods(ini, iniPath, "NVSE\\Plugins\\rowe_launcher.exe");
    if (launcher.empty()) {
        Say("rowe_launcher.exe not found in Mod Organizer's mods folder; nothing changed");
        return 1;
    }
    Say("the launcher: %s", launcher.c_str());

    // The executable entry: this launcher's, or a new one.
    int count = atoi(Mo2Get(ini, "customExecutables", "size").c_str());
    int mine = 0;
    std::string title;
    for (int i = 1; i <= count; ++i)
        if (Mo2Norm(Mo2Get(ini, "customExecutables", std::to_string(i) + "\\binary")) == Mo2Norm(launcher)) {
            mine = i;
            title = Mo2Get(ini, "customExecutables", std::to_string(i) + "\\title");
        }
    bool changed = false;
    if (!mine) {
        mine = count + 1;
        title = "ROWE Launcher";
        for (int i = 1; i <= count; ++i)
            if (Mo2Get(ini, "customExecutables", std::to_string(i) + "\\title") == title) title = "ROWE Launcher (ROWE)";
        std::string n = std::to_string(mine) + "\\";
        Mo2Set(&ini, "customExecutables", "size", std::to_string(mine));
        Mo2Set(&ini, "customExecutables", n + "arguments", "");
        Mo2Set(&ini, "customExecutables", n + "binary", Forward(launcher));
        Mo2Set(&ini, "customExecutables", n + "hide", "false");
        Mo2Set(&ini, "customExecutables", n + "ownicon", "true");
        Mo2Set(&ini, "customExecutables", n + "steamAppID", "");
        Mo2Set(&ini, "customExecutables", n + "title", title);
        Mo2Set(&ini, "customExecutables", n + "toolbar", "false");
        Mo2Set(&ini, "customExecutables", n + "workingDirectory", "");
        Say("added executable %d \"%s\"", mine, title.c_str());
        changed = true;
    } else {
        Say("executable %d \"%s\" already starts it", mine, title.c_str());
        if (Mo2Get(ini, "customExecutables", std::to_string(mine) + "\\hide") == "true") {
            Mo2Set(&ini, "customExecutables", std::to_string(mine) + "\\hide", "false");
            changed = true;
        }
    }
    if (start) {
        int index = 1;  // "<Edit...>" comes first
        for (int i = 1; i < mine; ++i)
            if (Mo2Get(ini, "customExecutables", std::to_string(i) + "\\hide") != "true") ++index;
        Mo2Set(&ini, "Widgets", "MainWindow_executablesListBox_index", std::to_string(index));
        Say("start option: list entry %d", index);
        changed = true;
    }
    if (changed) {
        std::string backup = iniPath + ".rowe-backup";
        if (!CopyFileA(iniPath.c_str(), backup.c_str(), FALSE)) Say("could not back up the ini; nothing changed");
        else if (!Mo2WriteIni(iniPath, ini)) Say("could not write %s (backup: %s)", iniPath.c_str(), backup.c_str());
        else Say("ModOrganizer.ini updated (backup: %s)", backup.c_str());
    }
    if (shortcut) {
        CoInitialize(nullptr);
        char desktop[MAX_PATH];
        std::string instance;
        if (Mo2Norm(Mo2Dir(iniPath)) != Mo2Norm(Mo2Dir(moExe))) {  // not portable: the instance's folder name
            instance = instanceDir.substr(0, instanceDir.size() - 1);
            instance = instance.substr(instance.find_last_of('\\') + 1);
        }
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
            std::string lnk = std::string(desktop) + "\\Fallout New Vegas (ROWE).lnk";
            bool ok = CreateShortcut(lnk, moExe, "\"moshortcut://" + instance + ":" + title + "\"", Mo2Dir(moExe), launcher,
                                     "Fallout: New Vegas through Mod Organizer 2, with the ROWE launcher");
            Say(ok ? "desktop shortcut %s" : "could not create the desktop shortcut %s", lnk.c_str());
        }
        CoUninitialize();
    }
    Say("done");
    if (s_log) fclose(s_log);
    if (once) CloseHandle(once);
    return 0;
}
