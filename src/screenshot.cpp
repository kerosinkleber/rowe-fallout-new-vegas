// Screenshots.
//
// The screenshot key (rowe.ini screenshotkey, default F12) saves the finished frame (HUD included,
// before the settings menu is drawn) to screenshotdir, at the supersampled size (screenshotsize=full,
// the frame the game rendered, before it is averaged down) or the window's, as PNG or JPEG
// (screenshotformat, screenshotquality), encoded by Windows' imaging codecs (WIC). Like ReShade, the
// game is not held up while the file is written: the frame is copied on the GPU in the frame it is
// taken, read back over the next frames in strips and encoded on a worker thread.
//
// The `shot` / `shotscene` / `shotmenu` commands (testing) save rowe_<date>_<time>.png next to the
// DLL at the window's size, at once, together with rowe_<date>_<time>.txt, a state snapshot taken
// in the same frame; `shotscene` captures the scene before the HUD (pre-UI hook), `shotmenu` the
// frame with the settings menu. `usershot` takes a screenshot as the key does.
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include "fnvfov.h"

static bool s_want, s_wantScene, s_wantMenu, s_wantUser;

void ScreenshotRequest(int kind)
{
    if (kind == kShotScene) s_wantScene = true;
    else if (kind == kShotWithMenu) s_wantMenu = true;
    else if (kind == kShotUser) s_wantUser = true;
    else s_want = true;
}

static void TimeStamp(char* out)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wsprintfA(out, "%04d%02d%02d_%02d%02d%02d_%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

// ---- test screenshots (commands): at once, window size, next to the DLL ----------------------

// Copies `src` (any size, X8R8G8B8 or A8R8G8B8, MSAA or not) to system memory and writes the PNG.
static bool Save(IDirect3DDevice9* dev, IDirect3DSurface9* src, const char* what)
{
    D3DSURFACE_DESC d;
    if (FAILED(src->GetDesc(&d))) return false;
    if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) {
        Log("screenshot: unsupported format %d", d.Format);
        return false;
    }
    IDirect3DSurface9* plain = src;
    IDirect3DSurface9* resolved = nullptr;
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
        if (FAILED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)))
            return false;
        dev->StretchRect(src, nullptr, resolved, nullptr, D3DTEXF_NONE);
        plain = resolved;
    }
    IDirect3DSurface9* sys = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(plain, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            BYTE* rgb = (BYTE*)malloc((size_t)d.Width * d.Height * 3);
            if (rgb) {
                for (UINT y = 0; y < d.Height; ++y) {
                    const BYTE* s = (const BYTE*)lr.pBits + (size_t)y * lr.Pitch;
                    BYTE* o = rgb + (size_t)y * d.Width * 3;
                    for (UINT x = 0; x < d.Width; ++x, s += 4, o += 3) {
                        o[0] = s[2];
                        o[1] = s[1];
                        o[2] = s[0];
                    }
                }
                char stamp[32], base[MAX_PATH], path[MAX_PATH];
                TimeStamp(stamp);
                wsprintfA(base, "%srowe_%s%s", ModDir(), stamp, what);
                wsprintfA(path, "%s.png", base);
                ok = WritePng(path, d.Width, d.Height, rgb);
                free(rgb);
                Log("screenshot: %s (%ux%u) %s", path, d.Width, d.Height, ok ? "saved" : "FAILED");
                wsprintfA(path, "%s.txt", base);
                WriteStateTo(path);
            }
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    if (resolved) resolved->Release();
    return ok;
}

// ---- user screenshots: GPU copy, read back in strips, encoded on a worker thread -------------

static const UINT kStripRows = 128;
static const int kQueueSize = 16;      // strips waiting for the encoder
static const int kStripsPerFrame = 4;  // read back per frame (each a short GPU stall)

struct Strip { BYTE* bgr; UINT rows; };
static CRITICAL_SECTION s_qLock;
static HANDLE s_qEvent;
static Strip s_queue[kQueueSize];
static int s_qHead, s_qCount;
static volatile bool s_allQueued, s_abort;
static volatile LONG s_busy;  // a user screenshot is being taken or written

static struct { char path[MAX_PATH]; UINT w, h; bool jpg; int quality; DWORD start; } s_job;

// GPU side (render thread).
static IDirect3DTexture9* s_copy;
static IDirect3DSurface9* s_copySurface;  // the frame, as taken
static IDirect3DSurface9* s_stripRT;
static IDirect3DSurface9* s_stripSys;
static UINT s_nextRow;

static void ReleaseGpu()
{
    if (s_stripSys) { s_stripSys->Release(); s_stripSys = nullptr; }
    if (s_stripRT) { s_stripRT->Release(); s_stripRT = nullptr; }
    if (s_copySurface) { s_copySurface->Release(); s_copySurface = nullptr; }
    if (s_copy) { s_copy->Release(); s_copy = nullptr; }
}

static bool Pop(Strip* out)
{
    EnterCriticalSection(&s_qLock);
    bool got = s_qCount > 0;
    if (got) {
        *out = s_queue[s_qHead];
        s_qHead = (s_qHead + 1) % kQueueSize;
        --s_qCount;
    }
    LeaveCriticalSection(&s_qLock);
    return got;
}

static bool Push(BYTE* bgr, UINT rows)
{
    EnterCriticalSection(&s_qLock);
    bool room = s_qCount < kQueueSize;
    if (room) {
        s_queue[(s_qHead + s_qCount) % kQueueSize] = {bgr, rows};
        ++s_qCount;
    }
    LeaveCriticalSection(&s_qLock);
    if (room) SetEvent(s_qEvent);
    return room;
}

static bool QueueFull()
{
    EnterCriticalSection(&s_qLock);
    bool full = s_qCount >= kQueueSize;
    LeaveCriticalSection(&s_qLock);
    return full;
}

static DWORD WINAPI EncodeThread(void*)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, s_job.path, -1, wpath, MAX_PATH);
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(wpath, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(s_job.jpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr) && props && s_job.jpg) {
        PROPBAG2 opt = {};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = s_job.quality / 100.f;
        props->Write(1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);
    if (SUCCEEDED(hr)) hr = frame->SetSize(s_job.w, s_job.h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;  // both encoders take 24bpp BGR
    if (FAILED(hr)) Log("screenshot: encoder setup failed (%08lx) for %s", hr, s_job.path);
    UINT written = 0;
    for (;;) {
        Strip st;
        if (Pop(&st)) {
            if (SUCCEEDED(hr) && !s_abort) hr = frame->WritePixels(st.rows, s_job.w * 3, s_job.w * 3 * st.rows, st.bgr);
            written += st.rows;
            free(st.bgr);
            continue;
        }
        if (s_abort || s_allQueued) {
            Strip rest;
            while (Pop(&rest)) free(rest.bgr);
            break;
        }
        WaitForSingleObject(s_qEvent, 200);
    }
    bool ok = SUCCEEDED(hr) && !s_abort && written == s_job.h;
    if (ok) hr = frame->Commit();
    if (ok && SUCCEEDED(hr)) hr = encoder->Commit();
    ok = ok && SUCCEEDED(hr);
    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok) DeleteFileA(s_job.path);
    Log("screenshot: %s (%ux%u, %s) %s in %lu ms", s_job.path, s_job.w, s_job.h, s_job.jpg ? "JPEG" : "PNG",
        ok ? "saved" : (s_abort ? "abandoned" : "FAILED"), GetTickCount() - s_job.start);
    InterlockedExchange(&s_busy, 0);
    return 0;
}

// The screenshot folder (created if missing), with a trailing backslash.
void ScreenshotFolder(char* out)
{
    char raw[MAX_PATH], dir[MAX_PATH];
    if (!g_settings.screenshotDir[0]) {
        lstrcpynA(out, ModDir(), MAX_PATH);
        return;
    }
    ExpandEnvironmentStringsA(g_settings.screenshotDir, raw, MAX_PATH);
    GetFullPathNameA(raw, MAX_PATH, dir, nullptr);  // relative to the current folder, the game's
    int n = lstrlenA(dir);
    if (n && dir[n - 1] != '\\' && dir[n - 1] != '/' && n < MAX_PATH - 1) lstrcatA(dir, "\\");
    for (char* p = dir + 3; *p; ++p) {  // each level, "C:\a\b\" -> "C:\a", "C:\a\b"
        if (*p != '\\' && *p != '/') continue;
        char c = *p;
        *p = 0;
        CreateDirectoryA(dir, nullptr);
        *p = c;
    }
    lstrcpynA(out, dir, MAX_PATH);
}

static void ScreenshotReadStrips(IDirect3DDevice9* dev, int count);

static void StartUserShot(IDirect3DDevice9* dev, IDirect3DSurface9* windowFrame)
{
    if (s_busy) {
        Log("screenshot: still writing the previous one, skipped");
        return;
    }
    IDirect3DSurface9* src = nullptr;
    if (!(g_settings.screenshotFull && SupersampleBackBuffer(&src))) {  // the supersampled frame, or the window's
        src = windowFrame;
        src->AddRef();
    }
    D3DSURFACE_DESC d;
    src->GetDesc(&d);
    if ((d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) || d.MultiSampleType != D3DMULTISAMPLE_NONE) {
        Log("screenshot: unsupported frame (format %d, MSAA %d)", d.Format, d.MultiSampleType);
        src->Release();
        return;
    }
    // The copy keeps this frame while it is read back; without the video memory for it the frame
    // itself is read back at once (the game waits for that one).
    bool copied = SUCCEEDED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &s_copy, nullptr)) &&
                  SUCCEEDED(s_copy->GetSurfaceLevel(0, &s_copySurface)) &&
                  SUCCEEDED(dev->StretchRect(src, nullptr, s_copySurface, nullptr, D3DTEXF_NONE));
    if (!copied) {
        ReleaseGpu();
        s_copySurface = src;
        src->AddRef();
    }
    src->Release();
    UINT rows = kStripRows < d.Height ? kStripRows : d.Height;
    if (FAILED(dev->CreateRenderTarget(d.Width, rows, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s_stripRT, nullptr)) ||
        FAILED(dev->CreateOffscreenPlainSurface(d.Width, rows, d.Format, D3DPOOL_SYSTEMMEM, &s_stripSys, nullptr))) {
        Log("screenshot: strip surfaces %ux%u failed", d.Width, rows);
        ReleaseGpu();
        return;
    }
    char dir[MAX_PATH], stamp[32];
    ScreenshotFolder(dir);
    TimeStamp(stamp);
    _snprintf(s_job.path, MAX_PATH - 1, "%sFalloutNV_%s.%s", dir, stamp, g_settings.screenshotJpg ? "jpg" : "png");
    s_job.w = d.Width;
    s_job.h = d.Height;
    s_job.jpg = g_settings.screenshotJpg;
    s_job.quality = g_settings.screenshotQuality;
    s_job.start = GetTickCount();
    s_nextRow = 0;
    s_allQueued = s_abort = false;
    if (!s_qEvent) {
        InitializeCriticalSection(&s_qLock);
        s_qEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
    InterlockedExchange(&s_busy, 1);
    HANDLE t = CreateThread(nullptr, 0, EncodeThread, nullptr, 0, nullptr);
    if (!t) {
        Log("screenshot: worker thread failed");
        ReleaseGpu();
        InterlockedExchange(&s_busy, 0);
        return;
    }
    CloseHandle(t);
    Log("screenshot: taking %ux%u%s -> %s", d.Width, d.Height, copied ? "" : " (no copy: read back at once)", s_job.path);
    if (!copied) {  // read it all now, waiting for the encoder where needed
        while (s_copySurface) {
            while (QueueFull() && !s_abort) Sleep(1);
            ScreenshotReadStrips(dev, 1);
        }
    }
}

// Reads up to `count` strips of the taken frame and queues them for the encoder.
static void ScreenshotReadStrips(IDirect3DDevice9* dev, int count)
{
    if (!s_copySurface) return;
    D3DSURFACE_DESC d;
    s_copySurface->GetDesc(&d);
    D3DSURFACE_DESC sd;
    s_stripRT->GetDesc(&sd);
    for (int i = 0; i < count && s_nextRow < d.Height && !QueueFull(); ++i) {
        UINT rows = d.Height - s_nextRow < sd.Height ? d.Height - s_nextRow : sd.Height;
        RECT sr = {0, (LONG)s_nextRow, (LONG)d.Width, (LONG)(s_nextRow + rows)}, dr = {0, 0, (LONG)d.Width, (LONG)rows};
        D3DLOCKED_RECT lr;
        BYTE* bgr = (BYTE*)malloc((size_t)d.Width * 3 * rows);
        if (!bgr || FAILED(dev->StretchRect(s_copySurface, &sr, s_stripRT, &dr, D3DTEXF_NONE)) ||
            FAILED(dev->GetRenderTargetData(s_stripRT, s_stripSys)) || FAILED(s_stripSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            free(bgr);
            Log("screenshot: read back failed at row %u", s_nextRow);
            s_abort = true;
            ReleaseGpu();
            return;
        }
        for (UINT y = 0; y < rows; ++y) {  // X8R8G8B8 is B, G, R, X in memory: drop X
            const BYTE* s = (const BYTE*)lr.pBits + (size_t)y * lr.Pitch;
            BYTE* o = bgr + (size_t)y * d.Width * 3;
            for (UINT x = 0; x < d.Width; ++x, s += 4, o += 3) {
                o[0] = s[0];
                o[1] = s[1];
                o[2] = s[2];
            }
        }
        s_stripSys->UnlockRect();
        Push(bgr, rows);
        s_nextRow += rows;
    }
    if (s_nextRow >= d.Height) {
        s_allQueued = true;
        SetEvent(s_qEvent);
        ReleaseGpu();
    }
}

void ScreenshotLost()
{
    if (s_copySurface || s_stripRT) {
        Log("screenshot: device reset while taking one, abandoned");
        s_abort = true;
        if (s_qEvent) SetEvent(s_qEvent);
    }
    ReleaseGpu();
}

void ScreenshotPresent(IDirect3DDevice9* dev, IDirect3DSurface9* frame)
{
    static bool prev;
    bool down = GameInFront() && (GetAsyncKeyState(g_settings.screenshotKey) & 0x8000) != 0;
    if (down && !prev) s_wantUser = true;
    prev = down;
    if (!frame) return;
    {
        D3DScope scope;
        ScreenshotReadStrips(dev, kStripsPerFrame);
        if (s_wantUser) {
            s_wantUser = false;
            StartUserShot(dev, frame);
        }
    }
    if (!s_want) return;
    s_want = false;
    Save(dev, frame, "");
}

void ScreenshotAfterOverlay(IDirect3DDevice9* dev, IDirect3DSurface9* frame)
{
    if (!s_wantMenu || !frame) return;
    s_wantMenu = false;
    Save(dev, frame, "_menu");
}

void ScreenshotPreUI(IDirect3DDevice9* dev)
{
    if (!s_wantScene) return;
    s_wantScene = false;
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) {
        Save(dev, rt, "_scene");
        rt->Release();
    }
}
