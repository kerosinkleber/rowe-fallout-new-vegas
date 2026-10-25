// Key names for rowe.ini (see keynames.h).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "keynames.h"

static const struct { const char* name; int vk; } kKeyNames[] = {
    {"Home", VK_HOME}, {"End", VK_END}, {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT}, {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL}, {"PrintScreen", VK_SNAPSHOT},
    {"Print", VK_SNAPSHOT}, {"Tab", VK_TAB}, {"Space", VK_SPACE}, {"Backspace", VK_BACK},
};

int KeyFromName(const char* n)
{
    if (!n || !n[0]) return 0;
    if (n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) return (int)strtol(n + 2, nullptr, 16) & 0xFF;
    if ((n[0] == 'F' || n[0] == 'f') && n[1] >= '1' && n[1] <= '9') {
        int f = atoi(n + 1);
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    if (!n[1] && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return n[0];
    if (!n[1] && n[0] >= 'a' && n[0] <= 'z') return n[0] - 'a' + 'A';
    if (!_strnicmp(n, "Num", 3) && n[3] >= '0' && n[3] <= '9' && !n[4]) return VK_NUMPAD0 + n[3] - '0';
    for (const auto& k : kKeyNames)
        if (!lstrcmpiA(n, k.name)) return k.vk;
    return 0;
}

int ShortcutFromName(const char* n)
{
    int mods = 0;
    for (;;) {
        if (!_strnicmp(n, "Ctrl+", 5)) { mods |= kShortcutCtrl; n += 5; }
        else if (!_strnicmp(n, "Alt+", 4)) { mods |= kShortcutAlt; n += 4; }
        else break;
    }
    int vk = KeyFromName(n);
    return vk ? vk | mods : 0;
}

void ShortcutName(int shortcut, char* out, int size)
{
    char key[32];
    KeyName(shortcut & 0xFF, key, sizeof(key));
    _snprintf(out, size, "%s%s%s", shortcut & kShortcutCtrl ? "Ctrl+" : "", shortcut & kShortcutAlt ? "Alt+" : "", key);
    out[size - 1] = 0;
}

int ShortcutModifiersHeld()
{
    return ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? kShortcutCtrl : 0) | ((GetAsyncKeyState(VK_MENU) & 0x8000) ? kShortcutAlt : 0);
}

bool ShortcutDown(int shortcut)
{
    int vk = shortcut & 0xFF;
    return vk && (GetAsyncKeyState(vk) & 0x8000) && ShortcutModifiersHeld() == (shortcut & (kShortcutCtrl | kShortcutAlt));
}

void KeyName(int vk, char* out, int size)
{
    if (vk >= VK_F1 && vk <= VK_F24) { _snprintf(out, size, "F%d", vk - VK_F1 + 1); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { _snprintf(out, size, "%c", vk); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { _snprintf(out, size, "Num%d", vk - VK_NUMPAD0); return; }
    for (const auto& k : kKeyNames)
        if (k.vk == vk) { lstrcpynA(out, k.name, size); return; }
    _snprintf(out, size, "0x%02X", vk);
}
