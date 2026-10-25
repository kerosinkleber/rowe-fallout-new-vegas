// Photo mode (the photo shortcut, default F10, also with Ctrl / Alt): the player flies with
// collision off ("tcl": the movement keys move them where the camera looks, up and down too),
// invisible (SetActorAlpha 0), undetectable (ToggleDetection), seen from the third person (a tap
// of the POV control if they were in the first; the invisible body leaves a free view), with the
// HUD hidden (ToggleMenus) if photohud=1 and people and creatures stopped (ToggleAI) if
// photofreeze=1. Because the player really moves, the game crosses cells and loads the world as
// when walking (a free camera, "tfc", leaves the player behind and nothing loads where it flies,
// and moving the player after it with SetPos leaves their cell behind and crashed the game).
// Holding Shift flies photofast times faster. The movement update (0x895110) asks the walk or run
// speed (0x884DC0 / 0x884EB0) each frame and moves the player by it, collision off too; detours on
// those two scale the player's result for just its six calls while Shift is held: three when it
// picks a new movement animation (0x896BFD, 0x896C0D, 0x896C6E), two for the one already playing
// (0x896F7A, 0x896F8A: taken after flying up or down, for example) and one more walk (0x896544). Nothing else changes speed: the movement settings, SpeedMult and the
// general GetMovementSpeed (0x8A0B10, which only xNVSE asks for the player) are not on that path.
// The game's Run control (Shift) is off while photo mode is on, or Shift would also switch to
// walking. Space flies up and Ctrl down (Jump and Sneak are off while it is on): see
// Hook_SetPosition. The mouse wheel zooms (dinput.cpp hands it over and keeps it from the game,
// fov.cpp applies PhotoZoomFov).
// The shortcut again ends it and puts the player back where photo mode started: there it leaves
// a marker (PlaceAtMe XMarkerHeading), and the player is moved to it (MoveTo) a moment after the
// collision is back on (a move while flying is lost). Opening a menu (the pause menu with its
// Save, the Pip-Boy), loading a game, the main menu and quitting end it as well, and Quicksave is
// off while it is on: a save would keep the invisible player.
// Everything is put back as it was: each toggle is toggled back only if photo mode toggled it
// (and the state is tracked, so an early end never toggles one twice), Quicksave and Run are
// (and Jump and Sneak) enabled only if they were, the speed scale goes back to 1, the marker is
// deleted.
// Starts only in gameplay; the console commands run on the game thread.
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include "fnvfov.h"

static bool s_active, s_keyDown;
static bool s_noclip, s_hudHidden, s_aiStopped, s_povSwitched, s_undetected, s_invisible, s_quicksaveOff, s_runOff, s_jumpOff,
    s_sneakOff;
static float s_startX, s_startY, s_startZ;  // where the player was when photo mode started
static float s_startRot;                    // their heading (degrees)
static DWORD s_startWorld;                  // their worldspace (0: an interior)
static void* s_marker;                      // the marker left there
static DWORD s_backAt;                      // when the player is to be moved back (0: not pending)
static int s_backTries;

bool PhotoActive() { return s_active; }

// The walk and run speed formulas (thiscall on the actor, the result in ST0).
typedef float(__fastcall* SpeedFn)(void* actor, void* edx);
static SpeedFn s_walkSpeed, s_runSpeed;
static float s_speedScale = 1.f;  // set and read on the game thread
static DWORD s_scaledCalls;

// The return addresses of the movement update's six walk / run speed calls (all store the speed it
// moves by, [ebp-0x50]).
static const DWORD kMoveSpeedCalls[] = {0x00896549, 0x00896C02, 0x00896C12, 0x00896C73, 0x00896F7F, 0x00896F8F};
static DWORD s_scaledBy[6];  // per call (state file)

static float Scaled(float v, void* actor, DWORD ret)
{
    if (s_speedScale == 1.f || actor != *(void**)0x011DEA3C) return v;
    for (int i = 0; i < 6; ++i)
        if (ret == kMoveSpeedCalls[i]) {
            ++s_scaledBy[i];
            ++s_scaledCalls;
            return v * s_speedScale;
        }
    return v;
}

static float __fastcall Hook_WalkSpeed(void* actor, void* edx) { return Scaled(s_walkSpeed(actor, edx), actor, (DWORD)_ReturnAddress()); }
static float __fastcall Hook_RunSpeed(void* actor, void* edx) { return Scaled(s_runSpeed(actor, edx), actor, (DWORD)_ReturnAddress()); }

// Up and down (Space / Ctrl): with collision off the player update puts the player each frame with
// PlayerCharacter::SetPosition (0x931620, thiscall, the position by pointer) from its noclip block
// (the call returning to 0x9427EF): the old position plus a vertical step. The detour adds the up /
// down step to that one call's height, at the flying speed (times photofast with Shift).
typedef void(__fastcall* SetPositionFn)(void* pc, void* edx, float* pos);
static SetPositionFn s_setPosition;
static int s_vertical;  // +1 up, -1 down, 0 (game thread)
static LARGE_INTEGER s_lastVertical;
static DWORD s_verticalCalls;
static const float kVerticalSpeed = 400.f;  // units per second, about the flying speed

static void __fastcall Hook_SetPosition(void* pc, void* edx, float* pos)
{
    if (s_vertical && (DWORD)_ReturnAddress() == 0x009427EF && pos && !IsBadReadPtr(pos, 12)) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        float dt = s_lastVertical.QuadPart ? (float)(now.QuadPart - s_lastVertical.QuadPart) / (float)freq.QuadPart : 0.f;
        s_lastVertical = now;
        if (dt > 0.1f) dt = 0.1f;  // after a pause or a hitch
        float step[3] = {pos[0], pos[1], pos[2] + s_vertical * kVerticalSpeed * s_speedScale * dt};
        ++s_verticalCalls;
        s_setPosition(pc, edx, step);
        return;
    }
    s_setPosition(pc, edx, pos);
}

// Wheel zoom: the shown world FOV's tan(fov / 2) times s_zoom, which eases (in log, a steady zoom)
// toward s_zoomTarget; a notch is 15 % closer or further, between 5 degrees and kFovMax. The lens
// works on the zoomed FOV as on any other. Back to 1 when photo mode ends.
static const float kPi = 3.14159265f;
static float s_zoom = 1.f, s_zoomTarget = 1.f;
static float s_zoomShown = 75.f, s_zoomFov = 75.f;  // the last FOV before / after the zoom
static LARGE_INTEGER s_zoomLast;

void PhotoWheel(long delta)
{
    if (!s_active) return;
    float base = tanf(s_zoomShown * kPi / 360.f);
    float lo = tanf(5.f * kPi / 360.f) / base, hi = tanf(kFovMax * kPi / 360.f) / base;
    float t = s_zoomTarget * powf(0.85f, delta / 120.f);
    s_zoomTarget = t < lo ? lo : t > hi ? hi : t;
}

float PhotoZoomFov(float fov)
{
    s_zoomShown = fov;
    if (s_zoom == 1.f) return s_zoomFov = fov;
    return s_zoomFov = atanf(tanf(fov * kPi / 360.f) * s_zoom) * 360.f / kPi;
}

static void ZoomEase()
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = s_zoomLast.QuadPart ? (float)(now.QuadPart - s_zoomLast.QuadPart) / (float)freq.QuadPart : 0.f;
    s_zoomLast = now;
    float a = dt * 10.f;
    if (a > 1.f) a = 1.f;
    s_zoom = expf(logf(s_zoom) + (logf(s_zoomTarget) - logf(s_zoom)) * a);
    if (fabsf(s_zoom - s_zoomTarget) < 1e-4f) s_zoom = s_zoomTarget;
}

void PhotoInstall()
{
    static const BYTE kSpeed[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};  // push ebp; mov ebp, esp; sub esp, 8
    s_walkSpeed = (SpeedFn)InstallDetour(0x00884DC0, kSpeed, sizeof(kSpeed), (void*)Hook_WalkSpeed, "walk speed (photo mode Shift)");
    s_runSpeed = (SpeedFn)InstallDetour(0x00884EB0, kSpeed, sizeof(kSpeed), (void*)Hook_RunSpeed, "run speed (photo mode Shift)");
    s_setPosition = (SetPositionFn)InstallDetour(0x00931620, kSpeed, sizeof(kSpeed), (void*)Hook_SetPosition,
                                                 "PlayerCharacter::SetPosition (photo mode up / down)");
}

static bool PlayerPos(float* x, float* y, float* z)
{
    BYTE* pl = *(BYTE**)0x011DEA3C;
    if (!pl || IsBadReadPtr(pl + 0x30, 12)) return false;
    *x = *(float*)(pl + 0x30);
    *y = *(float*)(pl + 0x34);
    *z = *(float*)(pl + 0x38);
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

// The player's cell form ID (+0x40, its ID at +0x0C).
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

// Without the marker: PositionWorld in an exterior (slow, sometimes lost), SetPos in an interior.
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

static void Enter()
{
    if (s_active || !InGameplay()) return;
    s_active = true;
    if (!PlayerPos(&s_startX, &s_startY, &s_startZ)) s_startX = s_startY = s_startZ = 0;
    BYTE* pl = *(BYTE**)0x011DEA3C;
    s_startRot = pl ? *(float*)(pl + 0x2C) * 57.29578f : 0.f;
    s_startWorld = WorldspaceId();
    s_backAt = 0;
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
    s_runOff = ScriptValue("IsControlDisabled 9") == 0;  // 9: Run (Shift: it would switch to walking)
    if (s_runOff) GameConsole("DisableControl 9");
    s_jumpOff = ScriptValue("IsControlDisabled 12") == 0;  // 12: Jump (Space flies up)
    if (s_jumpOff) GameConsole("DisableControl 12");
    s_sneakOff = ScriptValue("IsControlDisabled 8") == 0;  // 8: Sneak (Ctrl flies down)
    if (s_sneakOff) GameConsole("DisableControl 8");
    Log("photo: on at %.0f %.0f %.0f (worldspace %08lX, marker %p; people %s, HUD %s, Quicksave %s, Shift x%d%s)", s_startX, s_startY,
        s_startZ, s_startWorld, s_marker, s_aiStopped ? "stopped" : "moving", s_hudHidden ? "hidden" : "shown",
        s_quicksaveOff ? "turned off" : "was off", g_settings.photoFast, s_walkSpeed && s_runSpeed ? "" : ", speed detours missing");
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
    s_speedScale = 1.f;
    s_vertical = 0;
    s_zoom = s_zoomTarget = 1.f;
    float px = s_startX, py = s_startY, pz = s_startZ;
    PlayerPos(&px, &py, &pz);
    SetNoclip(false);
    if (s_povSwitched) GameConsole("TapControl 13");
    if (s_invisible) GameConsole("player.SetActorAlpha 1");
    if (s_undetected) GameConsole("tdetect");
    if (s_hudHidden) GameConsole("tm");
    if (s_aiStopped) GameConsole("tai");
    if (s_quicksaveOff) GameConsole("EnableControl 25");
    if (s_runOff) GameConsole("EnableControl 9");
    if (s_jumpOff) GameConsole("EnableControl 12");
    if (s_sneakOff) GameConsole("EnableControl 8");
    s_povSwitched = s_invisible = s_undetected = s_hudHidden = s_aiStopped = s_quicksaveOff = s_runOff = s_jumpOff = s_sneakOff = false;
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
    bool front = GameInFront();
    s_speedScale = front && (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? (float)g_settings.photoFast : 1.f;
    int vertical = !front ? 0 : (GetAsyncKeyState(VK_SPACE) & 0x8000) ? 1 : (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? -1 : 0;
    if (vertical != s_vertical) s_lastVertical.QuadPart = 0;  // the first step after a change has no time yet
    s_vertical = vertical;
    ZoomEase();
}

void PhotoWriteState(FILE* f)
{
    float px = 0, py = 0, pz = 0;
    PlayerPos(&px, &py, &pz);
    fprintf(f, "photo: %s, shortcut %x, fast x%d, freeze %d, hud %d; player %.1f %.1f %.1f, start %.1f %.1f %.1f (worldspace %08lX; now %08lX)\n",
            s_active ? "on" : "off", g_settings.photoKey, g_settings.photoFast, g_settings.photoFreeze, g_settings.photoHud, px, py, pz,
            s_startX, s_startY, s_startZ, s_startWorld, WorldspaceId());
    fprintf(f, "photo: cell %08lX; noclip %d, marker %p, move back %s (tries %d); speed detours %s, scale %.2f, scaled calls %lu; up/down %d (%s), %lu steps\n",
            CellId(), s_noclip, s_marker, s_backAt ? "pending" : "no", s_backTries, s_walkSpeed && s_runSpeed ? "installed" : "MISSING",
            s_speedScale, s_scaledCalls, s_vertical, s_setPosition ? "installed" : "MISSING", s_verticalCalls);
    fprintf(f, "photo: scaled per call %lu %lu %lu %lu %lu %lu\n", s_scaledBy[0], s_scaledBy[1], s_scaledBy[2], s_scaledBy[3], s_scaledBy[4],
            s_scaledBy[5]);
    fprintf(f, "photo: zoom %.3f (target %.3f), fov %.1f -> %.1f\n", s_zoom, s_zoomTarget, s_zoomShown, s_zoomFov);
}
