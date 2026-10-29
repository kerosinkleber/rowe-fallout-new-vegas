// Supersampling: the game renders at a higher "virtual" resolution and the frame is averaged down
// to the window at Present. Works in windowed, borderless and fullscreen mode.
//
// gowfov's version was fullscreen-only because its virtual size went through an engine function
// that also sized the window. Here the virtual size only reaches the renderer:
// - INI: the game reads iSize W/H (FalloutPrefs.ini [Display]) with GetPrivateProfileIntA; the
//   mod answers with the virtual size, so every render target, depth surface and viewport the
//   engine sizes from it is virtual. Writes of iSize are kept at the native size.
// - Overrides: a plugin can apply FalloutCustom.ini to the game's settings after those reads.
//   Before the first window is created the mod compares the settings with what it answered and,
//   if they were changed, takes them as the native size (SupersampleSyncSettings).
// - Window: the device window is created with, and the main window sized from, iSize; those
//   calls (CreateWindowExA, AdjustWindowRect(Ex)) get the native size back.
// - Device: CreateDevice / Reset get a native back buffer. The mod creates a virtual-size render
//   target, binds it in the back buffer's place and returns it from GetBackBuffer; the real back
//   buffer is only bound by the mod itself (resolve, screenshot, settings menu).
// The factor is read at start (rowe.ini supersample=, pixel-count multiplier). A changed factor or
// pixel size applies at once ("live change" below): the game resets its device with the new virtual
// size and builds its screen-sized targets again. At factor 1 the virtual back buffer is the
// window's size, so that works from a start without supersampling too.
//
// The pixelated look (pixelsize=N, 2-8) is the same path the other way round: the virtual size is
// the window's divided by N (rounded up, so whole pixels cover the window), and Present shows each
// rendered pixel as N x N window pixels (nearest neighbour, PSPixel): the whole image,
// interface included (pixelhud=1, the default). With pixelhud=0 the interface
// stays sharp: at the pre-UI point the frame is shown in the real back buffer and the engine draws
// its interface there, at the window's resolution (see SupersamplePreUI). pixelhud applies at once.
// It also works with the lens (which warps the small frame before it is shown) and photo mode.
#include <windows.h>
#include <psapi.h>
#include <math.h>
#include <string.h>
#include "fnvfov.h"
#include "downscale_vs.h"
#include "downscale_ps.h"
#include "downscale_fast_ps.h"
#include "pixel_ps.h"

static bool s_active;
static float s_scale = 1;              // per axis
static int s_nativeW, s_nativeH;       // the INI's iSize (or the override's, see SupersampleSyncSettings)
static int s_prefsW, s_prefsH;         // FalloutPrefs.ini's iSize, kept on writes
static int s_virtW, s_virtH;
static IDirect3DTexture9* s_fake;      // what the engine renders into
static IDirect3DSurface9* s_fakeSurface;
static IDirect3DVertexShader9* s_vs;
static IDirect3DPixelShader9* s_ps;
static D3DFORMAT s_format = D3DFMT_X8R8G8B8;
static DWORD s_resolves;
static bool s_overridden;  // the game's size was changed after the mod read iSize: off for this start
static int s_pixel;        // pixelated look: window pixels per rendered pixel (0 = supersampling)

static void SupersampleInstall();

void SupersampleInit()
{
    SupersampleInstall();
    if (g_settings.pixelSize >= 2) {
        s_pixel = g_settings.pixelSize;
        s_active = true;
        s_scale = 1.f / s_pixel;
        Log("supersample: pixelated look, %d window pixels per game pixel, interface %s%s", s_pixel, g_settings.pixelHud ? "pixelated" : "sharp",
            g_settings.supersample > 1.01f ? " (supersample= is ignored)" : "");
        return;
    }
    float f = g_settings.supersample;
    // At factor 1 the virtual back buffer is the window's size (one copy per frame), so the factor
    // can change live later; [dev] alwaysvirtual=0 leaves the game's own back buffer alone then.
    s_active = f > 1.01f || g_settings.devAlwaysVirtual;
    s_scale = f > 1.01f ? sqrtf(f) : 1.f;
    if (s_scale > 8.f) s_scale = 8.f;  // the resolve shader reads up to 9x9 source pixels
    Log("supersample: factor %g (scale %.3f per axis)%s", f, s_scale,
        f > 1.01f ? "" : s_active ? ", virtual back buffer at the window's size (for live changes)" : ", off");
}

bool SupersampleActive() { return s_active && s_virtW > 0; }
int SupersamplePixelSize() { return SupersampleActive() ? s_pixel : 0; }

// The virtual side for a native one: scaled and even (supersampling), or with the pixelated look
// the smallest even size whose pixels, N window pixels each, cover the native side.
static int VirtualSide(int native, float scale, int pixel)
{
    if (pixel) return ((native + pixel - 1) / pixel + 1) & ~1;
    return ((int)(native * scale + 0.5f)) & ~1;
}
static int VirtualSide(int native, float scale) { return VirtualSide(native, scale, s_pixel); }
bool SupersampleOverridden() { return s_overridden; }

// The sizes as soon as the INI has been read (before the device exists).
bool SupersampleVirtualSize(int* nativeW, int* nativeH, int* virtW, int* virtH)
{
    if (!SupersampleActive() || s_nativeW <= 0 || s_nativeH <= 0) return false;
    *nativeW = s_nativeW;
    *nativeH = s_nativeH;
    *virtW = s_virtW;
    *virtH = s_virtH;
    return true;
}

bool SupersampleSizes(int* nativeW, int* nativeH, int* virtW, int* virtH)
{
    if (!SupersampleActive() || !s_fake) return false;
    *nativeW = s_nativeW;
    *nativeH = s_nativeH;
    *virtW = s_virtW;
    *virtH = s_virtH;
    return true;
}

// ---- INI and window --------------------------------------------------------------------------

static bool IsSizeKey(LPCSTR app, LPCSTR key, bool* width)
{
    if (!s_active || !app || !key || IS_INTRESOURCE(app) || IS_INTRESOURCE(key) || lstrcmpiA(app, "Display") != 0) return false;
    if (!lstrcmpiA(key, "iSize W")) { *width = true; return true; }
    if (!lstrcmpiA(key, "iSize H")) { *width = false; return true; }
    return false;
}

// Both window sides are known before the first of them is answered (readBoth reads the other one),
// so one scale fits both: D3D9 textures (and the engine's own check) go up to 16384 per side on
// current cards, and a scale clamped for one side only would distort the picture.
UINT SupersampleIniInt(LPCSTR app, LPCSTR key, UINT value, UINT (*readOther)(bool width))
{
    bool width;
    if (!IsSizeKey(app, key, &width) || (int)value <= 0) return value;
    static float scale = -1;
    if (scale < 0) {
        UINT other = readOther ? readOther(!width) : value;
        UINT longest = value > other ? value : other;
        scale = s_scale;
        if (longest * scale > 16384) {
            scale = 16384.f / longest;
            Log("supersample: scale %.3f limited to %.3f (16384 pixels per side)", s_scale, scale);
        }
    }
    int v = VirtualSide((int)value, scale);
    if (width) { s_prefsW = s_nativeW = value; s_virtW = v; }
    else { s_prefsH = s_nativeH = value; s_virtH = v; }
    Log("supersample: iSize %s %u -> %d", width ? "W" : "H", value, v);
    return (UINT)v;
}

// The game's own antialiasing on top of supersampling: at high factors it adds next to nothing
// but costs a lot of video memory (the scene target holds every sample), so the samples are
// capped at factor x samples <= 128 (16x keeps 8x MSAA, 64x gets 2x). Only what the game reads
// at start is changed; FalloutPrefs.ini keeps the user's value.
static int s_gameMsaa = -1;
static int s_msaaCap = -1;  // after a live change: the most samples a scene target at the virtual size gets

static UINT CapMsaa(UINT value)
{
    float factor = s_scale * s_scale;
    UINT cap = value;
    // factor x samples <= 64: at 36x and up the game's MSAA is off. Next to 6x6 or more supersampling
    // it adds nothing visible but doubles its largest targets (VATS adds more of them, past 12 GB at 64x).
    while (cap > 1 && cap * factor > 64.5f) cap /= 2;  // the game uses 0, 2, 4, 8
    if (cap < 2) cap = 0;
    if (cap != value) Log("supersample: game antialiasing %ux limited to %ux at factor %.0f", value, cap, factor);
    return cap;
}

UINT SupersampleIniMsaa(LPCSTR app, LPCSTR key, UINT value)
{
    if (!s_active || !app || !key || IS_INTRESOURCE(app) || IS_INTRESOURCE(key) || lstrcmpiA(app, "Display") != 0 ||
        lstrcmpiA(key, "iMultiSample") != 0)
        return value;
    s_gameMsaa = (int)value;
    return CapMsaa(value);
}

LPCSTR SupersampleIniWrite(LPCSTR app, LPCSTR key, LPCSTR value, char* buf)
{
    bool width;
    if (s_gameMsaa >= 0 && app && key && !IS_INTRESOURCE(app) && !IS_INTRESOURCE(key) && !lstrcmpiA(app, "Display") &&
        !lstrcmpiA(key, "iMultiSample") && value && !IS_INTRESOURCE(value)) {
        wsprintfA(buf, "%d", s_gameMsaa);
        return buf;
    }
    if (!IsSizeKey(app, key, &width) || !value || IS_INTRESOURCE(value)) return value;
    wsprintfA(buf, "%d", width ? s_prefsW : s_prefsH);
    Log("supersample: prefs write iSize %s \"%s\" kept at %s", width ? "W" : "H", value, buf);
    return buf;
}

// Before the game creates its first window: if iSize in the game's settings is no longer what the
// mod answered (a plugin applied FalloutCustom.ini after the INI reads), that size is the native
// one; the virtual size is derived from it and written back. iMultiSample gets the same cap as
// when it is read from the INI. Writes to FalloutPrefs.ini keep that file's own values.
void SupersampleSyncSettings()
{
    static bool done;
    if (done || !s_active || s_virtW <= 0) return;
    done = true;
    BYTE* ow = FindSettingObject("iSize W:Display");
    BYTE* oh = FindSettingObject("iSize H:Display");
    if (!ow || !oh) return;
    int w = *(int*)(ow + 4), h = *(int*)(oh + 4);
    if ((w == s_virtW && h == s_virtH) || w <= 0 || h <= 0) return;
    float scale = s_scale;
    int longest = w > h ? w : h;
    if (longest * scale > 16384) scale = 16384.f / longest;
    s_nativeW = w;
    s_nativeH = h;
    s_virtW = VirtualSide(w, scale);
    s_virtH = VirtualSide(h, scale);
    *(int*)(ow + 4) = s_virtW;
    *(int*)(oh + 4) = s_virtH;
    Log("supersample: iSize was changed to %dx%d after the INI reads (e.g. FalloutCustom.ini): native %dx%d, virtual %dx%d "
        "(scale %.3f)", w, h, w, h, s_virtW, s_virtH, scale);
    if (BYTE* om = FindSettingObject("iMultiSample:Display")) {
        UINT v = *(UINT*)(om + 4), cap = CapMsaa(v);
        if (cap != v) *(UINT*)(om + 4) = cap;
    }
}

// ---- live change -----------------------------------------------------------------------------
// A new factor or pixel size while the game runs: the next Present reports a lost device (as the
// devicelost test does), the game releases its device resources and resets with its own present
// parameters, and at that Reset they get the new virtual size (SupersampleApplyPending). The game
// keeps that copy for later Resets, and builds its screen-sized targets (scene, bloom, menu blur)
// again from the new back buffer size; the virtual back buffer is created at it after the Reset.
static bool s_pending;
static int s_pendW, s_pendH, s_pendPixel;
static float s_pendScale;
static DWORD s_changes;
static int s_startW, s_startH;  // the virtual size the device was created with
static int s_prevW, s_prevH;    // the virtual size before the last live change
static DWORD s_sinceChange;  // frames presented since the last live change (logs the memory after 60)
// Undoing the last live change: what was shown before it, and the game's own Resets since.
static bool s_canUndo;
static bool s_undoing;  // the pending change is an undo
static float s_prevFactor;
static int s_prevPixel;
static DWORD s_changeTick;
static int s_resetsAfter;
static float s_followSS = -1;  // the settings the live size last followed
static int s_followPx = -1;

// The process's committed memory and its largest free address block (a 32-bit process runs out of
// address space before it runs out of memory), in MB.
static void ProcessMemory(double* privateMb, double* largestFreeMb)
{
    PROCESS_MEMORY_COUNTERS_EX pmc = {sizeof(pmc)};
    *privateMb = K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)) ? pmc.PrivateUsage / 1048576.0 : 0;
    SIZE_T largest = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (BYTE* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi); p = (BYTE*)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State == MEM_FREE && mbi.RegionSize > largest) largest = mbi.RegionSize;
        if ((BYTE*)mbi.BaseAddress + mbi.RegionSize < p) break;
    }
    *largestFreeMb = largest / 1048576.0;
}

// The virtual size for a factor (or pixel size, 2-8) at the native size; scale per axis.
static void SizesFor(float factor, int pixel, float* scale, int* w, int* h)
{
    float sc = pixel ? 1.f / pixel : factor > 1.01f ? sqrtf(factor) : 1.f;
    if (sc > 8.f) sc = 8.f;
    int longest = s_nativeW > s_nativeH ? s_nativeW : s_nativeH;
    if (!pixel && longest * sc > 16384) sc = 16384.f / longest;
    *scale = sc;
    *w = VirtualSide(s_nativeW, sc, pixel);
    *h = VirtualSide(s_nativeH, sc, pixel);
}

bool SupersampleLive() { return s_active && !s_overridden && s_nativeW > 0 && s_virtW > 0; }

const char* SupersampleRequest(float factor, int pixelSize)
{
    if (!SupersampleLive()) return s_overridden ? "off this start" : "supersampling was off at start";
    int pixel = pixelSize >= 2 ? (pixelSize > 8 ? 8 : pixelSize) : 0;
    float scale;
    int w, h;
    SizesFor(factor, pixel, &scale, &w, &h);
    if (w == s_virtW && h == s_virtH && pixel == s_pixel) return "unchanged";
    s_pendW = w;
    s_pendH = h;
    s_pendScale = scale;
    s_pendPixel = pixel;
    s_pending = true;
    Log("supersample: change to %s %g: virtual %dx%d -> %dx%d at the next Reset", pixel ? "pixel size" : "factor",
        pixel ? (double)pixel : (double)factor, s_virtW, s_virtH, w, h);
    D3DSimulateDeviceLost();
    return nullptr;
}

bool SupersamplePending() { return s_pending; }
DWORD SupersampleChanges() { return s_changes; }

// Goes back to what was shown before the last live change, with the settings (the menu's Revert,
// its countdown, or a change the game could not take).
void SupersampleUndo(const char* why)
{
    if (!s_canUndo) return;
    s_canUndo = false;
    Log("supersample: live change undone (%s)", why);
    g_settings.pixelSize = s_prevPixel ? s_prevPixel : 1;
    if (!s_prevPixel) g_settings.supersample = s_prevFactor;
    SettingsSave();
    s_followSS = g_settings.supersample;
    s_followPx = g_settings.pixelSize;
    s_undoing = SupersampleRequest(s_prevFactor, s_prevPixel) == nullptr;
}

bool SupersampleCanUndo() { return s_canUndo; }

// Once per frame: a factor or pixel size changed in the settings (the menu, the MCM, a command, a
// reload) is shown at once, outside loading screens.
void SupersampleFollowSettings()
{
    if (s_followSS < 0) {
        s_followSS = g_settings.supersample;
        s_followPx = g_settings.pixelSize;
        return;
    }
    if (s_followSS == g_settings.supersample && s_followPx == g_settings.pixelSize) return;
    if (s_pending || ((BYTE*)0x011F308F)[1007]) return;  // a change under way, or a loading screen
    s_followSS = g_settings.supersample;
    s_followPx = g_settings.pixelSize;
    if (!SupersampleLive()) return;  // applies at the next start
    const char* why = SupersampleRequest(g_settings.supersample, g_settings.pixelSize >= 2 ? g_settings.pixelSize : 0);
    if (why) Log("supersample: settings %g / pixel size %d not applied live (%s)", g_settings.supersample, g_settings.pixelSize, why);
}

// Depth surface creation after a live change: Gamebryo creates its depth surfaces again at the size
// they had (the start's or the one before the change). A multisampled one belongs to a multisampled
// scene target, which is made at the current size (Hook_CreateRenderTextureData), so it gets that
// size exactly. The back buffer's own (no MSAA) is also bound with small targets (shadow maps), so it
// only grows: D3D9 needs a depth surface at least as large as the render target.
bool SupersampleDepthSize(UINT* w, UINT* h, D3DMULTISAMPLE_TYPE ms)
{
    bool before = ((int)*w == s_startW && (int)*h == s_startH) || ((int)*w == s_prevW && (int)*h == s_prevH);
    if (!s_changes || !before || ((int)*w == s_virtW && (int)*h == s_virtH)) return false;
    if (ms > D3DMULTISAMPLE_NONMASKABLE) {
        *w = s_virtW;
        *h = s_virtH;
        return true;
    }
    if (s_virtW <= (int)*w && s_virtH <= (int)*h) return false;
    if (s_virtW > (int)*w) *w = s_virtW;
    if (s_virtH > (int)*h) *h = s_virtH;
    return true;
}

// Render textures after a live change: at a Reset Gamebryo creates each render texture's data again
// at its buffer's size (NiRenderedTexture +0x30, an Ni2DBuffer: width +8, height +0xC), which is
// the screen size it was made for (or a fraction: bloom and luminance use 1/2, 1/4, 1/16 ... 1/1024).
// The engine sets its viewports from the current size and reuses such textures from its pool (the
// MO2 list draws its world into one), so a frame smaller than before filled only a corner of them.
// Their buffers get the same fraction of the current virtual size first, so the texture, its buffer
// and the engine's viewports agree. (0x00E8FF60: creates the data of a render texture.)
static bool ScreenFraction(int w, int h, int fromW, int fromH, int* outW, int* outH)
{
    static const int kFractions[] = {1, 2, 4, 16, 64, 256, 1024};
    if (fromW <= 0 || (fromW == s_virtW && fromH == s_virtH)) return false;
    for (int k : kFractions) {
        if (w != fromW / k || h != fromH / k) continue;
        *outW = s_virtW / k > 0 ? s_virtW / k : 1;
        *outH = s_virtH / k > 0 ? s_virtH / k : 1;
        return true;
    }
    return false;
}

typedef void*(__cdecl* CreateRenderTextureDataFn)(BYTE* texture, void* arg, void* renderer);
static CreateRenderTextureDataFn s_createRenderTextureData;
static DWORD s_buffersResized;
// Only what Gamebryo recreates after a live change's Reset (before the next Present): textures the
// engine creates later have the size it wants (a fraction of the new size can equal another
// fraction of an old one: 15x8 is 1/1024 of 15360x8640 and 1/256 of 3840x2160).
static bool s_recreating;

void SupersampleRecreating(bool on) { s_recreating = on && s_changes; }

static void* __cdecl Hook_CreateRenderTextureData(BYTE* texture, void* arg, void* renderer)
{
    BYTE* buffer = s_recreating && texture ? *(BYTE**)(texture + 0x30) : nullptr;
    if (buffer && !IsBadWritePtr(buffer + 8, 8)) {
        int w = *(int*)(buffer + 8), h = *(int*)(buffer + 0xC), nw, nh;
        if (ScreenFraction(w, h, s_prevW, s_prevH, &nw, &nh) || ScreenFraction(w, h, s_startW, s_startH, &nw, &nh)) {
            if (++s_buffersResized <= 200) Log("supersample: render texture %p %dx%d made %dx%d (live change)", texture, w, h, nw, nh);
            *(int*)(buffer + 8) = nw;
            *(int*)(buffer + 0xC) = nh;
        }
    }
    return s_createRenderTextureData(texture, arg, renderer);
}

static void SupersampleInstall()
{
    static const BYTE kStart[] = {0x83, 0xEC, 0x08, 0x53, 0x55};  // sub esp, 8; push ebx; push ebp
    s_createRenderTextureData = (CreateRenderTextureDataFn)InstallDetour(0x00E8FF60, kStart, sizeof(kStart),
                                                                         (void*)Hook_CreateRenderTextureData, "render texture data (live supersampling)");
}

// Render target / depth surface creation: the samples for one at the virtual size after a live
// change, capped for the new factor (never more than the game asked for).
D3DMULTISAMPLE_TYPE SupersampleMsaaFor(UINT w, UINT h, D3DMULTISAMPLE_TYPE ms)
{
    if (s_msaaCap < 0 || ms <= D3DMULTISAMPLE_NONMASKABLE || (int)w != s_virtW || (int)h != s_virtH || (UINT)ms <= (UINT)s_msaaCap) return ms;
    return s_msaaCap >= 2 ? (D3DMULTISAMPLE_TYPE)s_msaaCap : D3DMULTISAMPLE_NONE;
}

void SupersampleApplyPending(D3DPRESENT_PARAMETERS* pp)
{
    // The game resetting by itself right after a change: it cannot keep its targets at that size
    // (36x with 8x MSAA reset over and over before the cap), so the change is undone.
    if (!s_pending && s_canUndo && GetTickCount() - s_changeTick < 10000 && ++s_resetsAfter >= 3) {
        SupersampleUndo("the game kept resetting its device at that size");
        OverlayNotice("Resolution change undone: the game could not run at that size");
    }
    if (!s_pending || !pp) return;
    s_pending = false;
    if (!s_active || (int)pp->BackBufferWidth != s_virtW || (int)pp->BackBufferHeight != s_virtH) {
        Log("supersample: change dropped: Reset asks for %ux%u, not the virtual %dx%d", pp->BackBufferWidth, pp->BackBufferHeight, s_virtW,
            s_virtH);
        return;
    }
    int oldW = s_virtW, oldH = s_virtH;
    s_prevW = oldW;
    s_prevH = oldH;
    s_prevFactor = s_scale * s_scale;
    s_prevPixel = s_pixel;
    s_canUndo = !s_undoing;
    s_undoing = false;
    s_changeTick = GetTickCount();
    s_resetsAfter = 0;
    s_virtW = s_pendW;
    s_virtH = s_pendH;
    s_scale = s_pendScale;
    s_pixel = s_pendPixel;
    pp->BackBufferWidth = s_virtW;
    pp->BackBufferHeight = s_virtH;
    // The game's own copies of the size (read again by some code after start).
    BYTE* ow = FindSettingObject("iSize W:Display");
    BYTE* oh = FindSettingObject("iSize H:Display");
    if (ow && oh && *(int*)(ow + 4) == oldW && *(int*)(oh + 4) == oldH) {
        *(int*)(ow + 4) = s_virtW;
        *(int*)(oh + 4) = s_virtH;
    }
    // The game's antialiasing is set at start (its sample count, 0x011C70CC, and each target keeps
    // its own); changing it live crashed. Its scene targets at the new size get the cap instead
    // (SupersampleMsaaFor), so a high factor does not keep 8x MSAA (36x with 8x: 7 GB, and the
    // game reset its device over and over).
    if (s_gameMsaa >= 0) s_msaaCap = CapMsaa((UINT)s_gameMsaa);
    // The screen size globals (both pairs set at start by 0x004DA670): 0x0118947C/80 (read through
    // 0x004DC1F0/0x004DC200 by the interface, e.g. Tile::Update's scissor rectangles, which clipped
    // the pause menu's buttons away, and by render code), and 0x011F9430/4 (the render target pool
    // sizes its textures from them; a plugin in the MO2 list made one more pool size read them).
    static int* const kScreenSizes[] = {(int*)0x0118947C, (int*)0x011F9430};
    for (int* screen : kScreenSizes) {
        if (screen[0] == oldW && screen[1] == oldH) {
            screen[0] = s_virtW;
            screen[1] = s_virtH;
        } else if (screen[0] || screen[1])
            Log("supersample: screen size globals at %p hold %dx%d, not %dx%d (left)", screen, screen[0], screen[1], oldW, oldH);
    }
    if (BYTE* rd = *(BYTE**)0x011F4748)
        if (*(int*)(rd + 0xA98) == oldW && *(int*)(rd + 0xA9C) == oldH) {
            *(int*)(rd + 0xA98) = s_virtW;
            *(int*)(rd + 0xA9C) = s_virtH;
        }
    ++s_changes;
    s_sinceChange = 1;
    Log("supersample: changed live (#%lu): virtual %dx%d -> %dx%d, %s", s_changes, oldW, oldH, s_virtW, s_virtH,
        s_pixel ? "pixelated" : "supersampled");
}

bool SupersampleFixSize(int* w, int* h)
{
    // After live changes the game sizes its window at a device reset with the size it started
    // with (or the one before the change): those are virtual sizes too.
    bool virt = (*w == s_virtW && *h == s_virtH) || (s_changes && ((*w == s_startW && *h == s_startH) || (*w == s_prevW && *h == s_prevH)));
    if (!SupersampleActive() || !virt || (*w == s_nativeW && *h == s_nativeH)) return false;
    *w = s_nativeW;
    *h = s_nativeH;
    return true;
}

// ---- device ----------------------------------------------------------------------------------

// Swaps the requested (virtual) back buffer size for the native one; returns true if it did.
bool SupersampleFixPresentParams(D3DPRESENT_PARAMETERS* pp)
{
    if (!SupersampleActive() || !pp) return false;
    if ((int)pp->BackBufferWidth != s_virtW || (int)pp->BackBufferHeight != s_virtH) {
        // Something set the game's size after the mod answered the iSize reads (for example a
        // FalloutCustom.ini applied by another plugin): the engine no longer renders at the virtual
        // size, so binding the virtual back buffer would only break Reset. Off for this start.
        Log("supersample: back buffer %ux%u is not the virtual size %dx%d (iSize %dx%d was changed after the mod read "
            "it, e.g. by FalloutCustom.ini); supersampling off for this start",
            pp->BackBufferWidth, pp->BackBufferHeight, s_virtW, s_virtH, s_nativeW, s_nativeH);
        s_active = false;
        s_overridden = true;
        return false;
    }
    pp->BackBufferWidth = s_nativeW;
    pp->BackBufferHeight = s_nativeH;
    s_format = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
    return true;
}

void SupersampleRestorePresentParams(D3DPRESENT_PARAMETERS* pp)
{
    pp->BackBufferWidth = s_virtW;
    pp->BackBufferHeight = s_virtH;
}

// ---- pixelated look: the interface at the window's resolution ---------------------------------
// From the pre-UI point to Present (s_split) the engine's interface goes to the real back buffer:
// render target 0 bindings of the virtual (or real) back buffer become the real one, viewports,
// scissor rectangles and cleared rectangles are scaled from the virtual size to the window's, and a
// depth surface smaller than the window is swapped for one of the window's size (s_uiDepth). The interface's
// layout and the mouse stay in the virtual size's pixels, stretched over the window; the frame
// itself is shown with whole N x N pixels (cropped by up to a few pixels at the edges), so the two
// differ by less than one game pixel at the edges and agree at the centre.
static bool s_split;                       // this frame's interface goes to the real back buffer
static bool s_rt0Ui;                       // render target 0 is the real back buffer for the interface
static IDirect3DSurface9* s_uiDepth;       // depth surface at the window's size
static IDirect3DSurface9* s_engineDepth;   // the engine's depth surface s_uiDepth stands in for (one reference)
static D3DVIEWPORT9 s_vpRaw, s_vpScaled;   // the engine's last viewport during the split, and what was set
static RECT s_scissorRaw;                  // the engine's last scissor rectangle (virtual pixels)
static bool s_scissorSet;                  // the engine has set one
static RECT s_scissorScaled;
static DWORD s_splits, s_splitSkips;
static const char* s_splitSkip = "";

static void ScaleXY(LONG x, LONG y, LONG* ox, LONG* oy)
{
    *ox = (LONG)floorf(x * (float)s_nativeW / s_virtW + 0.5f);
    *oy = (LONG)floorf(y * (float)s_nativeH / s_virtH + 0.5f);
    if (*ox < 0) *ox = 0;
    if (*oy < 0) *oy = 0;
    if (*ox > s_nativeW) *ox = s_nativeW;
    if (*oy > s_nativeH) *oy = s_nativeH;
}

static RECT ScaleRect(const RECT& r)
{
    RECT o;
    ScaleXY(r.left, r.top, &o.left, &o.top);
    ScaleXY(r.right, r.bottom, &o.right, &o.bottom);
    return o;
}

static D3DVIEWPORT9 ScaleViewport(const D3DVIEWPORT9& v)
{
    LONG x0, y0, x1, y1;
    ScaleXY((LONG)v.X, (LONG)v.Y, &x0, &y0);
    ScaleXY((LONG)(v.X + v.Width), (LONG)(v.Y + v.Height), &x1, &y1);
    D3DVIEWPORT9 o = v;
    o.X = x0;
    o.Y = y0;
    o.Width = x1 > x0 ? x1 - x0 : 1;
    o.Height = y1 > y0 ? y1 - y0 : 1;
    return o;
}

// A depth surface too small for the window (D3D9 needs depth at least as large as the target): the
// engine draws its interface with a 1024 x 1024 one (at least the virtual size).
static bool IsSmallDepth(IDirect3DSurface9* s)
{
    D3DSURFACE_DESC d;
    return s && s != s_uiDepth && SUCCEEDED(s->GetDesc(&d)) && ((int)d.Width < s_nativeW || (int)d.Height < s_nativeH) &&
           d.MultiSampleType == D3DMULTISAMPLE_NONE;
}

// Puts the engine's own bindings back (Present, device loss): render target 0 and depth as it set them.
static void EndSplit(IDirect3DDevice9* dev)
{
    if (!s_split) return;
    s_split = false;
    s_rt0Ui = false;
    if (!dev) return;
    IDirect3DSurface9 *real = nullptr, *rt = nullptr, *ds = nullptr;
    D3DRealBackBuffer(&real);
    dev->GetRenderTarget(0, &rt);
    if (rt && rt == real && s_fakeSurface) dev->SetRenderTarget(0, s_fakeSurface);
    dev->GetDepthStencilSurface(&ds);
    if (ds == s_uiDepth) dev->SetDepthStencilSurface(s_engineDepth);
    if (s_engineDepth) { s_engineDepth->Release(); s_engineDepth = nullptr; }
    if (s_scissorSet) dev->SetScissorRect(&s_scissorRaw);
    if (real) real->Release();
    if (rt) rt->Release();
    if (ds) ds->Release();
}

bool SupersampleSplitThisFrame() { return s_split; }

IDirect3DSurface9* SupersampleReplaceDepth(IDirect3DSurface9* s)
{
    if (!s_split || !s_uiDepth || !IsSmallDepth(s)) return s;
    if (s != s_engineDepth) {
        s->AddRef();
        if (s_engineDepth) s_engineDepth->Release();
        s_engineDepth = s;
    }
    return s_uiDepth;
}

bool SupersampleUiViewport(const D3DVIEWPORT9* vp, D3DVIEWPORT9* out)
{
    if (!s_rt0Ui || !vp) return false;
    s_vpRaw = *vp;
    s_vpScaled = *out = ScaleViewport(*vp);
    return true;
}

// GetViewport: the engine gets back what it set, not the scaled viewport.
void SupersampleUiGetViewport(D3DVIEWPORT9* vp)
{
    if (s_split && vp && !memcmp(vp, &s_vpScaled, sizeof(*vp))) *vp = s_vpRaw;
}

const RECT* SupersampleUiScissor(const RECT* r, RECT* buf)
{
    if (!SupersampleActive() || !r) return r;
    s_scissorRaw = *r;
    s_scissorSet = true;
    if (!s_rt0Ui) return r;
    s_scissorScaled = *buf = ScaleRect(*r);
    return buf;
}

void SupersampleUiGetScissor(RECT* r)
{
    if (s_split && r && !memcmp(r, &s_scissorScaled, sizeof(*r))) *r = s_scissorRaw;
}

const D3DRECT* SupersampleUiClearRects(DWORD count, const D3DRECT* rects, D3DRECT* buf, DWORD bufCount)
{
    if (!s_rt0Ui || !rects || !count || count > bufCount) return rects;
    for (DWORD i = 0; i < count; ++i) {
        RECT r = {rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2};
        r = ScaleRect(r);
        buf[i] = {r.left, r.top, r.right, r.bottom};
    }
    return buf;
}

void SupersampleLost()
{
    IDirect3DDevice9* dev = D3DDevice();
    if (s_split) {
        // Device loss between the pre-UI point and Present: the depth surface goes back unbound.
        s_split = s_rt0Ui = false;
        IDirect3DSurface9* ds = nullptr;
        if (dev && SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) {
            if (ds == s_uiDepth) dev->SetDepthStencilSurface(nullptr);
            ds->Release();
        }
    }
    if (s_engineDepth) { s_engineDepth->Release(); s_engineDepth = nullptr; }
    if (s_uiDepth) { s_uiDepth->Release(); s_uiDepth = nullptr; }
    // A texture's surface shares the texture's reference count: after the surface is released,
    // anything above the texture's own reference is held by someone else.
    if (s_fakeSurface) { s_fakeSurface->Release(); s_fakeSurface = nullptr; }
    if (s_fake) {
        s_fake->AddRef();
        ULONG refs = s_fake->Release();
        if (refs > 1) Log("supersample: virtual back buffer still has %lu other reference(s) at release (Reset will fail)", refs - 1);
        s_fake->Release();
        s_fake = nullptr;
    }
}

// After CreateDevice / Reset: the virtual back buffer, bound in place of the real one.
void SupersampleDeviceReady(IDirect3DDevice9* dev)
{
    if (!SupersampleActive()) return;
    D3DScope scope;
    SupersampleLost();
    HRESULT hr = dev->CreateTexture(s_virtW, s_virtH, 1, D3DUSAGE_RENDERTARGET, s_format, D3DPOOL_DEFAULT, &s_fake, nullptr);
    if (FAILED(hr) && s_canUndo) {  // right after a live change: back to the size before it
        Log("supersample: virtual back buffer %dx%d failed (%08lx)", s_virtW, s_virtH, hr);
        s_fake = nullptr;
        SupersampleUndo("not enough video memory");
        OverlayNotice("Resolution change undone: not enough video memory");
        return;
    }
    if (FAILED(hr)) {
        Log("supersample: virtual back buffer %dx%d failed (%08lx); supersampling off", s_virtW, s_virtH, hr);
        s_active = false;
        return;
    }
    s_fake->GetSurfaceLevel(0, &s_fakeSurface);
    dev->SetRenderTarget(0, s_fakeSurface);
    if (!s_startW) {
        s_startW = s_virtW;
        s_startH = s_virtH;
    }
    Log("supersample: virtual back buffer %dx%d (%p), native %dx%d", s_virtW, s_virtH, s_fakeSurface, s_nativeW, s_nativeH);
}

// Whether this resource is the virtual back buffer (its texture or its surface).
bool SupersampleIsVirtual(IUnknown* r)
{
    return r && ((s_fake && r == (IUnknown*)s_fake) || (s_fakeSurface && r == (IUnknown*)s_fakeSurface));
}

bool SupersampleBackBuffer(IDirect3DSurface9** out)
{
    if (!SupersampleActive() || !s_fakeSurface) return false;
    s_fakeSurface->AddRef();
    *out = s_fakeSurface;
    return true;
}

// SetRenderTarget(0): the real back buffer becomes the virtual one, except for the interface of
// the pixelated look (both go to the real one).
IDirect3DSurface9* SupersampleReplaceTarget(IDirect3DSurface9* s)
{
    if (!SupersampleActive() || !s_fakeSurface || !s) {
        s_rt0Ui = false;
        return s;
    }
    IDirect3DSurface9* real = nullptr;
    if (FAILED(D3DRealBackBuffer(&real)) || !real) return s;
    real->Release();
    if (s_split) {
        s_rt0Ui = s == real || s == s_fakeSurface;
        return s_rt0Ui ? real : s;
    }
    return s == real ? s_fakeSurface : s;
}

// Draws the virtual frame into target (the real back buffer): averaged down (supersampling) or
// with hard pixels (pixelated look). ownScene: called outside the engine's BeginScene (Present).
static void Show(IDirect3DDevice9* dev, IDirect3DSurface9* target, bool ownScene)
{
    if (!s_vs && FAILED(dev->CreateVertexShader((const DWORD*)g_downscaleVS, &s_vs))) return;
    if (!s_ps && FAILED(dev->CreatePixelShader((const DWORD*)g_downscalePS, &s_ps))) return;
    static IDirect3DPixelShader9 *s_fastPS, *s_pixelPS;
    if (!s_fastPS && FAILED(dev->CreatePixelShader((const DWORD*)g_downscaleFastPS, &s_fastPS))) s_fastPS = nullptr;
    if (s_pixel && !s_pixelPS && FAILED(dev->CreatePixelShader((const DWORD*)g_pixelPS, &s_pixelPS))) return;
    D3DSURFACE_DESC d;
    target->GetDesc(&d);
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    dev->SetRenderTarget(0, target);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.f, 1.f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(s_vs);
    // The same integer scale on both axes (factors 4, 9, ..., 64): the fast shader with sRGB-decoding
    // bilinear reads; otherwise the general one (exact fractional spans, point reads).
    float sx = (float)s_virtW / d.Width, sy = (float)s_virtH / d.Height;
    bool fast = !s_pixel && s_fastPS && s_virtW % d.Width == 0 && s_virtH % d.Height == 0 && fabsf(sx - sy) < 1e-3f && sx >= 2;
    dev->SetPixelShader(s_pixel ? s_pixelPS : fast ? s_fastPS : s_ps);
    dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    float c0[4] = {sx, sy, (float)s_virtW, (float)s_virtH};
    // Pixelated: N window pixels per frame pixel, the frame centred (its overhang cut off evenly).
    float c1[4] = {-(float)((s_virtW * s_pixel - (int)d.Width) / 2), -(float)((s_virtH * s_pixel - (int)d.Height) / 2), 0, 0};
    if (s_pixel) c0[0] = c0[1] = (float)s_pixel;
    float c2[4] = {-1.f / d.Width, 1.f / d.Height, 0, 0};
    dev->SetPixelShaderConstantF(0, c0, 1);
    if (s_pixel) dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetVertexShaderConstantF(2, c2, 1);
    dev->SetTexture(0, s_fake);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, fast ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, fast ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, fast ? TRUE : FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    struct V { float x, y, z, u, v; };
    const V quad[4] = {{-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}};
    if (!ownScene || SUCCEEDED(dev->BeginScene())) {
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        if (ownScene) dev->EndScene();
    }
    dev->SetTexture(0, nullptr);
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    sb->Apply();
    sb->Release();
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
}

// Pixelated look with a sharp interface, at the pre-UI point (the world, image space and the lens
// are in the virtual frame): the frame is shown in the real back buffer and the engine's interface
// is drawn over it there (see s_split).
void SupersamplePreUI(IDirect3DDevice9* dev)
{
    if (!s_pixel || g_settings.pixelHud || !SupersampleActive() || !s_fakeSurface || s_split) return;
    IDirect3DSurface9 *rt = nullptr, *real = nullptr, *ds = nullptr;
    dev->GetRenderTarget(0, &rt);
    if (rt) rt->Release();
    if (rt != s_fakeSurface) { s_splitSkip = "render target is not the back buffer"; ++s_splitSkips; return; }
    if (FAILED(D3DRealBackBuffer(&real)) || !real) return;
    D3DSURFACE_DESC d;
    real->GetDesc(&d);
    if (!s_uiDepth) {
        HRESULT hr = dev->CreateDepthStencilSurface(d.Width, d.Height, D3DFMT_D24S8, d.MultiSampleType, d.MultiSampleQuality, FALSE,
                                                    &s_uiDepth, nullptr);
        Log("supersample: interface depth surface %ux%u -> %08lx", d.Width, d.Height, hr);
        if (FAILED(hr)) {
            s_uiDepth = nullptr;
            s_splitSkip = "no depth surface";
            ++s_splitSkips;
            real->Release();
            return;
        }
    }
    Show(dev, real, false);  // inside the engine's scene
    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    dev->GetDepthStencilSurface(&ds);
    s_split = s_rt0Ui = true;
    dev->SetRenderTarget(0, real);
    if (IsSmallDepth(ds)) {
        s_engineDepth = ds;  // keeps the reference
        ds = nullptr;
        dev->SetDepthStencilSurface(s_uiDepth);
        dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.f, 0);
    }
    if (ds) ds->Release();
    D3DVIEWPORT9 scaled;
    SupersampleUiViewport(&vp, &scaled);
    dev->SetViewport(&scaled);
    if (s_scissorSet) {
        s_scissorScaled = ScaleRect(s_scissorRaw);
        dev->SetScissorRect(&s_scissorScaled);
    }
    real->Release();
    if (++s_splits == 1) Log("supersample: first frame with the interface at the window's resolution");
}

// Present: averages the virtual frame down into the real back buffer, or shows it with hard
// pixels; after an interface drawn at the window's resolution the frame is already there.
void SupersampleResolve(IDirect3DDevice9* dev, IDirect3DSurface9* realBB)
{
    SupersampleFollowSettings();
    if (s_sinceChange && ++s_sinceChange == 60) {
        long count;
        double mb;
        D3DResourceTotals(&count, &mb);
        double priv, largest;
        ProcessMemory(&priv, &largest);
        Log("supersample: 60 frames after live change #%lu: %ld default-pool resources, %.0f MB; process private %.0f MB, largest free "
            "block %.0f MB", s_changes, count, mb, priv, largest);
        s_sinceChange = 0;
    }
    if (s_split) {
        EndSplit(dev);
        return;
    }
    if (!SupersampleActive() || !s_fake) return;
    Show(dev, realBB, true);
    ++s_resolves;
}

void SupersampleWriteState(FILE* f)
{
    fprintf(f, "supersample: setting %g, active %d, native %dx%d, virtual %dx%d (scale %.3f), virtual back buffer %p, resolves %lu, "
            "live changes %lu, pending %d\n",
            g_settings.supersample, SupersampleActive(), s_nativeW, s_nativeH, s_virtW, s_virtH, s_scale, s_fakeSurface, s_resolves,
            s_changes, s_pending);
    fprintf(f, "pixelated: setting %d hud %s, active %d (%d window pixels per game pixel), interface at window resolution %lu frames, "
            "skipped %lu (%s), split now %d, depth %p\n",
            g_settings.pixelSize, g_settings.pixelHud ? "pixelated" : "sharp", SupersamplePixelSize() > 0, s_pixel, s_splits, s_splitSkips,
            s_splitSkip, s_split, s_uiDepth);
}
