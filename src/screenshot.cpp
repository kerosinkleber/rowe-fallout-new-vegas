// Screenshots.
//
// The screenshot key (rowe.ini screenshotkey, default F12) saves the finished frame (HUD included,
// and the stats window, before the settings menu is drawn) to screenshotdir, at the supersampled size (screenshotsize=full,
// the frame the game rendered, before it is averaged down) or the window's, as PNG or JPEG
// (screenshotformat, screenshotquality), encoded by Windows' imaging codecs (WIC). Like ReShade, the
// game is not held up while the file is written: the frame is copied on the GPU in the frame it is
// taken, read back over the next frames in strips and encoded on a worker thread.
//
// The `usershot` command takes a screenshot as the key does. Test screenshots come from fnvbridge
// (bridge_provider.cpp: capture points prelens, scene, final and overlay), with the bridge's state
// of the same frame.
//
// With showcase=1 the key takes the showcase series instead (see "showcase series" below).
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fnvfov.h"

static bool s_wantUser;

void ScreenshotUserRequest() { s_wantUser = true; }

static void TimeStamp(char* out)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wsprintfA(out, "%04d%02d%02d_%02d%02d%02d_%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
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

// The notice the overlay shows when a key's screenshot is written ("<file> was saved to <folder>"):
// written by the worker thread, then the count is raised; one shot is written at a time (s_busy).
static char s_notice[2 * MAX_PATH + 64];
static volatile LONG s_noticeCount;

// GPU side (render thread).
static IDirect3DTexture9* s_copy;
static IDirect3DSurface9* s_copySurface;  // the frame, as taken
static bool s_copyFresh;                   // s_copy was taken this frame: the overlay can still draw into it
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
    if (!s_abort) {
        const char* slash = strrchr(s_job.path, '\\');
        const char* file = slash ? slash + 1 : s_job.path;
        int dirLen = (int)(file - s_job.path);
        if (ok) _snprintf(s_notice, sizeof(s_notice) - 1, "%s was saved to %.*s", file, dirLen, s_job.path);
        else _snprintf(s_notice, sizeof(s_notice) - 1, "%s could not be saved to %.*s", file, dirLen, s_job.path);
        s_notice[sizeof(s_notice) - 1] = 0;
        InterlockedIncrement(&s_noticeCount);
    }
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

// full: at the supersampled size if there is one (else the window's); name: the file name without
// folder and extension (null: FalloutNV_<time>).
static void StartUserShot(IDirect3DDevice9* dev, IDirect3DSurface9* windowFrame, bool full, const char* name)
{
    if (s_busy) {
        Log("screenshot: still writing the previous one, skipped");
        return;
    }
    IDirect3DSurface9* src = nullptr;
    if (!(full && !SupersamplePixelSize() && SupersampleBackBuffer(&src))) {  // the supersampled frame, or the window's
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
    s_copyFresh = copied;
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
    if (name) _snprintf(s_job.path, MAX_PATH - 1, "%s%s.%s", dir, name, g_settings.screenshotJpg ? "jpg" : "png");
    else _snprintf(s_job.path, MAX_PATH - 1, "%sFalloutNV_%s.%s", dir, stamp, g_settings.screenshotJpg ? "jpg" : "png");
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

// ---- showcase series ---------------------------------------------------------------------------
// With showcase=1 the screenshot key takes a series instead of one shot: lens off, Panini,
// stereographic and fisheye at FOV 75, 90 and 160 (12 shots), at the window's size with the stats
// window in them (overlay.cpp draws it into each copy), named ROWE_showcase_<time>_fov<fov>_<lens>.
// Each setting gets kShowcaseSettle frames to take effect, and a shot waits until the previous one
// is written. The next shot's settings are applied only on the frame after a shot, because the
// overlay draws the stats window into the shot's copy later in the frame it is taken. The user's fov, lens and stats come back afterwards (SettingsSave writes those in
// the meantime); the key again stops the series.

static const float kShowcaseFov[] = {75.f, 90.f, 160.f};
static const int kShowcaseLens[] = {kLensOff, kLensPanini, kLensStereographic, kLensFisheye};
static const char* kShowcaseLensName[] = {"nolens", "panini", "stereographic", "fisheye"};
static const int kShowcaseShots = 12, kShowcaseSettle = 20;
static int s_showcaseShot = -1;  // the next shot of the running series (kShowcaseShots: all taken), -1 = none
static int s_showcaseWait;       // frames left before the next shot
static bool s_showcaseNext;      // a shot was taken last frame: apply the next one's settings now
static char s_showcaseStamp[32];
static float s_userFov;
static int s_userLens;
static bool s_userStats;

bool ShowcaseUserSettings(float* fov, int* lens, bool* stats)
{
    if (s_showcaseShot < 0) return false;
    *fov = s_userFov;
    *lens = s_userLens;
    *stats = s_userStats;
    return true;
}

static void ShowcaseApply(int shot)
{
    g_settings.fov = kShowcaseFov[shot / 4];
    g_settings.lens = kShowcaseLens[shot % 4];
    s_showcaseWait = kShowcaseSettle;
}

static void ShowcaseEnd(const char* how)
{
    g_settings.fov = s_userFov;
    g_settings.lens = s_userLens;
    g_settings.stats = s_userStats;
    Log("showcase: %s after %d of %d shots", how, s_showcaseShot, kShowcaseShots);
    s_showcaseShot = -1;
}

static void ShowcaseStart()
{
    s_userFov = g_settings.fov;
    s_userLens = g_settings.lens;
    s_userStats = g_settings.stats;
    g_settings.stats = true;
    TimeStamp(s_showcaseStamp);
    s_showcaseShot = 0;
    s_showcaseNext = false;
    ShowcaseApply(0);
    Log("showcase: a series of %d shots", kShowcaseShots);
}

static void ShowcaseFrame(IDirect3DDevice9* dev, IDirect3DSurface9* frame)
{
    if (s_showcaseShot < 0) return;
    if (s_showcaseShot >= kShowcaseShots) {  // the last one was taken last frame, stats window included
        ShowcaseEnd("done");
        return;
    }
    if (s_showcaseNext) {
        s_showcaseNext = false;
        ShowcaseApply(s_showcaseShot);
        return;
    }
    if (s_showcaseWait > 0) { --s_showcaseWait; return; }
    if (s_busy) return;  // the previous shot is still being written
    char name[96];
    _snprintf(name, sizeof(name) - 1, "ROWE_showcase_%s_fov%03d_%s", s_showcaseStamp, (int)kShowcaseFov[s_showcaseShot / 4],
              kShowcaseLensName[s_showcaseShot % 4]);
    name[sizeof(name) - 1] = 0;
    StartUserShot(dev, frame, false, name);
    if (++s_showcaseShot < kShowcaseShots) s_showcaseNext = true;
}

void ScreenshotPresent(IDirect3DDevice9* dev, IDirect3DSurface9* frame)
{
    static bool prev;
    bool down = GameInFront() && (GetAsyncKeyState(g_settings.screenshotKey) & 0x8000) != 0;
    if (down && !prev) s_wantUser = true;
    prev = down;
    if (!frame) return;
    BridgeCaptureSurface(kBridgeFinal, dev, frame);
    {
        D3DScope scope;
        ScreenshotReadStrips(dev, kStripsPerFrame);
        if (s_wantUser) {
            s_wantUser = false;
            if (s_showcaseShot >= 0) ShowcaseEnd("stopped");
            else if (g_settings.showcase) ShowcaseStart();
            else StartUserShot(dev, frame, g_settings.screenshotFull, nullptr);
        }
        ShowcaseFrame(dev, frame);
    }
}

LONG ScreenshotNotice(char* out, int size)
{
    LONG count = s_noticeCount;
    lstrcpynA(out, s_notice, size);
    return count;
}

IDirect3DSurface9* ScreenshotFreshCopy() { return s_copyFresh ? s_copySurface : nullptr; }

void ScreenshotAfterOverlay(IDirect3DDevice9* dev, IDirect3DSurface9* frame)
{
    if (frame) BridgeCaptureSurface(kBridgeOverlay, dev, frame);
    s_copyFresh = false;  // read back from the next frame on
}
