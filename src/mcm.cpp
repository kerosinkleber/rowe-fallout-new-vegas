// The Mod Configuration Menu (MCM, Pelinor's, optional): ROWE's settings as an MCM page.
//
// MCM lists one "dummy" item per mod (added to its form list MCMMods, form 0xAE6 of The Mod
// Configuration Menu.esp) and tells mods apart by the mod index of that item, so the page needs a
// form from a plugin: ROWE - MCM.esp holds one (MISC 0x800, named as the list shows it) and
// nothing else. Everything else is done here. While the pause menu (StartMenu, 1013) is open and
// ROWE's page is the active one (StartMenu/MCM/_ActiveMod = our mod index), MCM asks through
// traits of its tiles and the page answers them, as MCM mods' scripts do:
//   _Reset        fill in the options (*:1/*:N: _title, _type 1 list / 2 scale / 5 checkbox,
//                 _value, _enable 1 / 2 greyed); a list's current entry is value/*:1/string
//   _NewValue     option _ActiveOption was set to _Value (list: entry number from 1)
//   _ShowList 1   fill the list window (*:3: _title, *:3/*:K/text/string, _enable) and _Value
//   _ShowScale 1  the slider: _Value, _ValueMin/Max/Increment/Decimal, *:2/_title
//   _DefaultScale the slider's default; _Default all defaults
//   *:1/_optionID the option under the mouse: its help text goes to *:9/string (MCM_Info)
// The tiles are read and written with xNVSE's GetUIFloat / SetUIFloat / SetUIString, through
// small script functions (NVSEScriptInterface::CompileScript), one per path (and text), compiled
// when first needed and kept: string variables as parameters do not compile in these partial
// scripts, so paths and texts are literals and only numbers are passed. Without MCM, the ESP or
// the script interface, nothing happens.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <map>
#include <string>
#include "fnvfov.h"
#include "nvse.h"

struct Script;
struct NVSEScriptInterface {
    bool (*CallFunction)(Script*, void* callingObj, void* container, void* result, UINT8 numArgs, ...);
    UINT32 (*GetFunctionParams)(Script*, UINT8*);
    void* ExtractArgsEx;
    void* ExtractFormatStringArgs;
    bool (*CallFunctionAlt)(Script*, void* callingObj, UINT8 numArgs, ...);
    Script* (*CompileScript)(const char* text);
    Script* (*CompileExpression)(const char* text);
};
// NVSEArrayVarInterface::Element: a union (string, array, form, double) and its type.
struct ScriptResult {
    union { char* str; void* form; double num; };
    UINT8 type;  // 1 number, 2 form, 3 string
};

static NVSEScriptInterface* s_script;
static Script* s_register;
static bool s_compiled, s_failed;
static int s_modIndex = -1;  // ROWE - MCM.esp's, once registered
static bool s_wasOpen;
static int s_mouseover;

void McmInit(void* scriptInterface) { s_script = (NVSEScriptInterface*)scriptInterface; }

static Script* Compile(const char* what, const char* text)
{
    Script* s = s_script->CompileScript(text);
    if (!s) Log("mcm: compiling %s failed:\n%s", what, text);
    return s;
}

// A snippet per key (kind + path + text), compiled once; null (logged once) if it does not compile.
static Script* Snippet(const std::string& key, const std::string& text)
{
    static std::map<std::string, Script*> cache;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Script* s = Compile(key.c_str(), text.c_str());
    cache[key] = s;
    return s;
}

// A literal for the script text: a quote would end it.
static std::string Quoted(const char* text)
{
    std::string q = "\"";
    for (const char* c = text; *c; ++c) q += *c == '"' ? '\'' : *c == ';' ? ',' : *c;  // ";" would start a comment
    return q + "\"";
}

static bool CompileAll()
{
    if (s_compiled) return !s_failed;
    s_compiled = true;
    // Adds the dummy item to MCM's list (once per list: loading a save brings back the saved list)
    // and returns the ESP's mod index; nothing if MCM or the ESP is not loaded.
    s_register = Compile("registration",
                         "int iMCM\nint iMe\nref rList\nref rItem\nbegin function {}\n"
                         "if IsModLoaded \"The Mod Configuration Menu.esp\" && IsModLoaded \"ROWE - MCM.esp\"\n"
                         "let iMCM := GetModIndex \"The Mod Configuration Menu.esp\"\n"
                         "let iMe := GetModIndex \"ROWE - MCM.esp\"\n"
                         "let rList := BuildRef iMCM 2790\n"
                         "let rItem := BuildRef iMe 2048\n"
                         "if ListGetFormIndex rList rItem < 0\nListAddForm rList rItem\nendif\n"
                         "SetFunctionValue iMe\nendif\nend\n");
    s_failed = !s_register;
    return !s_failed;
}

static float GetF(const char* path)
{
    Script* s = Snippet(std::string("get ") + path, "begin function {}\nSetFunctionValue (GetUIFloat " + Quoted(path) + ")\nend\n");
    ScriptResult r = {};
    if (!s || !s_script->CallFunction(s, nullptr, nullptr, &r, 0) || r.type != 1) return 0.f;
    return (float)r.num;
}

float ScriptValue(const char* expression)
{
    if (!s_script) return 0.f;
    Script* s = Snippet(std::string("value ") + expression, std::string("begin function {}\nSetFunctionValue (") + expression + ")\nend\n");
    ScriptResult r = {};
    if (!s || !s_script->CallFunction(s, nullptr, nullptr, &r, 0) || r.type != 1) return 0.f;
    return (float)r.num;
}

void* ScriptRef(const char* statements)
{
    if (!s_script) return nullptr;
    Script* s = Snippet(std::string("ref ") + statements,
                        std::string("ref rOut\nref rBase\nbegin function {}\n") + statements + "\nSetFunctionValue rOut\nend\n");
    ScriptResult r = {};
    if (!s || !s_script->CallFunction(s, nullptr, nullptr, &r, 0) || r.type != 2) return nullptr;
    return r.form;
}

bool ScriptWithRef(const char* statement, void* ref)
{
    if (!s_script || !ref) return false;
    Script* s = Snippet(std::string("with ref ") + statement, std::string("ref rArg\nbegin function {rArg}\n") + statement + "\nend\n");
    return s && s_script->CallFunctionAlt(s, nullptr, 1, ref);
}

static void SetF(const char* path, float v)
{
    Script* s = Snippet(std::string("set ") + path, "float v\nbegin function {v}\nSetUIFloat " + Quoted(path) + " v\nend\n");
    if (s) s_script->CallFunctionAlt(s, nullptr, 1, *(UINT32*)&v);
}

static void SetS(const char* path, const char* text)
{
    Script* s = Snippet(std::string("text ") + path + "\n" + text,
                        "begin function {}\nSetUIString " + Quoted(path) + " " + Quoted(text) + "\nend\n");
    if (s) s_script->CallFunctionAlt(s, nullptr, 0);
}

static void Opt(int id, const char* trait, float v)
{
    char p[96];
    wsprintfA(p, "StartMenu/MCM/*:1/*:%d/%s", id, trait);
    SetF(p, v);
}

static void OptS(int id, const char* trait, const char* s)
{
    char p[96];
    wsprintfA(p, "StartMenu/MCM/*:1/*:%d/%s", id, trait);
    SetS(p, s);
}

// ---- the page ----------------------------------------------------------------------------------

enum { kList = 1, kScale = 2, kCheck = 5 };
enum {
    kOptFov = 1, kOptFovGame, kOptLens, kOptFisheye, kOptScope, kOptZoom, kOptZoomGame, kOptPipboy, kOptPipboyGame,
    kOptSupersample, kOptMousetrap, kOptIntros, kOptShotSize, kOptShotFormat, kOptShotQuality, kOptAutosave,
    kOptAutosaveMinutes, kOptAutosaveSlots, kOptPhotoFast, kOptPhotoHud, kOptPhotoFreeze, kOptCount
};

static const char* kLensNames[kLensChoiceCount] = {"Off", "Panini", "Stereographic", "Fisheye"};  // kLensChoices
static const char* kFisheyeNames[] = {"Fill", "Full circle"};
static const char* kScopeNames[] = {"Bent with the lens", "Flat"};
static const float kSupersample[] = {1, 2, 4, 9, 16, 25, 36, 49, 64};  // MCM lists hold 10 entries
static const char* kTrapNames[kMouseTrapModeCount] = {"Off", "In gameplay", "Always"};
static const char* kShotSize[] = {"Supersampled", "Window"};
static const char* kShotFormat[] = {"JPEG", "PNG"};

static const char* Title(int id)
{
    switch (id) {
    case kOptFov: return "Field of view";
    case kOptFovGame: return "Use the game's FOV";
    case kOptLens: return "Lens";
    case kOptFisheye: return "Fisheye";
    case kOptScope: return "Scope overlay";
    case kOptZoom: return "Zoom FOV (no weapon drawn)";
    case kOptZoomGame: return "Use the game's zoom";
    case kOptPipboy: return "Pip-Boy FOV";
    case kOptPipboyGame: return "Use the game's Pip-Boy FOV";
    case kOptSupersample: return SupersampleLive() ? "Supersampling" : "Supersampling (next start)";
    case kOptMousetrap: return "Mouse trap";
    case kOptIntros: return "Skip intro videos (next start)";
    case kOptShotSize: return "Screenshot size";
    case kOptShotFormat: return "Screenshot format";
    case kOptShotQuality: return "JPEG quality";
    case kOptAutosave: return "Autosave";
    case kOptAutosaveMinutes: return "Autosave every (minutes of play)";
    case kOptAutosaveSlots: return "Autosave slots";
    case kOptPhotoFast: return "Photo mode: fast speed (Shift)";
    case kOptPhotoHud: return "Photo mode: hide the HUD";
    case kOptPhotoFreeze: return "Photo mode: freeze people";
    }
    return "";
}

static const char* Help(int id)
{
    switch (id) {
    case kOptFov: return "Horizontal field of view in degrees at 4:3, like the game's own setting (wider screens show more).";
    case kOptFovGame: return "Leave the field of view to the game.";
    case kOptLens: return "How very wide views are drawn without stretched edges. "
                          "Panini keeps vertical lines straight, Stereographic keeps shapes round, Fisheye goes up to 160 degrees.";
    case kOptFisheye: return "Fill: the picture fills the screen's width. Full circle: at the widest FOV the whole rendered circle "
                             "is shown; lower FOVs and zooming magnify it.";
    case kOptScope: return "Whether the lens also bends the scope's crosshair and black mask.";
    case kOptZoom: return "Where the zoom ends when you hold the aim button with no weapon drawn.";
    case kOptZoomGame: return "Use the game's zoom (fIronSightsZoomDefault).";
    case kOptPipboy: return "The Pip-Boy's field of view: lower makes the Pip-Boy appear bigger.";
    case kOptPipboyGame: return "Use the game's Pip-Boy FOV (fPipboy1stPersonFOV).";
    case kOptSupersample:
        return SupersampleLive() ? "Renders at a larger size and scales it down: sharper, slower. Pixels rendered per pixel shown. "
                                   "Applies at once (the screen goes black for a moment); off while the pixelated look is on."
                                 : "Renders at a larger size and scales it down: sharper, slower. Pixels rendered per pixel shown. "
                                   "Applies at the next start (F8 menu: restart game).";
    case kOptMousetrap: return "In gameplay: the mouse stays in the window in gameplay and is free in menus. Always: also in menus.";
    case kOptIntros: return "Skip the intro videos. Applies at the next start.";
    case kOptShotSize: return "Screenshots (F12 by default) at the supersampled size or at the window's.";
    case kOptShotFormat: return "JPEG: about 5x smaller and much faster to write. PNG: lossless.";
    case kOptShotQuality: return "JPEG quality: visually lossless from about 90.";
    case kOptAutosave: return "Saves the game every so many minutes of play as ROWE Autosave 1, 2, ... in turn. Menus, loading "
                              "screens and combat are not counted. Any save starts the count again.";
    case kOptAutosaveMinutes: return "Minutes of play between autosaves.";
    case kOptAutosaveSlots: return "How many autosaves are kept: the oldest is overwritten.";
    case kOptPhotoFast: return "Photo mode (its shortcut, F10 by default, is set in the F8 menu or the launcher): how much "
                               "faster you fly while Shift is held.";
    case kOptPhotoHud: return "Hides the HUD while photo mode is on.";
    case kOptPhotoFreeze: return "Stops people and creatures where they are while photo mode is on (weather and effects go on).";
    }
    return "";
}

static int SupersampleEntry()
{
    for (int i = 0; i < 9; ++i)
        if (fabsf(kSupersample[i] - g_settings.supersample) < 0.01f) return i;
    return -1;
}

static float PipboyGameFov() { return *(float*)0x01203178 > 0 ? *(float*)0x01203178 : 47.f; }

static void ListValue(int id, const char* text) { OptS(id, "value/*:1/string", text); }

static void Fill()
{
    SetF("StartMenu/MCM/*:1/_columns", 1);
    for (int id = 1; id < kOptCount; ++id) {
        OptS(id, "_title", Title(id));
        Opt(id, "_enable", 1);
    }
    bool fovGame = g_settings.fov <= 0;
    Opt(kOptFov, "_type", kScale);
    Opt(kOptFov, "_value", floorf((fovGame ? FovBase() : g_settings.fov) + 0.5f));
    Opt(kOptFov, "_enable", fovGame ? 2.f : 1.f);
    Opt(kOptFovGame, "_type", kCheck);
    Opt(kOptFovGame, "_value", fovGame);
    Opt(kOptLens, "_type", kList);
    ListValue(kOptLens, LensName(g_settings.lens));
    Opt(kOptFisheye, "_type", kList);
    ListValue(kOptFisheye, kFisheyeNames[g_settings.fisheyeCircle ? 1 : 0]);
    Opt(kOptFisheye, "_enable", g_settings.lens == kLensFisheye ? 1.f : 2.f);
    Opt(kOptScope, "_type", kList);
    ListValue(kOptScope, kScopeNames[g_settings.scopeLens ? 0 : 1]);
    bool zoomGame = g_settings.zoomFov <= 0;
    Opt(kOptZoom, "_type", kScale);
    Opt(kOptZoom, "_value", floorf((zoomGame ? FovZoomDefault() : g_settings.zoomFov) + 0.5f));
    Opt(kOptZoom, "_enable", zoomGame ? 2.f : 1.f);
    Opt(kOptZoomGame, "_type", kCheck);
    Opt(kOptZoomGame, "_value", zoomGame);
    bool pipGame = g_settings.pipboyFov <= 0;
    Opt(kOptPipboy, "_type", kScale);
    Opt(kOptPipboy, "_value", floorf((pipGame ? PipboyGameFov() : g_settings.pipboyFov) + 0.5f));
    Opt(kOptPipboy, "_enable", pipGame ? 2.f : 1.f);
    Opt(kOptPipboyGame, "_type", kCheck);
    Opt(kOptPipboyGame, "_value", pipGame);
    Opt(kOptSupersample, "_type", kList);
    char ss[32];
    if (g_settings.supersample <= 1.01f) lstrcpyA(ss, "Off");
    else _snprintf(ss, sizeof(ss) - 1, "%gx", g_settings.supersample), ss[sizeof(ss) - 1] = 0;
    ListValue(kOptSupersample, ss);
    Opt(kOptMousetrap, "_type", kList);
    ListValue(kOptMousetrap, kTrapNames[g_settings.mousetrap]);
    Opt(kOptIntros, "_type", kCheck);
    Opt(kOptIntros, "_value", g_settings.skipIntros);
    Opt(kOptShotSize, "_type", kList);
    ListValue(kOptShotSize, kShotSize[g_settings.screenshotFull ? 0 : 1]);
    Opt(kOptShotFormat, "_type", kList);
    ListValue(kOptShotFormat, kShotFormat[g_settings.screenshotJpg ? 0 : 1]);
    Opt(kOptShotQuality, "_type", kScale);
    Opt(kOptShotQuality, "_value", (float)g_settings.screenshotQuality);
    Opt(kOptShotQuality, "_enable", g_settings.screenshotJpg ? 1.f : 2.f);
    Opt(kOptAutosave, "_type", kCheck);
    Opt(kOptAutosave, "_value", g_settings.autosave);
    Opt(kOptAutosaveMinutes, "_type", kScale);
    Opt(kOptAutosaveMinutes, "_value", (float)g_settings.autosaveMinutes);
    Opt(kOptAutosaveMinutes, "_enable", g_settings.autosave ? 1.f : 2.f);
    Opt(kOptAutosaveSlots, "_type", kScale);
    Opt(kOptAutosaveSlots, "_value", (float)g_settings.autosaveSlots);
    Opt(kOptAutosaveSlots, "_enable", g_settings.autosave ? 1.f : 2.f);
    Opt(kOptPhotoFast, "_type", kScale);
    Opt(kOptPhotoFast, "_value", (float)g_settings.photoFast);
    Opt(kOptPhotoHud, "_type", kCheck);
    Opt(kOptPhotoHud, "_value", g_settings.photoHud);
    Opt(kOptPhotoFreeze, "_type", kCheck);
    Opt(kOptPhotoFreeze, "_value", g_settings.photoFreeze);
}

static void FillList(int option)
{
    const char* const* names = nullptr;
    int count = 0, current = 0;
    char ssNames[9][16];
    const char* ssPtr[9];
    switch (option) {
    case kOptLens: names = kLensNames; count = kLensChoiceCount; current = LensChoiceIndex(g_settings.lens); break;
    case kOptFisheye: names = kFisheyeNames; count = 2; current = g_settings.fisheyeCircle ? 1 : 0; break;
    case kOptScope: names = kScopeNames; count = 2; current = g_settings.scopeLens ? 0 : 1; break;
    case kOptMousetrap: names = kTrapNames; count = kMouseTrapModeCount; current = g_settings.mousetrap; break;
    case kOptShotSize: names = kShotSize; count = 2; current = g_settings.screenshotFull ? 0 : 1; break;
    case kOptShotFormat: names = kShotFormat; count = 2; current = g_settings.screenshotJpg ? 0 : 1; break;
    case kOptSupersample:
        for (int i = 0; i < 9; ++i) {
            if (i == 0) lstrcpyA(ssNames[i], "Off");
            else _snprintf(ssNames[i], sizeof(ssNames[i]) - 1, "%gx", kSupersample[i]), ssNames[i][sizeof(ssNames[i]) - 1] = 0;
            ssPtr[i] = ssNames[i];
        }
        names = ssPtr;
        count = 9;
        current = SupersampleEntry();
        break;
    default: return;
    }
    SetS("StartMenu/MCM/*:3/_title", Title(option));
    for (int i = 0; i < count; ++i) {
        char p[64];
        wsprintfA(p, "StartMenu/MCM/*:3/*:%d/text/string", i + 1);
        SetS(p, names[i]);
        wsprintfA(p, "StartMenu/MCM/*:3/*:%d/_enable", i + 1);
        SetF(p, 1);
    }
    SetF("StartMenu/MCM/_Value", (float)(current + 1));
}

static void ShowScale(int option)
{
    float lo, hi, v;
    switch (option) {
    case kOptFov: lo = kFovMin; hi = kFovMax; v = g_settings.fov > 0 ? g_settings.fov : FovBase(); break;
    case kOptZoom: lo = kZoomFovMin; hi = kFovMax; v = g_settings.zoomFov > 0 ? g_settings.zoomFov : FovZoomDefault(); break;
    case kOptPipboy: lo = kPipboyFovMin; hi = kPipboyFovMax; v = g_settings.pipboyFov > 0 ? g_settings.pipboyFov : PipboyGameFov(); break;
    case kOptShotQuality: lo = 50; hi = 100; v = (float)g_settings.screenshotQuality; break;
    case kOptAutosaveMinutes: lo = 1; hi = 240; v = (float)g_settings.autosaveMinutes; break;
    case kOptAutosaveSlots: lo = 1; hi = 10; v = (float)g_settings.autosaveSlots; break;
    case kOptPhotoFast: lo = 1; hi = 50; v = (float)g_settings.photoFast; break;
    default: return;
    }
    SetF("StartMenu/MCM/_ValueDecimal", 0);
    SetF("StartMenu/MCM/_ValueIncrement", 1);
    SetF("StartMenu/MCM/_ValueMin", lo);
    SetF("StartMenu/MCM/_ValueMax", hi);
    SetF("StartMenu/MCM/_Value", floorf(v + 0.5f));
    SetS("StartMenu/MCM/*:2/_title", Title(option));
}

static float ScaleDefault(int option)
{
    switch (option) {
    case kOptFov: return floorf(FovBase() + 0.5f);
    case kOptZoom: return floorf(FovZoomDefault() + 0.5f);
    case kOptPipboy: return floorf(PipboyGameFov() + 0.5f);
    case kOptShotQuality: return 95;
    case kOptAutosaveMinutes: return 20;
    case kOptAutosaveSlots: return 3;
    case kOptPhotoFast: return 5;
    }
    return 0;
}

static void Apply(int option, float v)
{
    int entry = (int)v - 1;  // lists: entry numbers from 1
    switch (option) {
    case kOptFov: g_settings.fov = v; break;
    case kOptFovGame: g_settings.fov = v != 0 ? 0.f : floorf(FovBase() + 0.5f); break;
    case kOptLens: if (entry >= 0 && entry < kLensChoiceCount) g_settings.lens = kLensChoices[entry]; break;
    case kOptFisheye: g_settings.fisheyeCircle = entry == 1; break;
    case kOptScope: g_settings.scopeLens = entry == 0; break;
    case kOptZoom: g_settings.zoomFov = v; break;
    case kOptZoomGame: g_settings.zoomFov = v != 0 ? 0.f : floorf(FovZoomDefault() + 0.5f); break;
    case kOptPipboy: g_settings.pipboyFov = v; break;
    case kOptPipboyGame: g_settings.pipboyFov = v != 0 ? 0.f : floorf(PipboyGameFov() + 0.5f); break;
    case kOptSupersample: if (entry >= 0 && entry < 9) g_settings.supersample = kSupersample[entry]; break;
    case kOptMousetrap: if (entry >= 0 && entry < kMouseTrapModeCount) g_settings.mousetrap = entry; break;
    case kOptIntros: g_settings.skipIntros = v != 0; break;
    case kOptShotSize: g_settings.screenshotFull = entry == 0; break;
    case kOptShotFormat: g_settings.screenshotJpg = entry == 0; break;
    case kOptShotQuality: g_settings.screenshotQuality = (int)v; break;
    case kOptAutosave: g_settings.autosave = v != 0; break;
    case kOptAutosaveMinutes: g_settings.autosaveMinutes = (int)v; break;
    case kOptAutosaveSlots:
        g_settings.autosaveSlots = (int)v;
        if (g_settings.autosaveNext > g_settings.autosaveSlots) g_settings.autosaveNext = 1;
        break;
    case kOptPhotoFast: g_settings.photoFast = (int)v; break;
    case kOptPhotoHud: g_settings.photoHud = v != 0; break;
    case kOptPhotoFreeze: g_settings.photoFreeze = v != 0; break;
    default: return;
    }
    Log("mcm: option %d set to %g", option, v);
    SettingsSave();
}

static void Defaults()
{
    Settings d;
    g_settings.fov = d.fov;
    g_settings.lens = d.lens;
    g_settings.fisheyeCircle = d.fisheyeCircle;
    g_settings.scopeLens = d.scopeLens;
    g_settings.zoomFov = d.zoomFov;
    g_settings.pipboyFov = d.pipboyFov;
    g_settings.mousetrap = d.mousetrap;
    g_settings.screenshotFull = d.screenshotFull;
    g_settings.screenshotJpg = d.screenshotJpg;
    g_settings.screenshotQuality = d.screenshotQuality;
    Log("mcm: defaults");
    SettingsSave();
}

// Main loop (game thread), menus included.
void McmTick()
{
    if (!s_script) return;
    bool open = ((const BYTE*)0x011F308F)[1013] != 0;
    if (!open) { s_wasOpen = false; return; }
    if (!CompileAll()) return;
    if (!s_wasOpen) {  // the pause / main menu just opened: (re)register
        s_wasOpen = true;
        ScriptResult r = {};
        bool ok = s_script->CallFunction(s_register, nullptr, nullptr, &r, 0);
        int index = ok && r.type == 1 ? (int)r.num : -1;
        static bool logged;
        if (index != s_modIndex || !logged)
            Log("mcm: %s", index >= 0 ? "listed in The Mod Configuration Menu" : "no page: The Mod Configuration Menu.esp or ROWE - MCM.esp not active");
        logged = true;
        s_modIndex = index;
        s_mouseover = -1;
    }
    if (s_modIndex < 0 || (int)GetF("StartMenu/MCM/_ActiveMod") != s_modIndex) return;
    int option = (int)GetF("StartMenu/MCM/_ActiveOption");
    if (GetF("StartMenu/MCM/_Reset")) {
        SetF("StartMenu/MCM/_Reset", 0);
        Fill();
    } else if (GetF("StartMenu/MCM/_Default")) {
        SetF("StartMenu/MCM/_Default", 0);
        SetF("StartMenu/MCM/_Reset", 1);
        Defaults();
    } else if (GetF("StartMenu/MCM/_NewValue")) {
        SetF("StartMenu/MCM/_NewValue", 0);
        SetF("StartMenu/MCM/_Reset", 1);
        Apply(option, GetF("StartMenu/MCM/_Value"));
    } else if (GetF("StartMenu/MCM/_ShowList") == 1) {
        SetF("StartMenu/MCM/_ShowList", 2);
        FillList(option);
    } else if (GetF("StartMenu/MCM/_ShowScale") == 1) {
        SetF("StartMenu/MCM/_ShowScale", 2);
        ShowScale(option);
    } else if (GetF("StartMenu/MCM/_DefaultScale")) {
        SetF("StartMenu/MCM/_DefaultScale", 0);
        SetF("StartMenu/MCM/_ShowScale", 2);
        SetF("StartMenu/MCM/_Value", ScaleDefault(option));
    } else {
        int over = (int)GetF("StartMenu/MCM/*:1/_optionID");
        if (over != s_mouseover) {
            s_mouseover = over;
            bool show = over > 0 && over < kOptCount;
            SetF("StartMenu/MCM/MCM_Info/visible", show);
            if (show) SetS("StartMenu/MCM/*:9/string", Help(over));
        }
    }
}
