// Mod Organizer 2 setup after an install with the installer's options (fomod: "make the ROWE
// launcher MO2's start option", "desktop shortcut"): each chosen option installs a marker file
// next to the DLL (rowe_setup_start.txt, rowe_setup_shortcut.txt). The first ROWE program that
// runs under Mod Organizer (the plugin in the game, or the launcher) hands the job to the
// launcher's setup mode (launcher/mo2helper.cpp), which waits until Mod Organizer closes and then
// edits its ini. The launcher is copied to the temp folder and started through Explorer, so it runs
// outside Mod Organizer's virtual file system (Mod Organizer would otherwise wait for it before
// closing). The markers are then removed, so this happens once per install.
// Built into both rowe.dll and rowe_launcher.exe (no fnvfov.h: the caller passes its log).
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <string>
#include "mo2setup.h"

static bool Exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

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

// The instance's ModOrganizer.ini: next to ModOrganizer.exe (portable), else the current instance
// under %LOCALAPPDATA%\ModOrganizer.
static std::string Mo2Ini(const std::string& moExe)
{
    std::string ini = moExe.substr(0, moExe.find_last_of('\\') + 1) + "ModOrganizer.ini";
    if (Exists(ini)) return ini;
    char inst[MAX_PATH] = "", local[MAX_PATH];
    DWORD size = sizeof(inst);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Mod Organizer Team\\Mod Organizer", "CurrentInstance", RRF_RT_REG_SZ, nullptr, inst,
                     &size) == ERROR_SUCCESS &&
        inst[0] && SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
        ini = std::string(local) + "\\ModOrganizer\\" + inst + "\\ModOrganizer.ini";
        if (Exists(ini)) return ini;
    }
    return "";
}

void Mo2SetupStart(const char* pluginDir, Mo2SetupLog log)
{
    std::string dir = pluginDir;
    if (!dir.empty() && dir.back() != '\\') dir += '\\';
    std::string startMark = dir + "rowe_setup_start.txt", shortcutMark = dir + "rowe_setup_shortcut.txt";
    bool start = Exists(startMark), shortcut = Exists(shortcutMark);
    if (!start && !shortcut) return;
    std::string mo = Mo2Ancestor();
    if (mo.empty()) {
        log("mo2 setup: options chosen (start option %d, shortcut %d), but not running under Mod Organizer 2; left for later", start, shortcut);
        return;
    }
    std::string ini = Mo2Ini(mo);
    if (ini.empty()) {
        log("mo2 setup: ModOrganizer.ini not found for %s", mo.c_str());
        return;
    }
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    std::string helper = std::string(temp) + "rowe_mo2setup.exe", link = std::string(temp) + "rowe_mo2setup.lnk";
    if (!CopyFileA((dir + "rowe_launcher.exe").c_str(), helper.c_str(), FALSE)) {
        log("mo2 setup: could not copy rowe_launcher.exe to %s (%lu)", helper.c_str(), GetLastError());
        return;
    }
    // A shortcut with the arguments, opened by Explorer: the helper becomes Explorer's process.
    std::string args = "--mo2-setup \"" + ini + "\" \"" + mo + "\"" + (start ? " start" : "") + (shortcut ? " shortcut" : "");
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkA* sl = nullptr;
    bool made = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkA, (void**)&sl))) {
        sl->SetPath(helper.c_str());
        sl->SetArguments(args.c_str());
        sl->SetShowCmd(SW_SHOWMINNOACTIVE);
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
            wchar_t w[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, link.c_str(), -1, w, MAX_PATH);
            made = SUCCEEDED(pf->Save(w, TRUE));
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    if (!made) {
        log("mo2 setup: could not write %s", link.c_str());
        return;
    }
    char windir[MAX_PATH];
    GetWindowsDirectoryA(windir, MAX_PATH);
    std::string cmd = std::string("\"") + windir + "\\explorer.exe\" \"" + link + "\"";
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
    log("mo2 setup: handed to %s %s; it applies when Mod Organizer 2 closes (log: rowe_mo2setup.log in its logs folder)", helper.c_str(),
        args.c_str());
}
