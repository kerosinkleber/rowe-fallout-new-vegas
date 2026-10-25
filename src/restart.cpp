// Restart (settings menu button): applies the settings that only take effect at start
// (supersampling, intro videos) without leaving the game by hand. With a game loaded it is saved
// as "rowe_restart" first; rowe_restart.txt next to the DLL then holds this process's id and
// the commands for the next start (load that save). The game is started again the way it was
// started the same way: FalloutNV.exe itself when it loads xNVSE on its own (the 4GB patcher's exe,
// which names nvse_steam_loader.dll), otherwise nvse_loader.exe (a plain exe started directly runs
// without xNVSE). A hidden PowerShell starts it
// first waits until this process has ended (the game refuses a second instance before any plugin
// loads), and this one quits. rowe_restart.txt is only used by a start within two minutes.
// Children of the game inherit Mod Organizer 2's virtual file system, so the restart stays in it.
#include <windows.h>
#include <stdio.h>
#include "fnvfov.h"

static const char* kSave = "rowe_restart";
static bool s_restarting;
static void RestartFile(char* out) { wsprintfA(out, "%srowe_restart.txt", ModDir()); }

// What starts the game with xNVSE: the exe itself if it loads xNVSE (the 4GB patcher's exe names
// nvse_steam_loader.dll), otherwise nvse_loader.exe next to it.
static void Launcher(char* out)
{
    GetModuleFileNameA(nullptr, out, MAX_PATH);
    bool selfLoads = false;
    HANDLE f = CreateFileA(out, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(f, nullptr), got = 0;
        char* data = size != INVALID_FILE_SIZE ? (char*)VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE) : nullptr;
        if (data && ReadFile(f, data, size, &got, nullptr)) {
            static const char kName[] = "nvse_steam_loader.dll";
            for (DWORD i = 0; i + sizeof(kName) - 1 <= got && !selfLoads; ++i)
                selfLoads = data[i] == 'n' && !_strnicmp(data + i, kName, sizeof(kName) - 1);
        }
        if (data) VirtualFree(data, 0, MEM_RELEASE);
        CloseHandle(f);
    }
    if (selfLoads) return;
    char* slash = strrchr(out, '\\');
    if (slash) lstrcpyA(slash + 1, "nvse_loader.exe");
}

bool RestartGameLoaded()
{
    BYTE* pl = *(BYTE**)0x011DEA3C;
    return pl && !IsBadReadPtr(pl + 0x40, 4) && *(void**)(pl + 0x40) != nullptr;
}

bool RestartPending() { return s_restarting; }

void RestartGame()
{
    if (s_restarting) return;
    s_restarting = true;
    bool save = RestartGameLoaded();
    Log("restart: requested (%s)", save ? "saving the game first" : "no game loaded");
    char line[128];
    if (save) {
        wsprintfA(line, "con save %s", kSave);
        GameCmdQueue(line);
        GameCmdQueue("wait 60");
    }
    GameCmdQueue(save ? "restartnow save" : "restartnow");
}

// Writes the file for the next start, starts the game again and queues the quit.
void RestartNow(bool saved)
{
    char path[MAX_PATH];
    RestartFile(path);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "pid %lu\n", GetCurrentProcessId());
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        fprintf(f, "time %llu\n", ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
        if (saved) fprintf(f, "waitmenu 1013 3600\nwait 60\ncon load %s\n", kSave);
        fclose(f);
    }
    char dir[MAX_PATH], launch[MAX_PATH];
    Launcher(launch);
    lstrcpyA(dir, launch);
    char* slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    // PowerShell single-quoted strings: ' doubled.
    char qLaunch[2 * MAX_PATH], qDir[2 * MAX_PATH];
    char* o = qLaunch;
    for (const char* c = launch; *c; ++c) { if (*c == '\'') *o++ = '\''; *o++ = *c; }
    *o = 0;
    o = qDir;
    for (const char* c = dir; *c; ++c) { if (*c == '\'') *o++ = '\''; *o++ = *c; }
    *o = 0;
    char cmd[6 * MAX_PATH];
    _snprintf(cmd, sizeof(cmd) - 1,
              "powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command \"Wait-Process -Id %lu -Timeout 120 "
              "-ErrorAction SilentlyContinue; Start-Process -FilePath '%s' -WorkingDirectory '%s'\"",
              GetCurrentProcessId(), qLaunch, qDir);
    cmd[sizeof(cmd) - 1] = 0;
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log("restart: starting %s again, quitting", launch);
        GameCmdQueue("con qqq");
    } else {
        Log("restart: could not start %s (%lu)", launch, GetLastError());
        DeleteFileA(path);
        s_restarting = false;
    }
}

// NVSEPlugin_Load of the new start: waits for the old process, then queues its commands.
void RestartAtLoad()
{
    char path[MAX_PATH];
    RestartFile(path);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    int queued = 0;
    char cmds[32][512];
    int ncmds = 0;
    bool stale = false;
    while (fgets(line, sizeof(line), f)) {
        char* e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        DWORD pid;
        if (sscanf(line, "pid %lu", &pid) == 1) {
            HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (old) {
                DWORD r = WaitForSingleObject(old, 60000);
                Log("restart: previous process %lu %s", pid, r == WAIT_OBJECT_0 ? "has ended" : "is still running (gave up waiting)");
                CloseHandle(old);
            }
            continue;
        }
        unsigned long long t;
        if (sscanf(line, "time %llu", &t) == 1) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            unsigned long long now = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            stale = now > t + 2ull * 60 * 10000000;  // 100 ns units
            continue;
        }
        if (line[0] && ncmds < 32) lstrcpynA(cmds[ncmds++], line, 512);
    }
    fclose(f);
    DeleteFileA(path);
    if (stale) {
        Log("restart: rowe_restart.txt is older than two minutes, ignored");
        return;
    }
    for (int i = 0; i < ncmds; ++i) { GameCmdQueue(cmds[i]); ++queued; }
    Log("restart: this start follows a restart (%d command(s) queued)", queued);
}
