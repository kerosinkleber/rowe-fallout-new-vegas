// Photo mode (the photo shortcut, default F10, also with Ctrl / Alt): the player flies with
// collision off ("tcl": the movement keys move them where the camera looks, up and down too),
// invisible (SetActorAlpha 0), undetectable (ToggleDetection), seen from the third person (a tap
// of the POV control if they were in the first; the invisible body leaves a free view), with the
// HUD hidden (ToggleMenus) if photohud=1 and people and creatures stopped (ToggleAI) if
// photofreeze=1. Because the player really moves, the game crosses cells and loads the world as
// when walking (a free camera, "tfc", leaves the player behind and nothing loads where it flies,
// and moving the player after it with SetPos leaves their cell behind and crashed the game).
// The flying speed is fixed (the movement settings, SpeedMult, a faster game time and moving the
// player on with SetPos change nothing), so Shift dashes: while it is held the free camera flies
// photofast times its speed (fUFOCamSpeedMult:Controls), and on release the player is moved to
// the camera with the collision on for that moment (a move while flying is lost) and flies on from
// there, so the world loads there. The move is MoveTo the start marker plus the offset to the
// camera: the engine's own move, which puts the player into the right cell at once (PositionWorld
// took seconds or did not land at all, and SetPos leaves the cell behind).
// The shortcut again ends it and puts the player back where photo mode started: there it leaves
// a marker (PlaceAtMe XMarkerHeading), and the player is moved to it (MoveTo) a moment after the
// collision is back on. Opening a menu (the pause menu with its Save, the Pip-Boy), loading a game,
// the main menu and quitting end it as well, and Quicksave is off while it is on: a save would
// keep the invisible player.
// Everything is put back as it was: each toggle is toggled back only if photo mode toggled it
// (and the state is tracked, so a dash or an early end never toggles one twice), the camera speed
// setting gets its saved value, Quicksave is enabled only if it was, the marker is deleted.
// Starts only in gameplay; the console commands run on the game thread.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include "fnvfov.h"

static bool s_active, s_keyDown;
static bool s_noclip, s_hudHidden, s_aiStopped, s_povSwitched, s_undetected, s_invisible, s_quicksaveOff;
static float s_startX, s_startY, s_startZ;  // where the player was when photo mode started
static float s_startRot;                    // their heading (degrees)
static DWORD s_startWorld;                  // their worldspace (0: an interior)
static void* s_marker;                      // the marker left there
static DWORD s_backAt;                      // when the player is to be moved back (0: not pending)
static int s_backTries;

// The Shift dash: 1 the free camera flies, 2 the player is being moved to where it stopped.
static int s_dash;
static float s_dashX, s_dashY, s_dashZ, s_dashRot;
static DWORD s_dashWorld, s_dashAt, s_dashStart;
static bool s_dashSent;
static BYTE* s_camSpeed;  // fUFOCamSpeedMult:Controls
static float s_camSpeedSaved;

bool PhotoActive() { return s_active; }

static bool PlayerPos(float* x, float* y, float* z)
{
    BYTE* pl = *(BYTE**)0x011DEA3C;
    if (!pl || IsBadReadPtr(pl + 0x30, 12)) return false;
    *x = *(float*)(pl + 0x30);
    *y = *(float*)(pl + 0x34);
    *z = *(float*)(pl + 0x38);
    return true;
}

// The world camera (the scene graph's NiCamera): its world translate (+0x8C) and its heading in
// degrees from its view direction, the world rotation's first column (+0x68, +0x74, +0x80).
static bool CameraPos(float* x, float* y, float* z, float* heading)
{
    BYTE* sg = *(BYTE**)0x011DEB7C;
    BYTE* cam = sg && !IsBadReadPtr(sg + 0xAC, 4) ? *(BYTE**)(sg + 0xAC) : nullptr;
    if (!cam || IsBadReadPtr(cam + 0x68, 0x3C)) return false;
    *x = *(float*)(cam + 0x8C);
    *y = *(float*)(cam + 0x90);
    *z = *(float*)(cam + 0x94);
    float dx = *(float*)(cam + 0x68), dy = *(float*)(cam + 0x74);
    float h = atan2f(dx, dy) * 57.29578f;
    *heading = h < 0 ? h + 360.f : h;
    return true;
}

// The player's worldspace form ID in an exterior (cell +0xC0, a form of type 0x41), else 0.
static DWORD WorldspaceId()
{
    BYTE* pl = *(BYTE**)0x011DEA3C;
    BYTE* cell = pl && !IsBadReadPtr(pl + 0x40, 4) ? *(BYTE**)(pl + 0x40) : nullptr;
    BYTE* ws = cell && !IsBadReadPtr(cell + 0xC0, 4) ? *(BYTE**)(cell + 0xC0) : nullptr;
    if (!ws || IsBadReadPtr(ws, 0x10) || ws[4] != 0x41) return 0;
    return *(DWORD*)(ws + 0x0C);
}

// The player's cell form ID (+0x40, its ID at +0x0C), to see the moves land in the right one.
static DWORD CellId()
{
    BYTE* pl = *(BYTE**)0x011DEA3C;
    BYTE* cell = pl && !IsBadReadPtr(pl + 0x40, 4) ? *(BYTE**)(pl + 0x40) : nullptr;
    return cell && !IsBadReadPtr(cell + 0x0C, 4) ? *(DWORD*)(cell + 0x0C) : 0;
}

static void SetNoclip(bool on)
{
    if (on == s_noclip) return;
    GameConsole("tcl");
    s_noclip = on;
}

// Moves the player: PositionWorld in an exterior (it finds the cell for the position), SetPos in
// an interior (one cell).
static void MovePlayer(float x, float y, float z, float heading, DWORD world)
{
    char line[128];
    if (world) {
        _snprintf(line, sizeof(line) - 1, "player.PositionWorld %.1f %.1f %.1f %.1f %08lX", x, y, z, heading, world);
        line[sizeof(line) - 1] = 0;
        GameConsole(line);
        return;
    }
    const float v[] = {x, y, z};
    for (int i = 0; i < 3; ++i) {
        _snprintf(line, sizeof(line) - 1, "player.SetPos %c %.1f", "xyz"[i], v[i]);
        line[sizeof(line) - 1] = 0;
        GameConsole(line);
    }
}

static void DashStart()
{
    if (!s_camSpeed) s_camSpeed = FindSettingObject("fUFOCamSpeedMult:Controls");
    if (s_camSpeed) {
        s_camSpeedSaved = *(float*)(s_camSpeed + 4);
        *(float*)(s_camSpeed + 4) = s_camSpeedSaved * (float)g_settings.photoFast;
    }
    GameConsole("tfc");
    s_dash = 1;
    s_dashStart = GetTickCount();
}

// Ends the free camera and puts its speed back; true if the camera position was read.
static bool DashCameraOff()
{
    bool got = CameraPos(&s_dashX, &s_dashY, &s_dashZ, &s_dashRot);
    GameConsole("tfc");
    if (s_camSpeed) *(float*)(s_camSpeed + 4) = s_camSpeedSaved;
    return got;
}

static void DashRelease()
{
    if (!DashCameraOff()) {
        s_dash = 0;
        return;
    }
    s_dashZ -= 120.f;  // the camera is at about eye height
    s_dashWorld = WorldspaceId();
    SetNoclip(false);  // a move while flying is lost
    s_dash = 2;
    s_dashSent = false;
    s_dashAt = GetTickCount() + 150;
}

// After the release: sends the move, then waits until the player is there (or 10 s) to fly again.
static void DashMove()
{
    if (!s_dashSent) {
        // MoveTo the start marker plus the offset to the camera: the engine's own move, which lands
        // at once (PositionWorld took seconds or did not land at all).
        char line[96];
        _snprintf(line, sizeof(line) - 1, "player.MoveTo rArg %.1f %.1f %.1f", s_dashX - s_startX, s_dashY - s_startY, s_dashZ - s_startZ);
        line[sizeof(line) - 1] = 0;
        if (!(s_marker && ScriptWithRef(line, s_marker))) MovePlayer(s_dashX, s_dashY, s_dashZ, s_dashRot, s_dashWorld);
        s_dashSent = true;
        s_dashAt = GetTickCount() + 50;
        return;
    }
    float x, y, z;
    bool there = PlayerPos(&x, &y, &z) && (x - s_dashX) * (x - s_dashX) + (y - s_dashY) * (y - s_dashY) < 300.f * 300.f;
    DWORD ms = GetTickCount() - s_dashStart;
    if (!there && GetTickCount() - s_dashAt < 10000) return;
    if (there) {  // MoveTo turned them to the marker's heading: the way the camera looked instead
        char line[48];
        _snprintf(line, sizeof(line) - 1, "player.SetAngle z %.1f", s_dashRot);
        line[sizeof(line) - 1] = 0;
        GameConsole(line);
    }
    SetNoclip(true);
    s_dash = 0;
    Log("photo: dash %s %.0f %.0f %.0f (heading %.0f), %u ms after Shift went down; cell %08lX", there ? "to" : "did NOT reach", s_dashX,
        s_dashY, s_dashZ, s_dashRot, ms, CellId());
}

static void Enter()
{
    if (s_active || !InGameplay()) return;
    s_active = true;
    if (!PlayerPos(&s_startX, &s_startY, &s_startZ)) s_startX = s_startY = s_startZ = 0;
    BYTE* pl = *(BYTE**)0x011DEA3C;
    s_startRot = pl ? *(float*)(pl + 0x2C) * 57.29578f : 0.f;
    s_startWorld = WorldspaceId();
    s_backAt = 0;
    s_dash = 0;
    // An XMarkerHeading (0x34 in FalloutNV.esm) by its ID: editor IDs are not kept at runtime.
    s_marker = ScriptRef("let rBase := BuildRef 0 52\nlet rOut := player.PlaceAtMe rBase 1");
    SetNoclip(true);
    GameConsole("player.SetActorAlpha 0");
    s_invisible = true;
    GameConsole("tdetect");
    s_undetected = true;
    s_povSwitched = ScriptValue("IsPC1stPerson") != 0;
    if (s_povSwitched) GameConsole("TapControl 13");  // to the third person
    s_hudHidden = g_settings.photoHud;
    if (s_hudHidden) GameConsole("tm");
    s_aiStopped = g_settings.photoFreeze;
    if (s_aiStopped) GameConsole("tai");
    s_quicksaveOff = ScriptValue("IsControlDisabled 25") == 0;  // 25: Quicksave
    if (s_quicksaveOff) GameConsole("DisableControl 25");
    Log("photo: on at %.0f %.0f %.0f (worldspace %08lX, marker %p; people %s, HUD %s, Quicksave %s, Shift dash x%d)", s_startX,
        s_startY, s_startZ, s_startWorld, s_marker, s_aiStopped ? "stopped" : "moving", s_hudHidden ? "hidden" : "shown",
        s_quicksaveOff ? "turned off" : "was off", g_settings.photoFast);
}

void PhotoExit(const char* why)
{
    if (!s_active) {
        if (s_backAt) Log("photo: not moved back (%s)", why);
        s_backAt = 0;
        s_marker = nullptr;  // gone with the game that is left
        return;
    }
    s_active = false;
    if (s_dash == 1) DashCameraOff();
    s_dash = 0;
    float px = s_startX, py = s_startY, pz = s_startZ;
    PlayerPos(&px, &py, &pz);
    SetNoclip(false);
    if (s_povSwitched) GameConsole("TapControl 13");
    if (s_invisible) GameConsole("player.SetActorAlpha 1");
    if (s_undetected) GameConsole("tdetect");
    if (s_hudHidden) GameConsole("tm");
    if (s_aiStopped) GameConsole("tai");
    if (s_quicksaveOff) GameConsole("EnableControl 25");
    s_povSwitched = s_invisible = s_undetected = s_hudHidden = s_aiStopped = s_quicksaveOff = false;
    // A load, the main menu and quitting leave this game: there is nothing to move back.
    bool back = lstrcmpA(why, "a game is loaded") != 0 && lstrcmpA(why, "main menu") != 0 && lstrcmpA(why, "quit") != 0;
    if (back) {
        s_backAt = GetTickCount() + 150;
        s_backTries = 0;
    } else {
        s_marker = nullptr;
    }
    Log("photo: off (%s) at %.0f %.0f %.0f", why, px, py, pz);
}

void PhotoToggle()
{
    if (s_active) PhotoExit("toggled");
    else if (!s_backAt) Enter();
}

// Moves the player back to the marker (or, without one, to the start position), checks two seconds
// later that they got there, tries once more if not, and then removes the marker.
static void MoveBack()
{
    float x, y, z;
    bool there = PlayerPos(&x, &y, &z) && (x - s_startX) * (x - s_startX) + (y - s_startY) * (y - s_startY) < 200.f * 200.f;
    if (s_backTries > 0 && (there || s_backTries == 3)) {
        Log("photo: %s at %.0f %.0f %.0f (start %.0f %.0f %.0f)", there ? "back" : "NOT back", x, y, z, s_startX, s_startY, s_startZ);
        if (s_marker) ScriptWithRef("rArg.Disable\nrArg.MarkForDelete", s_marker);
        s_marker = nullptr;
        s_backAt = 0;
        return;
    }
    if (s_backTries == 2) {  // checked once more after the second move
        ++s_backTries;
        s_backAt = GetTickCount() + 2000;
        return;
    }
    ++s_backTries;
    if (!(s_marker && ScriptWithRef("player.MoveTo rArg", s_marker)))
        MovePlayer(s_startX, s_startY, s_startZ, s_startRot, s_startWorld);
    s_backAt = GetTickCount() + 2000;
}

// Main loop (game thread).
void PhotoTick()
{
    bool down = GameInFront() && !OverlayOpen() && ShortcutDown(g_settings.photoKey);
    if (down && !s_keyDown) PhotoToggle();
    s_keyDown = down;
    if (s_backAt && (int)(GetTickCount() - s_backAt) >= 0 && InGameplay()) MoveBack();
    if (!s_active) return;
    if (MenuOpen()) {
        PhotoExit("a menu opened");
        return;
    }
    bool shift = GameInFront() && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && g_settings.photoFast > 1;
    if (s_dash == 0 && shift && InGameplay()) DashStart();
    else if (s_dash == 1 && !shift) DashRelease();
    else if (s_dash == 2 && (int)(GetTickCount() - s_dashAt) >= 0) DashMove();
}

void PhotoWriteState(FILE* f)
{
    float px = 0, py = 0, pz = 0, cx = 0, cy = 0, cz = 0, ch = 0;
    PlayerPos(&px, &py, &pz);
    CameraPos(&cx, &cy, &cz, &ch);
    fprintf(f, "photo: %s, shortcut %x, dash x%d (%d), freeze %d, hud %d; player %.1f %.1f %.1f, start %.1f %.1f %.1f (worldspace %08lX; now %08lX)\n",
            s_active ? "on" : "off", g_settings.photoKey, g_settings.photoFast, s_dash, g_settings.photoFreeze, g_settings.photoHud, px,
            py, pz, s_startX, s_startY, s_startZ, s_startWorld, WorldspaceId());
    fprintf(f, "photo: cell %08lX, camera %.1f %.1f %.1f heading %.0f; noclip %d, marker %p, move back %s (tries %d), cam speed %.2f\n",
            CellId(), cx, cy, cz, ch, s_noclip, s_marker, s_backAt ? "pending" : "no", s_backTries, s_camSpeed ? *(float*)(s_camSpeed + 4) : -1.f);
}
