// Command channel for driving the game from tools without screenshots or focus tricks.
//
//  rowe_cmd.txt  one command per line; the file is read (and deleted) on the game thread and its
//                  lines are queued and run in order, one per main-loop frame:
//                    con <console line>    run a console line through xNVSE (e.g. con load "Save 1")
//                    key <key>             tap a key at the DirectInput layer (xNVSE TapKey)
//                    keydown / keyup <key> hold / release it (xNVSE HoldKey / ReleaseKey)
//                    wait <frames>         pause the queue for that many presented frames
//                    waitgameplay [frames] pause until gameplay (no menu, not loading), or the timeout
//                    waitfocus [frames]    pause until the game window is in front (input needs it)
//                    waitnomenu <id> [frames]  pause until that menu is closed (1007 = loading screen)
//                    waitmenu <id> [frames]    pause until that menu is open (1013 = main/pause menu)
//                    trace                 one-frame D3D9 trace (rowe_trace.txt)
//                    fovtrace <frames>     FOV setter trace (rowe_fovtrace.csv)
//                    log <text>            marker line in rowe.log
//                    shot / shotscene      screenshot of the finished frame / of the scene before the HUD
//                    shotmenu              screenshot including the settings menu
//                    usershot              screenshot as the screenshot key takes it (screenshotdir, size, format)
//                    photo                 photo mode on / off, as the photo shortcut
//                    overlay <action>      settings menu: open, close, up, down, left, right, home
//                    set <key> <value>     change a setting (fov, zoomfov, lens, pipboyfov, supersample, mousetrap, skipintros, scopelens)
//                    reload                re-read rowe.ini
//                    devicelost            the next Present reports a lost device, so the game resets it
//                    restart               as the menu's restart button: save, start again, load the save
//                    ifmenu <id> <command> run the command only if that menu is visible (1013 = pause menu)
//                    ifnomenu <id> <command> run the command only if that menu is not visible
//                    cursor <x> <y>        write the game's menu cursor fields (render pixels; does not move it)
//                    wincursor <x> <y>     move the Windows cursor to that client position
//                    consolepos <x> <y>    move the console text (applies at the next printed line)
//                    nodes <name> <depth>  log the interface scene below that node ("-": the root)
//                  <key> is a name (a-z, 0-9, f1-f12, tab, esc, enter, space, lalt, lshift, lctrl,
//                  lmb, rmb, mmb, up, down, left, right) or a DirectInput code (decimal; mouse 256+).
//  The queue state (pending lines, current wait, last command) is in rowe_state.txt.
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
static bool s_waitGameplay, s_waitFocus;
static int s_waitNoMenu;  // menu id to wait for to close
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

static int KeyCode(const char* name)
{
    static const struct { const char* name; int code; } kKeys[] = {
        {"esc", 1}, {"tab", 15}, {"enter", 28}, {"space", 57}, {"lctrl", 29}, {"lshift", 42}, {"lalt", 56},
        {"f1", 59}, {"f2", 60}, {"f3", 61}, {"f4", 62}, {"f5", 63}, {"f6", 64}, {"f7", 65}, {"f8", 66}, {"f9", 67},
        {"f10", 68}, {"f11", 87}, {"f12", 88}, {"up", 200}, {"left", 203}, {"right", 205}, {"down", 208},
        {"lmb", 256}, {"rmb", 257}, {"mmb", 258}, {"grave", 41},
        {"q", 16}, {"w", 17}, {"e", 18}, {"r", 19}, {"t", 20}, {"y", 21}, {"u", 22}, {"i", 23}, {"o", 24}, {"p", 25},
        {"a", 30}, {"s", 31}, {"d", 32}, {"f", 33}, {"g", 34}, {"h", 35}, {"j", 36}, {"k", 37}, {"l", 38},
        {"z", 44}, {"x", 45}, {"c", 46}, {"v", 47}, {"b", 48}, {"n", 49}, {"m", 50},
        {"1", 2}, {"2", 3}, {"3", 4}, {"4", 5}, {"5", 6}, {"6", 7}, {"7", 8}, {"8", 9}, {"9", 10}, {"0", 11},
    };
    for (auto& k : kKeys)
        if (lstrcmpiA(k.name, name) == 0) return k.code;
    char* end;
    long v = strtol(name, &end, 10);
    return *end == 0 && v > 0 && v < 264 ? (int)v : -1;
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

static void Key(const char* fn, const char* name)
{
    int code = KeyCode(name);
    if (code < 0) { Log("cmd: unknown key '%s'", name); return; }
    char line[64];
    wsprintfA(line, "%s %d", fn, code);
    RunConsole(line);
}

// The game reads input only while it is in front.
static bool HasFocus()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void Run(char* cmd);

static void SetCommand(char* arg)
{
    char* value = strchr(arg, ' ');
    if (!value) return;
    *value++ = 0;
    float v = (float)atof(value);
    if (!lstrcmpiA(arg, "fov")) g_settings.fov = v;
    else if (!lstrcmpiA(arg, "lens")) g_settings.lens = (int)v;
    else if (!lstrcmpiA(arg, "pipboyfov")) g_settings.pipboyFov = v;
    else if (!lstrcmpiA(arg, "zoomfov")) g_settings.zoomFov = v;
    else if (!lstrcmpiA(arg, "supersample")) g_settings.supersample = v;
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
    else if (lstrcmpiA(cmd, "key") == 0 && arg) Key("TapKey", arg);
    else if (lstrcmpiA(cmd, "keydown") == 0 && arg) Key("HoldKey", arg);
    else if (lstrcmpiA(cmd, "keyup") == 0 && arg) Key("ReleaseKey", arg);
    else if (lstrcmpiA(cmd, "wait") == 0 && arg) s_waitFrames = atoi(arg);
    else if (lstrcmpiA(cmd, "waitgameplay") == 0) {
        s_waitGameplay = true;
        s_waitFrames = arg ? atoi(arg) : 3600;
    }
    else if (lstrcmpiA(cmd, "waitmenu") == 0 && arg) {
        s_waitMenu = atoi(arg);
        char* t = strchr(arg, ' ');
        s_waitFrames = t ? atoi(t + 1) : 3600;
    }
    else if (lstrcmpiA(cmd, "waitnomenu") == 0 && arg) {
        s_waitNoMenu = atoi(arg);
        char* t = strchr(arg, ' ');
        s_waitFrames = t ? atoi(t + 1) : 3600;
    }
    else if (lstrcmpiA(cmd, "waitfocus") == 0) {
        s_waitFocus = true;
        s_waitFrames = arg ? atoi(arg) : 36000;
        if (!HasFocus()) Log("cmd: waiting for the game window to be in front (click into it)");
    }
    else if (lstrcmpiA(cmd, "trace") == 0) {
        char path[MAX_PATH];
        wsprintfA(path, "%srowe_trace.req", ModDir());
        FILE* f = fopen(path, "w");
        if (f) fclose(f);
        Log("cmd: frame trace requested");
    }
    else if (lstrcmpiA(cmd, "fovtrace") == 0) FovTraceStart(arg ? atoi(arg) : 300);
    else if (lstrcmpiA(cmd, "log") == 0) Log("cmd: ---- %s ----", arg ? arg : "");
    else if (lstrcmpiA(cmd, "shot") == 0) ScreenshotRequest(kShotFrame);
    else if (lstrcmpiA(cmd, "shotscene") == 0) ScreenshotRequest(kShotScene);
    else if (lstrcmpiA(cmd, "shotmenu") == 0) ScreenshotRequest(kShotWithMenu);
    else if (lstrcmpiA(cmd, "usershot") == 0) ScreenshotRequest(kShotUser);
    else if (lstrcmpiA(cmd, "photo") == 0) PhotoToggle();
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
    else if ((lstrcmpiA(cmd, "ifmenu") == 0 || lstrcmpiA(cmd, "ifnomenu") == 0) && arg) {
        bool wantOpen = lstrcmpiA(cmd, "ifmenu") == 0;
        int id = atoi(arg);
        char* rest = strchr(arg, ' ');
        bool open = id > 0 && id < 1200 && ((BYTE*)0x011F308F)[id];
        Log("cmd: %s %d: %s", cmd, id, open ? "open" : "not open");
        if (open == wantOpen && rest) Run(rest + 1);
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
            if (s_waitGameplay && InGameplay()) {
                Log("cmd: gameplay reached");
                s_waitFrames = 0;
            } else if (s_waitMenu && ((BYTE*)0x011F308F)[s_waitMenu]) {
                Log("cmd: menu %d open", s_waitMenu);
                s_waitFrames = 0;
            } else if (s_waitNoMenu && !((BYTE*)0x011F308F)[s_waitNoMenu]) {
                Log("cmd: menu %d closed", s_waitNoMenu);
                s_waitFrames = 0;
            } else if (s_waitFocus && HasFocus()) {
                Log("cmd: game window is in front");
                s_waitFrames = 0;
            } else {
                s_waitFrames = (int)step >= s_waitFrames ? 0 : s_waitFrames - (int)step;
                if (!s_waitFrames && (s_waitGameplay || s_waitFocus || s_waitNoMenu || s_waitMenu))
                    Log("cmd: %s timed out", s_waitGameplay ? "waitgameplay" : s_waitFocus ? "waitfocus" : s_waitMenu ? "waitmenu" : "waitnomenu");
            }
        }
        if (s_waitFrames > 0) return;
    }
    s_waitGameplay = s_waitFocus = false;
    s_waitNoMenu = s_waitMenu = 0;
    if (!s_count) return;
    char cmd[512];
    lstrcpyA(cmd, s_queue[s_head]);
    s_head = (s_head + 1) % 256;
    --s_count;
    Run(cmd);
}

void GameCmdWriteState(FILE* f)
{
    fprintf(f, "cmd queue: %d pending, wait %d%s, last \"%s\", in gameplay %d, focus %d\n", s_count, s_waitFrames,
            s_waitGameplay ? " (for gameplay)" : s_waitFocus ? " (for focus)" : "", s_last, InGameplay(), HasFocus());
}
