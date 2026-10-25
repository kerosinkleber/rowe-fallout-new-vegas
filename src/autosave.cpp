// Autosave: every `autosaveminutes` minutes of play (time in gameplay only: not in menus, loading
// screens or with the game in the background) the game is saved as "ROWE Autosave N", N going
// round 1..autosaveslots (rowe.ini autosavenext remembers the next one across starts). When the
// time is up during combat or with the player dead it waits until that is over. Loading, starting
// or saving a game (any save, the player's own too) starts the count again.
#include <windows.h>
#include <stdio.h>
#include "fnvfov.h"

static DWORD s_prevTick;
static DWORD s_playedMs;   // play time since the last save or load
static bool s_waitingLogged;

void AutosaveReset(const char* why)
{
    if (s_playedMs > 1000) Log("autosave: count restarted (%s) after %lu s of play", why, s_playedMs / 1000);
    s_playedMs = 0;
    s_waitingLogged = false;
}

// Main loop (game thread).
void AutosaveTick()
{
    DWORD now = GetTickCount(), dt = now - s_prevTick;
    s_prevTick = now;
    if (dt > 1000) dt = 0;  // a hitch, a loading screen or the game in the background
    if (!g_settings.autosave || !InGameplay() || !GameInFront()) return;
    s_playedMs += dt;
    if (s_playedMs < (DWORD)g_settings.autosaveMinutes * 60000) return;
    // Not in combat, not dead (script functions through xNVSE; without them, no check).
    if (ScriptValue("player.IsInCombat || player.GetDead") != 0) {
        if (!s_waitingLogged) Log("autosave: due, waiting until the player is out of combat");
        s_waitingLogged = true;
        return;
    }
    int slot = g_settings.autosaveNext;
    if (slot < 1 || slot > g_settings.autosaveSlots) slot = 1;
    char line[96];
    wsprintfA(line, "con save \"ROWE Autosave %d\"", slot);
    Log("autosave: saving to slot %d after %lu s of play", slot, s_playedMs / 1000);
    GameCmdNow(line);
    g_settings.autosaveNext = slot % g_settings.autosaveSlots + 1;
    SettingsSave();
    s_playedMs = 0;
    s_waitingLogged = false;
}

void AutosaveWriteState(FILE* f)
{
    fprintf(f, "autosave: %s, every %d min, %d slots, next %d, played %lu s\n", g_settings.autosave ? "on" : "off", g_settings.autosaveMinutes,
            g_settings.autosaveSlots, g_settings.autosaveNext, s_playedMs / 1000);
}
