// The mod's former name (see oldnames.h).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "oldnames.h"

bool TakeOverOldIni(const char* dir)
{
    std::string now = std::string(dir) + "rowe.ini", old = std::string(dir) + "fnvfov.ini";
    if (GetFileAttributesA(now.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    FILE* in = fopen(old.c_str(), "rb");
    if (!in) return false;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), in)) > 0;) text.append(buf, n);
    fclose(in);
    for (size_t at = 0; (at = text.find("[fnvfov]", at)) != std::string::npos; at += 6) text.replace(at, 8, "[rowe]");
    FILE* out = fopen(now.c_str(), "wb");
    if (!out) return false;
    bool ok = fwrite(text.data(), 1, text.size(), out) == text.size();
    fclose(out);
    return ok;
}
