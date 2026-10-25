// Key names as rowe.ini stores them (screenshotkey=, menukey=). Shared by the plugin and the launcher.
#pragma once

int KeyFromName(const char* name);              // "F12", "Home", "A", "0x2C", ... -> virtual key, 0 = unknown
void KeyName(int vk, char* out, int size);      // the name KeyFromName reads
