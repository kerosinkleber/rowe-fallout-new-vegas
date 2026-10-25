// rowe.ini next to the DLL, section [rowe]. Values are read once at load (and on request
// with the `reload` command) and written back whenever the settings menu changes one.
//
//   fov=0            0 = game default, otherwise 60-160 (horizontal FOV at 4:3, like the game's
//                    own fDefaultWorldFOV; 16:9 shows more)
//   lens=1           0 off, 1 auto, 2 Panini, 3 stereographic, 4 fisheye (equidistant, up to 160
//                    degrees across, black corners)
//   zoomfov=0        0 = game default (fIronSightsZoomDefault, 50, through the FOV remap), otherwise
//                    10-160 in the FOV slider's unit: where the zoom with the weapon holstered ends
//   pipboyfov=0      0 = game default (fPipboy1stPersonFOV, 47), otherwise 20-90: the Pip-Boy's FOV
//                    (lower = the Pip-Boy appears bigger)
//   supersample=1    pixel-count multiplier 1-64 (4 = twice the width and height, 64 = 8 times); next start
//   mousetrap=0      0 off, 1 gameplay only, 2 menus too
//   skipintros=1     1 skip the intro videos (next start)
//   fisheyecircle=0  fisheye: 1 shows all of the render as a circle fitted to the screen's shorter side
//                    (smaller, black around it), 0 fills the width (the circle's sides are cut off)
//   scopelens=1      1 the lens also bends the scope overlay (crosshair, mask), 0 the overlay is flat
//                    over the lensed world
//   screenshotdir=   folder for screenshots (created if missing; %VARIABLES% expanded, relative paths
//                    start at the game folder); empty = next to the DLL
//   screenshotkey=F12  key name (F1-F24, A-Z, 0-9, Home, End, Insert, Delete, PageUp, PageDown,
//                    Pause, ScrollLock, PrintScreen, Num0-Num9) or a virtual key code (0x7B)
//   menukey=F8       the key that opens and closes the settings menu (names as screenshotkey)
//   screenshotsize=full  full = the supersampled size the game renders at, window = the window's size
//   screenshotformat=jpg  jpg (quality below; small and fast) or png (lossless, about 5x larger, slower)
//   screenshotquality=95  JPEG quality 1-100
//   reshadefirst=1   with ReShade's add-on build: its effects are rendered into the supersampled frame
//                    (in screenshots, applied before the frame is averaged down); 0 = ReShade as usual
//   stats=0          1 shows the stats window (active lens, supersampling size), also with the menu closed
//   statssize=80     its text size, 50-200 percent of the menu's
//   statsx=-1        its position (top-left corner) as fractions of the screen, set by dragging it
//   statsy=-1        with the menu open; -1 = the top right corner
//   autosave=0       1 saves every autosaveminutes minutes of play (not in menus, loading or combat)
//   autosaveminutes=20  1-240
//   autosaveslots=3  the saves "ROWE Autosave 1"..N are used in turn (1-10); autosavenext= the next
//
// Section [dev] (testing only, FalloutPrefs.ini is not changed):
//   fullscreen=0/1   overrides the game's bFull Screen at start; -1 or absent leaves it alone
//   size=WxH         window size for the next start only (the key is removed once read)
//   vatsshrink=0     VATS's image-space textures at the game's (supersampled) size, for comparison
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "fnvfov.h"
#include "oldnames.h"

Settings g_settings;

static void Path(char* out) { wsprintfA(out, "%srowe.ini", ModDir()); }

static float ReadFloat(const char* key, float def)
{
    char path[MAX_PATH], buf[32], d[32];
    Path(path);
    sprintf(d, "%g", def);
    GetPrivateProfileStringA("rowe", key, d, buf, sizeof(buf), path);
    return (float)atof(buf);
}

static void ReadString(const char* key, const char* def, char* out, int size)
{
    char path[MAX_PATH];
    Path(path);
    GetPrivateProfileStringA("rowe", key, def, out, size, path);
}

void SettingsLoad()
{
    static bool tookOver;
    if (!tookOver && (tookOver = true, TakeOverOldIni(ModDir()))) Log("settings: rowe.ini written from fnvfov.ini (the mod's former name)");
    Settings s;
    s.fov = ReadFloat("fov", 0);
    if (s.fov != 0 && s.fov < kFovMin) s.fov = kFovMin;
    if (s.fov > kFovMax) s.fov = kFovMax;
    s.pipboyFov = ReadFloat("pipboyfov", 0);
    if (s.pipboyFov != 0 && s.pipboyFov < kPipboyFovMin) s.pipboyFov = kPipboyFovMin;
    if (s.pipboyFov > kPipboyFovMax) s.pipboyFov = kPipboyFovMax;
    s.zoomFov = ReadFloat("zoomfov", 0);
    if (s.zoomFov != 0 && s.zoomFov < kZoomFovMin) s.zoomFov = kZoomFovMin;
    if (s.zoomFov > kFovMax) s.zoomFov = kFovMax;
    s.lens = (int)ReadFloat("lens", 1);
    if (s.lens < 0 || s.lens >= kLensModeCount) s.lens = 1;
    s.supersample = ReadFloat("supersample", 1);
    if (s.supersample < 1) s.supersample = 1;
    if (s.supersample > 64) s.supersample = 64;
    s.mousetrap = (int)ReadFloat("mousetrap", 0);
    if (s.mousetrap < 0 || s.mousetrap >= kMouseTrapModeCount) s.mousetrap = 0;
    s.skipIntros = ReadFloat("skipintros", 1) != 0;
    s.scopeLens = ReadFloat("scopelens", 1) != 0;
    s.fisheyeCircle = ReadFloat("fisheyecircle", 0) != 0;
    ReadString("screenshotdir", "", s.screenshotDir, sizeof(s.screenshotDir));
    char buf[32];
    ReadString("screenshotkey", "F12", buf, sizeof(buf));
    s.screenshotKey = KeyFromName(buf);
    if (!s.screenshotKey) {
        Log("settings: unknown screenshotkey '%s', using F12", buf);
        s.screenshotKey = VK_F12;
    }
    ReadString("menukey", "F8", buf, sizeof(buf));
    s.menuKey = KeyFromName(buf);
    if (!s.menuKey) {
        Log("settings: unknown menukey '%s', using F8", buf);
        s.menuKey = VK_F8;
    }
    if (s.screenshotKey == s.menuKey) Log("settings: screenshotkey '%s' is also the settings menu key", buf);
    ReadString("screenshotsize", "full", buf, sizeof(buf));
    s.screenshotFull = lstrcmpiA(buf, "window") != 0;
    ReadString("screenshotformat", "jpg", buf, sizeof(buf));
    s.screenshotJpg = !lstrcmpiA(buf, "jpg") || !lstrcmpiA(buf, "jpeg");
    s.screenshotQuality = (int)ReadFloat("screenshotquality", 95);
    s.reshadeFirst = ReadFloat("reshadefirst", 1) != 0;
    s.stats = ReadFloat("stats", 0) != 0;
    s.statsSize = (int)ReadFloat("statssize", 80);
    if (s.statsSize < 50) s.statsSize = 50;
    if (s.statsSize > 200) s.statsSize = 200;
    s.statsX = ReadFloat("statsx", -1);
    s.statsY = ReadFloat("statsy", -1);
    if (s.statsX < 0 || s.statsY < 0 || s.statsX > 1 || s.statsY > 1) s.statsX = s.statsY = -1;
    s.autosave = ReadFloat("autosave", 0) != 0;
    s.autosaveMinutes = (int)ReadFloat("autosaveminutes", 20);
    if (s.autosaveMinutes < 1) s.autosaveMinutes = 1;
    if (s.autosaveMinutes > 240) s.autosaveMinutes = 240;
    s.autosaveSlots = (int)ReadFloat("autosaveslots", 3);
    if (s.autosaveSlots < 1) s.autosaveSlots = 1;
    if (s.autosaveSlots > 10) s.autosaveSlots = 10;
    s.autosaveNext = (int)ReadFloat("autosavenext", 1);
    if (s.autosaveNext < 1 || s.autosaveNext > s.autosaveSlots) s.autosaveNext = 1;
    if (s.screenshotQuality < 1) s.screenshotQuality = 1;
    if (s.screenshotQuality > 100) s.screenshotQuality = 100;
    char path[MAX_PATH];
    Path(path);
    s.devFullscreen = GetPrivateProfileIntA("dev", "fullscreen", -1, path);
    s.devVatsShrink = GetPrivateProfileIntA("dev", "vatsshrink", 1, path) != 0;
    char size[32] = "";
    GetPrivateProfileStringA("dev", "size", "", size, sizeof(size), path);
    if (size[0] && sscanf(size, "%dx%d", &s.devWidth, &s.devHeight) == 2 && s.devWidth >= 320 && s.devHeight >= 240) {
        WritePrivateProfileStringA("dev", "size", nullptr, path);  // one start only
        Log("settings: [dev] size %dx%d for this start", s.devWidth, s.devHeight);
    } else {
        s.devWidth = s.devHeight = 0;
    }
    g_settings = s;
    Log("settings: zoomfov %g", s.zoomFov);
    Log("settings: fov %g lens %d pipboyfov %g supersample %g mousetrap %d skipintros %d scopelens %d", s.fov, s.lens, s.pipboyFov,
        s.supersample, s.mousetrap, s.skipIntros, s.scopeLens);
}

void SettingsSave()
{
    char path[MAX_PATH], v[32];
    Path(path);
    sprintf(v, "%g", g_settings.fov);
    WritePrivateProfileStringA("rowe", "fov", v, path);
    sprintf(v, "%d", g_settings.lens);
    WritePrivateProfileStringA("rowe", "lens", v, path);
    WritePrivateProfileStringA("rowe", "lensstrength", nullptr, path);  // the old custom lens' strength
    WritePrivateProfileStringA("rowe", "fisheyecircle", g_settings.fisheyeCircle ? "1" : "0", path);
    sprintf(v, "%g", g_settings.zoomFov);
    WritePrivateProfileStringA("rowe", "zoomfov", v, path);
    sprintf(v, "%g", g_settings.pipboyFov);
    WritePrivateProfileStringA("rowe", "pipboyfov", v, path);
    sprintf(v, "%g", g_settings.supersample);
    WritePrivateProfileStringA("rowe", "supersample", v, path);
    sprintf(v, "%d", g_settings.mousetrap);
    WritePrivateProfileStringA("rowe", "mousetrap", v, path);
    WritePrivateProfileStringA("rowe", "skipintros", g_settings.skipIntros ? "1" : "0", path);
    WritePrivateProfileStringA("rowe", "scopelens", g_settings.scopeLens ? "1" : "0", path);
    WritePrivateProfileStringA("rowe", "screenshotdir", g_settings.screenshotDir, path);
    KeyName(g_settings.screenshotKey, v, sizeof(v));
    WritePrivateProfileStringA("rowe", "screenshotkey", v, path);
    KeyName(g_settings.menuKey, v, sizeof(v));
    WritePrivateProfileStringA("rowe", "menukey", v, path);
    WritePrivateProfileStringA("rowe", "screenshotsize", g_settings.screenshotFull ? "full" : "window", path);
    WritePrivateProfileStringA("rowe", "screenshotformat", g_settings.screenshotJpg ? "jpg" : "png", path);
    sprintf(v, "%d", g_settings.screenshotQuality);
    WritePrivateProfileStringA("rowe", "screenshotquality", v, path);
    WritePrivateProfileStringA("rowe", "reshadefirst", g_settings.reshadeFirst ? "1" : "0", path);
    WritePrivateProfileStringA("rowe", "stats", g_settings.stats ? "1" : "0", path);
    sprintf(v, "%d", g_settings.statsSize);
    WritePrivateProfileStringA("rowe", "statssize", v, path);
    sprintf(v, "%.4f", g_settings.statsX);
    WritePrivateProfileStringA("rowe", "statsx", v, path);
    sprintf(v, "%.4f", g_settings.statsY);
    WritePrivateProfileStringA("rowe", "statsy", v, path);
    WritePrivateProfileStringA("rowe", "autosave", g_settings.autosave ? "1" : "0", path);
    sprintf(v, "%d", g_settings.autosaveMinutes);
    WritePrivateProfileStringA("rowe", "autosaveminutes", v, path);
    sprintf(v, "%d", g_settings.autosaveSlots);
    WritePrivateProfileStringA("rowe", "autosaveslots", v, path);
    sprintf(v, "%d", g_settings.autosaveNext);
    WritePrivateProfileStringA("rowe", "autosavenext", v, path);
}
