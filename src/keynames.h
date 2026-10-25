// Key names as rowe.ini stores them (screenshotkey=, menukey=). Shared by the plugin and the launcher.
#pragma once

int KeyFromName(const char* name);              // "F12", "Home", "A", "0x2C", ... -> virtual key, 0 = unknown
void KeyName(int vk, char* out, int size);      // the name KeyFromName reads

// Shortcuts: a key with Ctrl and/or Alt (Shift is left out: photo mode uses it for speed),
// stored as "Ctrl+Alt+P". The int is the virtual key plus kShortcutCtrl / kShortcutAlt.
enum { kShortcutCtrl = 0x100, kShortcutAlt = 0x200 };
int ShortcutFromName(const char* name);         // 0 = unknown
void ShortcutName(int shortcut, char* out, int size);
bool ShortcutDown(int shortcut);                // its key and exactly its modifiers held (GetAsyncKeyState)
int ShortcutModifiersHeld();                    // kShortcutCtrl / kShortcutAlt for the modifiers held now
