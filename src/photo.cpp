// Photo mode (the photo shortcut, default F10, also with Ctrl / Alt): the player flies with
// collision off ("tcl": the movement keys move them where the camera looks, up and down too),
// invisible (SetActorAlpha 0, and their 3D culled, which also hides the shadow: see HideBody),
// with the weapon holstered (drawn again at the end), undetectable (ToggleDetection), seen from
// the first person (a tap of the POV control if they were in the third: the camera turns around
// the eye; the first-person arms are culled too), with the HUD hidden (ToggleMenus) if photohud=1 and people and creatures stopped
// (ToggleAI) if photofreeze=1. Because the player really moves, the game crosses cells and loads
// the world as when walking (a free camera, "tfc", leaves the player behind and nothing loads where
// it flies, and moving the player after it with SetPos leaves their cell behind and crashed the game).
// Holding Shift flies photofast times faster, Space flies up and Ctrl down, at a steady speed: see
// Hook_SetPosition. Some of the game's controls are off while photo mode is on (kControlsOff: Run,
// Jump and Sneak, whose keys fly; the Pip-Boy, which would end it; attacking, aiming, drawing the
// weapon, VATS and the hotkeys, so the invisible player cannot fight). The mouse wheel zooms
// (dinput.cpp hands it over and keeps it from the game, fov.cpp applies PhotoZoomFov).
// The shortcut again ends it and puts the player back where photo mode started: there it leaves
// a marker (PlaceAtMe XMarkerHeading), and the player is moved to it (MoveTo) a moment after the
// collision is back on (a move while flying is lost). The pause menu (Esc) pauses it: it carries on
// where it was when the menu closes (the HUD shown while the menu is open, so the menu shows).
// Opening another menu (the Pip-Boy key is off), loading a game, the main menu and quitting end it. No game
// is saved while it is on (Hook_SaveGame: the pause menu's Save, Quicksave, autosaves, the console):
// a save would keep the invisible player in mid-air.
// Everything is put back as it was: each toggle is toggled back only if photo mode toggled it
// (and the state is tracked, so an early end never toggles one twice), each control is enabled
// again only if it was, the speed scale goes back to 1, the marker is deleted.
// Starts only in gameplay; the console commands run on the game thread.
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include "fnvfov.h"

static bool s_active, s_keyDown;
static bool s_bodyHidden;  // the player's 3D is culled by HideBody
static bool s_holstered;   // photo mode holstered the player's weapon
// Put back once the game is in gameplay again: photo mode may end in a menu (the Pip-Boy, which
// holsters the weapon itself; drawing it there crashed the game when the Pip-Boy closed), and the
// POV control does nothing in a menu.
static bool s_drawPending, s_povPending;
// The weapon is holstered and drawn again a moment after the POV switch, never in the same frame:
// both rebuild the player's animations (the MO2 game crashed in a background animation update
// 24 s after a photo mode whose end switched the POV and drew the weapon in one frame).
static const DWORD kWeaponDelayMs = 400;
static DWORD s_holsterAt, s_drawAt;  // when to holster (photo mode on) / draw again (off); 0: not pending
static bool s_noclip, s_hudHidden, s_aiStopped, s_povSwitched, s_undetected, s_invisible;
// The game's controls that are off while photo mode is on (DisableControl), and the keys photo mode
// flies with, hidden from the game and from script mods (DisableKey: a sprint mod sprinted on Shift
// and its first-person sprint animation shook the camera; photo mode reads them from Windows), each
// enabled again at the end only if photo mode turned it off.
static const struct { int control; const char* why; bool key; } kControlsOff[] = {
    {25, "Quicksave"},
    {9, "Run (Shift would also switch to walking)"},
    {12, "Jump (Space flies up)"},
    {8, "Sneak (Ctrl flies down)"},
    {14, "the Pip-Boy (it would end photo mode)"},
    {4, "Attack"},
    {6, "Aim"},
    {7, "Ready weapon"},
    {16, "VATS"},
    {17, "Hotkey 1 (it equips a weapon)"}, {18, "Ammo swap"}, {19, "Hotkey 3"}, {20, "Hotkey 4"},
    {21, "Hotkey 5"}, {22, "Hotkey 6"}, {23, "Hotkey 7"}, {24, "Hotkey 8"},
    {42, "Left Shift key (faster)", true}, {54, "Right Shift key", true}, {57, "Space key (up)", true},
    {29, "Left Ctrl key (down)", true}, {157, "Right Ctrl key", true},
};
static const int kControlsOffCount = sizeof(kControlsOff) / sizeof(kControlsOff[0]);
static bool s_controlOff[kControlsOffCount];  // photo mode turned it off
static float s_startX, s_startY, s_startZ;  // where the player was when photo mode started
static float s_startRot;                    // their heading (degrees)
static DWORD s_startWorld;                  // their worldspace (0: an interior)
static void* s_marker;                      // the marker left there
static DWORD s_backAt;                      // when the player is to be moved back (0: not pending)
static int s_backTries;
static bool s_paused;      // the pause menu is open over photo mode
static bool s_hudRehide;   // the HUD photo mode hid is shown while paused, hidden again after
static DWORD s_menuSince;  // a menu opened over photo mode then, not yet told whether the pause menu
static DWORD s_savesRefused;

bool PhotoActive() { return s_active; }
bool PhotoFlying() { return s_active && !s_paused; }

// No save while photo mode is on: the pause menu's Save, Quicksave, autosaves and the console's
// SaveGame all call BGSSaveLoadGame::SaveGame (0x8503B0, thiscall (name, -1, flag), ret 0xC).
typedef bool(__fastcall* SaveGameFn)(void* self, void* edx, const char* name, int slot, int flag);
static SaveGameFn s_saveGame;

static bool __fastcall Hook_SaveGame(void* self, void* edx, const char* name, int slot, int flag)
{
    if (!s_active) return s_saveGame(self, edx, name, slot, flag);
    ++s_savesRefused;
    Log("photo: save \"%s\" refused (photo mode is on)", name && !IsBadReadPtr(name, 1) ? name : "");
    OverlayNotice("Saving is off in photo mode");
    return false;
}

// Speed, up and down: with collision off the player update puts the player each frame with
// PlayerCharacter::SetPosition (0x931620, thiscall, the position by pointer) from its noclip block
// (the call returning to 0x9427EF). By then the game has already moved them by the frame's flight
// (the position passed is the current one, moved only in height), so the detour sees all of it,
// however it came about (animation, kNVSE, ...): it puts the player at the last position it set
// plus that movement times photofast (Shift), plus the up / down step (Space / Ctrl, about the
// flying speed, times photofast with Shift). The movement settings, SpeedMult and the game's walk
// / run speed are no lever: the first two do not touch flying, and the walk / run speed (scaled at
// the movement update's calls 0x896BFD ...) only moved the player in the vanilla animation setup.
// The game's step is uneven (at 60 fps 5.4, 5.1, 5.4, 5.1, 3.4 units in turn, and now and then a
// frame without one and a double one after): the camera, fixed to the player, jerked forward and
// up and down, times photofast with Shift. So the detour flies with its own steady velocity: the
// game's steps averaged over kSmoothS (a velocity eased toward step / frame time), times the real
// frame time; the speed factor eases too (Shift).
typedef void(__fastcall* SetPositionFn)(void* pc, void* edx, float* pos);
static SetPositionFn s_setPosition;
static float s_speedScale = 1.f;  // set and read on the game thread
static int s_vertical;            // +1 up, -1 down, 0
static float s_last[3];           // the position this call set last
static bool s_haveLast;
static LARGE_INTEGER s_lastTime;
static float s_vel[3];            // the game's flight, averaged (units per second)
static float s_scaleShown = 1.f;  // s_speedScale, eased
static DWORD s_noclipCalls, s_scaledCalls;
static const float kVerticalSpeed = 400.f;  // units per second, about the flying speed
static const float kSmoothS = 0.08f;        // the averaging time (a 5-frame pattern at 60 fps)

static void __fastcall Hook_SetPosition(void* pc, void* edx, float* pos)
{
    if (!s_active || (DWORD)_ReturnAddress() != 0x009427EF || !pos || IsBadReadPtr(pos, 12)) {
        s_setPosition(pc, edx, pos);
        return;
    }
    ++s_noclipCalls;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = s_lastTime.QuadPart ? (float)(now.QuadPart - s_lastTime.QuadPart) / (float)freq.QuadPart : 0.f;
    s_lastTime = now;
    float out[3] = {pos[0], pos[1], pos[2]};
    float d[3] = {pos[0] - s_last[0], pos[1] - s_last[1], pos[2] - s_last[2]};
    // A pause, a hitch or a jump that is not flying (a move by a script): start over from here.
    bool flying = s_haveLast && dt < 0.25f && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1000.f * 1000.f;
    if (flying) {
        // (k * d / dt is about d / kSmoothS whatever dt is: a frame after a very short one adds no spike)
        float k = 1.f - expf(-dt / kSmoothS);
        s_scaleShown += (s_speedScale - s_scaleShown) * (1.f - expf(-dt / 0.15f));
        for (int i = 0; i < 3; ++i) {
            if (dt > 0) s_vel[i] += (d[i] / dt - s_vel[i]) * k;
            out[i] = s_last[i] + s_vel[i] * s_scaleShown * dt;
        }
        if (s_speedScale != 1.f) ++s_scaledCalls;
    } else {
        s_vel[0] = s_vel[1] = s_vel[2] = 0;
        s_scaleShown = s_speedScale;
    }
    if (flying && s_vertical) out[2] += s_vertical * kVerticalSpeed * s_scaleShown * (dt > 0.1f ? 0.1f : dt);
    s_setPosition(pc, edx, out);
    memcpy(s_last, out, sizeof(out));
    s_haveLast = true;
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

float PhotoZoomedFov(float* unzoomed)
{
    *unzoomed = s_zoomShown;
    return s_zoomFov;
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

// The hidden player, again right before the shadow maps and the first-person model are drawn
// (Render's three calls each of RenderShadowMaps 0x871290 and RenderFirstPerson 0x875110): the game
// clears the culled bits every frame, and set only in the main loop or at the start of Render a bit
// was clear again when the shadows were drawn.
static void HideBody(bool hide);
static DWORD s_drawCalls;
static int s_drawSites;

static void BeforeDraw()
{
    ++s_drawCalls;
    if (s_active) HideBody(true);
}

// pushad; pushfd; call fn; popfd; popad; jmp [target]
static void* MakeThunk(void* fn, void* target)
{
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    BYTE* p = t;
    *p++ = 0x60;
    *p++ = 0x9C;
    *p++ = 0xE8; *(int*)p = (int)((BYTE*)fn - (p + 4)); p += 4;
    *p++ = 0x9D;
    *p++ = 0x61;
    void** slot = (void**)(t + 48);
    *slot = target;
    *p++ = 0xFF; *p++ = 0x25; *(void***)p = slot;
    return t;
}

void PhotoInstall()
{
    static const BYTE kStart[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};  // push ebp; mov ebp, esp; sub esp, 8
    s_setPosition = (SetPositionFn)InstallDetour(0x00931620, kStart, sizeof(kStart), (void*)Hook_SetPosition,
                                                 "PlayerCharacter::SetPosition (photo mode speed, up / down)");
    static const BYTE kSaveStart[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x44, 0x03, 0x00, 0x00};  // push ebp; mov ebp, esp; sub esp, 344h
    s_saveGame = (SaveGameFn)InstallDetour(0x008503B0, kSaveStart, sizeof(kSaveStart), (void*)Hook_SaveGame,
                                           "BGSSaveLoadGame::SaveGame (no save in photo mode)");
    static const struct { DWORD va, target; const char* name; } kDrawSites[] = {
        {0x00870851, 0x00871290, "RenderShadowMaps call (photo mode: no player shadow)"},
        {0x00870A74, 0x00871290, "RenderShadowMaps call (photo mode: no player shadow)"},
        {0x00870C3C, 0x00871290, "RenderShadowMaps call (photo mode: no player shadow)"},
        {0x0087093D, 0x00875110, "RenderFirstPerson call (photo mode: no arms)"},
        {0x00870B21, 0x00875110, "RenderFirstPerson call (photo mode: no arms)"},
        {0x00870F74, 0x00875110, "RenderFirstPerson call (photo mode: no arms)"},
    };
    for (const auto& s : kDrawSites) {
        BYTE* site = (BYTE*)s.va;
        if (site[0] != 0xE8) {
            PatchCall(s.va, s.target, nullptr, s.name);  // logs the skip
            continue;
        }
        void* thunk = MakeThunk((void*)BeforeDraw, (void*)(s.va + 5 + *(int*)(site + 1)));
        if (thunk && PatchCall(s.va, s.target, thunk, s.name)) ++s_drawSites;
    }
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

// The player's 3D: the third-person body (TESObjectREFR::renderState +0x64, its NiNode +0x14) or
// the first-person arms and weapon (PlayerCharacter +0x694), or null.
static BYTE* PlayerNode(bool firstPerson)
{
    BYTE* pl = *(BYTE**)0x011DEA3C;
    BYTE* node = nullptr;
    if (firstPerson) {
        node = pl && !IsBadReadPtr(pl + 0x694, 4) ? *(BYTE**)(pl + 0x694) : nullptr;
    } else {
        BYTE* rs = pl && !IsBadReadPtr(pl + 0x64, 4) ? *(BYTE**)(pl + 0x64) : nullptr;
        node = rs && !IsBadReadPtr(rs + 0x14, 4) ? *(BYTE**)(rs + 0x14) : nullptr;
    }
    return node && !IsBadReadPtr(node, 0x34) ? node : nullptr;
}

// Culls the player's 3D, both models (NiAVObject flags +0x30, bit 0 "app culled"), or puts each
// bit back as it was before (in the first person the game keeps the body culled itself).
// SetActorAlpha 0 fades the body but not its shadow (nor a drawn weapon's); a culled node draws
// nothing, those included. Set again every frame while photo mode is on, and right before the
// shadow maps and the first-person pass: the game clears the bits each frame.
static BYTE* s_hidNode[2];  // [0] body, [1] first-person model, as culled
static DWORD s_hidWas[2];   // their bit 0 before

static void HideBody(bool hide)
{
    for (int i = 0; i < 2; ++i) {
        BYTE* node = PlayerNode(i == 1);
        if (!node) continue;
        DWORD* flags = (DWORD*)(node + 0x30);
        if (hide) {
            if (s_hidNode[i] != node) {
                s_hidNode[i] = node;
                s_hidWas[i] = *flags & 1;
                const char* name = !IsBadReadPtr(node + 8, 4) && *(char**)(node + 8) && !IsBadReadPtr(*(char**)(node + 8), 1)
                                       ? *(char**)(node + 8) : "?";
                Log("photo: player %s %p '%.40s' flags %08lX culled", i ? "first-person model" : "body", node, name, *flags);
            }
            *flags |= 1;
        } else if (s_hidNode[i] == node) {
            *flags = (*flags & ~1u) | s_hidWas[i];
        }
    }
    if (!hide) s_hidNode[0] = s_hidNode[1] = nullptr;
    s_bodyHidden = hide;
}

// Draws or holsters the player's weapon with its animation (Actor 0x8A6840, thiscall (bool out),
// as JIP LN's SetWeaponOut).
static void SetWeaponOut(bool out)
{
    void* pl = *(void**)0x011DEA3C;
    if (!pl) return;
    typedef void(__thiscall * SetWeaponOutFn)(void*, bool);
    ((SetWeaponOutFn)0x008A6840)(pl, out);
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

// Photo mode starts kStartDelayFrames after its start marker is placed, never in the same frame: in
// the user's MO2 list (TTW) placing it in the frame photo mode started crashed the game 3 times in
// about 5 starts (a background thread walking nearby creatures' 3D, the new marker's address in a
// register), while placing it alone or photo mode without it did not crash (8 tries each).
static const DWORD kStartDelayFrames = 10;
static DWORD s_startAt;  // the frame photo mode starts at (its marker placed); 0: no start pending
static void Enter();

// Remembers where photo mode starts and places the marker there; Enter follows a few frames later.
static void Prepare()
{
    if (s_active || s_startAt || !InGameplay()) return;
    if (s_backAt) {  // started again before the player was moved back: the same start and marker
        Enter();
        return;
    }
    if (!PlayerPos(&s_startX, &s_startY, &s_startZ)) s_startX = s_startY = s_startZ = 0;
    BYTE* pl = *(BYTE**)0x011DEA3C;
    s_startRot = pl ? *(float*)(pl + 0x2C) * 57.29578f : 0.f;
    s_startWorld = WorldspaceId();
    // An XMarkerHeading (0x34 in FalloutNV.esm) by its ID: editor IDs are not kept at runtime.
    s_marker = g_settings.devPhotoMarker ? ScriptRef("let rBase := BuildRef 0 52\nlet rOut := player.PlaceAtMe rBase 1") : nullptr;
    s_startAt = FrameCount() + kStartDelayFrames;
    Log("photo: start marker %p placed at %.0f %.0f %.0f, photo mode starts in %lu frames", s_marker, s_startX, s_startY, s_startZ,
        kStartDelayFrames);
}

static void Enter()
{
    if (s_active || !InGameplay()) return;
    s_active = true;
    s_haveLast = false;
    s_lastTime.QuadPart = 0;
    // Started again before the player was moved back: it carries on from the same start (and
    // marker), and what was still to be put back is put back at its end.
    bool resume = s_backAt != 0;
    s_backAt = 0;
    SetNoclip(true);
    GameConsole("player.SetActorAlpha 0");
    s_invisible = true;
    GameConsole("tdetect");
    s_undetected = true;
    bool third = ScriptValue("IsPC1stPerson") == 0;
    if (third) GameConsole("TapControl 13");  // to the first person
    s_povSwitched = third || s_povPending;
    HideBody(true);
    bool out = *(void**)0x011DEA3C && PlayerWeaponOut();
    s_holsterAt = out ? GetTickCount() + kWeaponDelayMs : 0;  // (PhotoTick)
    s_holstered = s_drawPending || s_drawAt;  // still holstered from the last time
    s_povPending = s_drawPending = false;
    s_drawAt = 0;
    s_hudHidden = g_settings.photoHud;
    if (s_hudHidden) GameConsole("tm");
    s_aiStopped = g_settings.photoFreeze;
    if (s_aiStopped) GameConsole("tai");
    int turnedOff = 0;
    for (int i = 0; i < kControlsOffCount; ++i) {
        char line[48];
        const char* what = kControlsOff[i].key ? "Key" : "Control";
        wsprintfA(line, "Is%sDisabled %d", what, kControlsOff[i].control);
        s_controlOff[i] = ScriptValue(line) == 0;
        if (!s_controlOff[i]) continue;
        wsprintfA(line, "Disable%s %d", what, kControlsOff[i].control);
        GameConsole(line);
        ++turnedOff;
    }
    Log("photo: on%s at %.0f %.0f %.0f (worldspace %08lX, marker %p; people %s, HUD %s, %d of %d controls and keys turned off, Shift x%d%s%s)",
        resume ? " again" : "", s_startX, s_startY, s_startZ, s_startWorld, s_marker, s_aiStopped ? "stopped" : "moving",
        s_hudHidden ? "hidden" : "shown", turnedOff, kControlsOffCount, g_settings.photoFast, s_holsterAt ? ", weapon to holster" : s_holstered ? ", weapon holstered" : "",
        s_setPosition ? "" : ", SetPosition detour missing: no Shift, Space, Ctrl");
}

void PhotoExit(const char* why)
{
    if (!s_active) {
        if (s_backAt) Log("photo: not moved back (%s)", why);
        if (s_startAt) Log("photo: not started (%s)", why);
        s_backAt = s_startAt = 0;
        s_drawPending = s_povPending = false;
        s_drawAt = 0;
        s_marker = nullptr;  // gone with the game that is left
        return;
    }
    s_active = false;
    s_paused = s_hudRehide = false;  // (the HUD is shown while paused: s_hudHidden is clear then)
    s_menuSince = 0;
    s_speedScale = 1.f;
    s_vertical = 0;
    s_zoom = s_zoomTarget = 1.f;
    float px = s_startX, py = s_startY, pz = s_startZ;
    PlayerPos(&px, &py, &pz);
    SetNoclip(false);
    // A load, the main menu and quitting leave this game: its 3D may be gone.
    bool back = lstrcmpA(why, "a game is loaded") != 0 && lstrcmpA(why, "main menu") != 0 && lstrcmpA(why, "quit") != 0;
    if (back && s_bodyHidden) HideBody(false);
    s_bodyHidden = false;
    s_drawPending = back && s_holstered;  // (in gameplay: see PutBackPending)
    s_povPending = back && s_povSwitched;
    s_holstered = false;
    s_holsterAt = 0;  // not holstered yet: nothing to draw again
    if (s_invisible) GameConsole("player.SetActorAlpha 1");
    if (s_undetected) GameConsole("tdetect");
    if (s_hudHidden) GameConsole("tm");
    if (s_aiStopped) GameConsole("tai");
    for (int i = 0; i < kControlsOffCount; ++i) {
        if (!s_controlOff[i]) continue;
        char line[48];
        wsprintfA(line, "Enable%s %d", kControlsOff[i].key ? "Key" : "Control", kControlsOff[i].control);
        GameConsole(line);
        s_controlOff[i] = false;
    }
    s_povSwitched = s_invisible = s_undetected = s_hudHidden = s_aiStopped = false;
    if (back) {  // (else there is nothing to move back)
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
    else Prepare();  // (a second press while a start is pending does nothing)
}

// Moves the player back to the marker (or, without one, to the start position), checks two seconds
// later that they got there, tries once more if not, and then removes the marker.
static void MoveBack()
{
    float x, y, z;
    bool there = PlayerPos(&x, &y, &z) && (x - s_startX) * (x - s_startX) + (y - s_startY) * (y - s_startY) < 64.f * 64.f;
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

// Back in gameplay after photo mode: the POV back to the third person; the weapon is drawn again
// a moment later (PhotoTick).
static void PutBackPending()
{
    if (s_povPending) GameConsole("TapControl 13");
    if (s_drawPending) s_drawAt = GetTickCount() + kWeaponDelayMs;
    Log("photo: put back%s%s", s_povPending ? ", third person" : "", s_drawPending ? ", weapon to draw" : "");
    s_povPending = s_drawPending = false;
}

static bool Due(DWORD at) { return at && (int)(GetTickCount() - at) >= 0; }

// The world camera's height over the player (world translate +0x8C), its range per state write.
static float s_camLo = 1e9f, s_camHi = -1e9f;
static void CameraRange()
{
    BYTE* sg = *(BYTE**)0x011DEB7C;
    BYTE* cam = sg && !IsBadReadPtr(sg + 0xAC, 4) ? *(BYTE**)(sg + 0xAC) : nullptr;
    float x, y, z;
    if (!cam || IsBadReadPtr(cam + 0x8C, 12) || !PlayerPos(&x, &y, &z)) return;
    float h = *(float*)(cam + 0x94) - z;
    if (h < s_camLo) s_camLo = h;
    if (h > s_camHi) s_camHi = h;
}

// Main loop (game thread).
void PhotoTick()
{
    CameraRange();
    bool down = GameInFront() && !OverlayOpen() && ShortcutDown(g_settings.photoKey);
    if (down && !s_keyDown) PhotoToggle();
    s_keyDown = down;
    if (s_startAt && (int)(FrameCount() - s_startAt) >= 0) {
        s_startAt = 0;
        if (InGameplay()) Enter();
        else {  // a menu opened meanwhile: no photo mode, the marker removed again
            Log("photo: not started (left gameplay)");
            if (s_marker) ScriptWithRef("rArg.Disable\nrArg.MarkForDelete", s_marker);
            s_marker = nullptr;
        }
    }
    if (!s_active && (s_povPending || s_drawPending) && InGameplay()) PutBackPending();
    if (!s_active && Due(s_drawAt) && InGameplay()) {
        s_drawAt = 0;
        bool out = PlayerWeaponOut();
        if (!out) SetWeaponOut(true);
        Log("photo: weapon %s", out ? "already drawn" : "drawn again");
    }
    if (s_backAt && (int)(GetTickCount() - s_backAt) >= 0 && InGameplay()) MoveBack();
    if (!s_active) return;
    // The pause menu pauses photo mode (any other menu ends it). Photo mode's hidden HUD would
    // hide the pause menu too: it is shown while the menu is open. The game is in menu mode a few
    // frames before the pause menu shows as visible, so a menu is only told apart after up to
    // kMenuWaitMs (frozen meanwhile).
    static const DWORD kMenuWaitMs = 500;
    bool menu = MenuOpen();
    bool paused = menu && ((BYTE*)0x011F308F)[1013];  // the visible-menu table, 1013 = StartMenu
    if (menu && !paused) {
        if (!s_menuSince) s_menuSince = GetTickCount();
        if (GetTickCount() - s_menuSince >= kMenuWaitMs) {
            s_menuSince = 0;
            PhotoExit("a menu opened");
            return;
        }
        s_speedScale = 1.f;
        s_vertical = 0;
        HideBody(true);
        return;
    }
    s_menuSince = 0;
    if (paused != s_paused) {
        s_paused = paused;
        if (paused && s_hudHidden) {
            GameConsole("tm");
            s_hudHidden = false;
            s_hudRehide = true;
        } else if (!paused && s_hudRehide) {
            GameConsole("tm");
            s_hudHidden = true;
            s_hudRehide = false;
        }
        Log("photo: %s", paused ? "paused (pause menu)" : "resumed");
    }
    if (paused) {
        s_speedScale = 1.f;
        s_vertical = 0;
        HideBody(true);
        return;  // (the weapon is not holstered in a menu: see s_drawPending)
    }
    if (Due(s_holsterAt)) {
        s_holsterAt = 0;
        if (PlayerWeaponOut()) {
            SetWeaponOut(false);
            s_holstered = true;
            Log("photo: weapon holstered");
        }
    }
    bool front = GameInFront();
    s_speedScale = front && (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? (float)g_settings.photoFast : 1.f;
    int vertical = !front ? 0 : (GetAsyncKeyState(VK_SPACE) & 0x8000) ? 1 : (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? -1 : 0;
    s_vertical = vertical;
    HideBody(true);
    ZoomEase();
}

void PhotoWriteState(FILE* f)
{
    float px = 0, py = 0, pz = 0;
    PlayerPos(&px, &py, &pz);
    fprintf(f, "photo: %s, shortcut %x, fast x%d, freeze %d, hud %d; player %.1f %.1f %.1f, start %.1f %.1f %.1f (worldspace %08lX; now %08lX)\n",
            s_active ? "on" : "off", g_settings.photoKey, g_settings.photoFast, g_settings.photoFreeze, g_settings.photoHud, px, py, pz,
            s_startX, s_startY, s_startZ, s_startWorld, WorldspaceId());
    fprintf(f, "photo: cell %08lX; noclip %d, marker %p, move back %s (tries %d); SetPosition detour %s, %lu noclip calls, scale %.2f "
               "(%lu scaled), up/down %d\n",
            CellId(), s_noclip, s_marker, s_backAt ? "pending" : "no", s_backTries, s_setPosition ? "installed" : "MISSING", s_noclipCalls,
            s_speedScale, s_scaledCalls, s_vertical);
    BYTE *body = PlayerNode(false), *arms = PlayerNode(true);
    fprintf(f, "photo: player body %p flags %08lX, first-person model %p flags %08lX, hidden by photo mode %d (draw calls hooked %d of 6, "
               "%lu calls); 1st person %d; weapon out %d, holstered by photo mode %d\n",
            body, body ? *(DWORD*)(body + 0x30) : 0, arms, arms ? *(DWORD*)(arms + 0x30) : 0, s_bodyHidden, s_drawSites, s_drawCalls,
            *(BYTE**)0x011DEA3C ? !*(*(BYTE**)0x011DEA3C + 0x64A) : 0, *(void**)0x011DEA3C ? PlayerWeaponOut() : 0, s_holstered);
    fprintf(f, "photo: zoom %.3f (target %.3f), fov %.1f -> %.1f\n", s_zoom, s_zoomTarget, s_zoomShown, s_zoomFov);
    fprintf(f, "photo: paused %d (HUD shown for the menu %d), saves refused %lu (SaveGame detour %s)\n", s_paused, s_hudRehide, s_savesRefused,
            s_saveGame ? "installed" : "MISSING");
    // Shift as the game and script mods see it, and how far the camera moved against the player
    // since the last state (a sprint animation shakes it).
    bool game = *(void**)0x011DEA3C != nullptr;
    fprintf(f, "photo: camera height over the player %.1f..%.1f since the last state; Shift held %d (seen by the game %d), keys turned off:",
            s_camLo, s_camHi, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, game ? (int)ScriptValue("IsKeyPressed 42") : 0);
    s_camLo = 1e9f, s_camHi = -1e9f;
    for (int i = 0; i < kControlsOffCount; ++i)
        if (kControlsOff[i].key && s_controlOff[i]) fprintf(f, " %d", kControlsOff[i].control);
    fprintf(f, "\n");
}
