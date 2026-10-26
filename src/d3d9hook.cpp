// The engine's D3D9 device: wraps the engine's Direct3DCreate9(Ex) lookup (the exe resolves it
// with GetProcAddress, not through its import table), hooks IDirect3D9::CreateDevice and then the
// device's vtable. Present and Reset dispatch to the mod's per-frame work (frame.cpp).
//
// It also logs the device parameters, every render target and depth surface the engine creates,
// and the first callers of the back buffer and render target getters; when rowe_trace.req
// exists, the next whole frame (Present to Present) is logged call by call to rowe_trace.txt.
//
// The vtable is patched in place, like gowfov's swap chain hooks: whatever d3d9.dll is loaded
// (system, ENB, ReShade, DXVK), its device is the one hooked. Calls the mod makes itself run in a
// D3DScope, which the hooks pass straight through.
#define CINTERFACE
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdarg.h>
#include <d3d9.h>
#include "fnvfov.h"

typedef IDirect3D9*(WINAPI* Direct3DCreate9Fn)(UINT);
typedef HRESULT(WINAPI* Direct3DCreate9ExFn)(UINT, IDirect3D9Ex**);

static Direct3DCreate9Fn s_realCreate9;
static Direct3DCreate9ExFn s_realCreate9Ex;
static IDirect3DDevice9* s_device;
static bool s_deviceIsEx;
static IDirect3DDevice9Vtbl* s_vtbl;

#define ORIG(iface, name) static decltype(((iface##Vtbl*)0)->name) o_##name
ORIG(IDirect3DDevice9Ex, ResetEx);
ORIG(IDirect3DDevice9, Reset);
ORIG(IDirect3DDevice9, Present);
ORIG(IDirect3DDevice9, TestCooperativeLevel);
ORIG(IDirect3DDevice9, GetBackBuffer);
ORIG(IDirect3DDevice9, GetSwapChain);
ORIG(IDirect3DDevice9, CreateTexture);
ORIG(IDirect3DDevice9, CreateRenderTarget);
ORIG(IDirect3DDevice9, CreateDepthStencilSurface);
ORIG(IDirect3DDevice9, CreateAdditionalSwapChain);
ORIG(IDirect3DDevice9, CreateCubeTexture);
ORIG(IDirect3DDevice9, CreateVolumeTexture);
ORIG(IDirect3DDevice9, CreateVertexBuffer);
ORIG(IDirect3DDevice9, CreateIndexBuffer);
ORIG(IDirect3DDevice9, CreateOffscreenPlainSurface);
ORIG(IDirect3DDevice9, CreateStateBlock);
ORIG(IDirect3DDevice9, EndStateBlock);
ORIG(IDirect3DDevice9, CreateQuery);
ORIG(IDirect3DDevice9, SetRenderTarget);
ORIG(IDirect3DDevice9, GetRenderTarget);
ORIG(IDirect3DDevice9, SetDepthStencilSurface);
ORIG(IDirect3DDevice9, GetDepthStencilSurface);
ORIG(IDirect3DDevice9, Clear);
ORIG(IDirect3DDevice9, StretchRect);
ORIG(IDirect3DDevice9, SetViewport);
ORIG(IDirect3DDevice9, GetViewport);
ORIG(IDirect3DDevice9, SetScissorRect);
ORIG(IDirect3DDevice9, GetScissorRect);
ORIG(IDirect3DDevice9, BeginScene);
ORIG(IDirect3DDevice9, EndScene);
ORIG(IDirect3DDevice9, DrawPrimitive);
ORIG(IDirect3DDevice9, DrawIndexedPrimitive);
ORIG(IDirect3DDevice9, DrawPrimitiveUP);
ORIG(IDirect3DDevice9, DrawIndexedPrimitiveUP);
ORIG(IDirect3DDevice9, SetTransform);
ORIG(IDirect3DDevice9, GetRenderTargetData);
ORIG(IDirect3DDevice9, GetFrontBufferData);
ORIG(IDirect3DDevice9, SetPixelShaderConstantF);
ORIG(IDirect3DDevice9, SetVertexShaderConstantF);
ORIG(IDirect3DDevice9, SetTexture);

// IDirect3D9 / IDirect3D9Ex originals, per vtable: the game probes with Direct3DCreate9 and uses
// Direct3DCreate9Ex in fullscreen, and the two classes have their own vtables.
struct D3DOrig {
    void* vtable;
    decltype(((IDirect3D9Vtbl*)0)->CreateDevice) CreateDevice;
    decltype(((IDirect3D9Vtbl*)0)->GetAdapterModeCount) GetAdapterModeCount;
    decltype(((IDirect3D9Vtbl*)0)->EnumAdapterModes) EnumAdapterModes;
    decltype(((IDirect3D9ExVtbl*)0)->CreateDeviceEx) CreateDeviceEx;
};
static D3DOrig s_d3dOrig[4];

static D3DOrig* Orig(void* d3d)
{
    void* vt = *(void**)d3d;
    for (D3DOrig& o : s_d3dOrig)
        if (o.vtable == vt) return &o;
    return &s_d3dOrig[0];  // not reached: hooks only exist on recorded vtables
}

static int s_internal;  // > 0 while the mod itself calls the device

D3DScope::D3DScope() { ++s_internal; }
D3DScope::~D3DScope() { --s_internal; }

static void SwapSlot(void* slot, void* hook)
{
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *(void**)slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
}
#define VHOOK(vt, name) (o_##name = (vt)->name, SwapSlot(&(vt)->name, (void*)H_##name))

// ---- names -----------------------------------------------------------------------------------

static const char* FormatName(D3DFORMAT f)
{
    switch ((DWORD)f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
    case D3DFMT_R16F: return "R16F";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_G16R16F: return "G16R16F";
    case D3DFMT_G32R32F: return "G32R32F";
    case D3DFMT_A2R10G10B10: return "A2R10G10B10";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_A8: return "A8";
    case D3DFMT_L8: return "L8";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D32: return "D32";
    case D3DFMT_UNKNOWN: return "UNKNOWN";
    case MAKEFOURCC('I', 'N', 'T', 'Z'): return "INTZ";
    case MAKEFOURCC('R', 'A', 'W', 'Z'): return "RAWZ";
    case MAKEFOURCC('D', 'F', '2', '4'): return "DF24";
    case MAKEFOURCC('D', 'F', '1', '6'): return "DF16";
    case MAKEFOURCC('N', 'U', 'L', 'L'): return "NULL";
    }
    static char buf[4][16];
    static int i;
    char* b = buf[i++ & 3];
    if ((DWORD)f > 0xFF) wsprintfA(b, "'%c%c%c%c'", (char)f, (char)((DWORD)f >> 8), (char)((DWORD)f >> 16), (char)((DWORD)f >> 24));
    else wsprintfA(b, "fmt%u", (DWORD)f);
    return b;
}

static const char* SwapEffectName(D3DSWAPEFFECT e)
{
    switch (e) {
    case D3DSWAPEFFECT_DISCARD: return "DISCARD";
    case D3DSWAPEFFECT_FLIP: return "FLIP";
    case D3DSWAPEFFECT_COPY: return "COPY";
    case D3DSWAPEFFECT_OVERLAY: return "OVERLAY";
    case D3DSWAPEFFECT_FLIPEX: return "FLIPEX";
    }
    return "?";
}

static int s_bbWidth, s_bbHeight;

void D3DBackBufferSize(int* w, int* h)
{
    int nw, nh;
    if (SupersampleSizes(&nw, &nh, w, h)) return;
    *w = s_bbWidth;
    *h = s_bbHeight;
}

// Also remembers the back buffer size (the result is logged last).
static void LogPP(const char* what, const D3DPRESENT_PARAMETERS* pp)
{
    if (!pp) return;
    s_bbWidth = pp->BackBufferWidth;
    s_bbHeight = pp->BackBufferHeight;
    Log("%s: back buffer %ux%u %s x%u, MSAA %u (quality %lu), swap %s, window %p, windowed %d, auto depth %d %s, flags %08lx, refresh %u, interval %08x",
        what, pp->BackBufferWidth, pp->BackBufferHeight, FormatName(pp->BackBufferFormat), pp->BackBufferCount,
        pp->MultiSampleType, pp->MultiSampleQuality, SwapEffectName(pp->SwapEffect), pp->hDeviceWindow, pp->Windowed,
        pp->EnableAutoDepthStencil, FormatName(pp->AutoDepthStencilFormat), pp->Flags, pp->FullScreen_RefreshRateInHz,
        pp->PresentationInterval);
}

// Surface labels so the frame trace can say which target is bound.
struct Label { void* surface; char text[24]; };
static Label s_labels[512];
static int s_labelCount;
static int s_texSerial, s_rtSerial, s_dsSerial;

static void SetLabel(void* surface, const char* text)
{
    for (int i = 0; i < s_labelCount; ++i)
        if (s_labels[i].surface == surface) { lstrcpynA(s_labels[i].text, text, 24); return; }
    if (s_labelCount < 512) {
        s_labels[s_labelCount].surface = surface;
        lstrcpynA(s_labels[s_labelCount++].text, text, 24);
    }
}

static void Describe(IDirect3DSurface9* s, char* out, int size)
{
    if (!s) { lstrcpynA(out, "null", size); return; }
    const char* label = "?";
    for (int i = 0; i < s_labelCount; ++i)
        if (s_labels[i].surface == s) { label = s_labels[i].text; break; }
    D3DSURFACE_DESC d;
    if (FAILED(s->lpVtbl->GetDesc(s, &d))) { wsprintfA(out, "%s(%p)", label, s); return; }
    wsprintfA(out, "%s %ux%u %s%s", label, d.Width, d.Height, FormatName(d.Format),
              d.MultiSampleType ? " MSAA" : "");
}

static void LabelBackBuffers(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(o_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s) {
        SetLabel(s, "BB0");
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(s->lpVtbl->GetDesc(s, &desc))) {
            s_bbWidth = desc.Width;
            s_bbHeight = desc.Height;
        }
        char d[96];
        Describe(s, d, sizeof(d));
        Log("back buffer: %s (%p)", d, s);
        s->lpVtbl->Release(s);
    }
    s = nullptr;
    if (SUCCEEDED(o_GetDepthStencilSurface(dev, &s)) && s) {
        SetLabel(s, "autoDS");
        char d[96];
        Describe(s, d, sizeof(d));
        Log("auto depth surface: %s (%p)", d, s);
        s->lpVtbl->Release(s);
    }
}

// ---- frame trace -----------------------------------------------------------------------------

static FILE* s_trace;        // open while a frame is being traced
static int s_traceDraws, s_tracePrims, s_traceSeq;
static CRITICAL_SECTION s_traceLock;
static bool s_traceLockInit;

bool D3DTracing() { return s_trace != nullptr; }

// GPU time per trace step: a timestamp query after every trace line; read back at the trace's end
// (the wait for them is part of that frame only). kGpuMarks bounds one frame.
static const int kGpuMarks = 1024;
static IDirect3DQuery9* s_gpuQuery[kGpuMarks];
static int s_gpuSeq[kGpuMarks];
static int s_gpuCount;
static IDirect3DQuery9 *s_gpuDisjoint, *s_gpuFreq;

static void GpuMark(int seq)
{
    if (!s_device || s_gpuCount >= kGpuMarks) return;
    IDirect3DQuery9** q = &s_gpuQuery[s_gpuCount];
    if (!*q && FAILED(s_device->lpVtbl->CreateQuery(s_device, D3DQUERYTYPE_TIMESTAMP, q))) return;
    (*q)->lpVtbl->Issue(*q, D3DISSUE_END);
    s_gpuSeq[s_gpuCount++] = seq;
}

static void GpuBegin()
{
    s_gpuCount = 0;
    if (!s_device) return;
    if (!s_gpuDisjoint) s_device->lpVtbl->CreateQuery(s_device, D3DQUERYTYPE_TIMESTAMPDISJOINT, &s_gpuDisjoint);
    if (!s_gpuFreq) s_device->lpVtbl->CreateQuery(s_device, D3DQUERYTYPE_TIMESTAMPFREQ, &s_gpuFreq);
    if (s_gpuDisjoint) s_gpuDisjoint->lpVtbl->Issue(s_gpuDisjoint, D3DISSUE_BEGIN);
}

// Appends "step: GPU ms since the previous step" for the traced frame.
static void GpuWrite(FILE* f)
{
    if (!s_gpuDisjoint || !s_gpuFreq || !s_gpuCount) return;
    s_gpuDisjoint->lpVtbl->Issue(s_gpuDisjoint, D3DISSUE_END);
    s_gpuFreq->lpVtbl->Issue(s_gpuFreq, D3DISSUE_END);
    UINT64 freq = 0;
    BOOL disjoint = TRUE;
    while (s_gpuFreq->lpVtbl->GetData(s_gpuFreq, &freq, sizeof(freq), D3DGETDATA_FLUSH) == S_FALSE) {}
    while (s_gpuDisjoint->lpVtbl->GetData(s_gpuDisjoint, &disjoint, sizeof(disjoint), D3DGETDATA_FLUSH) == S_FALSE) {}
    fprintf(f, "---- GPU time per step (ms since the previous step; frequency %llu%s) ----\n", freq,
            disjoint ? ", DISJOINT: unreliable" : "");
    UINT64 prev = 0, first = 0;
    for (int i = 0; i < s_gpuCount; ++i) {
        UINT64 t = 0;
        while (s_gpuQuery[i]->lpVtbl->GetData(s_gpuQuery[i], &t, sizeof(t), D3DGETDATA_FLUSH) == S_FALSE) {}
        if (i == 0) first = prev = t;
        if (freq) fprintf(f, "%5d gpu %+8.3f  (at %8.3f)\n", s_gpuSeq[i], (t - prev) * 1000.0 / freq, (t - first) * 1000.0 / freq);
        prev = t;
    }
}

// Milliseconds since the trace began (CPU time: a gap means the CPU waited or the driver blocked).
static LARGE_INTEGER s_traceStart;
static double TraceMs()
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (now.QuadPart - s_traceStart.QuadPart) * 1000.0 / freq.QuadPart;
}

static void FlushDraws()
{
    if (s_traceDraws) {
        int seq = s_traceSeq++;
        fprintf(s_trace, "%5d %8.2f   %d draws, %d primitives\n", seq, TraceMs(), s_traceDraws, s_tracePrims);
        GpuMark(seq);
    }
    s_traceDraws = s_tracePrims = 0;
}

static void Trace(const char* fmt, ...)
{
    if (!s_trace || s_internal) return;
    EnterCriticalSection(&s_traceLock);
    if (s_trace) {
        FlushDraws();
        int seq = s_traceSeq++;
        fprintf(s_trace, "%5d %8.2f [%5lu] ", seq, TraceMs(), GetCurrentThreadId());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(s_trace, fmt, ap);
        va_end(ap);
        fputc('\n', s_trace);
        GpuMark(seq);
    }
    LeaveCriticalSection(&s_traceLock);
}

static void TraceDraw(UINT prims)
{
    if (!s_trace || s_internal) return;
    ++s_traceDraws;
    s_tracePrims += prims;
}

void D3DTraceStamp(const char* what) { Trace("== engine: %s", what); }

void D3DDescribeRT(char* out, int size)
{
    out[0] = 0;
    if (!s_device) return;
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(o_GetRenderTarget(s_device, 0, &s)) && s) {
        Describe(s, out, size);
        s->lpVtbl->Release(s);
    }
}

static void TraceBeginOrEnd()
{
    char path[MAX_PATH];
    if (s_trace) {
        Trace("Present (trace end)");
        EnterCriticalSection(&s_traceLock);
        GpuWrite(s_trace);
        fclose(s_trace);
        s_trace = nullptr;
        LeaveCriticalSection(&s_traceLock);
        Log("frame trace written to rowe_trace.txt");
        return;
    }
    wsprintfA(path, "%srowe_trace.req", ModDir());
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(path);
    wsprintfA(path, "%srowe_trace.txt", ModDir());
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n==== frame trace %02d:%02d:%02d ====\n", t.wHour, t.wMinute, t.wSecond);
    s_traceSeq = 0;
    QueryPerformanceCounter(&s_traceStart);
    GpuBegin();
    s_trace = f;
    char rt[96];
    D3DDescribeRT(rt, sizeof(rt));
    Trace("trace begin after Present; RT0 = %s", rt);
}

// ---- device hooks ----------------------------------------------------------------------------

static void TraceSurface(const char* what, DWORD index, IDirect3DSurface9* s)
{
    if (!s_trace) return;
    char d[96];
    Describe(s, d, sizeof(d));
    Trace("%s[%lu] = %s", what, index, d);
}

// Reset fails while any D3DPOOL_DEFAULT resource is alive, and a bound render target counts: with
// supersampling the virtual back buffer is bound in the real one's place, so the real one goes
// back first.
static void UnbindForReset(IDirect3DDevice9* dev)
{
    // The trace's GPU queries (recreated on the next trace).
    for (int i = 0; i < kGpuMarks; ++i)
        if (s_gpuQuery[i]) { s_gpuQuery[i]->lpVtbl->Release(s_gpuQuery[i]); s_gpuQuery[i] = nullptr; }
    if (s_gpuDisjoint) { s_gpuDisjoint->lpVtbl->Release(s_gpuDisjoint); s_gpuDisjoint = nullptr; }
    if (s_gpuFreq) { s_gpuFreq->lpVtbl->Release(s_gpuFreq); s_gpuFreq = nullptr; }
    s_gpuCount = 0;
    IDirect3DSurface9* real = nullptr;
    if (SUCCEEDED(o_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &real)) && real) {
        o_SetRenderTarget(dev, 0, real);
        real->lpVtbl->Release(real);
    }
    // Every other binding too: the device holds a reference to each bound texture and render target,
    // and Reset fails while the virtual back buffer (or any default-pool resource) is still referenced.
    for (DWORD i = 1; i < 4; ++i) {
        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(o_GetRenderTarget(dev, i, &rt)) && rt) {
            if (SupersampleIsVirtual((IUnknown*)rt)) Log("reset: virtual back buffer was bound as render target %lu", i);
            rt->lpVtbl->Release(rt);
            o_SetRenderTarget(dev, i, nullptr);
        }
    }
    static const DWORD kStages[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, D3DVERTEXTEXTURESAMPLER0,
                                    D3DVERTEXTEXTURESAMPLER1, D3DVERTEXTEXTURESAMPLER2, D3DVERTEXTEXTURESAMPLER3};
    for (DWORD stage : kStages) {
        IDirect3DBaseTexture9* t = nullptr;
        if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, stage, &t)) && t) {
            if (SupersampleIsVirtual((IUnknown*)t)) Log("reset: virtual back buffer was bound as texture %lu", stage);
            t->lpVtbl->Release(t);
            dev->lpVtbl->SetTexture(dev, stage, nullptr);
        }
    }
}

// [dev] test: "devicelost" (gamecmd) makes the next Present report a lost device and
// TestCooperativeLevel "not reset" until the game resets, as after the lock screen or a driver event.
static volatile LONG s_simLost;
void D3DSimulateDeviceLost()
{
    s_simLost = 1;
    Log("devicelost: next Present reports D3DERR_DEVICELOST");
}

static HRESULT STDMETHODCALLTYPE H_TestCooperativeLevel(IDirect3DDevice9* dev)
{
    if (s_simLost == 2) return D3DERR_DEVICENOTRESET;
    return o_TestCooperativeLevel(dev);
}

static void GameQueriesRelease();
static void GameQueriesRestore(IDirect3DDevice9* dev);

static HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    LogPP("Reset (requested)", pp);
    {
        D3DScope scope;
        UnbindForReset(dev);
        FrameDeviceLost();
    }
    bool ss = SupersampleFixPresentParams(pp);
    GameQueriesRelease();
    D3DReportLiveResources("Reset");
    HRESULT hr = o_Reset(dev, pp);
    s_simLost = 0;
    LogPP("Reset (result)", pp);
    if (ss) SupersampleRestorePresentParams(pp);
    Log("Reset -> %08lx (from %p)", hr, _ReturnAddress());
    if (SUCCEEDED(hr)) {
        LabelBackBuffers(dev);
        SupersampleDeviceReady(dev);
    }
    GameQueriesRestore(dev);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    static bool first = true;
    if (first) {
        first = false;
        Log("first Present (src %p, dst %p, window %p, from %p)", src, dst, wnd, _ReturnAddress());
    }
    if (!s_traceLockInit) { InitializeCriticalSection(&s_traceLock); s_traceLockInit = true; }
    ReconEngineOnPresent();
    {
        D3DScope scope;
        FramePresent(dev);
    }
    if (s_simLost) {  // simulated loss: the frame is dropped
        s_simLost = 2;
        TraceBeginOrEnd();
        return D3DERR_DEVICELOST;
    }
    HRESULT hr = o_Present(dev, src, dst, wnd, dirty);
    TraceBeginOrEnd();
    return hr;
}

// The first callers of the getters show who would need the "virtual" back buffer in phase 5.
static void NoteGetter(const char* what, void* from, LONG* count)
{
    if (!s_internal && InterlockedIncrement(count) <= 12) Log("%s called from %p", what, from);
}

static HRESULT STDMETHODCALLTYPE H_GetBackBuffer(IDirect3DDevice9* dev, UINT chain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** out)
{
    static LONG n;
    NoteGetter("GetBackBuffer", _ReturnAddress(), &n);
    if (!s_internal && chain == 0 && index == 0 && SupersampleBackBuffer(out)) return D3D_OK;
    HRESULT hr = o_GetBackBuffer(dev, chain, index, type, out);
    if (s_trace && SUCCEEDED(hr)) TraceSurface("GetBackBuffer", index, *out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_GetSwapChain(IDirect3DDevice9* dev, UINT chain, IDirect3DSwapChain9** out)
{
    static LONG n;
    NoteGetter("GetSwapChain", _ReturnAddress(), &n);
    return o_GetSwapChain(dev, chain, out);
}

static HRESULT STDMETHODCALLTYPE H_GetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9** out)
{
    static LONG n;
    NoteGetter("GetRenderTarget", _ReturnAddress(), &n);
    return o_GetRenderTarget(dev, index, out);
}

static HRESULT STDMETHODCALLTYPE H_GetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9** out)
{
    static LONG n;
    NoteGetter("GetDepthStencilSurface", _ReturnAddress(), &n);
    return o_GetDepthStencilSurface(dev, out);
}

// VATS's own image-space targets at a smaller size. Entering VATS creates, at the supersampled size,
// an R32F texture for a depth pass of the whole scene (read by its blur / combine) and two A8R8G8B8
// textures at a quarter of it for its glow blur. Smaller ones are read the same way (texture
// coordinates) and cost a fraction: the depth at twice the window size, the blur at a quarter of
// the window, the size the game uses without supersampling. While one is the render target, the
// game's viewport is scaled to it. The scale is kept on the surface (private data).
static const GUID kShrinkGuid = {0x6f1e2b7a, 0x41c3, 0x4d58, {0x9a, 0x0e, 0x77, 0x2b, 0x51, 0xc4, 0x3d, 0x10}};
struct Shrink { float x, y; };
static Shrink s_rt0Shrink;  // render target 0's scale (0 = not shrunk)
static float s_rt0W, s_rt0H;  // its actual size

static bool ShrinkSize(UINT* w, UINT* h, DWORD usage, D3DFORMAT fmt)
{
    int nw, nh, vw, vh;
    if (!g_settings.devVatsShrink || !(usage & D3DUSAGE_RENDERTARGET) || !SupersampleSizes(&nw, &nh, &vw, &vh)) return false;
    UINT tw = 0, th = 0;
    if (fmt == D3DFMT_R32F && *w == (UINT)vw && *h == (UINT)vh) { tw = nw * 2; th = nh * 2; }
    else if ((fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_A16B16G16R16F) && *w == (UINT)vw / 4 && *h == (UINT)vh / 4) {
        tw = nw / 4;  // blur / bloom sources: their kernels work in texels, so this keeps the game's radius
        th = nh / 4;
    }
    if (!tw || !th || tw >= *w || th >= *h) return false;
    *w = tw;
    *h = th;
    return true;
}

// ---- default-pool resource tracking ------------------------------------------------------------
// Reset fails while any D3DPOOL_DEFAULT resource still exists. Every one created through the device
// gets a marker (private data with D3DSPD_IUNKNOWN: Direct3D releases it when the resource is
// destroyed) holding who created it; at Reset the live markers are listed by creating module.
static const GUID kTrackGuid = {0x3c1d7e52, 0x8a41, 0x4f0b, {0x9e, 0x27, 0x51, 0xd8, 0x6a, 0x0c, 0x33, 0x9f}};
static CRITICAL_SECTION s_trackLock;
static bool s_trackLockInit;
struct Marker;
static Marker* s_markers;
static LONG s_markerCount;

struct Marker {
    IUnknownVtbl* lpVtbl;
    LONG refs;
    void* from;
    char what[48];
    Marker *prev, *next;
};

static HRESULT STDMETHODCALLTYPE MarkerQI(IUnknown* u, REFIID riid, void** out)
{
    if (IsEqualGUID(riid, IID_IUnknown)) {
        *out = u;
        u->lpVtbl->AddRef(u);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE MarkerAddRef(IUnknown* u) { return InterlockedIncrement(&((Marker*)u)->refs); }
static ULONG STDMETHODCALLTYPE MarkerRelease(IUnknown* u)
{
    Marker* m = (Marker*)u;
    LONG n = InterlockedDecrement(&m->refs);
    if (n == 0) {
        EnterCriticalSection(&s_trackLock);
        if (m->prev) m->prev->next = m->next;
        else s_markers = m->next;
        if (m->next) m->next->prev = m->prev;
        --s_markerCount;
        LeaveCriticalSection(&s_trackLock);
        HeapFree(GetProcessHeap(), 0, m);
    }
    return n;
}
static IUnknownVtbl s_markerVtbl = {MarkerQI, MarkerAddRef, MarkerRelease};

static void Track(IDirect3DResource9* r, D3DPOOL pool, void* from, const char* fmt, ...)
{
    if (!r || pool != D3DPOOL_DEFAULT) return;
    if (!s_trackLockInit) { InitializeCriticalSection(&s_trackLock); s_trackLockInit = true; }
    Marker* m = (Marker*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Marker));
    if (!m) return;
    m->lpVtbl = &s_markerVtbl;
    m->refs = 1;
    m->from = from;
    va_list a;
    va_start(a, fmt);
    _vsnprintf(m->what, sizeof(m->what) - 1, fmt, a);
    va_end(a);
    EnterCriticalSection(&s_trackLock);
    m->next = s_markers;
    if (s_markers) s_markers->prev = m;
    s_markers = m;
    ++s_markerCount;
    LeaveCriticalSection(&s_trackLock);
    IUnknown* u = (IUnknown*)m;
    r->lpVtbl->SetPrivateData(r, kTrackGuid, u, sizeof(u), D3DSPD_IUNKNOWN);  // with D3DSPD_IUNKNOWN the data is the pointer itself
    u->lpVtbl->Release(u);  // the resource holds it now (or it is gone if SetPrivateData failed)
}

// State blocks, queries and additional swap chains carry no private data: they are tracked through
// their Release (the vtable is shared by every object of the kind, so it is patched once).
struct LiveObj { void* obj; void* from; const char* kind; void** slot; DWORD type; LiveObj* next; };
static LiveObj* s_liveObjs;
typedef ULONG(STDMETHODCALLTYPE* ReleaseFn)(void*);
static ReleaseFn s_sbRelease, s_queryRelease, s_chainRelease;

static void ForgetObj(void* obj)
{
    EnterCriticalSection(&s_trackLock);
    for (LiveObj** p = &s_liveObjs; *p; p = &(*p)->next) {
        if ((*p)->obj == obj) {
            LiveObj* dead = *p;
            *p = dead->next;
            HeapFree(GetProcessHeap(), 0, dead);
            break;
        }
    }
    LeaveCriticalSection(&s_trackLock);
}

static ULONG STDMETHODCALLTYPE SbRelease(void* o) { ULONG n = s_sbRelease(o); if (!n) ForgetObj(o); return n; }
static ULONG STDMETHODCALLTYPE QueryRelease(void* o) { ULONG n = s_queryRelease(o); if (!n) ForgetObj(o); return n; }
static ULONG STDMETHODCALLTYPE ChainRelease(void* o) { ULONG n = s_chainRelease(o); if (!n) ForgetObj(o); return n; }

static void TrackObj(void* obj, void* from, const char* kind, ReleaseFn* orig, ReleaseFn hook)
{
    if (!obj) return;
    if (!s_trackLockInit) { InitializeCriticalSection(&s_trackLock); s_trackLockInit = true; }
    void** vt = *(void***)obj;
    if (vt[2] != (void*)hook) {
        if (!*orig) *orig = (ReleaseFn)vt[2];
        DWORD old;
        VirtualProtect(&vt[2], sizeof(void*), PAGE_READWRITE, &old);
        vt[2] = (void*)hook;
        VirtualProtect(&vt[2], sizeof(void*), old, &old);
    }
    LiveObj* l = (LiveObj*)HeapAlloc(GetProcessHeap(), 0, sizeof(LiveObj));
    if (!l) return;
    l->obj = obj;
    l->from = from;
    l->kind = kind;
    l->slot = nullptr;
    l->type = 0;
    EnterCriticalSection(&s_trackLock);
    l->next = s_liveObjs;
    s_liveObjs = l;
    LeaveCriticalSection(&s_trackLock);
}

static HRESULT STDMETHODCALLTYPE H_CreateStateBlock(IDirect3DDevice9* dev, D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out)
{
    HRESULT hr = o_CreateStateBlock(dev, type, out);
    if (SUCCEEDED(hr) && !s_internal) TrackObj(*out, _ReturnAddress(), "state block", &s_sbRelease, (ReleaseFn)SbRelease);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_EndStateBlock(IDirect3DDevice9* dev, IDirect3DStateBlock9** out)
{
    HRESULT hr = o_EndStateBlock(dev, out);
    if (SUCCEEDED(hr) && !s_internal && out) TrackObj(*out, _ReturnAddress(), "state block (recorded)", &s_sbRelease, (ReleaseFn)SbRelease);
    return hr;
}

// The game's occlusion query wrapper (0x00C4ECF0) creates its query into its own first field and
// never releases it when the device is lost, so Reset fails while one exists (seen in the Tale of
// Two Wastelands list: two of them, from the render code at 0x00800F30). Their slots are noted so
// the queries can be released before Reset and created again after it (GameQueriesRelease/Restore).
static const DWORD kGameQuerySite = 0x00C4ED34;

static HRESULT STDMETHODCALLTYPE H_CreateQuery(IDirect3DDevice9* dev, D3DQUERYTYPE type, IDirect3DQuery9** out)
{
    HRESULT hr = o_CreateQuery(dev, type, out);
    if (SUCCEEDED(hr) && !s_internal && out && *out) {
        TrackObj(*out, _ReturnAddress(), "query", &s_queryRelease, (ReleaseFn)QueryRelease);
        if ((DWORD)_ReturnAddress() == kGameQuerySite) {
            EnterCriticalSection(&s_trackLock);
            if (s_liveObjs && s_liveObjs->obj == *out) {
                s_liveObjs->slot = (void**)out;
                s_liveObjs->type = type;
            }
            LeaveCriticalSection(&s_trackLock);
        }
    }
    return hr;
}

struct QuerySlot { void** slot; DWORD type; };
static QuerySlot s_releasedQueries[64];
static int s_releasedCount;

// Before Reset: the game's wrapped queries that still hold their query are released and the slot
// cleared (the wrapper's release, 0x00C4ED40, checks for null).
static void GameQueriesRelease()
{
    s_releasedCount = 0;
    if (!s_trackLockInit) return;
    for (;;) {
        IDirect3DQuery9* q = nullptr;
        EnterCriticalSection(&s_trackLock);
        for (LiveObj* l = s_liveObjs; l; l = l->next) {
            if (l->slot && !IsBadReadPtr(l->slot, 4) && *l->slot == l->obj && s_releasedCount < 64) {
                q = (IDirect3DQuery9*)l->obj;
                s_releasedQueries[s_releasedCount].slot = l->slot;
                s_releasedQueries[s_releasedCount].type = l->type;
                ++s_releasedCount;
                *l->slot = nullptr;
                l->slot = nullptr;
                break;
            }
        }
        LeaveCriticalSection(&s_trackLock);
        if (!q) break;
        q->lpVtbl->Release(q);  // the wrapper's reference (QueryRelease forgets it at zero)
    }
    if (s_releasedCount) Log("reset: released %d occlusion query(ies) the game keeps (0x00C4ECF0 wrappers)", s_releasedCount);
}

// After Reset: the same queries again, into the same slots.
static void GameQueriesRestore(IDirect3DDevice9* dev)
{
    int made = 0;
    for (int i = 0; i < s_releasedCount; ++i) {
        IDirect3DQuery9* q = nullptr;
        if (SUCCEEDED(o_CreateQuery(dev, (D3DQUERYTYPE)s_releasedQueries[i].type, &q)) && q) {
            *s_releasedQueries[i].slot = q;
            TrackObj(q, (void*)kGameQuerySite, "query", &s_queryRelease, (ReleaseFn)QueryRelease);
            EnterCriticalSection(&s_trackLock);
            if (s_liveObjs && s_liveObjs->obj == q) {
                s_liveObjs->slot = s_releasedQueries[i].slot;
                s_liveObjs->type = s_releasedQueries[i].type;
            }
            LeaveCriticalSection(&s_trackLock);
            ++made;
        }
    }
    if (s_releasedCount) Log("reset: created %d of %d occlusion query(ies) again", made, s_releasedCount);
    s_releasedCount = 0;
}

static void ReportLiveObjs()
{
    int n = 0;
    for (LiveObj* l = s_liveObjs; l; l = l->next) ++n;
    Log("  plus %d state block(s) / query(ies) / swap chain(s):", n);
    int shown = 0;
    for (LiveObj* l = s_liveObjs; l && shown < 40; l = l->next, ++shown) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)l->from, &mod) && mod)
            GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = strrchr(name, '\\');
        Log("  %s %p created by %s+%lx (%p)", l->kind, l->obj, base ? base + 1 : name, mod ? (DWORD)l->from - (DWORD)mod : 0, l->from);
    }
}

// Lists the default-pool resources that still exist, by the module that created them.
void D3DReportLiveResources(const char* when)
{
    if (!s_trackLockInit) return;
    EnterCriticalSection(&s_trackLock);
    Log("%s: %ld default-pool resource(s) still exist", when, s_markerCount);
    int shown = 0;
    for (Marker* m = s_markers; m && shown < 40; m = m->next, ++shown) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)m->from, &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof(name));
        }
        const char* base = strrchr(name, '\\');
        Log("  %s created by %s+%lx (%p)", m->what, base ? base + 1 : name, mod ? (DWORD)m->from - (DWORD)mod : 0, m->from);
    }
    ReportLiveObjs();
    LeaveCriticalSection(&s_trackLock);
}

static HRESULT STDMETHODCALLTYPE H_CreateCubeTexture(IDirect3DDevice9* dev, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                     IDirect3DCubeTexture9** out, HANDLE* shared)
{
    HRESULT hr = o_CreateCubeTexture(dev, edge, levels, usage, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && !s_internal) Track((IDirect3DResource9*)*out, pool, _ReturnAddress(), "cube texture %u %s", edge, FormatName(fmt));
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateVolumeTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt,
                                                       D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared)
{
    HRESULT hr = o_CreateVolumeTexture(dev, w, h, d, levels, usage, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && !s_internal) Track((IDirect3DResource9*)*out, pool, _ReturnAddress(), "volume texture %ux%ux%u", w, h, d);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateVertexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                      IDirect3DVertexBuffer9** out, HANDLE* shared)
{
    HRESULT hr = o_CreateVertexBuffer(dev, len, usage, fvf, pool, out, shared);
    if (SUCCEEDED(hr) && !s_internal) Track((IDirect3DResource9*)*out, pool, _ReturnAddress(), "vertex buffer %u bytes", len);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateIndexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                     IDirect3DIndexBuffer9** out, HANDLE* shared)
{
    HRESULT hr = o_CreateIndexBuffer(dev, len, usage, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && !s_internal) Track((IDirect3DResource9*)*out, pool, _ReturnAddress(), "index buffer %u bytes", len);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateOffscreenPlainSurface(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                                               IDirect3DSurface9** out, HANDLE* shared)
{
    HRESULT hr = o_CreateOffscreenPlainSurface(dev, w, h, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && !s_internal) Track((IDirect3DResource9*)*out, pool, _ReturnAddress(), "offscreen surface %ux%u %s", w, h, FormatName(fmt));
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                                 D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared)
{
    UINT askW = w, askH = h;
    bool shrunk = !s_internal && ShrinkSize(&w, &h, usage, fmt);
    HRESULT hr = o_CreateTexture(dev, w, h, levels, usage, fmt, pool, out, shared);
    if (SUCCEEDED(hr)) Track((IDirect3DResource9*)*out, pool, s_internal ? (void*)H_CreateTexture : _ReturnAddress(), "texture %ux%u %s", w, h, FormatName(fmt));
    if (shrunk && SUCCEEDED(hr)) {
        Shrink sc = {(float)w / askW, (float)h / askH};
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED((*out)->lpVtbl->GetSurfaceLevel(*out, 0, &s)) && s) {
            s->lpVtbl->SetPrivateData(s, kShrinkGuid, &sc, sizeof(sc), 0);
            s->lpVtbl->Release(s);
        }
        Log("CreateTexture %ux%u %s made %ux%u (image space at window scale)", askW, askH, FormatName(fmt), w, h);
    }
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
        int id = ++s_texSerial;
        Log("CreateTexture #%d %ux%u %s levels %u usage %08lx pool %d -> %08lx %p (from %p)", id, w, h, FormatName(fmt), levels,
            usage, pool, hr, SUCCEEDED(hr) ? *out : nullptr, _ReturnAddress());
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED((*out)->lpVtbl->GetSurfaceLevel(*out, 0, &s)) && s) {
            char label[24];
            wsprintfA(label, "%s#%d", usage & D3DUSAGE_DEPTHSTENCIL ? "dstex" : "rttex", id);
            SetLabel(s, label);
            s->lpVtbl->Release(s);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateRenderTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                                      DWORD q, BOOL lockable, IDirect3DSurface9** out, HANDLE* shared)
{
    HRESULT hr = o_CreateRenderTarget(dev, w, h, fmt, ms, q, lockable, out, shared);
    if (SUCCEEDED(hr)) Track((IDirect3DResource9*)*out, D3DPOOL_DEFAULT, s_internal ? (void*)H_CreateRenderTarget : _ReturnAddress(), "render target %ux%u %s", w, h, FormatName(fmt));
    int id = ++s_rtSerial;
    Log("CreateRenderTarget #%d %ux%u %s MSAA %d lockable %d -> %08lx %p (from %p)", id, w, h, FormatName(fmt), ms, lockable, hr,
        SUCCEEDED(hr) ? *out : nullptr, _ReturnAddress());
    if (SUCCEEDED(hr)) {
        char label[24];
        wsprintfA(label, "rt#%d", id);
        SetLabel(*out, label);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateDepthStencilSurface(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                                             DWORD q, BOOL discard, IDirect3DSurface9** out, HANDLE* shared)
{
    HRESULT hr = o_CreateDepthStencilSurface(dev, w, h, fmt, ms, q, discard, out, shared);
    if (SUCCEEDED(hr)) Track((IDirect3DResource9*)*out, D3DPOOL_DEFAULT, s_internal ? (void*)H_CreateDepthStencilSurface : _ReturnAddress(), "depth surface %ux%u %s", w, h, FormatName(fmt));
    int id = ++s_dsSerial;
    Log("CreateDepthStencilSurface #%d %ux%u %s MSAA %d -> %08lx %p (from %p)", id, w, h, FormatName(fmt), ms, hr,
        SUCCEEDED(hr) ? *out : nullptr, _ReturnAddress());
    if (SUCCEEDED(hr)) {
        char label[24];
        wsprintfA(label, "ds#%d", id);
        SetLabel(*out, label);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateAdditionalSwapChain(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** out)
{
    LogPP("CreateAdditionalSwapChain", pp);
    HRESULT hr = o_CreateAdditionalSwapChain(dev, pp, out);
    if (SUCCEEDED(hr) && out) TrackObj(*out, _ReturnAddress(), "additional swap chain", &s_chainRelease, (ReleaseFn)ChainRelease);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
    if (!s_internal && index == 0) s = SupersampleReplaceTarget(s);
    if (index == 0) {
        DWORD size = sizeof(s_rt0Shrink);
        D3DSURFACE_DESC d;
        if (!s || FAILED(s->lpVtbl->GetPrivateData(s, kShrinkGuid, &s_rt0Shrink, &size)) || FAILED(s->lpVtbl->GetDesc(s, &d)))
            s_rt0Shrink = {0, 0};
        else {
            s_rt0W = (float)d.Width;
            s_rt0H = (float)d.Height;
        }
    }
    TraceSurface("SetRenderTarget", index, s);
    return o_SetRenderTarget(dev, index, s);
}

static HRESULT STDMETHODCALLTYPE H_SetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s)
{
    if (!s_internal) s = SupersampleReplaceDepth(s);
    TraceSurface("SetDepthStencilSurface", 0, s);
    return o_SetDepthStencilSurface(dev, s);
}

static HRESULT STDMETHODCALLTYPE H_Clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    Trace("Clear(flags %lx, color %08lx, z %g, rects %lu)", flags, color, z, count);
    D3DRECT scaled[16];
    if (!s_internal) rects = SupersampleUiClearRects(count, rects, scaled, 16);
    return o_Clear(dev, count, rects, flags, color, z, stencil);
}

static HRESULT STDMETHODCALLTYPE H_StretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst,
                                               const RECT* dr, D3DTEXTUREFILTERTYPE filter)
{
    if (s_trace) {
        char a[96], b[96];
        Describe(src, a, sizeof(a));
        Describe(dst, b, sizeof(b));
        Trace("StretchRect(%s%s -> %s%s, filter %d)", a, sr ? " (rect)" : "", b, dr ? " (rect)" : "", filter);
    }
    return o_StretchRect(dev, src, sr, dst, dr, filter);
}

static HRESULT STDMETHODCALLTYPE H_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp)
{
    if (vp) Trace("SetViewport(%lu,%lu %lux%lu)", vp->X, vp->Y, vp->Width, vp->Height);
    D3DVIEWPORT9 ui;
    if (!s_internal && SupersampleUiViewport(vp, &ui)) return o_SetViewport(dev, &ui);  // the pixelated look's sharp interface
    if (vp && !s_internal && s_rt0Shrink.x > 0) {  // a shrunk VATS target: the game's full-size viewport, scaled
        D3DVIEWPORT9 v = *vp;
        v.X = (DWORD)(vp->X * s_rt0Shrink.x);
        v.Y = (DWORD)(vp->Y * s_rt0Shrink.y);
        v.Width = (DWORD)(vp->Width * s_rt0Shrink.x + 0.5f);
        v.Height = (DWORD)(vp->Height * s_rt0Shrink.y + 0.5f);
        if (v.Width < 1) v.Width = 1;
        if (v.Height < 1) v.Height = 1;
        return o_SetViewport(dev, &v);
    }
    return o_SetViewport(dev, vp);
}

static HRESULT STDMETHODCALLTYPE H_SetScissorRect(IDirect3DDevice9* dev, const RECT* r)
{
    if (r) Trace("SetScissorRect(%ld,%ld-%ld,%ld)", r->left, r->top, r->right, r->bottom);
    RECT ui;
    if (!s_internal) r = SupersampleUiScissor(r, &ui);
    return o_SetScissorRect(dev, r);
}

// While the pixelated look's interface is drawn at the window's resolution, the engine gets back
// the viewport and scissor rectangle it set (in its own, virtual, pixels).
static HRESULT STDMETHODCALLTYPE H_GetViewport(IDirect3DDevice9* dev, D3DVIEWPORT9* vp)
{
    HRESULT hr = o_GetViewport(dev, vp);
    if (SUCCEEDED(hr) && !s_internal) SupersampleUiGetViewport(vp);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_GetScissorRect(IDirect3DDevice9* dev, RECT* r)
{
    HRESULT hr = o_GetScissorRect(dev, r);
    if (SUCCEEDED(hr) && !s_internal) SupersampleUiGetScissor(r);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_BeginScene(IDirect3DDevice9* dev)
{
    Trace("BeginScene");
    return o_BeginScene(dev);
}

static HRESULT STDMETHODCALLTYPE H_EndScene(IDirect3DDevice9* dev)
{
    Trace("EndScene");
    return o_EndScene(dev);
}

static HRESULT STDMETHODCALLTYPE H_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    if (s_trace && m && state == D3DTS_PROJECTION)
        Trace("SetTransform(PROJECTION) m00 %.4f m11 %.4f m22 %.4f m32 %.4f", m->_11, m->_22, m->_33, m->_43);
    return o_SetTransform(dev, state, m);
}

// Trace only: shader constants and bound textures (what image-space passes read, and with which
// offsets).
static void TraceConstants(const char* what, UINT reg, const float* v, UINT count)
{
    if (!s_trace || s_internal || !v) return;
    char line[512];
    int n = wsprintfA(line, "%s c%u:", what, reg);
    for (UINT i = 0; i < count && i < 8 && n < 440; ++i)
        n += _snprintf(line + n, sizeof(line) - n, " (%.6g %.6g %.6g %.6g)", v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]);
    if (count > 8) n += _snprintf(line + n, sizeof(line) - n, " ... %u vectors", count);
    Trace("%s", line);
}

// While a shrunk texture is render target 0, the game's image-space shaders get their sample steps
// as constants in texels of the size it asked for (blur taps at -7..7 x 1/2160 for a 3840x2160
// request): constants equal to 1/2, 1 or 2 such texels become the same in texels of the real size,
// so blurs keep their radius on screen (as without supersampling) instead of shrinking with it.
static const float* FixTexelSteps(const float* v, UINT count, float* out)
{
    if (s_internal || s_rt0Shrink.x <= 0 || !v || count > 64) return v;
    float askW = s_rt0W / s_rt0Shrink.x, askH = s_rt0H / s_rt0Shrink.y;
    static const float kSteps[] = {0.5f, 1.f, 2.f};
    bool changed = false;
    for (UINT i = 0; i < count * 4; ++i) {
        float x = v[i], a = fabsf(x);
        out[i] = x;
        for (float k : kSteps) {
            if (fabsf(a - k / askW) < 1e-4f * (k / askW)) { out[i] = (x < 0 ? -k : k) / s_rt0W; changed = true; break; }
            if (fabsf(a - k / askH) < 1e-4f * (k / askH)) { out[i] = (x < 0 ? -k : k) / s_rt0H; changed = true; break; }
        }
    }
    return changed ? out : v;
}

static HRESULT STDMETHODCALLTYPE H_SetPixelShaderConstantF(IDirect3DDevice9* dev, UINT reg, const float* v, UINT count)
{
    float fixed[64 * 4];
    v = FixTexelSteps(v, count, fixed);
    TraceConstants("SetPixelShaderConstantF", reg, v, count);
    return o_SetPixelShaderConstantF(dev, reg, v, count);
}

static HRESULT STDMETHODCALLTYPE H_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT reg, const float* v, UINT count)
{
    float fixed[64 * 4];
    v = FixTexelSteps(v, count, fixed);
    TraceConstants("SetVertexShaderConstantF", reg, v, count);
    return o_SetVertexShaderConstantF(dev, reg, v, count);
}

static HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t)
{
    if (s_trace && !s_internal && t && t->lpVtbl->GetType(t) == D3DRTYPE_TEXTURE) {
        IDirect3DSurface9* surf = nullptr;
        IDirect3DTexture9* tex = (IDirect3DTexture9*)t;
        if (SUCCEEDED(tex->lpVtbl->GetSurfaceLevel(tex, 0, &surf)) && surf) {
            char d[96];
            Describe(surf, d, sizeof(d));
            surf->lpVtbl->Release(surf);
            if (strstr(d, "rttex")) Trace("SetTexture(%lu) = %s", stage, d);  // render targets only
        }
    }
    return o_SetTexture(dev, stage, t);
}

static HRESULT STDMETHODCALLTYPE H_GetRenderTargetData(IDirect3DDevice9* dev, IDirect3DSurface9* rt, IDirect3DSurface9* dst)
{
    static LONG n;
    NoteGetter("GetRenderTargetData", _ReturnAddress(), &n);
    if (!s_trace) return o_GetRenderTargetData(dev, rt, dst);
    char a[96], b[96];
    Describe(rt, a, sizeof(a));
    Describe(dst, b, sizeof(b));
    Trace("GetRenderTargetData(%s -> %s) from %p", a, b, _ReturnAddress());
    HRESULT hr = o_GetRenderTargetData(dev, rt, dst);
    Trace("GetRenderTargetData done -> %08lx", hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_GetFrontBufferData(IDirect3DDevice9* dev, UINT chain, IDirect3DSurface9* dst)
{
    static LONG n;
    NoteGetter("GetFrontBufferData", _ReturnAddress(), &n);
    return o_GetFrontBufferData(dev, chain, dst);
}

static HRESULT STDMETHODCALLTYPE H_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT start, UINT count)
{
    TraceDraw(count);
    return o_DrawPrimitive(dev, t, start, count);
}

static HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT verts,
                                                        UINT start, UINT count)
{
    TraceDraw(count);
    return o_DrawIndexedPrimitive(dev, t, base, minIdx, verts, start, count);
}

static HRESULT STDMETHODCALLTYPE H_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride)
{
    TraceDraw(count);
    return o_DrawPrimitiveUP(dev, t, count, data, stride);
}

static HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT minIdx, UINT verts, UINT count,
                                                          const void* idx, D3DFORMAT fmt, const void* data, UINT stride)
{
    TraceDraw(count);
    return o_DrawIndexedPrimitiveUP(dev, t, minIdx, verts, count, idx, fmt, data, stride);
}

static HRESULT STDMETHODCALLTYPE H_ResetEx(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode);

IDirect3DDevice9* D3DDevice() { return s_device; }

HRESULT D3DRealBackBuffer(IDirect3DSurface9** out)
{
    *out = nullptr;
    return s_device ? o_GetBackBuffer(s_device, 0, 0, D3DBACKBUFFER_TYPE_MONO, out) : E_FAIL;
}

void D3DAttach(IDirect3DDevice9* dev, const char* how)
{
    if (!dev) return;
    if (s_device) {
        Log("device check (%s): %p, hooked device %p, same vtable %s", how, dev, s_device, dev->lpVtbl == s_vtbl ? "yes" : "NO");
        return;
    }
    s_device = dev;
    s_vtbl = dev->lpVtbl;
    if (!s_traceLockInit) { InitializeCriticalSection(&s_traceLock); s_traceLockInit = true; }
    IDirect3DDevice9Vtbl* vt = dev->lpVtbl;
    VHOOK(vt, Reset);
    VHOOK(vt, Present);
    VHOOK(vt, TestCooperativeLevel);
    VHOOK(vt, GetBackBuffer);
    VHOOK(vt, GetSwapChain);
    VHOOK(vt, CreateTexture);
    VHOOK(vt, CreateRenderTarget);
    VHOOK(vt, CreateDepthStencilSurface);
    VHOOK(vt, CreateAdditionalSwapChain);
    VHOOK(vt, CreateCubeTexture);
    VHOOK(vt, CreateVolumeTexture);
    VHOOK(vt, CreateVertexBuffer);
    VHOOK(vt, CreateIndexBuffer);
    VHOOK(vt, CreateOffscreenPlainSurface);
    VHOOK(vt, CreateStateBlock);
    VHOOK(vt, EndStateBlock);
    VHOOK(vt, CreateQuery);
    VHOOK(vt, SetRenderTarget);
    VHOOK(vt, GetRenderTarget);
    VHOOK(vt, SetDepthStencilSurface);
    VHOOK(vt, GetDepthStencilSurface);
    VHOOK(vt, Clear);
    VHOOK(vt, StretchRect);
    VHOOK(vt, SetViewport);
    VHOOK(vt, GetViewport);
    VHOOK(vt, SetScissorRect);
    VHOOK(vt, GetScissorRect);
    VHOOK(vt, BeginScene);
    VHOOK(vt, EndScene);
    VHOOK(vt, DrawPrimitive);
    VHOOK(vt, DrawIndexedPrimitive);
    VHOOK(vt, DrawPrimitiveUP);
    VHOOK(vt, DrawIndexedPrimitiveUP);
    VHOOK(vt, SetTransform);
    VHOOK(vt, GetRenderTargetData);
    VHOOK(vt, SetPixelShaderConstantF);
    VHOOK(vt, SetVertexShaderConstantF);
    VHOOK(vt, SetTexture);
    VHOOK(vt, GetFrontBufferData);
    if (s_deviceIsEx) {
        IDirect3DDevice9ExVtbl* vx = ((IDirect3DDevice9Ex*)dev)->lpVtbl;
        VHOOK(vx, ResetEx);
    }
    Log("device %p hooked (%s, vtable %p, Ex %d)", dev, how, vt, s_deviceIsEx);
    LabelBackBuffers(dev);
}

// ---- IDirect3D9 hooks ------------------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE H_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                                D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    RECT cr = {};
    GetClientRect(focus, &cr);
    Log("CreateDevice(adapter %u, type %d, focus window %p (client %ldx%ld), flags %08lx) from %p", adapter, type, focus, cr.right,
        cr.bottom, flags, _ReturnAddress());
    LogPP("CreateDevice (requested)", pp);
    // A pure device rejects Get* calls and state block captures, which the mod's passes need.
    if (flags & D3DCREATE_PUREDEVICE) {
        flags &= ~D3DCREATE_PUREDEVICE;
        Log("CreateDevice: D3DCREATE_PUREDEVICE removed");
    }
    bool ss = SupersampleFixPresentParams(pp);
    if (ss) LogPP("CreateDevice (native, supersampling)", pp);
    HRESULT hr = Orig(d3d)->CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    LogPP("CreateDevice (result)", pp);
    if (ss) SupersampleRestorePresentParams(pp);
    Log("CreateDevice -> %08lx, device %p", hr, SUCCEEDED(hr) ? *out : nullptr);
    if (SUCCEEDED(hr)) {
        D3DAttach(*out, "CreateDevice");
        SupersampleDeviceReady(*out);
    }
    return hr;
}

// The fullscreen display mode of CreateDeviceEx / ResetEx follows the back buffer size.
static void FixMode(D3DDISPLAYMODEEX* mode, const D3DPRESENT_PARAMETERS* pp)
{
    if (!mode || !pp) return;
    mode->Width = pp->BackBufferWidth;
    mode->Height = pp->BackBufferHeight;
}

static HRESULT STDMETHODCALLTYPE H_CreateDeviceEx(IDirect3D9Ex* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                                  D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out)
{
    Log("CreateDeviceEx(adapter %u, type %d, focus window %p, flags %08lx, fullscreen mode %ux%u) from %p", adapter, type, focus, flags,
        mode ? mode->Width : 0, mode ? mode->Height : 0, _ReturnAddress());
    LogPP("CreateDeviceEx (requested)", pp);
    if (flags & D3DCREATE_PUREDEVICE) {
        flags &= ~D3DCREATE_PUREDEVICE;
        Log("CreateDeviceEx: D3DCREATE_PUREDEVICE removed");
    }
    bool ss = SupersampleFixPresentParams(pp);
    UINT modeW = mode ? mode->Width : 0, modeH = mode ? mode->Height : 0;
    if (ss) {
        FixMode(mode, pp);
        LogPP("CreateDeviceEx (native, supersampling)", pp);
    }
    HRESULT hr = Orig(d3d)->CreateDeviceEx(d3d, adapter, type, focus, flags, pp, mode, out);
    LogPP("CreateDeviceEx (result)", pp);
    if (ss) {
        SupersampleRestorePresentParams(pp);
        if (mode) { mode->Width = modeW; mode->Height = modeH; }
    }
    Log("CreateDeviceEx -> %08lx", hr);
    if (SUCCEEDED(hr)) {
        s_deviceIsEx = true;
        D3DAttach((IDirect3DDevice9*)*out, "CreateDeviceEx");
        SupersampleDeviceReady((IDirect3DDevice9*)*out);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_ResetEx(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode)
{
    LogPP("ResetEx (requested)", pp);
    {
        D3DScope scope;
        UnbindForReset((IDirect3DDevice9*)dev);
        FrameDeviceLost();
    }
    bool ss = SupersampleFixPresentParams(pp);
    UINT modeW = mode ? mode->Width : 0, modeH = mode ? mode->Height : 0;
    if (ss) FixMode(mode, pp);
    HRESULT hr = o_ResetEx(dev, pp, mode);
    if (ss) {
        SupersampleRestorePresentParams(pp);
        if (mode) { mode->Width = modeW; mode->Height = modeH; }
    }
    Log("ResetEx -> %08lx (from %p)", hr, _ReturnAddress());
    if (SUCCEEDED(hr)) {
        LabelBackBuffers((IDirect3DDevice9*)dev);
        SupersampleDeviceReady((IDirect3DDevice9*)dev);
    }
    return hr;
}

// With supersampling, the virtual size is listed as one more display mode (if the adapter does not
// have it), so the game's fullscreen mode check accepts iSize; CreateDevice gets the native mode.
static bool FakeMode(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt, UINT realCount, D3DDISPLAYMODE* out)
{
    int nw, nh, vw, vh;
    if (s_internal || !SupersampleVirtualSize(&nw, &nh, &vw, &vh) || fmt != D3DFMT_X8R8G8B8) return false;
    UINT refresh = 0;
    for (UINT i = 0; i < realCount; ++i) {
        D3DDISPLAYMODE m;
        if (FAILED(Orig(d3d)->EnumAdapterModes(d3d, adapter, fmt, i, &m))) continue;
        if ((int)m.Width == vw && (int)m.Height == vh) return false;  // a real mode already
        if ((int)m.Width == nw && (int)m.Height == nh && m.RefreshRate > refresh) refresh = m.RefreshRate;
    }
    if (out) *out = {(UINT)vw, (UINT)vh, refresh ? refresh : 60, fmt};
    return true;
}

static UINT STDMETHODCALLTYPE H_GetAdapterModeCount(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt)
{
    UINT n = Orig(d3d)->GetAdapterModeCount(d3d, adapter, fmt);
    if (FakeMode(d3d, adapter, fmt, n, nullptr)) ++n;
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 10) Log("GetAdapterModeCount(%u, %s) -> %u (from %p)", adapter, FormatName(fmt), n, _ReturnAddress());
    return n;
}

static HRESULT STDMETHODCALLTYPE H_EnumAdapterModes(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt, UINT mode, D3DDISPLAYMODE* out)
{
    UINT real = Orig(d3d)->GetAdapterModeCount(d3d, adapter, fmt);
    if (mode == real && out && FakeMode(d3d, adapter, fmt, real, out)) {
        static bool logged;
        if (!logged) {
            logged = true;
            Log("supersample: listing %ux%u@%u as an extra display mode", out->Width, out->Height, out->RefreshRate);
        }
        return D3D_OK;
    }
    HRESULT hr = Orig(d3d)->EnumAdapterModes(d3d, adapter, fmt, mode, out);
    static LONG calls;
    if (InterlockedIncrement(&calls) == 1) Log("EnumAdapterModes first call (from %p)", _ReturnAddress());
    return hr;
}

static void HookD3D(IDirect3D9* d3d, bool ex)
{
    if (!d3d) return;
    IDirect3D9Vtbl* vt = d3d->lpVtbl;
    D3DOrig* slot = nullptr;
    for (D3DOrig& o : s_d3dOrig) {
        if (o.vtable == vt) {
            // Windows' d3d9.dll uses one vtable for plain and Ex objects: the Ex slot is hooked
            // when the first Ex object shows up.
            if (ex && !o.CreateDeviceEx) {
                IDirect3D9ExVtbl* vx = ((IDirect3D9Ex*)d3d)->lpVtbl;
                o.CreateDeviceEx = vx->CreateDeviceEx;
                SwapSlot(&vx->CreateDeviceEx, (void*)H_CreateDeviceEx);
                Log("D3D: CreateDeviceEx hooked on vtable %p", vt);
            }
            return;
        }
        if (!o.vtable && !slot) slot = &o;
    }
    if (!slot) return;
    slot->vtable = vt;
    slot->CreateDevice = vt->CreateDevice;
    slot->GetAdapterModeCount = vt->GetAdapterModeCount;
    slot->EnumAdapterModes = vt->EnumAdapterModes;
    SwapSlot(&vt->CreateDevice, (void*)H_CreateDevice);
    SwapSlot(&vt->GetAdapterModeCount, (void*)H_GetAdapterModeCount);
    SwapSlot(&vt->EnumAdapterModes, (void*)H_EnumAdapterModes);
    if (ex) {
        IDirect3D9ExVtbl* vx = ((IDirect3D9Ex*)d3d)->lpVtbl;
        slot->CreateDeviceEx = vx->CreateDeviceEx;
        SwapSlot(&vx->CreateDeviceEx, (void*)H_CreateDeviceEx);
    }
    Log("D3D: IDirect3D9%s vtable %p hooked", ex ? "Ex" : "", vt);
}

static IDirect3D9* WINAPI My_Direct3DCreate9(UINT sdk)
{
    IDirect3D9* d3d = s_realCreate9(sdk);
    Log("Direct3DCreate9(%u) -> %p (from %p, main thread %lu)", sdk, d3d, _ReturnAddress(), MainThreadId());
    HookD3D(d3d, false);
    return d3d;
}

static HRESULT WINAPI My_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    HRESULT hr = s_realCreate9Ex(sdk, out);
    Log("Direct3DCreate9Ex(%u) -> %08lx %p (from %p)", sdk, hr, SUCCEEDED(hr) ? *out : nullptr, _ReturnAddress());
    if (SUCCEEDED(hr)) HookD3D((IDirect3D9*)*out, true);
    return hr;
}

void* D3DWrapProc(const char* name, void* real)
{
    if (strcmp(name, "Direct3DCreate9") == 0) {
        s_realCreate9 = (Direct3DCreate9Fn)real;
        return (void*)My_Direct3DCreate9;
    }
    if (strcmp(name, "Direct3DCreate9Ex") == 0) {
        s_realCreate9Ex = (Direct3DCreate9ExFn)real;
        return (void*)My_Direct3DCreate9Ex;
    }
    return real;
}
