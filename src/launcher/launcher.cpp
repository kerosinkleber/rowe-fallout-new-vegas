// rowe_launcher.exe: picks a save and starts the game into it, with the vanilla launcher's
// graphics settings (FalloutNVLauncher.exe: the same INI keys, value steps and presets) and the
// mod's supersampling. Drawn with Dear ImGui in the style of the game's HUD (hudstyle.cpp), with
// the game's UI font from its archives and the HUD colour from its INI files.
//
// Under Mod Organizer 2 it must be started from MO2 (as an executable) so it sees the profile's
// INI files and saves and the game it starts runs in MO2's virtual file system.
#include <windows.h>
#include <d3d9.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "gamefiles.h"
#include "hudstyle.h"
#include "keynames.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "sharetechmono_ttf.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static GamePaths g_paths;
static Ini g_ini;
static std::string g_status;  // the last action's result
static bool g_statusError;

static void Status(bool error, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    g_status = buf;
    g_statusError = error;
}

// ANSI (the game's files) to UTF-8 (ImGui), and back.
static std::string Utf8(const std::string& s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, nullptr, 0, nullptr, nullptr);
    std::string out(m, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, &out[0], m, nullptr, nullptr);
    return out;
}

static std::string AnsiFromWide(const std::wstring& w)
{
    int m = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(m, 0);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &out[0], m, nullptr, nullptr);
    return out;
}

static std::string Ansi(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return AnsiFromWide(w);
}

// ---- settings --------------------------------------------------------------------------------
// The vanilla launcher's options (FalloutNVLauncher.exe 1.2.14: its dialogs, the keys it writes and
// the values per slider position), and its presets (Low.ini ... VeryHigh.ini, merged key by key).

struct Choice { int value; const char* label; };
enum Kind { kBool, kBoolInv, kChoices, kInt, kFloat };
enum Tab { kTabDisplay, kTabDetail, kTabWater, kTabShadows, kTabView, kTabDistant, kTabMod, kTabCount };
static const char* kTabNames[kTabCount] = {"Display", "Detail", "Water", "Shadows", "View distance", "Distant LOD", "Mod"};

struct Setting {
    Tab tab;
    const char* label;
    const char* section;
    const char* key;
    Kind kind;
    float lo, hi;
    const char* fmt;
    float def;
    const Choice* choices;
    int nchoices;
    const char* tip;
};

static const Choice kAA[] = {{0, "off"}, {2, "2x"}, {4, "4x"}, {8, "8x"}};
static const Choice kAniso[] = {{1, "off"}, {2, "2x"}, {4, "4x"}, {8, "8x"}, {15, "15x"}};
static const Choice kTexture[] = {{2, "small"}, {1, "medium"}, {0, "large"}};
static const Choice kLowHigh[] = {{0, "low"}, {1, "medium"}, {2, "high"}};
static const Choice kWaterMs[] = {{1, "off"}, {2, "2x"}, {4, "4x"}, {8, "8x"}};
static const Choice kShadowRes[] = {{256, "low"}, {512, "medium"}, {1024, "high"}, {2048, "ultra"}};
#define CH(a) a, (int)(sizeof(a) / sizeof(a[0]))

static const Setting kSettings[] = {
    {kTabDisplay, "Windowed", "Display", "bFull Screen", kBoolInv, 0, 1, nullptr, 1, nullptr, 0, "Run in a window instead of full screen."},
    {kTabDisplay, "Vertical sync", "Display", "iPresentInterval", kBool, 0, 1, nullptr, 1, nullptr, 0, "Wait for the display's refresh."},
    {kTabDisplay, "Antialiasing", "Display", "iMultiSample", kChoices, 0, 0, nullptr, 0, CH(kAA),
     "Multisampling. With supersampling the mod limits it so that both together stay within 64 samples per pixel."},
    {kTabDisplay, "Anisotropic filtering", "Display", "iMaxAnisotropy", kChoices, 0, 0, nullptr, 1, CH(kAniso), "Sharper textures at grazing angles."},
    {kTabDetail, "Texture quality", "Display", "iTexMipMapSkip", kChoices, 0, 0, nullptr, 0, CH(kTexture), "Texture resolution (mip levels skipped)."},
    {kTabDetail, "Decal cap", "Display", "iMaxDecalsPerFrame", kInt, 2, 10, "%d", 10, nullptr, 0, "Decals (blood, bullet holes) added per frame."},
    {kTabDetail, "Radial blur quality", "Imagespace", "iRadialBlurLevel", kChoices, 0, 0, nullptr, 2, CH(kLowHigh), nullptr},
    {kTabDetail, "Depth of field", "Imagespace", "bDoDepthOfField", kBool, 0, 1, nullptr, 1, nullptr, 0, nullptr},
    {kTabDetail, "Transparency multisampling", "Display", "bTransparencyMultisampling", kBool, 0, 1, nullptr, 0, nullptr, 0,
     "Antialiased edges on alpha-tested textures (foliage, fences)."},
    {kTabWater, "Water refractions", "Water", "bUseWaterRefractions", kBool, 0, 1, nullptr, 1, nullptr, 0, nullptr},
    {kTabWater, "Water reflections", "Water", "bUseWaterReflections", kBool, 0, 1, nullptr, 1, nullptr, 0, nullptr},
    {kTabWater, "Water displacement", "Water", "bUseWaterDisplacements", kBool, 0, 1, nullptr, 1, nullptr, 0, "Ripples from rain and movement."},
    {kTabWater, "Depth fog", "Water", "bUseWaterDepth", kBool, 0, 1, nullptr, 1, nullptr, 0, nullptr},
    {kTabWater, "Full detail reflections", "Water", "bForceHighDetailReflections", kBool, 0, 1, nullptr, 0, nullptr, 0, nullptr},
    {kTabWater, "Full scene reflections", "Water", "bAutoWaterSilhouetteReflections", kBoolInv, 0, 1, nullptr, 1, nullptr, 0,
     "Reflect the whole scene instead of silhouettes."},
    {kTabWater, "Water multisampling", "Display", "iWaterMultisamples", kChoices, 0, 0, nullptr, 1, CH(kWaterMs), nullptr},
    {kTabShadows, "Enable shadows", "Display", "bDrawShadows", kBool, 0, 1, nullptr, 0, nullptr, 0, "Actor shadows."},
    {kTabShadows, "Max exterior shadows", "Display", "iActorShadowCountExt", kInt, 2, 6, "%d", 2, nullptr, 0, nullptr},
    {kTabShadows, "Max interior shadows", "Display", "iActorShadowCountInt", kInt, 2, 6, "%d", 2, nullptr, 0, nullptr},
    {kTabShadows, "Shadow quality", "Display", "iShadowMapResolution", kChoices, 0, 0, nullptr, 1024, CH(kShadowRes),
     "Shadow map size: 256, 512, 1024 or 2048 pixels (2048 is beyond the vanilla launcher)."},
    {kTabShadows, "Shadow filtering", "Display", "iShadowFilter", kChoices, 0, 0, nullptr, 2, CH(kLowHigh), nullptr},
    {kTabView, "Grass fade", "Grass", "fGrassStartFadeDistance", kFloat, 0, 7000, "%.0f", 7000, nullptr, 0, nullptr},
    {kTabView, "Actor fade", "LOD", "fLODFadeOutMultActors", kFloat, 2, 15, "%.1f", 15, nullptr, 0, nullptr},
    {kTabView, "Item fade", "LOD", "fLODFadeOutMultItems", kFloat, 1, 15, "%.1f", 15, nullptr, 0, nullptr},
    {kTabView, "Object fade", "LOD", "fLODFadeOutMultObjects", kFloat, 1, 15, "%.1f", 15, nullptr, 0, nullptr},
    {kTabView, "Shadow fade", "Display", "fShadowLODStartFade", kFloat, 100, 1000, "%.0f", 1000, nullptr, 0, nullptr},
    {kTabView, "Light fade", "Display", "fLightLODStartFade", kFloat, 200, 3500, "%.0f", 3500, nullptr, 0, nullptr},
    {kTabView, "Specularity fade", "Display", "fSpecularLODStartFade", kFloat, 200, 2000, "%.0f", 2000, nullptr, 0, nullptr},
    {kTabDistant, "Tree LOD fade", "TerrainManager", "fTreeLoadDistance", kFloat, 0, 40000, "%.0f", 40000, nullptr, 0, nullptr},
    {kTabDistant, "Object LOD fade", "TerrainManager", "fBlockLoadDistanceLow", kFloat, 25000, 50000, "%.0f", 50000, nullptr, 0, nullptr},
    {kTabDistant, "Land quality", "TerrainManager", "fSplitDistanceMult", kFloat, 0.75f, 1.5f, "%.2f", 1.5f, nullptr, 0, nullptr},
};
static const int kSettingCount = sizeof(kSettings) / sizeof(kSettings[0]);
static float g_value[kSettingCount];
static IniFile g_where[kSettingCount];

// Settings with their own rows.
static int g_adapter, g_width, g_height, g_effects;  // effects: 0 none, 1 bloom, 2 HDR
static int g_reflect;                                 // water reflection quality 0..2 (256 / 512 / 1024), -1 other
static bool g_softReflect;
static float g_supersample = 1;
static IniFile g_sizeWhere;
// The mod's keys and screenshot folder (rowe.ini; key names as KeyFromName reads them).
static int g_menuKey = VK_F8, g_shotKey = VK_F12;
static std::string g_shotDir;  // ANSI, as in the INI file
static bool g_autosave;          // rowe.ini autosave / autosaveminutes / autosaveslots (the plugin's autosave)
static int g_autosaveMin = 20, g_autosaveSlots = 3;
static int g_photoKey = VK_F10;  // rowe.ini photokey (a shortcut: key | Ctrl / Alt), photofast, photohud, photofreeze
static int g_photoFast = 5;
static bool g_photoHud = true, g_photoFreeze;
static bool g_slideshow = true;  // [launcher] slideshow: the loading screens behind the launcher
static int g_slideBright = 50;   // [launcher] slidebrightness: how much of them shows through, percent
static int g_fontBase = 28;      // [launcher] textsize: the text size in pixels for a kFontRefW x kFontRefH window
static int g_fontUsed = 28;      // g_fontBase as applied (the slider's value once let go)
static int g_glowStrength = 100; // [launcher] glowstrength: the text glow's darkness, percent of the standard (0 = none)
static int g_glowSize = 100;     // [launcher] glowsize: how far it reaches, percent of the standard
static const float kFontRefW = 1350.f, kFontRefH = 800.f;

static int ReadModKey(const char* key, const char* def)
{
    char v[32];
    GetPrivateProfileStringA("rowe", key, def, v, sizeof(v), g_paths.modIni.c_str());
    int vk = KeyFromName(v);
    return vk ? vk : KeyFromName(def);
}

static float ReadValue(const Setting& s)
{
    if (s.kind == kFloat) return g_ini.GetFloat(s.section, s.key, s.def);
    return (float)g_ini.GetInt(s.section, s.key, (int)s.def);
}

static void Reload()
{
    for (int i = 0; i < kSettingCount; ++i) {
        g_value[i] = ReadValue(kSettings[i]);
        g_where[i] = g_ini.Where(kSettings[i].section, kSettings[i].key);
    }
    g_adapter = g_ini.GetInt("Display", "iAdapter", 0);
    g_width = g_ini.GetInt("Display", "iSize W", 1280);
    g_height = g_ini.GetInt("Display", "iSize H", 720);
    g_sizeWhere = g_ini.Where("Display", "iSize W");
    bool hdr = g_ini.GetInt("BlurShaderHDR", "bDoHighDynamicRange", 1) != 0, bloom = g_ini.GetInt("BlurShader", "bUseBlurShader", 0) != 0;
    g_effects = hdr ? 2 : bloom ? 1 : 0;
    int rw = g_ini.GetInt("Water", "iWaterReflectWidth", 512);
    g_reflect = rw == 256 ? 0 : rw == 512 ? 1 : rw == 1024 ? 2 : -1;
    g_softReflect = g_ini.GetInt("Water", "bUseWaterReflectionBlur", 1) != 0;
    char v[32];
    GetPrivateProfileStringA("rowe", "supersample", "1", v, sizeof(v), g_paths.modIni.c_str());
    g_supersample = (float)atof(v);
    if (g_supersample < 1) g_supersample = 1;
    g_menuKey = ReadModKey("menukey", "F8");
    g_shotKey = ReadModKey("screenshotkey", "F12");
    char dir[MAX_PATH];
    GetPrivateProfileStringA("rowe", "screenshotdir", "", dir, sizeof(dir), g_paths.modIni.c_str());
    g_shotDir = dir;
    g_autosave = GetPrivateProfileIntA("rowe", "autosave", 0, g_paths.modIni.c_str()) != 0;
    g_autosaveMin = (int)GetPrivateProfileIntA("rowe", "autosaveminutes", 20, g_paths.modIni.c_str());
    if (g_autosaveMin < 1) g_autosaveMin = 1;
    if (g_autosaveMin > 240) g_autosaveMin = 240;
    g_autosaveSlots = (int)GetPrivateProfileIntA("rowe", "autosaveslots", 3, g_paths.modIni.c_str());
    if (g_autosaveSlots < 1) g_autosaveSlots = 1;
    if (g_autosaveSlots > 10) g_autosaveSlots = 10;
    {
        char k[48];
        GetPrivateProfileStringA("rowe", "photokey", "F10", k, sizeof(k), g_paths.modIni.c_str());
        g_photoKey = ShortcutFromName(k);
        if (!g_photoKey) g_photoKey = VK_F10;
    }
    g_photoFast = (int)GetPrivateProfileIntA("rowe", "photofast", 5, g_paths.modIni.c_str());
    if (g_photoFast < 1) g_photoFast = 1;
    if (g_photoFast > 50) g_photoFast = 50;
    g_photoHud = GetPrivateProfileIntA("rowe", "photohud", 1, g_paths.modIni.c_str()) != 0;
    g_photoFreeze = GetPrivateProfileIntA("rowe", "photofreeze", 0, g_paths.modIni.c_str()) != 0;
    g_slideshow = GetPrivateProfileIntA("launcher", "slideshow", 1, g_paths.modIni.c_str()) != 0;
    g_slideBright = (int)GetPrivateProfileIntA("launcher", "slidebrightness", 50, g_paths.modIni.c_str());
    if (g_slideBright < 10) g_slideBright = 10;
    if (g_slideBright > 100) g_slideBright = 100;
    g_fontBase = (int)GetPrivateProfileIntA("launcher", "textsize", 28, g_paths.modIni.c_str());
    if (g_fontBase < 14) g_fontBase = 14;
    if (g_fontBase > 48) g_fontBase = 48;
    g_fontUsed = g_fontBase;
    g_glowStrength = (int)GetPrivateProfileIntA("launcher", "glowstrength", 100, g_paths.modIni.c_str());
    if (g_glowStrength < 0) g_glowStrength = 0;
    if (g_glowStrength > 200) g_glowStrength = 200;
    g_glowSize = (int)GetPrivateProfileIntA("launcher", "glowsize", 100, g_paths.modIni.c_str());
    if (g_glowSize < 25) g_glowSize = 25;
    if (g_glowSize > 300) g_glowSize = 300;
}

static void WriteValue(int i)
{
    const Setting& s = kSettings[i];
    bool ok = s.kind == kFloat ? g_ini.SetFloat(s.section, s.key, g_value[i]) : g_ini.SetInt(s.section, s.key, (int)g_value[i]);
    g_where[i] = g_ini.Where(s.section, s.key);
    if (ok) Status(false, "%s: saved to %s", s.label, g_where[i] == kIniCustom ? "FalloutCustom.ini" : "FalloutPrefs.ini");
    else Status(true, "%s: could not write the INI file (%lu)", s.label, GetLastError());
}

static void ApplyPreset(const char* file, const char* name)
{
    std::string path = g_paths.game + file;
    char sections[4096];
    DWORD n = GetPrivateProfileSectionNamesA(sections, sizeof(sections), path.c_str());
    if (!n) { Status(true, "%s not found in the game folder", file); return; }
    int count = 0;
    for (const char* sec = sections; *sec; sec += strlen(sec) + 1) {
        char keys[16384];
        DWORD k = GetPrivateProfileSectionA(sec, keys, sizeof(keys), path.c_str());
        for (const char* kv = keys; k && *kv; kv += strlen(kv) + 1) {
            const char* eq = strchr(kv, '=');
            if (!eq) continue;
            std::string key(kv, eq - kv);
            g_ini.Set(sec, key.c_str(), eq + 1);
            ++count;
        }
    }
    Reload();
    Status(false, "%s preset applied (%d settings from %s)", name, count, file);
}

// ---- Direct3D --------------------------------------------------------------------------------

static IDirect3D9* g_d3d;
static IDirect3DDevice9* g_dev;
static D3DPRESENT_PARAMETERS g_pp;
static UINT g_resizeW, g_resizeH;

struct Mode { int w, h; };
static std::vector<std::string> g_adapterNames;
static std::vector<Mode> g_modes;
static bool g_aaSupported[4] = {true, true, true, true};
static int g_maxTexture = 16384;

static void EnumerateAdapter()
{
    g_adapterNames.clear();
    for (UINT a = 0; a < g_d3d->GetAdapterCount(); ++a) {
        D3DADAPTER_IDENTIFIER9 id = {};
        g_d3d->GetAdapterIdentifier(a, 0, &id);
        g_adapterNames.push_back(id.Description);
    }
    UINT adapter = g_adapter >= 0 && g_adapter < (int)g_d3d->GetAdapterCount() ? g_adapter : 0;
    g_modes.clear();
    for (UINT i = 0, n = g_d3d->GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8); i < n; ++i) {
        D3DDISPLAYMODE m;
        if (FAILED(g_d3d->EnumAdapterModes(adapter, D3DFMT_X8R8G8B8, i, &m)) || m.Width < 640) continue;
        bool dup = false;
        for (const Mode& o : g_modes) dup |= o.w == (int)m.Width && o.h == (int)m.Height;
        if (!dup) g_modes.push_back({(int)m.Width, (int)m.Height});
    }
    std::sort(g_modes.begin(), g_modes.end(), [](const Mode& a, const Mode& b) { return a.w != b.w ? a.w > b.w : a.h > b.h; });
    for (int i = 1; i < 4; ++i)
        g_aaSupported[i] = SUCCEEDED(g_d3d->CheckDeviceMultiSampleType(adapter, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, FALSE,
                                                                        (D3DMULTISAMPLE_TYPE)kAA[i].value, nullptr));
    D3DCAPS9 caps;
    if (SUCCEEDED(g_d3d->GetDeviceCaps(adapter, D3DDEVTYPE_HAL, &caps))) g_maxTexture = (int)caps.MaxTextureWidth;
}

static IDirect3DTexture9* g_shot;
static int g_shotFor = -2, g_shotW, g_shotH;

static void ResetDevice()
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    if (g_dev->Reset(&g_pp) == D3DERR_INVALIDCALL) return;
    ImGui_ImplDX9_CreateDeviceObjects();
}

// ---- saves -----------------------------------------------------------------------------------

static std::vector<SaveInfo> g_saves;
static int g_selected = 0;  // -1: the main menu, else an index into g_saves

static void LoadSaves()
{
    g_saves = ListSaves(g_paths.saves);
    g_selected = g_saves.empty() ? -1 : 0;
}

static void UpdateShot()
{
    if (g_shotFor == g_selected) return;
    g_shotFor = g_selected;
    if (g_shot) { g_shot->Release(); g_shot = nullptr; }
    if (g_selected < 0 || g_selected >= (int)g_saves.size()) return;
    const SaveInfo& s = g_saves[g_selected];
    std::vector<unsigned char> px;
    if (!LoadSaveShot(g_paths.saves, s, &px)) return;
    if (FAILED(g_dev->CreateTexture(s.shotW, s.shotH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_shot, nullptr))) return;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_shot->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < s.shotH; ++y) memcpy((BYTE*)lr.pBits + y * lr.Pitch, px.data() + (size_t)y * s.shotW * 4, (size_t)s.shotW * 4);
        g_shot->UnlockRect(0);
    }
    g_shotW = s.shotW;
    g_shotH = s.shotH;
}

static std::string SaveDate(const SaveInfo& s)
{
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&s.written, &local);
    FileTimeToSystemTime(&local, &st);
    char buf[64];
    wsprintfA(buf, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// ---- loading screen slideshow ----------------------------------------------------------------
// The game's loading screen pictures behind the launcher, crossfading in random order with a slow
// zoom. They are the textures of the game's LSCR records, textures\interface\loading\
// loading_<kind>NN.dds (loose or from the archives, so replacers show too). A thread reads them;
// the textures are made on the main thread, one per frame. rowe.ini [launcher] slideshow=0 turns
// it off.

struct Slide { IDirect3DTexture9* tex; int w, h; };
static std::vector<Slide> g_slides;
static std::thread g_slideThread;
static std::atomic<bool> g_slideStop;
static std::mutex g_slideLock;
static std::vector<std::vector<unsigned char>> g_slideFiles;  // read, not yet made into textures
static int g_slideCur = -1, g_slideNext = -1;
static double g_slideBegan;     // when the current slide began fading in
static std::vector<int> g_slideDeck;  // the slides still to show in this round, shuffled
static const double kSlideHold = 9.0, kSlideFade = 1.6;
static const float kSlideZoom = 0.06f;  // zoom over one slide's time

static bool IsLoadingScreen(const char* n)
{
    static const char* kKinds[] = {"billboard", "building", "bulletinboard", "desktop", "magazines", "posters"};
    if (strncmp(n, "loading_", 8)) return false;
    for (const char* k : kKinds) {
        size_t l = strlen(k);
        const char* d = n + 8 + l;
        if (strncmp(n + 8, k, l) || !isdigit((unsigned char)*d)) continue;
        while (isdigit((unsigned char)*d)) ++d;
        return !strcmp(d, ".dds");
    }
    return false;
}

static void StartSlideshow()
{
    if (g_slideThread.joinable() || !g_slideshow) return;
    GamePaths paths = g_paths;
    Ini ini = g_ini;
    g_slideThread = std::thread([paths, ini] {
        ReadGameFolder(paths, ini, "textures\\interface\\loading", IsLoadingScreen, [](const std::string&, std::vector<unsigned char>& data) {
            std::lock_guard<std::mutex> lock(g_slideLock);
            g_slideFiles.push_back(std::move(data));
            return !g_slideStop;
        }, &g_slideStop);
    });
}

static void StopSlideshow()
{
    g_slideStop = true;
    if (g_slideThread.joinable()) g_slideThread.join();
    for (Slide& s : g_slides) s.tex->Release();
    g_slides.clear();
}

// A DDS file's first level as a texture: DXT1/3/5 as they are, or 32-bit BGRA.
static bool MakeSlide(const std::vector<unsigned char>& d, Slide* out)
{
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4)) return false;
    int h = *(const int*)(d.data() + 12), w = *(const int*)(d.data() + 16);
    DWORD pfFlags = *(const DWORD*)(d.data() + 80), fourcc = *(const DWORD*)(d.data() + 84), bits = *(const DWORD*)(d.data() + 88);
    DWORD rMask = *(const DWORD*)(d.data() + 92);
    if (w < 16 || h < 16 || w > g_maxTexture || h > g_maxTexture) return false;
    D3DFORMAT fmt;
    size_t rowBytes;
    int rows;
    if (pfFlags & 4) {  // compressed
        if (fourcc != D3DFMT_DXT1 && fourcc != D3DFMT_DXT3 && fourcc != D3DFMT_DXT5) return false;
        fmt = (D3DFORMAT)fourcc;
        rowBytes = (size_t)((w + 3) / 4) * (fourcc == D3DFMT_DXT1 ? 8 : 16);
        rows = (h + 3) / 4;
    } else if (bits == 32 && rMask == 0x00FF0000) {
        fmt = D3DFMT_X8R8G8B8;  // the slides are drawn opaque
        rowBytes = (size_t)w * 4;
        rows = h;
    } else return false;
    if (d.size() < 128 + rowBytes * rows) return false;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(g_dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_MANAGED, &t, nullptr))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, nullptr, 0))) { t->Release(); return false; }
    for (int y = 0; y < rows; ++y) memcpy((BYTE*)lr.pBits + (size_t)y * lr.Pitch, d.data() + 128 + (size_t)y * rowBytes, rowBytes);
    t->UnlockRect(0);
    *out = {t, w, h};
    return true;
}

static int NextSlide()
{
    if (g_slideDeck.empty()) {
        for (int i = 0; i < (int)g_slides.size(); ++i)
            if (i != g_slideCur || g_slides.size() == 1) g_slideDeck.push_back(i);
        for (int i = (int)g_slideDeck.size() - 1; i > 0; --i) std::swap(g_slideDeck[i], g_slideDeck[rand() % (i + 1)]);
    }
    int n = g_slideDeck.back();
    g_slideDeck.pop_back();
    return n;
}

// Texture alpha off for the slides (some have a partial alpha channel), vertex alpha for the fades.
static void SlideAlphaOn(const ImDrawList*, const ImDrawCmd*) { g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); }
static void SlideAlphaOff(const ImDrawList*, const ImDrawCmd*) { g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); }

static void DrawSlideshow()
{
    if (!g_slideshow) return;
    {
        std::vector<unsigned char> file;
        {
            std::lock_guard<std::mutex> lock(g_slideLock);
            if (!g_slideFiles.empty()) {
                file = std::move(g_slideFiles.back());
                g_slideFiles.pop_back();
            }
        }
        Slide sl;
        if (!file.empty() && MakeSlide(file, &sl)) {
            g_slides.push_back(sl);
            g_slideDeck.insert(g_slideDeck.begin() + rand() % (g_slideDeck.size() + 1), (int)g_slides.size() - 1);
        }
    }
    if (g_slides.empty()) return;
    double now = ImGui::GetTime();
    if (g_slideCur < 0) {
        g_slideCur = NextSlide();
        g_slideBegan = now;
    }
    double age = now - g_slideBegan;
    if (age >= kSlideHold && g_slideNext < 0 && g_slides.size() > 1) g_slideNext = NextSlide();
    if (g_slideNext >= 0 && age >= kSlideHold + kSlideFade) {
        g_slideCur = g_slideNext;
        g_slideNext = -1;
        g_slideBegan += kSlideHold;
        age = now - g_slideBegan;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    auto draw = [&](int i, double a, double alpha) {
        const Slide& sl = g_slides[i];
        float zoom = 1.f + kSlideZoom * (float)fmin(a / (kSlideHold + kSlideFade), 1.0);
        float sc = fmaxf(ds.x / sl.w, ds.y / sl.h) * zoom, w = sl.w * sc, h = sl.h * sc;
        ImVec2 p((ds.x - w) * 0.5f, (ds.y - h) * 0.5f);
        dl->AddImage((ImTextureID)(intptr_t)sl.tex, p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, (int)(255 * fmin(fmax(alpha, 0.0), 1.0))));
    };
    dl->AddCallback(SlideAlphaOn, nullptr);
    draw(g_slideCur, age, age / kSlideFade);  // the first one fades in from black
    if (g_slideNext >= 0) draw(g_slideNext, age - kSlideHold, (age - kSlideHold) / kSlideFade);
    dl->AddCallback(SlideAlphaOff, nullptr);
}

// ---- drawing ---------------------------------------------------------------------------------

static float g_px;            // the text size in pixels: g_fontUsed scaled with the window (FontPx)
static float g_hudColor[3];
static HWND g_hwnd;

// The text size for the window's client area: g_fontUsed at kFontRefW x kFontRefH, scaled with the
// smaller of the width and height ratios (so the layout fits a narrow window too). 0: minimized.
static float FontPx()
{
    RECT rc;
    if (!GetClientRect(g_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return 0.f;
    float k = fminf(rc.right / kFontRefW, rc.bottom / kFontRefH);
    return fminf(96.f, fmaxf(10.f, floorf(g_fontUsed * k + 0.5f)));
}

static const char* WhereNote(IniFile w) { return w == kIniCustom ? "set in FalloutCustom.ini: changes are written there" : nullptr; }

static void LabelCell(const char* label, const char* tip, IniFile where)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip) HudTip(tip);
    if (where == kIniCustom) {
        ImGui::SameLine();
        ImGui::TextDisabled("*");
        HudTip(WhereNote(where));
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

static void DrawSetting(int i)
{
    const Setting& s = kSettings[i];
    LabelCell(s.label, s.tip, g_where[i]);
    ImGui::PushID(i);
    switch (s.kind) {
    case kBool:
    case kBoolInv: {
        bool on = (g_value[i] != 0) != (s.kind == kBoolInv);
        if (HudCheckbox("##b", &on)) {
            g_value[i] = (float)(on != (s.kind == kBoolInv));
            WriteValue(i);
        }
        break;
    }
    case kChoices: {
        bool known = false;
        for (int c = 0; c < s.nchoices; ++c) {
            if (c) ImGui::SameLine();
            int cur = (int)g_value[i];
            if (s.choices == kAA && cur == 1) cur = 0;  // the presets write 1 for off
            bool on = cur == s.choices[c].value;
            known |= on;
            bool unsupported = s.choices == kAA && !g_aaSupported[c];
            ImGui::BeginDisabled(unsupported);
            if (HudOption(s.choices[c].label, on) && !on) {
                g_value[i] = (float)s.choices[c].value;
                WriteValue(i);
            }
            ImGui::EndDisabled();
        }
        if (!known) {
            ImGui::SameLine();
            ImGui::TextDisabled("(now %d)", (int)g_value[i]);
        }
        break;
    }
    case kInt: {
        int v = (int)g_value[i];
        if (HudSliderInt("##i", &v, (int)s.lo, (int)s.hi, s.fmt)) g_value[i] = (float)v;
        if (ImGui::IsItemDeactivatedAfterEdit()) WriteValue(i);
        break;
    }
    case kFloat: {
        float v = g_value[i];
        if (HudSliderFloat("##f", &v, s.lo, s.hi, s.fmt)) {
            // The vanilla launcher's steps: whole numbers, or 0.05 below 2.
            v = s.hi <= 2.f ? floorf(v * 20.f + 0.5f) / 20.f : s.hi <= 20.f ? floorf(v * 10.f + 0.5f) / 10.f : floorf(v + 0.5f);
            g_value[i] = v;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) WriteValue(i);
        break;
    }
    }
    ImGui::PopID();
}

static const float kSupersampleSteps[] = {1.f, 2.f, 2.25f, 3.f, 4.f, 6.f, 8.f, 9.f, 10.f, 12.f, 16.f, 25.f, 36.f, 49.f, 64.f};

static void FactorLabel(char* out, int size, float f)
{
    if (f <= 1.01f) { _snprintf(out, size, "off"); return; }
    float s = sqrtf(f);
    int w = (int)(g_width * s + 0.5f) & ~1, h = (int)(g_height * s + 0.5f) & ~1;
    _snprintf(out, size, "%gx  (%dx%d)%s", f, w, h, (w > 16384 || h > 16384) ? "  limited to 16384" : "");
}

static void Aspect(char* out, int size, int w, int h)
{
    int a = w, b = h;
    while (b) { int t = a % b; a = b; b = t; }
    int rw = a ? w / a : 0, rh = a ? h / a : 0;
    if (rw == 8 && rh == 5) { rw = 16; rh = 10; }
    if (rw > 64) _snprintf(out, size, "%.2f:1", (float)w / h);
    else _snprintf(out, size, "%d:%d", rw, rh);
}

// Your resolution presets: rowe.ini [launcher] resolutions=1400x1400,3440x1440,...
static std::vector<Mode> g_presets;
static int g_typedFrom;

static void LoadPresets()
{
    char v[2048];
    GetPrivateProfileStringA("launcher", "resolutions", "", v, sizeof(v), g_paths.modIni.c_str());
    g_presets.clear();
    for (char* t = strtok(v, ", "); t; t = strtok(nullptr, ", ")) {
        int w, h;
        if (sscanf(t, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) g_presets.push_back({w, h});
    }
}

static void SavePresets()
{
    std::string v;
    for (const Mode& m : g_presets) {
        char t[32];
        wsprintfA(t, "%s%dx%d", v.empty() ? "" : ",", m.w, m.h);
        v += t;
    }
    if (!WritePrivateProfileStringA("launcher", "resolutions", v.empty() ? nullptr : v.c_str(), g_paths.modIni.c_str()))
        Status(true, "Could not write %s", g_paths.modIni.c_str());
}

static bool IsMode(int w, int h)
{
    for (const Mode& m : g_modes)
        if (m.w == w && m.h == h) return true;
    return false;
}

static void SetResolution(int w, int h)
{
    g_width = w;
    g_height = h;
    g_ini.SetInt("Display", "iSize W", w);
    g_ini.SetInt("Display", "iSize H", h);
    g_sizeWhere = g_ini.Where("Display", "iSize W");
    Status(false, "Resolution %dx%d saved to %s", w, h, g_sizeWhere == kIniCustom ? "FalloutCustom.ini" : "FalloutPrefs.ini");
}

static void DrawDisplayRows()
{
    // Adapter
    LabelCell("Graphics adapter", nullptr, g_ini.Where("Display", "iAdapter"));
    const char* cur = g_adapter < (int)g_adapterNames.size() ? g_adapterNames[g_adapter].c_str() : "?";
    if (ImGui::BeginCombo("##adapter", cur)) {
        for (int a = 0; a < (int)g_adapterNames.size(); ++a)
            if (ImGui::Selectable(g_adapterNames[a].c_str(), a == g_adapter)) {
                g_adapter = a;
                g_ini.SetInt("Display", "iAdapter", a);
                g_ini.Set("Display", "sD3DDevice", ("\"" + g_adapterNames[a] + "\"").c_str());
                EnumerateAdapter();
                Status(false, "Graphics adapter: %s", g_adapterNames[a].c_str());
            }
        ImGui::EndCombo();
    }
    // Resolution: your presets, then the monitor's modes, and any size typed in.
    LabelCell("Resolution", "The game's window or screen size (iSize W / H).", g_sizeWhere);
    char lab[96], asp[32];
    Aspect(asp, sizeof(asp), g_width, g_height);
    _snprintf(lab, sizeof(lab) - 1, "%dx%d  (%s)", g_width, g_height, asp);
    lab[sizeof(lab) - 1] = 0;
    if (ImGui::BeginCombo("##res", lab, ImGuiComboFlags_HeightLarge)) {
        bool listed = false;
        for (const Mode& m : g_presets) listed |= m.w == g_width && m.h == g_height;
        for (const Mode& m : g_modes) listed |= m.w == g_width && m.h == g_height;
        if (!listed) ImGui::Selectable(lab, true);
        if (!g_presets.empty()) ImGui::SeparatorText("your presets");
        for (const Mode& m : g_presets) {
            Aspect(asp, sizeof(asp), m.w, m.h);
            char l[64];
            _snprintf(l, sizeof(l) - 1, "%dx%d  (%s)##p", m.w, m.h, asp);
            l[sizeof(l) - 1] = 0;
            if (ImGui::Selectable(l, m.w == g_width && m.h == g_height)) SetResolution(m.w, m.h);
        }
        ImGui::SeparatorText("display modes");
        for (const Mode& m : g_modes) {
            Aspect(asp, sizeof(asp), m.w, m.h);
            char l[64];
            _snprintf(l, sizeof(l) - 1, "%dx%d  (%s)", m.w, m.h, asp);
            l[sizeof(l) - 1] = 0;
            if (ImGui::Selectable(l, m.w == g_width && m.h == g_height)) SetResolution(m.w, m.h);
        }
        ImGui::EndCombo();
    }
    LabelCell("Custom size", "Any width and height: use it now, or keep it in the list above as a preset (stored in rowe.ini).", kIniNone);
    static int typedW = -1, typedH = -1;
    if (typedW < 0) { typedW = g_width; typedH = g_height; }
    float numW = ImGui::CalcTextSize("000000").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(numW);
    ImGui::InputInt("##cw", &typedW, 0, 0);
    ImGui::SameLine();
    ImGui::TextUnformatted("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(numW);
    ImGui::InputInt("##ch", &typedH, 0, 0);
    bool valid = typedW >= 320 && typedH >= 240 && typedW <= 16384 && typedH <= 16384;
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("use")) SetResolution(typedW, typedH);
    ImGui::SameLine();
    bool saved = false;
    for (const Mode& m : g_presets) saved |= m.w == typedW && m.h == typedH;
    if (!saved && ImGui::Button("save preset")) {
        g_presets.push_back({typedW, typedH});
        SavePresets();
        SetResolution(typedW, typedH);
        Status(false, "Preset %dx%d saved", typedW, typedH);
    }
    if (saved && ImGui::Button("delete preset")) {
        for (size_t k = 0; k < g_presets.size(); ++k)
            if (g_presets[k].w == typedW && g_presets[k].h == typedH) { g_presets.erase(g_presets.begin() + k); break; }
        SavePresets();
        Status(false, "Preset %dx%d deleted", typedW, typedH);
    }
    ImGui::EndDisabled();
    if (g_typedFrom != g_width * 65536 + g_height) {  // the size changed elsewhere: show it here
        g_typedFrom = g_width * 65536 + g_height;
        typedW = g_width;
        typedH = g_height;
    }
    for (int i = 0; i < kSettingCount; ++i) {
        if (kSettings[i].tab != kTabDisplay) continue;
        DrawSetting(i);
        if (!strcmp(kSettings[i].key, "bFull Screen") && g_value[i] != 0 && !IsMode(g_width, g_height)) {
            ImGui::SameLine();
            ImGui::TextDisabled("no display mode: borderless, centred");
            HudTip("Full screen needs one of the monitor's display modes. At other sizes ROWE runs the game in a borderless "
                   "window of that size, centred on a black background.");
        }
    }
    // Screen effects
    LabelCell("Screen effects", "HDR lighting, bloom, or neither.", g_ini.Where("BlurShaderHDR", "bDoHighDynamicRange"));
    static const char* kEffects[] = {"none", "bloom", "HDR"};
    for (int e = 0; e < 3; ++e) {
        if (e) ImGui::SameLine();
        if (HudOption(kEffects[e], g_effects == e) && g_effects != e) {
            g_effects = e;
            g_ini.SetInt("BlurShader", "bUseBlurShader", e == 1);
            g_ini.SetInt("BlurShaderHDR", "bDoHighDynamicRange", e == 2);
            Status(false, "Screen effects: %s", kEffects[e]);
        }
    }
    // Supersampling (the mod's)
    LabelCell("Supersampling", "ROWE renders at a larger size and scales it down to the resolution above: sharper edges and detail, "
                               "at the cost of speed. The value is the number of pixels rendered per pixel shown.",
              kIniNone);
    char ss[96];
    FactorLabel(ss, sizeof(ss), g_supersample);
    if (ImGui::BeginCombo("##ss", ss, ImGuiComboFlags_HeightLarge)) {
        for (float f : kSupersampleSteps) {
            char l[96];
            FactorLabel(l, sizeof(l), f);
            if (ImGui::Selectable(l, fabsf(f - g_supersample) < 0.01f)) {
                g_supersample = f;
                char v[16];
                _snprintf(v, sizeof(v) - 1, "%g", f);
                v[sizeof(v) - 1] = 0;
                if (WritePrivateProfileStringA("rowe", "supersample", v, g_paths.modIni.c_str())) Status(false, "Supersampling %gx saved to rowe.ini", f);
                else Status(true, "Could not write %s", g_paths.modIni.c_str());
            }
        }
        ImGui::EndCombo();
    }
}

static void DrawWaterRows()
{
    for (int i = 0; i < kSettingCount; ++i)
        if (kSettings[i].tab == kTabWater) DrawSetting(i);
    static const int kRes[] = {256, 512, 1024};
    auto blurAmount = [](int q) { return q == 2 ? 4 : 2; };  // the vanilla launcher's iWaterBlurAmount per quality
    LabelCell("Reflection quality", "Reflection texture size: 256, 512 or 1024 pixels.", g_ini.Where("Water", "iWaterReflectWidth"));
    for (int q = 0; q < 3; ++q) {
        if (q) ImGui::SameLine();
        if (HudOption(kLowHigh[q].label, g_reflect == q) && g_reflect != q) {
            g_reflect = q;
            g_ini.SetInt("Water", "iWaterReflectWidth", kRes[q]);
            g_ini.SetInt("Water", "iWaterReflectHeight", kRes[q]);
            if (g_softReflect) g_ini.SetInt("Water", "iWaterBlurAmount", blurAmount(q));
            Status(false, "Reflection quality: %d pixels", kRes[q]);
        }
    }
    LabelCell("Soft reflections", "Blurred water reflections.", g_ini.Where("Water", "bUseWaterReflectionBlur"));
    if (HudCheckbox("##soft", &g_softReflect)) {
        g_ini.SetInt("Water", "bUseWaterReflectionBlur", g_softReflect);
        if (g_softReflect) g_ini.SetInt("Water", "iWaterBlurAmount", blurAmount(g_reflect < 0 ? 1 : g_reflect));
        Status(false, "Soft reflections %s", g_softReflect ? "on" : "off");
    }
}

// ---- the mod's keys and screenshot folder ----------------------------------------------------

static int* g_captureInto;  // the key the next key press sets (g_menuKey, g_shotKey or g_photoKey), or null

static void WriteMod(const char* key, const char* value, const char* what)
{
    if (WritePrivateProfileStringA("rowe", key, value, g_paths.modIni.c_str())) Status(false, "%s saved to rowe.ini", what);
    else Status(true, "Could not write %s (%lu)", g_paths.modIni.c_str(), GetLastError());
}

// A key press while "press a key..." is shown (from the window procedure). Esc cancels; a key one
// of the other two already uses is refused (one key would do two things).
static bool CaptureKey(int vk)
{
    if (!g_captureInto) return false;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN)
        return true;  // a modifier alone is no key
    int* into = g_captureInto;
    g_captureInto = nullptr;
    if (vk == VK_ESCAPE) { Status(false, "Key unchanged"); return true; }
    // The photo shortcut takes Ctrl / Alt held with it; no key may be one another already uses.
    int value = vk;
    if (into == &g_photoKey) value |= ShortcutModifiersHeld();
    char name[48];
    ShortcutName(value, name, sizeof(name));
    const char* taken = into != &g_menuKey && value == g_menuKey             ? "menu"
                        : into != &g_shotKey && value == g_shotKey           ? "screenshot"
                        : into != &g_photoKey && value == g_photoKey         ? "photo mode"
                                                                             : nullptr;
    if (taken) {
        Status(true, "%s is already the %s key", name, taken);
        return true;
    }
    *into = value;
    const char* what = into == &g_menuKey ? "Menu" : into == &g_shotKey ? "Screenshot" : "Photo mode";
    char status[96];
    _snprintf(status, sizeof(status) - 1, "%s key %s", what, name);
    status[sizeof(status) - 1] = 0;
    WriteMod(into == &g_menuKey ? "menukey" : into == &g_shotKey ? "screenshotkey" : "photokey", name, status);
    return true;
}

// The folder the mod saves screenshots to, resolved as its ScreenshotFolder does: empty = next to
// the DLL, %VARIABLES% expanded, relative paths from the game folder.
static std::string ShotFolder()
{
    if (g_shotDir.empty()) return g_paths.game + "Data\\NVSE\\Plugins\\";
    char raw[MAX_PATH], full[MAX_PATH];
    ExpandEnvironmentStringsA(g_shotDir.c_str(), raw, MAX_PATH);
    std::string p = raw;
    bool absolute = (p.size() > 1 && p[1] == ':') || (!p.empty() && (p[0] == '\\' || p[0] == '/'));
    if (!absolute) p = g_paths.game + p;
    if (GetFullPathNameA(p.c_str(), MAX_PATH, full, nullptr)) p = full;
    if (!p.empty() && p.back() != '\\' && p.back() != '/') p += '\\';
    return p;
}

static void BrowseShotFolder()
{
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Screenshot folder");
        wchar_t start[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, ShotFolder().c_str(), -1, start, MAX_PATH);
        IShellItem* folder = nullptr;
        if (GetFileAttributesW(start) != INVALID_FILE_ATTRIBUTES && SUCCEEDED(SHCreateItemFromParsingName(start, nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->Show(GetActiveWindow())) && SUCCEEDED(dlg->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                g_shotDir = AnsiFromWide(path);
                CoTaskMemFree(path);
                WriteMod("screenshotdir", g_shotDir.c_str(), ("Screenshot folder " + Utf8(g_shotDir)).c_str());
            }
            item->Release();
        }
        dlg->Release();
    } else Status(true, "The folder picker is not available");
    if (com) CoUninitialize();
}

static void DrawModRows()
{
    for (int k = 0; k < 2; ++k) {
        bool menu = k == 0;
        int* key = menu ? &g_menuKey : &g_shotKey;
        LabelCell(menu ? "Menu key" : "Screenshot key",
                  menu ? "Opens and closes the mod's settings menu in the game. Click, then press the key to use (Esc cancels). "
                         "Pick one the game does not use: F8 is free in its default controls (F9 is QuickLoad)."
                       : "Saves a screenshot in the game (size and format: in the mod's menu). Click, then press the key to use "
                         "(Esc cancels).",
                  kIniNone);
        char name[32], btn[64];
        KeyName(*key, name, sizeof(name));
        if (g_captureInto == key) _snprintf(btn, sizeof(btn) - 1, "press a key...###k%d", k);
        else _snprintf(btn, sizeof(btn) - 1, "%s###k%d", name, k);
        btn[sizeof(btn) - 1] = 0;
        if (ImGui::Button(btn, ImVec2(-FLT_MIN, 0))) g_captureInto = g_captureInto == key ? nullptr : key;
    }
    LabelCell("Screenshot folder", "Where screenshots are saved. %VARIABLES% work, relative paths start at the game folder; "
                                   "empty saves next to the mod's DLL (Data\\NVSE\\Plugins). Press Enter to apply.",
              kIniNone);
    static char edit[MAX_PATH * 3];
    static bool editing;
    if (!editing) lstrcpynA(edit, Utf8(g_shotDir).c_str(), sizeof(edit));
    ImGui::InputTextWithHint("##shotdir", "next to the DLL", edit, sizeof(edit));
    editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_shotDir = Ansi(edit);
        WriteMod("screenshotdir", g_shotDir.c_str(), g_shotDir.empty() ? "Screenshot folder (next to the DLL)" : ("Screenshot folder " + std::string(edit)).c_str());
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    if (ImGui::Button("browse")) BrowseShotFolder();
    HudTip("Choose the folder in Windows' folder picker.");
    ImGui::SameLine();
    if (ImGui::Button("open")) {
        std::string dir = ShotFolder();
        SHCreateDirectoryExA(nullptr, dir.c_str(), nullptr);  // the mod creates it too, at its first screenshot
        if ((INT_PTR)ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
            Status(true, "Could not open %s", Utf8(dir).c_str());
    }
    HudTip("Open the folder in Explorer.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Utf8(ShotFolder()).c_str());
    // The mod's autosave: on/off, the minutes typed in, the number of slots.
    LabelCell("Autosave", "Saves the game every so many minutes of play (menus, loading screens and time with the game in the "
                          "background do not count) as \"ROWE Autosave 1\", 2, ... in turn. During combat it waits until the "
                          "fight is over. Any save, your own too, starts the count again.",
              kIniNone);
    if (HudCheckbox("every##autosave", &g_autosave)) WriteMod("autosave", g_autosave ? "1" : "0", g_autosave ? "Autosave on" : "Autosave off");
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_autosave);
    float numW = ImGui::CalcTextSize("0000").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(numW);
    int minutes = g_autosaveMin;
    if (ImGui::InputInt("##autosavemin", &minutes, 0, 0)) g_autosaveMin = minutes < 1 ? 1 : minutes > 240 ? 240 : minutes;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char v[16], what[64];
        wsprintfA(v, "%d", g_autosaveMin);
        wsprintfA(what, "Autosave every %d min", g_autosaveMin);
        WriteMod("autosaveminutes", v, what);
    }
    HudTip("Minutes of play between autosaves, 1-240. Press Enter or click elsewhere to apply.");
    ImGui::SameLine();
    ImGui::TextUnformatted("minutes");
    ImGui::EndDisabled();
    LabelCell("Autosave slots", "How many autosaves are kept (1-10): the oldest is overwritten.", kIniNone);
    ImGui::BeginDisabled(!g_autosave);
    ImGui::SetNextItemWidth(numW);
    int slots = g_autosaveSlots;
    if (ImGui::InputInt("##autosaveslots", &slots, 0, 0)) g_autosaveSlots = slots < 1 ? 1 : slots > 10 ? 10 : slots;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char v[16], what[64];
        wsprintfA(v, "%d", g_autosaveSlots);
        wsprintfA(what, "Autosave slots: %d", g_autosaveSlots);
        WriteMod("autosaveslots", v, what);
    }
    HudTip("How many autosaves are kept (1-10): the oldest is overwritten.");
    ImGui::EndDisabled();
    // Photo mode: its shortcut, the fast speed, the HUD and the frozen world.
    LabelCell("Photo mode shortcut", "Starts and ends photo mode in the game (you fly, invisible and "
                                     "undetectable). Click, then press the key, with Ctrl and/or Alt if you like (Shift is the "
                                     "speed key; Ctrl is also the game's sneak key, Alt is free). Esc cancels.",
              kIniNone);
    {
        char name[48], btn[80];
        ShortcutName(g_photoKey, name, sizeof(name));
        if (g_captureInto == &g_photoKey) _snprintf(btn, sizeof(btn) - 1, "press a key...###photokey");
        else _snprintf(btn, sizeof(btn) - 1, "%s###photokey", name);
        btn[sizeof(btn) - 1] = 0;
        if (ImGui::Button(btn, ImVec2(-FLT_MIN, 0))) g_captureInto = g_captureInto == &g_photoKey ? nullptr : &g_photoKey;
    }
    LabelCell("Photo fast speed", "How much faster you fly in photo mode while Shift is held.", kIniNone);
    HudSliderInt("##photofast", &g_photoFast, 1, 50, "%dx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char v[16], what[64];
        wsprintfA(v, "%d", g_photoFast);
        wsprintfA(what, "Photo fast speed %dx", g_photoFast);
        WriteMod("photofast", v, what);
    }
    LabelCell("Photo: hide the HUD", "Hides the HUD while photo mode is on.", kIniNone);
    if (HudCheckbox("##photohud", &g_photoHud)) WriteMod("photohud", g_photoHud ? "1" : "0", g_photoHud ? "Photo mode hides the HUD" : "Photo mode keeps the HUD");
    LabelCell("Photo: freeze people", "Stops people and creatures where they are while photo mode is on (weather and effects go on).", kIniNone);
    if (HudCheckbox("##photofreeze", &g_photoFreeze))
        WriteMod("photofreeze", g_photoFreeze ? "1" : "0", g_photoFreeze ? "Photo mode freezes people" : "Photo mode lets people move");
    LabelCell("Launcher background", "The game's loading screens as a slideshow behind this launcher (also those of loading "
                                     "screen replacers). Stored in rowe.ini, [launcher] slideshow=.",
              kIniNone);
    if (HudCheckbox("loading screens##slides", &g_slideshow)) {
        if (g_slideshow) StartSlideshow();
        WritePrivateProfileStringA("launcher", "slideshow", g_slideshow ? "1" : "0", g_paths.modIni.c_str());
        Status(false, "Loading screen slideshow %s", g_slideshow ? "on" : "off");
    }
    if (g_slideshow && !g_slides.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d pictures", (int)g_slides.size());
    }
    LabelCell("Background brightness", "How much of the loading screens shows through behind the launcher (the rest is the "
                                        "window's dark background). Stored in rowe.ini, [launcher] slidebrightness=.",
              kIniNone);
    ImGui::BeginDisabled(!g_slideshow);
    HudSliderInt("##bright", &g_slideBright, 10, 100, "%d%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char v[16];
        _snprintf(v, sizeof(v) - 1, "%d", g_slideBright);
        v[sizeof(v) - 1] = 0;
        if (WritePrivateProfileStringA("launcher", "slidebrightness", v, g_paths.modIni.c_str())) Status(false, "Background brightness %d%% saved to rowe.ini", g_slideBright);
        else Status(true, "Could not write %s", g_paths.modIni.c_str());
    }
    ImGui::EndDisabled();
    LabelCell("Font size", "The launcher's text size in pixels for a window of 1350 x 800; it grows and shrinks with the "
                           "window (\"now\": at its current size). Applies when you let go. Stored in rowe.ini, [launcher] textsize=.",
              kIniNone);
    char fontFmt[32];
    _snprintf(fontFmt, sizeof(fontFmt) - 1, "%%d px  now %d", (int)g_px);
    fontFmt[sizeof(fontFmt) - 1] = 0;
    HudSliderInt("##font", &g_fontBase, 14, 48, fontFmt);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_fontUsed = g_fontBase;
        char v[16];
        _snprintf(v, sizeof(v) - 1, "%d", g_fontBase);
        v[sizeof(v) - 1] = 0;
        if (WritePrivateProfileStringA("launcher", "textsize", v, g_paths.modIni.c_str())) Status(false, "Font size %d px saved to rowe.ini", g_fontBase);
        else Status(true, "Could not write %s", g_paths.modIni.c_str());
    }
    // The glow around text on the background (AddTextShadows).
    for (int k = 0; k < 2; ++k) {
        bool strength = k == 0;
        LabelCell(strength ? "Text glow" : "Glow size",
                  strength ? "How dark the glow around the text on the background is: 100% is the standard, 0 turns it off. "
                             "Stored in rowe.ini, [launcher] glowstrength=."
                           : "How far the glow reaches around the letters: 100% is the standard. Stored in rowe.ini, [launcher] glowsize=.",
                  kIniNone);
        int* value = strength ? &g_glowStrength : &g_glowSize;
        ImGui::BeginDisabled(!strength && g_glowStrength == 0);
        HudSliderInt(strength ? "##glow" : "##glowsize", value, strength ? 0 : 25, strength ? 200 : 300, "%d%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char v[16];
            _snprintf(v, sizeof(v) - 1, "%d", *value);
            v[sizeof(v) - 1] = 0;
            if (WritePrivateProfileStringA("launcher", strength ? "glowstrength" : "glowsize", v, g_paths.modIni.c_str()))
                Status(false, "%s %d%% saved to rowe.ini", strength ? "Text glow" : "Glow size", *value);
            else Status(true, "Could not write %s", g_paths.modIni.c_str());
        }
        ImGui::EndDisabled();
    }
}

static void DrawSettings(float height)
{
    ImGui::BeginChild("settings", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    // Presets, as the vanilla launcher's buttons.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    static const char* kPresetFiles[] = {"Low.ini", "Medium.ini", "High.ini", "VeryHigh.ini"};
    static const char* kPresetNames[] = {"Low", "Medium", "High", "Ultra"};
    for (int i = 0; i < 4; ++i) {
        ImGui::SameLine();
        if (ImGui::Button(kPresetNames[i])) ApplyPreset(kPresetFiles[i], kPresetNames[i]);
        HudTip("Sets the detail, water, shadow and view distance settings like the vanilla launcher's button.");
    }
    float labelW = g_px * 12.5f;
    if (ImGui::BeginTabBar("tabs")) {
        for (int t = 0; t < kTabCount; ++t) {
            if (!ImGui::BeginTabItem(kTabNames[t])) continue;
            if (ImGui::BeginTable("t", 2, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, labelW);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
                if (t == kTabDisplay) DrawDisplayRows();
                else if (t == kTabWater) DrawWaterRows();
                else if (t == kTabMod) DrawModRows();
                else
                    for (int i = 0; i < kSettingCount; ++i)
                        if (kSettings[i].tab == t) DrawSetting(i);
                ImGui::EndTable();
                HudTableFrame(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), labelW);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

// One save in the list: two lines, the chosen one boxed like a Pip-Boy entry.
static bool SaveRow(int index, const char* line1, const char* line2, const char* right, bool on)
{
    ImGui::PushID(index);
    float lineH = ImGui::GetTextLineHeight();
    ImVec2 pad = ImGui::GetStyle().FramePadding;
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(ImGui::GetContentRegionAvail().x, lineH * 2 + pad.y * 3));
    bool dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (on) dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150));  // a backing, as framed text has no shadow
    if (on || ImGui::IsItemHovered()) dl->AddRectFilled(a, b, HudCol(1.f, on ? 0.16f : 0.08f));
    if (on) dl->AddRect(a, b, HudCol(), 0.f, 0, fmaxf(1.f, floorf(HudT() * 0.6f)));
    // The date at the right of the first line, or of the second if the first is too long for it;
    // if neither has room, the line is cut off before it.
    float y1 = a.y + pad.y, y2 = a.y + pad.y * 2 + lineH, rightW = right ? ImGui::CalcTextSize(right).x : 0.f;
    float gap = ImGui::CalcTextSize("  ").x, room = b.x - a.x - pad.x * 2 - rightW - gap;
    float rightY = !right || ImGui::CalcTextSize(line1).x <= room ? y1 : ImGui::CalcTextSize(line2).x <= room ? y2 : y1;
    ImVec2 clipMax(right ? b.x - pad.x - rightW - gap : b.x, b.y);
    dl->PushClipRect(a, rightY == y1 ? clipMax : b, true);
    dl->AddText(ImVec2(a.x + pad.x, y1), on ? HudCol() : HudCol(0.85f), line1);
    dl->PopClipRect();
    dl->PushClipRect(a, rightY == y2 ? clipMax : b, true);
    dl->AddText(ImVec2(a.x + pad.x, y2), HudCol(0.6f), line2);
    dl->PopClipRect();
    if (right) dl->AddText(ImVec2(b.x - pad.x - rightW, rightY), HudCol(0.6f), right);
    ImGui::PopID();
    if (dbl) return true;
    return clicked;
}

static bool g_launch;  // PLAY was pressed (or a save double-clicked)

static void DrawSaves(float height)
{
    ImGui::BeginChild("saves", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (SaveRow(-1, "MAIN MENU", "start without loading a save", nullptr, g_selected == -1)) {
        if (g_selected == -1 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) g_launch = true;
        g_selected = -1;
    }
    for (int i = 0; i < (int)g_saves.size(); ++i) {
        const SaveInfo& s = g_saves[i];
        char l1[256], l2[256];
        if (s.level > 0) _snprintf(l1, sizeof(l1) - 1, "%s  -  level %d", Utf8(s.player).c_str(), s.level);
        else _snprintf(l1, sizeof(l1) - 1, "%s", Utf8(s.file).c_str());
        l1[sizeof(l1) - 1] = 0;
        _snprintf(l2, sizeof(l2) - 1, "%s  %s", Utf8(s.location).c_str(), s.playTime.c_str());
        l2[sizeof(l2) - 1] = 0;
        std::string date = SaveDate(s);
        if (SaveRow(i, l1, l2, date.c_str(), g_selected == i)) {
            if (g_selected == i && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) g_launch = true;
            g_selected = i;
        }
        HudTip(Utf8(s.file).c_str());
    }
    ImGui::EndChild();
}

static bool g_running;

static void DrawLauncher()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    if (g_slideshow && !g_slides.empty()) ImGui::SetNextWindowBgAlpha(1.f - g_slideBright / 100.f);
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextUnformatted("FALLOUT: NEW VEGAS");
    ImGui::SameLine();
    ImGui::TextDisabled("ROWE launcher");
    HudTip("ROWE for Fallout New Vegas: Reinhold Oberst\xC3\xBC" "rcks wunderbare Erweiterungen");
    {
        char where[512];
        if (!g_paths.mo2.empty())
            _snprintf(where, sizeof(where) - 1, "%s  (Mod Organizer 2, profile %s)", g_paths.game.c_str(), g_paths.mo2Profile.c_str());
        else _snprintf(where, sizeof(where) - 1, "%s", g_paths.game.c_str());
        where[sizeof(where) - 1] = 0;
        std::string w = Utf8(where);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(w.c_str()).x);
        ImGui::TextDisabled("%s", w.c_str());
    }
    HudSeparator();

    float footer = g_px * 3.2f;
    float bodyH = ImGui::GetContentRegionAvail().y - footer;
    float leftW = floorf(ImGui::GetContentRegionAvail().x * 0.40f);
    // Left: the chosen save's picture and the list.
    ImGui::BeginChild("left", ImVec2(leftW, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    UpdateShot();
    float picW = ImGui::GetContentRegionAvail().x, picH = floorf(picW * 9.f / 16.f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(picW, picH));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_shot && g_shotW > 0 && g_shotH > 0) {
        // The picture keeps its shape (a game at 1:1 saves square ones): fitted into the box, black around it.
        float scale = fminf(picW / g_shotW, picH / g_shotH), w = g_shotW * scale, h = g_shotH * scale;
        ImVec2 a(floorf(p0.x + (picW - w) * 0.5f), floorf(p0.y + (picH - h) * 0.5f));
        dl->AddRectFilled(p0, ImVec2(p0.x + picW, p0.y + picH), IM_COL32(0, 0, 0, 255));
        dl->AddImage((ImTextureID)(intptr_t)g_shot, a, ImVec2(a.x + w, a.y + h));
    }
    if (!g_shot) {
        const char* t = g_selected < 0 ? "MAIN MENU" : "no picture";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddRectFilled(p0, ImVec2(p0.x + picW, p0.y + picH), HudCol(0.12f, 1.f));
        dl->AddText(ImVec2(p0.x + (picW - ts.x) * 0.5f, p0.y + (picH - ts.y) * 0.5f), HudCol(0.6f), t);
    }
    dl->AddRect(p0, ImVec2(p0.x + picW, p0.y + picH), HudCol(), 0.f, 0, fmaxf(1.f, floorf(HudT() * 0.6f)));
    ImGui::Spacing();
    ImGui::Text("SAVES  (%d)", (int)g_saves.size());
    if (g_saves.empty()) {
        ImGui::TextDisabled("%s", Utf8(g_paths.saves).c_str());
        if (g_paths.mo2.empty() && g_ini.Get("General", "SLocalSavePath", "").find("__MO_") == 0)
            ImGui::TextWrapped("These INI files belong to Mod Organizer 2: start the launcher from MO2 to see that profile's saves.");
    }
    DrawSaves(ImGui::GetContentRegionAvail().y);
    ImGui::EndChild();
    ImGui::SameLine(0, g_px * 1.2f);
    // Right: the settings.
    ImGui::BeginChild("right", ImVec2(0, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::TextUnformatted("SETTINGS");
    DrawSettings(ImGui::GetContentRegionAvail().y);
    ImGui::EndChild();

    // Footer: status and PLAY.
    HudSeparator();
    std::string what = g_selected < 0 ? "Start at the main menu" : "Load " + Utf8(g_saves[g_selected].file);
    float playW = g_px * 8, playH = ImGui::GetFrameHeight() * 1.3f;
    ImGui::BeginGroup();
    ImGui::TextUnformatted(what.c_str());
    if (g_running) ImGui::TextColored(ImVec4(1.f, 0.35f, 0.25f, 1.f), "The game is running.");
    else if (!g_status.empty()) {
        if (g_statusError) ImGui::TextColored(ImVec4(1.f, 0.35f, 0.25f, 1.f), "%s", g_status.c_str());
        else ImGui::TextDisabled("%s", g_status.c_str());
    }
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - playW);
    ImGui::BeginDisabled(g_running);
    ImGui::PushStyleColor(ImGuiCol_Button, HudCol(0.45f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, HudCol(0.65f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, HudCol(0.85f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.03f, 0.03f, 0.03f, 1.f));
    if (ImGui::Button("PLAY", ImVec2(playW, playH))) g_launch = true;
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    HudWindowFrame();
    ImGui::End();
}

// ---- text shadows ----------------------------------------------------------------------------
// A dark glow around the text that stands on the slideshow, for legibility. After ImGui::Render
// each glyph (a quad from the font atlas whose UVs are not its white pixel) gets dark copies in two
// rings around it (8 directions each, the outer ring turned by 22.5 degrees and fainter). The glows
// of a run of glyphs are all drawn before its glyphs, so no letter's glow covers its neighbours.
// Darkness and reach: g_glowStrength, g_glowSize (the Mod tab). Text on a box drawn earlier in the
// same window (buttons, fields, tabs, the chosen option or save, popups, tooltips) gets none; hover
// highlights (fainter than kBoxAlpha) and the launcher window's own background (the whole display)
// are no boxes. (The launcher is built with 32-bit draw indices for the extra vertices.)

static void AddTextShadows(ImDrawData* dd)
{
    if (g_glowStrength <= 0) return;
    static const int kBoxAlpha = 32;  // of 255
    struct Box { float x0, y0, x1, y1; };
    struct Shadow { float dx, dy, alpha; };
    ImGuiIO& io = ImGui::GetIO();
    ImTextureData* fontTex = io.Fonts->TexData;  // (its ID is set only once the backend uploads it)
    ImVec2 white = io.Fonts->TexUvWhitePixel;
    std::vector<Shadow> shadows;
    for (int ring = 1; ring >= 0; --ring)
        for (int i = 0; i < 8; ++i) {
            float r = g_px * (ring ? 0.16f : 0.08f) * g_glowSize / 100.f, angle = (i + ring * 0.5f) * 3.14159265f / 4.f;
            shadows.push_back({r * cosf(angle), r * sinf(angle), fminf(1.f, (ring ? 0.22f : 0.45f) * g_glowStrength / 100.f)});
        }
    const unsigned long long maxIdx = 1ull << (8 * sizeof(ImDrawIdx));
    std::vector<Box> boxes;
    std::vector<ImDrawIdx> glows, glyphs;  // the current run of glyphs
    for (ImDrawList* dl : dd->CmdLists) {
        boxes.clear();
        ImVector<ImDrawIdx> idx;
        idx.reserve(dl->IdxBuffer.Size * 2);
        auto flush = [&] {
            for (ImDrawIdx i : glows) idx.push_back(i);
            for (ImDrawIdx i : glyphs) idx.push_back(i);
            glows.clear();
            glyphs.clear();
        };
        for (ImDrawCmd& cmd : dl->CmdBuffer) {
            int start = idx.Size;
            const ImDrawIdx* src = dl->IdxBuffer.Data + cmd.IdxOffset;
            bool font = !cmd.UserCallback && fontTex && cmd.TexRef._TexData == fontTex;
            for (unsigned k = 0; k < cmd.ElemCount;) {
                ImDrawIdx a = src[k];
                bool quad = k + 6 <= cmd.ElemCount && src[k + 1] == a + 1 && src[k + 2] == a + 2 && src[k + 3] == a && src[k + 4] == a + 2 &&
                            src[k + 5] == a + 3;
                if (!quad) {
                    flush();
                    idx.push_back(src[k]);
                    idx.push_back(src[k + 1]);
                    idx.push_back(src[k + 2]);
                    k += 3;
                    continue;
                }
                ImDrawVert v[4];
                memcpy(v, dl->VtxBuffer.Data + cmd.VtxOffset + a, sizeof(v));
                Box b = {v[0].pos.x, v[0].pos.y, v[0].pos.x, v[0].pos.y};
                for (const ImDrawVert& q : v) {
                    b.x0 = fminf(b.x0, q.pos.x); b.y0 = fminf(b.y0, q.pos.y);
                    b.x1 = fmaxf(b.x1, q.pos.x); b.y1 = fmaxf(b.y1, q.pos.y);
                }
                bool solid = v[0].uv.x == white.x && v[0].uv.y == white.y && v[2].uv.x == white.x && v[2].uv.y == white.y;
                bool glyph = !solid && font;
                if (!glyph) flush();
                if (solid) {
                    bool whole = b.x1 - b.x0 >= io.DisplaySize.x * 0.9f && b.y1 - b.y0 >= io.DisplaySize.y * 0.9f;
                    if ((int)((v[0].col >> IM_COL32_A_SHIFT) & 0xFF) >= kBoxAlpha && !whole) boxes.push_back(b);
                } else if (font && dl->VtxBuffer.Size + 4 * shadows.size() - cmd.VtxOffset <= maxIdx) {
                    float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
                    bool boxed = false;
                    for (const Box& o : boxes) boxed |= cx >= o.x0 && cx <= o.x1 && cy >= o.y0 && cy <= o.y1;
                    for (size_t i = 0; !boxed && i < shadows.size(); ++i) {
                        const Shadow& sh = shadows[i];
                        unsigned base = dl->VtxBuffer.Size - cmd.VtxOffset;
                        for (const ImDrawVert& q : v) {
                            ImDrawVert w = q;
                            w.pos.x += sh.dx;
                            w.pos.y += sh.dy;
                            w.col = (ImU32)(((q.col >> IM_COL32_A_SHIFT) & 0xFF) * sh.alpha) << IM_COL32_A_SHIFT;
                            dl->VtxBuffer.push_back(w);
                        }
                        const unsigned order[] = {0, 1, 2, 0, 2, 3};
                        for (unsigned o : order) glows.push_back((ImDrawIdx)(base + o));
                    }
                }
                for (int i = 0; i < 6; ++i) {
                    if (glyph) glyphs.push_back(src[k + i]);
                    else idx.push_back(src[k + i]);
                }
                k += 6;
            }
            flush();
            cmd.IdxOffset = start;
            cmd.ElemCount = idx.Size - start;
        }
        dl->IdxBuffer.swap(idx);
    }
    dd->TotalIdxCount = dd->TotalVtxCount = 0;
    for (ImDrawList* dl : dd->CmdLists) {
        dd->TotalIdxCount += dl->IdxBuffer.Size;
        dd->TotalVtxCount += dl->VtxBuffer.Size;
    }
}

// ---- window ----------------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    // While a key is being chosen its press goes to CaptureKey, not to ImGui (Space or Enter would
    // press the button again). PrintScreen sends only a key up.
    if (((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && w != VK_SNAPSHOT) || (msg == WM_KEYUP && w == VK_SNAPSHOT))
        if (CaptureKey((int)w)) return 0;
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return true;
    switch (msg) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { g_resizeW = LOWORD(l); g_resizeH = HIWORD(l); }
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

static void HudColorFromIni(float c[3])
{
    std::string v = g_ini.Get("Interface", "uHUDColor", "");
    unsigned long u = v.empty() ? 0xFFB642FFul : strtoul(v.c_str(), nullptr, 0);
    c[0] = ((u >> 24) & 0xFF) / 255.f;
    c[1] = ((u >> 16) & 0xFF) / 255.f;
    c[2] = ((u >> 8) & 0xFF) / 255.f;
    if (c[0] + c[1] + c[2] < 0.3f) { c[0] = 1.f; c[1] = 0.71f; c[2] = 0.26f; }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
    typedef BOOL(WINAPI * SetCtx)(HANDLE);
    if (SetCtx f = (SetCtx)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext")) f((HANDLE)-4);  // per monitor v2

    if (!FindGame(&g_paths)) {
        MessageBoxA(nullptr, "Fallout: New Vegas was not found.\n\nPut rowe_launcher.exe next to FalloutNV.exe (or start it from "
                             "Mod Organizer 2).",
                    "ROWE launcher", MB_ICONERROR);
        return 1;
    }
    g_ini.fallout = g_paths.myGames + "Fallout.ini";
    g_ini.prefs = g_paths.myGames + "FalloutPrefs.ini";
    g_ini.custom = g_paths.myGames + "FalloutCustom.ini";
    // The save folder: Fallout.ini's SLocalSavePath (Mod Organizer 2 sets its own there).
    {
        char v[MAX_PATH];
        GetPrivateProfileStringA("General", "SLocalSavePath", "Saves\\", v, sizeof(v), g_ini.fallout.c_str());
        std::string sp = v;
        if (!sp.empty() && sp.back() != '\\') sp += '\\';
        g_paths.saves = g_paths.myGames + sp;
    }
    Reload();
    LoadSaves();
    LoadPresets();
    srand(GetTickCount());

    WNDCLASSEXA wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, "rowe_launcher", nullptr};
    HICON iconBig = nullptr, iconSmall = nullptr;
    ExtractIconExA((g_paths.game + "FalloutNV.exe").c_str(), 0, &iconBig, &iconSmall, 1);
    wc.hIcon = iconBig;
    wc.hIconSm = iconSmall;
    RegisterClassExA(&wc);
    HDC screen = GetDC(nullptr);
    float dpiScale = GetDeviceCaps(screen, LOGPIXELSY) / 96.f;
    ReleaseDC(nullptr, screen);
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    float winPx = floorf(22.f * dpiScale + 0.5f);  // the window's first size (the text then follows it)
    int ww = (int)fminf(winPx * 62, (work.right - work.left) * 0.92f), wh = (int)fminf(winPx * 38, (work.bottom - work.top) * 0.92f);
    HWND hwnd = g_hwnd = CreateWindowExA(0, wc.lpszClassName, "Fallout: New Vegas - ROWE launcher", WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - ww) / 2,
                                work.top + (work.bottom - work.top - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE

    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) { MessageBoxA(hwnd, "Direct3D 9 is not available.", "ROWE launcher", MB_ICONERROR); return 1; }
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.EnableAutoDepthStencil = FALSE;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_pp, &g_dev))) {
        MessageBoxA(hwnd, "Could not create a Direct3D 9 device.", "ROWE launcher", MB_ICONERROR);
        return 1;
    }
    EnumerateAdapter();
    StartSlideshow();
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    std::string fontUsed;
    g_px = FontPx();
    if (g_px <= 0) g_px = winPx;
    if (!(LoadGameFont(g_paths, g_ini, &fontUsed) && GameFontAdd(io.Fonts, g_px))) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF((void*)g_shareTechMono, (int)g_shareTechMonoSize, g_px, &cfg);
    }
    HudColorFromIni(g_hudColor);
    HudApplyStyle(g_px, g_hudColor, 1.f);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_dev);
    if (!g_ini.Has(g_ini.prefs, "Display", "iSize W"))
        Status(true, "FalloutPrefs.ini not found: start the game (or the vanilla launcher) once first");
    else if (!fontUsed.empty())
        Status(false, "%d saves in %s", (int)g_saves.size(), Utf8(g_paths.saves).c_str());

    DWORD lastCheck = 0;
    bool done = false;
    while (!done) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
            if (m.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (GetTickCount() - lastCheck > 1000) {
            lastCheck = GetTickCount();
            g_running = GameRunning();
        }
        if (g_resizeW && g_resizeH) {
            g_pp.BackBufferWidth = g_resizeW;
            g_pp.BackBufferHeight = g_resizeH;
            g_resizeW = g_resizeH = 0;
            ResetDevice();
        }
        float px = FontPx();
        if (px > 0 && px != g_px) {  // window resized or the slider let go; ImGui bakes the font at the new size itself
            g_px = px;
            HudApplyStyle(g_px, g_hudColor, 1.f);
        }
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawSlideshow();
        DrawLauncher();
        ImGui::EndFrame();
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(8, 7, 5), 1.f, 0);
        if (SUCCEEDED(g_dev->BeginScene())) {
            ImGui::Render();
            AddTextShadows(ImGui::GetDrawData());
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_dev->EndScene();
        }
        if (g_dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST && g_dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
            ResetDevice();
        if (g_launch) {
            g_launch = false;
            if (g_running) continue;
            std::string err;
            if (LaunchGame(g_paths, g_selected < 0 ? "" : g_saves[g_selected].file, &err)) done = true;
            else Status(true, "%s", err.c_str());
        }
    }
    StopSlideshow();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_shot) g_shot->Release();
    g_dev->Release();
    g_d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
