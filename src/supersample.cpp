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
// The factor is read at start (rowe.ini supersample=, pixel-count multiplier) and applies to
// the next start when changed in the menu.
#include <windows.h>
#include <math.h>
#include <string.h>
#include "fnvfov.h"
#include "downscale_vs.h"
#include "downscale_ps.h"
#include "downscale_fast_ps.h"

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

void SupersampleInit()
{
    float f = g_settings.supersample;
    s_active = f > 1.01f;
    s_scale = s_active ? sqrtf(f) : 1.f;
    if (s_scale > 8.f) s_scale = 8.f;  // the resolve shader reads up to 9x9 source pixels
    Log("supersample: factor %g (scale %.3f per axis)%s", f, s_scale, s_active ? "" : ", off");
}

bool SupersampleActive() { return s_active && s_virtW > 0; }
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
    int v = ((int)(value * scale + 0.5f)) & ~1;
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
    s_virtW = ((int)(w * scale + 0.5f)) & ~1;
    s_virtH = ((int)(h * scale + 0.5f)) & ~1;
    *(int*)(ow + 4) = s_virtW;
    *(int*)(oh + 4) = s_virtH;
    Log("supersample: iSize was changed to %dx%d after the INI reads (e.g. FalloutCustom.ini): native %dx%d, virtual %dx%d "
        "(scale %.3f)", w, h, w, h, s_virtW, s_virtH, scale);
    if (BYTE* om = FindSettingObject("iMultiSample:Display")) {
        UINT v = *(UINT*)(om + 4), cap = CapMsaa(v);
        if (cap != v) *(UINT*)(om + 4) = cap;
    }
}

bool SupersampleFixSize(int* w, int* h)
{
    if (!SupersampleActive() || *w != s_virtW || *h != s_virtH) return false;
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

void SupersampleLost()
{
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
    if (FAILED(hr)) {
        Log("supersample: virtual back buffer %dx%d failed (%08lx); supersampling off", s_virtW, s_virtH, hr);
        s_active = false;
        return;
    }
    s_fake->GetSurfaceLevel(0, &s_fakeSurface);
    dev->SetRenderTarget(0, s_fakeSurface);
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

IDirect3DSurface9* SupersampleReplaceTarget(IDirect3DSurface9* s)
{
    if (!SupersampleActive() || !s_fakeSurface || !s) return s;
    IDirect3DSurface9* real = nullptr;
    if (FAILED(D3DRealBackBuffer(&real)) || !real) return s;
    real->Release();
    return s == real ? s_fakeSurface : s;
}

// Present: averages the virtual frame down into the real back buffer.
void SupersampleResolve(IDirect3DDevice9* dev, IDirect3DSurface9* realBB)
{
    if (!SupersampleActive() || !s_fake) return;
    if (!s_vs && FAILED(dev->CreateVertexShader((const DWORD*)g_downscaleVS, &s_vs))) return;
    if (!s_ps && FAILED(dev->CreatePixelShader((const DWORD*)g_downscalePS, &s_ps))) return;
    static IDirect3DPixelShader9* s_fastPS;
    if (!s_fastPS && FAILED(dev->CreatePixelShader((const DWORD*)g_downscaleFastPS, &s_fastPS))) s_fastPS = nullptr;
    D3DSURFACE_DESC d;
    realBB->GetDesc(&d);
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    dev->SetRenderTarget(0, realBB);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.f, 1.f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(s_vs);
    // The same integer scale on both axes (factors 4, 9, ..., 64): the fast shader with sRGB-decoding
    // bilinear reads; otherwise the general one (exact fractional spans, point reads).
    float sx = (float)s_virtW / d.Width, sy = (float)s_virtH / d.Height;
    bool fast = s_fastPS && s_virtW % d.Width == 0 && s_virtH % d.Height == 0 && fabsf(sx - sy) < 1e-3f && sx >= 2;
    dev->SetPixelShader(fast ? s_fastPS : s_ps);
    dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    float c0[4] = {(float)s_virtW / d.Width, (float)s_virtH / d.Height, (float)s_virtW, (float)s_virtH};
    float c2[4] = {-1.f / d.Width, 1.f / d.Height, 0, 0};
    dev->SetPixelShaderConstantF(0, c0, 1);
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
    if (SUCCEEDED(dev->BeginScene())) {
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        dev->EndScene();
    }
    dev->SetTexture(0, nullptr);
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    sb->Apply();
    sb->Release();
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    ++s_resolves;
}

void SupersampleWriteState(FILE* f)
{
    fprintf(f, "supersample: setting %g, active %d, native %dx%d, virtual %dx%d, virtual back buffer %p, resolves %lu\n",
            g_settings.supersample, SupersampleActive(), s_nativeW, s_nativeH, s_virtW, s_virtH, s_fakeSurface, s_resolves);
}
