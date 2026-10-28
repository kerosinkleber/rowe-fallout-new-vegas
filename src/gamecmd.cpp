// Command channel: ROWE's own command sequences, and a file for tools that start the game.
//
//  rowe_cmd.txt  one command per line; the file is read (and deleted) on the game thread and its
//                  lines are queued and run in order, one per main-loop frame. tools\continue.ps1 and
//                  tools\play.ps1 use it to load a save; the restart and the launcher queue the same
//                  lines (GameCmdQueue), the settings menu runs single lines (GameCmdNow).
//                    con <console line>    run a console line through xNVSE (e.g. con load "Save 1")
//                    wait <frames>         pause the queue for that many presented frames
//                    waitmenu <id> [frames]  pause until that menu is open (1013 = main/pause menu)
//                    trace                 one-frame D3D9 trace (rowe_trace.txt)
//                    fovtrace <frames>     FOV setter trace (rowe_fovtrace.csv)
//                    usershot              screenshot as the screenshot key takes it (screenshotdir, size, format)
//                    photo                 photo mode on / off, as the photo shortcut
//                    wheel <delta>         photo mode's wheel zoom, as the mouse wheel (120 per notch, + closer)
//                    overlay <action>      settings menu: open, close, tab <name> (View, Image, ...)
//                    set <key> <value>     change a setting and save rowe.ini (fov, zoomfov, lens, pipboyfov,
//                                          supersample, pixelsize, pixelhud, mousetrap, skipintros, scopelens)
//                    reload                re-read rowe.ini
//                    devicelost            the next Present reports a lost device, so the game resets it
//                    restart               as the menu's restart button: save, start again, load the save
//                    cursor <x> <y>        write the game's menu cursor fields (render pixels; does not move it)
//                    wincursor <x> <y>     move the Windows cursor to that client position
//                    consolepos <x> <y>    move the console text (applies at the next printed line)
//                    nodes <name> <depth>  log the interface scene below that node ("-": the root)
//
//  Tests drive the game through fnvbridge (B:\fnvbridge, see TESTING.md): its run_script plays the
//  scripts in tools\tests (key input, waits for gameplay or menus, screenshots, tile dumps, log
//  markers) and sends ROWE's own commands here through the rowe provider (bridge_provider.cpp).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fnvfov.h"

struct ConsoleInterface {
    UINT32 version;
    bool (*RunScriptLine)(const char* line, void* refr);
    bool (*RunScriptLine2)(const char* line, void* refr, bool suppressOutput);
};

static ConsoleInterface* s_console;
static char s_queue[256][512];
static int s_head, s_count;
static int s_waitFrames;
static int s_waitMenu;    // menu id to wait for to open
static char s_last[600];
static DWORD s_loops;

// Appends a line to the queue (the mod's own sequences, e.g. the restart).
void GameCmdQueue(const char* line)
{
    if (s_count == 256) { Log("cmd: queue full, dropped '%s'", line); return; }
    lstrcpynA(s_queue[(s_head + s_count++) % 256], line, 512);
}

// Runs a line on the next main-loop tick, ahead of the queue and its waits (the settings menu's buttons).
static char s_now[8][128];
static int s_nowCount;

void GameCmdNow(const char* line)
{
    if (s_nowCount == 8) { Log("cmd: immediate queue full, dropped '%s'", line); return; }
    lstrcpynA(s_now[s_nowCount++], line, 128);
}

void GameCmdInit(void* consoleInterface)
{
    s_console = (ConsoleInterface*)consoleInterface;
    Log("cmd: console interface %p (version %u)", s_console, s_console ? s_console->version : 0);
}

static bool RunConsole(const char* line)
{
    if (!s_console || !s_console->RunScriptLine) {
        Log("cmd: no console interface for '%s'", line);
        return false;
    }
    bool ok = s_console->RunScriptLine(line, nullptr);
    Log("cmd: con %s -> %s", line, ok ? "ok" : "FAILED");
    return ok;
}

bool GameConsole(const char* line) { return RunConsole(line); }

static void SetCommand(char* arg)
{
    char* value = strchr(arg, ' ');
    if (!value) return;
    *value++ = 0;
    float v = (float)atof(value);
    if (!lstrcmpiA(arg, "fov")) g_settings.fov = v;
    else if (!lstrcmpiA(arg, "lens")) g_settings.lens = (int)v == 1 ? kLensPanini : (int)v;  // (1: the former auto)
    else if (!lstrcmpiA(arg, "pipboyfov")) g_settings.pipboyFov = v;
    else if (!lstrcmpiA(arg, "zoomfov")) g_settings.zoomFov = v;
    else if (!lstrcmpiA(arg, "supersample")) g_settings.supersample = v;
    else if (!lstrcmpiA(arg, "pixelsize")) g_settings.pixelSize = (int)v;
    else if (!lstrcmpiA(arg, "pixelhud")) g_settings.pixelHud = v != 0;
    else if (!lstrcmpiA(arg, "mousetrap")) g_settings.mousetrap = (int)v;
    else if (!lstrcmpiA(arg, "skipintros")) g_settings.skipIntros = v != 0;
    else if (!lstrcmpiA(arg, "scopelens")) g_settings.scopeLens = v != 0;
    else if (!lstrcmpiA(arg, "fisheyecircle")) g_settings.fisheyeCircle = v != 0;
    else { Log("cmd: unknown setting '%s'", arg); return; }
    SettingsSave();
    SettingsLoad();  // clamps and logs
}

static void Run(char* cmd)
{
    lstrcpynA(s_last, cmd, sizeof(s_last));
    char* arg = strchr(cmd, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ') ++arg;
    }
    if (lstrcmpiA(cmd, "con") == 0 && arg) RunConsole(arg);
    else if (lstrcmpiA(cmd, "wait") == 0 && arg) s_waitFrames = atoi(arg);
    else if (lstrcmpiA(cmd, "waitmenu") == 0 && arg) {
        s_waitMenu = atoi(arg);
        char* t = strchr(arg, ' ');
        s_waitFrames = t ? atoi(t + 1) : 3600;
    }
    else if (lstrcmpiA(cmd, "trace") == 0) {
        char path[MAX_PATH];
        wsprintfA(path, "%srowe_trace.req", ModDir());
        FILE* f = fopen(path, "w");
        if (f) fclose(f);
        Log("cmd: frame trace requested");
    }
    else if (lstrcmpiA(cmd, "fovtrace") == 0) FovTraceStart(arg ? atoi(arg) : 300);
    else if (lstrcmpiA(cmd, "usershot") == 0) ScreenshotUserRequest();
    else if (lstrcmpiA(cmd, "photo") == 0) PhotoToggle();
    else if (lstrcmpiA(cmd, "wheel") == 0 && arg) PhotoWheel(atol(arg));
    else if (lstrcmpiA(cmd, "overlay") == 0 && arg) OverlayCommand(arg);
    else if (lstrcmpiA(cmd, "reload") == 0) SettingsLoad();
    else if (lstrcmpiA(cmd, "devicelost") == 0) D3DSimulateDeviceLost();
    else if (lstrcmpiA(cmd, "restart") == 0) RestartGame();
    else if (lstrcmpiA(cmd, "restartnow") == 0) RestartNow(arg && !lstrcmpiA(arg, "save"));
    else if (lstrcmpiA(cmd, "wincursor") == 0 && arg) {
        POINT p = {};
        if (sscanf(arg, "%ld %ld", &p.x, &p.y) == 2 && GameWindow()) {
            ClientToScreen(GameWindow(), &p);
            SetCursorPos(p.x, p.y);
        }
        Log("cmd: Windows cursor set to client %s", arg);
    }
    else if (lstrcmpiA(cmd, "cursor") == 0 && arg) {
        float x = 0, y = 0;
        if (sscanf(arg, "%f %f", &x, &y) == 2) DinputSetGameCursor(x, y);
        Log("cmd: game cursor set to %.1f %.1f", x, y);
    }
    else if (lstrcmpiA(cmd, "set") == 0 && arg) SetCommand(arg);
    else if (lstrcmpiA(cmd, "nodes") == 0 && arg) {
        // nodes <name> <depth>: logs the interface scene below that node ("-" for the root).
        char name[128] = "";
        int depth = 3;
        sscanf(arg, "%127s %d", name, &depth);
        UiDumpNodes(name, depth);
    }
    else if (lstrcmpiA(cmd, "consolepos") == 0 && arg) {
        // The console's text position (iConsoleTextXPos / YPos, copied into the console at +30h / +34h).
        BYTE* console = *(BYTE**)0x011D8CE8;
        int x = 0, y = 0;
        if (console && sscanf(arg, "%d %d", &x, &y) == 2) {
            Log("cmd: console text position %d %d -> %d %d", *(int*)(console + 0x30), *(int*)(console + 0x34), x, y);
            *(int*)(console + 0x30) = x;
            *(int*)(console + 0x34) = y;
        }
    }
    else Log("cmd: unknown command '%s'", cmd);
}

// Appends the lines of rowe_cmd.txt to the queue and deletes the file.
static void ReadFile()
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe_cmd.txt", ModDir());
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    int added = 0;
    while (fgets(line, sizeof(line), f)) {
        char* e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!line[0] || line[0] == '#') continue;
        if (s_count == 256) { Log("cmd: queue full, dropped '%s'", line); continue; }
        lstrcpyA(s_queue[(s_head + s_count++) % 256], line);
        ++added;
    }
    fclose(f);
    DeleteFileA(path);
    Log("cmd: queued %d command(s) from rowe_cmd.txt", added);
}

// Waits count presented frames (FrameCount), not main-loop iterations: on loading screens the
// loop can spin without presenting.
void GameCmdTick()
{
    ++s_loops;
    for (int i = 0; i < s_nowCount; ++i) Run(s_now[i]);
    s_nowCount = 0;
    if (s_loops % 10 == 0) ReadFile();
    if (s_waitFrames > 0) {
        DWORD now = FrameCount();
        static DWORD last;
        if (now != last) {
            DWORD step = now - last;
            last = now;
            if (s_waitMenu && ((BYTE*)0x011F308F)[s_waitMenu]) {
                Log("cmd: menu %d open", s_waitMenu);
                s_waitFrames = 0;
            } else {
                s_waitFrames = (int)step >= s_waitFrames ? 0 : s_waitFrames - (int)step;
                if (!s_waitFrames && s_waitMenu) Log("cmd: WARNING: waitmenu %d timed out", s_waitMenu);
            }
        }
        if (s_waitFrames > 0) return;
    }
    s_waitMenu = 0;
    if (!s_count) return;
    char cmd[512];
    lstrcpyA(cmd, s_queue[s_head]);
    s_head = (s_head + 1) % 256;
    --s_count;
    Run(cmd);
}

void GameCmdWriteState(FILE* f)
{
    fprintf(f, "cmd queue: %d pending, wait %d%s, last \"%s\", in gameplay %d, in front %d\n", s_count, s_waitFrames,
            s_waitMenu ? " (for a menu)" : "", s_last, InGameplay(), GameInFront());
}
