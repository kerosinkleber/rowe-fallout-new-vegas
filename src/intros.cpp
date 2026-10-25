// Intro skip: blanks the intro video settings in memory (sIntroSequence, sMainMenuMovieIntro,
// sIntroMovie), right after the game has read its INI files and before it creates its window.
// No file is changed. Vanilla Fallout: New Vegas leaves the first two empty and names a missing
// file in the third, so this matters for INI tweaks and mods that add startup videos; the
// new-game cinematic (FNVIntro.bik) is not touched.
#include <windows.h>
#include <string.h>
#include "fnvfov.h"

static const char* const kSettings[] = {"sIntroSequence:General", "sMainMenuMovieIntro:General", "sIntroMovie:General"};
static char s_empty[1] = "";
static int s_cleared;
static BYTE* s_objects[3];

// At plugin load: the Setting objects still point at their names in the exe then (after the INI
// files are read they no longer do, so they cannot be found by name later).
void IntrosInit()
{
    for (int i = 0; i < 3; ++i) {
        s_objects[i] = FindSettingObject(kSettings[i]);
        if (!s_objects[i]) Log("intros: setting %s not found", kSettings[i]);
    }
}

void IntrosApply()
{
    static bool done;
    if (done) return;
    done = true;
    if (!g_settings.skipIntros) {
        Log("intros: not skipped (skipintros=0)");
        return;
    }
    for (int i = 0; i < 3; ++i) {
        const char* name = kSettings[i];
        BYTE* obj = s_objects[i];
        if (!obj) continue;
        const char* old = *(const char**)(obj + 4);
        *(char**)(obj + 4) = s_empty;
        ++s_cleared;
        Log("intros: %s was \"%s\", now empty", name, old && !IsBadReadPtr(old, 1) ? old : "?");
    }
}

void IntrosWriteState(FILE* f)
{
    fprintf(f, "intros: skip %d, settings cleared %d\n", g_settings.skipIntros, s_cleared);
}
