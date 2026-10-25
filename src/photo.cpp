// Photo mode (the photo shortcut, default F10, also with Ctrl / Alt): the fly camera
// (ToggleFlyCam; "tfc 1" with the world frozen if photofreeze=1), the player invisible
// (SetActorAlpha 0) and undetectable (ToggleDetection), the HUD hidden (ToggleMenus) if photohud=1.
// Holding Shift multiplies the fly camera's speed (fUFOCamSpeedMult:Controls) by photofast. The
// shortcut again ends it and restores all of that; so do loading a game, going to the main menu
// and quitting (before the game goes on, so detection and the HUD are never left off).
// Starts only in gameplay; the console commands run on the game thread.
#include <windows.h>
#include <stdio.h>
#include "fnvfov.h"

static bool s_active, s_hudHidden;
static BYTE* s_speed;       // the fly camera speed setting object
static float s_baseSpeed = 1.f;
static bool s_keyDown;

bool PhotoActive() { return s_active; }

static float* Speed()
{
    if (!s_speed) s_speed = FindSettingObject("fUFOCamSpeedMult:Controls");
    return s_speed ? (float*)(s_speed + 4) : nullptr;
}

static void Enter()
{
    if (s_active || !InGameplay()) return;
    s_active = true;
    if (float* sp = Speed()) s_baseSpeed = *sp > 0 ? *sp : 1.f;
    GameConsole(g_settings.photoFreeze ? "tfc 1" : "tfc");
    GameConsole("player.SetActorAlpha 0");
    GameConsole("tdetect");
    s_hudHidden = g_settings.photoHud;
    if (s_hudHidden) GameConsole("tm");
    Log("photo: on (world %s, HUD %s, Shift x%d)", g_settings.photoFreeze ? "frozen" : "running", s_hudHidden ? "hidden" : "shown",
        g_settings.photoFast);
}

void PhotoExit(const char* why)
{
    if (!s_active) return;
    s_active = false;
    if (float* sp = Speed()) *sp = s_baseSpeed;
    GameConsole("tfc");
    GameConsole("player.SetActorAlpha 1");
    GameConsole("tdetect");
    if (s_hudHidden) GameConsole("tm");
    s_hudHidden = false;
    Log("photo: off (%s)", why);
}

void PhotoToggle()
{
    if (s_active) PhotoExit("toggled");
    else Enter();
}

// Main loop (game thread).
void PhotoTick()
{
    bool down = GameInFront() && !OverlayOpen() && ShortcutDown(g_settings.photoKey);
    if (down && !s_keyDown) PhotoToggle();
    s_keyDown = down;
    if (!s_active) return;
    if (float* sp = Speed()) {
        bool fast = GameInFront() && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
        *sp = s_baseSpeed * (fast ? (float)g_settings.photoFast : 1.f);
    }
}

void PhotoWriteState(FILE* f)
{
    float* sp = Speed();
    fprintf(f, "photo: %s, shortcut %x, fast x%d, freeze %d, hud %d, fly speed %.2f (base %.2f)\n", s_active ? "on" : "off",
            g_settings.photoKey, g_settings.photoFast, g_settings.photoFreeze, g_settings.photoHud, sp ? *sp : -1.f, s_baseSpeed);
}
