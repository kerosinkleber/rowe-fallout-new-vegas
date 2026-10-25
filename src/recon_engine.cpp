// Reconnaissance: engine-side hooks and the state file.
//
// - BSSceneGraph::SetCameraFOV (0x00C52020) and its companion 0x00B54000 are detoured and log
//   every caller and every change of the value per caller (which callers are world, viewmodel,
//   aiming, VATS, ...), plus the camera frustum the setter produced.
// - The render call sites the lens and supersampling phases will use are stamped: their first
//   call (thread), their count per frame, and their position in the frame trace (with the bound
//   render target and the world camera frustum at that point).
// - rowe_state.txt is rewritten four times a second with menu state, camera, player FOV fields,
//   window and settings, to watch while playing.
//
// Addresses and layouts are from xNVSE, JIP LN NVSE, NVR, JohnnyGuitar and psycho (see the
// research report); the ones in conflict are logged both ways.
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <string.h>
#include "fnvfov.h"

static const float kDeg = 57.29578f;

// ---- engine globals --------------------------------------------------------------------------

static BYTE* OsGlobals() { return *(BYTE**)0x011DEA0C; }
static BYTE* Renderer() { return *(BYTE**)0x011F4748; }       // NiDX9Renderer
static BYTE* Player() { return *(BYTE**)0x011DEA3C; }         // PlayerCharacter
static BYTE* WorldSceneGraph() { return *(BYTE**)0x011DEB7C; }
static BYTE* InterfaceMgr() { return *(BYTE**)0x011D8A80; }
static const BYTE* MenuVisible() { return (const BYTE*)0x011F308F; }  // indexed by menu id

static bool Readable(const void* p, int n) { return p && !IsBadReadPtr(p, n); }
static float F(const BYTE* base, int off) { return Readable(base + off, 4) ? *(const float*)(base + off) : NAN; }
static BYTE B(const BYTE* base, int off) { return Readable(base + off, 1) ? base[off] : 0xEE; }
static void* P(const BYTE* base, int off) { return Readable(base + off, 4) ? *(void* const*)(base + off) : nullptr; }

// ---- settings found by name ------------------------------------------------------------------
// A Setting object is {vtable, value, name}; it is found as the .data dword pointing at its name.

struct Setting { const char* name; BYTE* obj; };
static Setting s_settings[] = {
    {"iSize W:Display", nullptr}, {"iSize H:Display", nullptr}, {"bFull Screen:Display", nullptr},
    {"bBorder:Display", nullptr}, {"iPresentInterval:Display", nullptr}, {"iMultiSample:Display", nullptr},
    {"fDefaultWorldFOV:Display", nullptr}, {"fDefault1stPersonFOV:Display", nullptr}, {"fDefaultFOV:Display", nullptr},
    {"sIntroSequence:General", nullptr}, {"sMainMenuMovieIntro:General", nullptr}, {"bAllowScreenShot:Display", nullptr},
    {"SScreenShotBaseName:Display", nullptr}, {"iScreenShotIndex:Display", nullptr}, {"bAlwaysActive:General", nullptr},
};

static bool Section(const char* want, DWORD* begin, DWORD* end)
{
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (strncmp((char*)s->Name, want, 8) == 0) {
            *begin = (DWORD)base + s->VirtualAddress;
            *end = *begin + s->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

BYTE* FindSettingObject(const char* name)
{
    DWORD rb, re, db, de;
    if (!Section(".rdata", &rb, &re) || !Section(".data", &db, &de)) return nullptr;
    int len = lstrlenA(name) + 1;
    for (DWORD a = rb; a + len <= re; ++a) {
        if (memcmp((void*)a, name, len) != 0 || *(BYTE*)(a - 1) != 0) continue;
        for (DWORD p = db; p + 4 <= de; p += 4) {
            if (*(DWORD*)p != a) continue;
            BYTE* obj = (BYTE*)(p - 8);
            DWORD vt = *(DWORD*)obj;
            if (vt >= rb && vt < re) return obj;
        }
    }
    return nullptr;
}

static void FindSettings()
{
    for (Setting& s : s_settings) s.obj = FindSettingObject(s.name);
}

static void SettingValue(const Setting& s, char* out, int size)
{
    if (!s.obj) { lstrcpynA(out, "(not found)", size); return; }
    BYTE* v = s.obj + 4;
    switch (s.name[0]) {
    case 'i': _snprintf(out, size, "%d", *(int*)v); break;
    case 'f': _snprintf(out, size, "%g", *(float*)v); break;
    case 'b': _snprintf(out, size, "%u", *v); break;
    case 's': case 'S': {
        const char* str = *(const char**)v;
        _snprintf(out, size, "\"%s\"", Readable(str, 1) ? str : "(bad)");
        break;
    }
    default: _snprintf(out, size, "%08lx", *(DWORD*)v);
    }
    out[size - 1] = 0;
}

static void LogSettings(const char* when)
{
    for (const Setting& s : s_settings) {
        char v[160];
        SettingValue(s, v, sizeof(v));
        Log("setting (%s) %-30s object %p = %s", when, s.name, s.obj, v);
    }
}

// ---- camera ----------------------------------------------------------------------------------
// NiCamera frustum {l, r, t, b, n, f}: JIP puts it at +0xDC, xNVSE at +0xEC. Both are read.

struct Frustum { float l, r, t, b, n, f; };
static Frustum ReadFrustum(const BYTE* cam, int off)
{
    Frustum fr = {NAN, NAN, NAN, NAN, NAN, NAN};
    if (Readable(cam + off, sizeof(fr))) fr = *(const Frustum*)(cam + off);
    return fr;
}
static BYTE* WorldCamera() { BYTE* sg = WorldSceneGraph(); return sg ? (BYTE*)P(sg, 0xAC) : nullptr; }
static float HFovDeg(const Frustum& f) { return 2.f * atanf((f.r - f.l) * 0.5f) * kDeg; }
static float VFovDeg(const Frustum& f) { return 2.f * atanf((f.t - f.b) * 0.5f) * kDeg; }

static Frustum s_worldDrawFrustum[2], s_preUiFrustum;

// ---- SetCameraFOV and 0xB54000 ---------------------------------------------------------------

typedef DWORD(__fastcall* SetCameraFovFn)(void* sg, void* edx, float fov, DWORD force, void* cam, DWORD setLod);
typedef void(__cdecl* FovCompFn)(float fov);
static SetCameraFovFn s_setCameraFov;
static FovCompFn s_fovComp;

// Per call site: the log gets a line when a site is first seen and whenever its value changes,
// each hook with its own line budget (a busy hook must not silence the other).
struct Caller { char kind; void* ret; void* sg; LONG count; float last; };
static Caller s_callers[96];
static int s_callerCount;
static LONG s_fovLines, s_compLines, s_setFovCalls, s_compCalls;
static const LONG kLineBudget = 3000;

static Caller* FindCaller(char kind, void* ret)
{
    for (int i = 0; i < s_callerCount; ++i)
        if (s_callers[i].ret == ret && s_callers[i].kind == kind) return &s_callers[i];
    if (s_callerCount == 96) return nullptr;
    Caller* c = &s_callers[s_callerCount++];
    c->kind = kind;
    c->ret = ret;
    c->last = NAN;
    return c;
}

static bool TakeLine(LONG* budget, const char* hook)
{
    LONG n = InterlockedIncrement(budget);
    if (n == kLineBudget + 1) Log("%s: log line budget used up; see rowe_fovtrace.csv for details", hook);
    return n <= kLineBudget;
}

// ---- FOV trace (rowe_fovtrace.csv) ----------------------------------------------------------
// Every setter call for a number of presents: what each call site asked for and what it produced.

struct FovRow { DWORD frame; float t; char kind; void* ret; void* sg; float fov; BYTE force, setLod; float r, tt, p670, p674, p678; BYTE b64A, b64C, menu; };
static const int kMaxFovRows = 60000;
static FovRow* s_fovRows;
static volatile LONG s_fovRowCount;
static volatile bool s_fovTracing;
static DWORD s_fovTraceEnd;
static LARGE_INTEGER s_fovT0, s_qpf;
static DWORD s_presentCount;

static DWORD s_lastWorldDraw = (DWORD)-1000;

DWORD FrameCount() { return s_presentCount; }
DWORD LastWorldDrawFrame() { return s_lastWorldDraw; }

void FovTraceStart(int frames)
{
    if (s_fovTracing) return;
    if (!s_fovRows) s_fovRows = (FovRow*)VirtualAlloc(nullptr, sizeof(FovRow) * kMaxFovRows, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s_fovRows) return;
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_fovT0);
    s_fovRowCount = 0;
    s_fovTraceEnd = s_presentCount + (frames > 0 ? frames : 300);
    s_fovTracing = true;
    Log("fovtrace: started for %d frames at frame %lu", frames, s_presentCount);
}

static void FovTraceAdd(char kind, void* ret, void* sg, float fov, DWORD force, DWORD setLod, const BYTE* camera)
{
    if (!s_fovTracing) return;
    LONG i = InterlockedIncrement(&s_fovRowCount) - 1;
    if (i >= kMaxFovRows) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    Frustum fr = ReadFrustum(camera, 0xDC);
    BYTE* pl = Player();
    typedef bool(__cdecl * IsMenuModeFn)();
    FovRow& r = s_fovRows[i];
    r = {s_presentCount, (float)((now.QuadPart - s_fovT0.QuadPart) * 1000.0 / s_qpf.QuadPart), kind, ret, sg, fov, (BYTE)force,
         (BYTE)setLod, fr.r, fr.t, F(pl, 0x670), F(pl, 0x674), F(pl, 0x678), B(pl, 0x64A), B(pl, 0x64C),
         (BYTE)((IsMenuModeFn)0x00702360)()};
}

static void FovTraceDump()
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe_fovtrace.csv", ModDir());
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "frame,t_ms,kind,ret,sg,fov,force,setlod,frustum_r,frustum_t,h_deg,pl_world,pl_1st,pl_shoulder,b64A,b64C,menumode\n");
    LONG n = s_fovRowCount < kMaxFovRows ? s_fovRowCount : kMaxFovRows;
    for (LONG i = 0; i < n; ++i) {
        const FovRow& r = s_fovRows[i];
        fprintf(f, "%lu,%.2f,%c,%p,%p,%.3f,%u,%u,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f,%u,%u,%u\n", r.frame, r.t, r.kind, r.ret, r.sg, r.fov,
                r.force, r.setLod, r.r, r.tt, 2.f * atanf(r.r) * kDeg, r.p670, r.p674, r.p678, r.b64A, r.b64C, r.menu);
    }
    fclose(f);
    Log("fovtrace: wrote %ld rows to rowe_fovtrace.csv", n);
}

// Writes a trace that is still recording (the game is exiting).
void FovTraceFinish()
{
    if (!s_fovTracing) return;
    s_fovTracing = false;
    FovTraceDump();
}

static DWORD __fastcall Hook_SetCameraFov(void* sg, void* edx, float fov, DWORD force, void* cam, DWORD setLod)
{
    void* ret = _ReturnAddress();
    float asked = fov;
    fov = FovRemapSetter(ret, sg, fov);
    DWORD r = s_setCameraFov(sg, edx, fov, force, cam, setLod);
    InterlockedIncrement(&s_setFovCalls);
    BYTE* camera = cam ? (BYTE*)cam : (BYTE*)P((BYTE*)sg, 0xAC);
    FovTraceAdd('S', ret, sg, fov, force & 0xFF, setLod & 0xFF, camera);
    Caller* c = FindCaller('S', ret);
    if (!c) return r;
    bool isNew = c->count++ == 0;
    if ((isNew || fabsf(asked - c->last) > 0.05f || sg != c->sg) && TakeLine(&s_fovLines, "SetCameraFOV")) {
        Frustum a = ReadFrustum(camera, 0xDC);
        BYTE* pl = Player();
        Log("SetCameraFOV %s caller %p: fov %.3f -> %.3f force %lu cam %p setLOD %lu sg %p%s | frustum r %.4f t %.4f (h %.2f v %.2f) near %.1f far %.0f"
            " | LOD %.3f | player fov %.2f/%.2f/%.2f 3rd %u/%u | frame %lu",
            isNew ? "NEW" : "chg", ret, asked, fov, force & 0xFF, cam, setLod & 0xFF, sg, sg == WorldSceneGraph() ? " (world)" : "",
            a.r, a.t, HFovDeg(a), VFovDeg(a), a.n, a.f, F(camera, 0x110), F(pl, 0x670), F(pl, 0x674), F(pl, 0x678), B(pl, 0x64A),
            B(pl, 0x64C), s_presentCount);
    }
    c->last = asked;
    c->sg = sg;
    return r;
}

static void __cdecl Hook_FovComp(float fov)
{
    void* ret = _ReturnAddress();
    InterlockedIncrement(&s_compCalls);
    fov = FovRemapComp(fov);
    FovTraceAdd('C', ret, nullptr, fov, 0, 0, nullptr);
    Caller* c = FindCaller('C', ret);
    if (c) {
        bool isNew = c->count++ == 0;
        if ((isNew || fabsf(fov - c->last) > 0.05f) && TakeLine(&s_compLines, "FovComp"))
            Log("FovComp(0xB54000) %s caller %p: fov %.3f | frame %lu", isNew ? "NEW" : "chg", ret, fov, s_presentCount);
        c->last = fov;
    }
    s_fovComp(fov);
}

// ---- render call-site stamps -----------------------------------------------------------------

struct Site { DWORD va, target; const char* name; void* chained; LONG perFrame, lastFrame, total; DWORD thread; };
static Site s_sites[] = {
    {0x00870AE8, 0x00873200, "world draw A (RenderWorldSceneGraph)"},
    {0x00870E18, 0x00873200, "world draw B (RenderWorldSceneGraph)"},
    {0x00876136, 0x00B55AC0, "ProcessImageSpaceShaders call"},
    {0x00870994, 0x00875FD0, "image-space + splatter call (RTM site)"},
    {0x008703F1, 0x005D4A40, "post-image-space call (JG site)"},
    {0x007144D3, 0x007134D0, "pre-UI: InterfaceManager::Render call"},
};
static const int kSiteCount = sizeof(s_sites) / sizeof(s_sites[0]);
static const int kSitePostImageSpace = 4;  // before the viewmodel pass

static void __cdecl Stamp(int id)
{
    Site& s = s_sites[id];
    ++s.perFrame;
    if (InterlockedIncrement(&s.total) == 1) {
        s.thread = GetCurrentThreadId();
        Log("first call through %s at %08lx (main thread %lu)", s.name, s.va, MainThreadId());
    }
    BYTE* cam = WorldCamera();
    Frustum fr = ReadFrustum(cam, 0xDC);
    if (id <= 1) {
        s_worldDrawFrustum[id] = fr;
        s_lastWorldDraw = s_presentCount;
        LensWorldDraw(fr.r);
    }
    if (id == kSiteCount - 1) s_preUiFrustum = fr;
    if (D3DTracing()) {
        char rt[96], line[256];
        D3DDescribeRT(rt, sizeof(rt));
        _snprintf(line, sizeof(line), "%s; RT0 = %s; world camera %p frustum r %.4f t %.4f (h %.2f v %.2f)", s.name, rt, cam, fr.r,
                  fr.t, HFovDeg(fr), VFovDeg(fr));
        line[sizeof(line) - 1] = 0;
        D3DTraceStamp(line);
    }
    if (id == kSitePostImageSpace && D3DDevice()) {
        D3DScope scope;
        LensPostImageSpace(D3DDevice());
    }
    if (id == kSiteCount - 1 && D3DDevice()) {
        D3DScope scope;
        LensPreUI(D3DDevice());
        ScreenshotPreUI(D3DDevice());
    }
}

// Thunk per site: pushad; pushfd; push id; call Stamp; add esp,4; popfd; popad; jmp [target].
static void* MakeStampThunk(int id, void* target)
{
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    BYTE* p = t;
    *p++ = 0x60;
    *p++ = 0x9C;
    *p++ = 0x68; *(int*)p = id; p += 4;
    *p++ = 0xE8; *(int*)p = (int)((BYTE*)Stamp - (p + 4)); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x9D;
    *p++ = 0x61;
    void** slot = (void**)(t + 48);
    *slot = target;
    *p++ = 0xFF; *p++ = 0x25; *(void***)p = slot;
    return t;
}

static void InstallStamps()
{
    for (int i = 0; i < kSiteCount; ++i) {
        Site& s = s_sites[i];
        BYTE* site = (BYTE*)s.va;
        if (site[0] != 0xE8) {
            PatchCall(s.va, s.target, nullptr, s.name);  // logs and records the skip
            continue;
        }
        // Build the thunk for the current target first, then patch the call to the thunk.
        void* current = (void*)(s.va + 5 + *(int*)(site + 1));
        void* thunk = MakeStampThunk(i, current);
        if (thunk) s.chained = PatchCall(s.va, s.target, thunk, s.name);
    }
}

// ---- state file ------------------------------------------------------------------------------

static LONG s_mainLoops, s_presents;
static DWORD s_mainLoopThread, s_presentThread;
static DWORD s_lastStateTick;
static int s_bbWidth, s_bbHeight;

void ReconEngineOnPresent()
{
    if (!s_presentThread) {
        s_presentThread = GetCurrentThreadId();
        Log("first Present on thread %lu (main thread %lu)", s_presentThread, MainThreadId());
    }
    ++s_presents;
    ++s_presentCount;
    for (int i = 0; i < kSiteCount; ++i) {
        s_sites[i].lastFrame = s_sites[i].perFrame;
        s_sites[i].perFrame = 0;
    }
    if (s_fovTracing) {
        FovTraceAdd('P', nullptr, nullptr, 0, 0, 0, WorldCamera());
        if (s_presentCount >= s_fovTraceEnd) {
            s_fovTracing = false;
            FovTraceDump();
        }
    }
}

// Writes a state snapshot to `path` (through a temporary file, so readers never see half a file).
void WriteStateTo(const char* path)
{
    char tmp[MAX_PATH];
    wsprintfA(tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "frame=%lu time=%02d:%02d:%02d main_loops=%ld\n", s_presentCount, t.wHour, t.wMinute, t.wSecond, s_mainLoops);
    fprintf(f, "threads: main %lu, main loop %lu, present %lu\n", MainThreadId(), s_mainLoopThread, s_presentThread);

    BYTE* im = InterfaceMgr();
    typedef bool(__cdecl * IsMenuModeFn)();
    fprintf(f, "menu: IsMenuMode %d, InterfaceManager %p currentMode %lu pipBoyMode %lu, console %u, byte 2D %u, byte 2B %u, loading %u\n",
            ((IsMenuModeFn)0x00702360)(), im, im ? *(DWORD*)(im + 0x0C) : 0, im ? *(DWORD*)(im + 0x4BC) : 0,
            *(BYTE*)0x011DEA2E, *(BYTE*)0x011DEA2D, *(BYTE*)0x011DEA2B, MenuVisible()[1007]);
    fprintf(f, "menu stack:");
    for (int i = 0; im && i < 10; ++i) {
        DWORD id = *(DWORD*)(im + 0x114 + 4 * i);
        if (id) fprintf(f, " %lu", id);
    }
    fprintf(f, "\nvisible menus:");
    for (int id = 1000; id < 1100; ++id)
        if (MenuVisible()[id]) fprintf(f, " %d", id);
    fprintf(f, "\n");
    fprintf(f, "hooks: SetCameraFOV %ld calls, FovComp %ld calls, fovtrace %s\n", s_setFovCalls, s_compCalls,
            s_fovTracing ? "recording" : "idle");

    fprintf(f, "stamps per frame (last frame / total):");
    for (int i = 0; i < kSiteCount; ++i) fprintf(f, "\n  %-42s %ld / %ld (thread %lu)", s_sites[i].name, s_sites[i].lastFrame, s_sites[i].total, s_sites[i].thread);
    fprintf(f, "\n");

    BYTE* pl = Player();
    fprintf(f, "player %p: fov world %.3f 1st %.3f shoulder %.3f; byte 64A %u, byte 64C %u\n", pl, F(pl, 0x670), F(pl, 0x674),
            F(pl, 0x678), B(pl, 0x64A), B(pl, 0x64C));
    // TESObjectREFR: pos +0x30, parentCell +0x40; TESForm refID +0x0C; cell full name at +0x1C.
    BYTE* cell = (BYTE*)P(pl, 0x40);
    const char* cellName = (const char*)P(cell, 0x1C);
    fprintf(f, "player pos %.1f %.1f %.1f, cell %p id %08lx \"%s\"\n", F(pl, 0x30), F(pl, 0x34), F(pl, 0x38), cell,
            Readable(cell, 0x10) ? *(DWORD*)(cell + 0x0C) : 0, Readable(cellName, 1) ? cellName : "");
    BYTE* sg = WorldSceneGraph();
    BYTE* cam = WorldCamera();
    Frustum a = ReadFrustum(cam, 0xDC), b = ReadFrustum(cam, 0xEC);
    float aspect = s_bbHeight ? (float)s_bbWidth / s_bbHeight : NAN;
    fprintf(f, "world scene graph %p: camera %p, byte B8 %u, float BC %.3f\n", sg, cam, B(sg, 0xB8), F(sg, 0xBC));
    fprintf(f, "  frustum@DC l %.4f r %.4f t %.4f b %.4f n %.2f f %.1f -> h %.2f v %.2f, engine fov (4:3 ref) %.2f\n", a.l, a.r, a.t,
            a.b, a.n, a.f, HFovDeg(a), VFovDeg(a), 2.f * atanf(a.r / (0.75f * aspect)) * kDeg);
    fprintf(f, "  frustum@EC %.4f %.4f %.4f %.4f; LODAdjust@110 %.3f\n", b.l, b.r, b.t, b.b, F(cam, 0x110));
    for (int i = 0; i < 2; ++i)
        fprintf(f, "  at world draw %c: r %.4f t %.4f (h %.2f)\n", 'A' + i, s_worldDrawFrustum[i].r, s_worldDrawFrustum[i].t,
                HFovDeg(s_worldDrawFrustum[i]));
    fprintf(f, "  at pre-UI: r %.4f t %.4f (h %.2f)\n", s_preUiFrustum.r, s_preUiFrustum.t, HFovDeg(s_preUiFrustum));

    BYTE* rd = Renderer();
    fprintf(f, "renderer %p (TESMain renderer %p): device %p (hooked %p), size fields %lu x %lu, windows %p %p\n", rd,
            *(void**)0x011C73B4, P(rd, 0x288), D3DDevice(), rd ? *(DWORD*)(rd + 0xA98) : 0, rd ? *(DWORD*)(rd + 0xA9C) : 0,
            P(rd, 0x3BC), P(rd, 0x3C0));
    HWND wnd = OsGlobals() ? *(HWND*)(OsGlobals() + 8) : nullptr;
    RECT wr = {}, cr = {}, clip = {};
    POINT cur = {};
    GetWindowRect(wnd, &wr);
    GetClientRect(wnd, &cr);
    GetClipCursor(&clip);
    GetCursorPos(&cur);
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    fprintf(f, "window %p: rect %ld,%ld-%ld,%ld client %ldx%ld style %08lx foreground %d (process in front %d); cursor %ld,%ld clip %ld,%ld-%ld,%ld\n",
            wnd, wr.left, wr.top, wr.right, wr.bottom, cr.right, cr.bottom, (DWORD)GetWindowLongA(wnd, GWL_STYLE),
            GetForegroundWindow() == wnd, fgPid == GetCurrentProcessId(), cur.x, cur.y, clip.left, clip.top, clip.right, clip.bottom);
    GameCmdWriteState(f);
    ModWriteState(f);
    for (const Setting& s : s_settings) {
        char v[160];
        SettingValue(s, v, sizeof(v));
        fprintf(f, "setting %-30s %s\n", s.name, v);
    }
    fprintf(f, "FOV call sites (S = SetCameraFOV, C = FovComp 0xB54000):\n");
    for (int i = 0; i < s_callerCount; ++i)
        fprintf(f, "  %c %p: %ld calls, last fov %.3f, sg %p\n", s_callers[i].kind, s_callers[i].ret, s_callers[i].count, s_callers[i].last,
                s_callers[i].sg);
    PatchWriteStatus(f);
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

static void WriteState()
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe_state.txt", ModDir());
    WriteStateTo(path);
}

void ReconEngineMainLoop()
{
    if (!s_mainLoopThread) {
        s_mainLoopThread = GetCurrentThreadId();
        Log("first MainGameLoop message on thread %lu (main thread %lu)", s_mainLoopThread, MainThreadId());
    }
    ++s_mainLoops;
    GameCmdTick();
    McmTick();
    DWORD now = GetTickCount();
    if (now - s_lastStateTick >= 250) {
        s_lastStateTick = now;
        D3DBackBufferSize(&s_bbWidth, &s_bbHeight);
        WriteState();
    }
}

// ---- lifecycle -------------------------------------------------------------------------------

void ReconEngineAtLoad()
{
    BYTE* os = OsGlobals();
    Log("at Load: OSGlobals %p (window %p, main thread %lu), renderer %p, TESMain renderer %p, player %p, InterfaceManager %p, world scene graph %p",
        os, os ? *(HWND*)(os + 8) : nullptr, MainThreadId(), Renderer(), *(void**)0x011C73B4, Player(), InterfaceMgr(), WorldSceneGraph());
    // The engine's cached D3D9 entry points and object (filled by 0x00E69410 / 0x00E69460).
    Log("at Load: D3D9 cache: Direct3DCreate9 %p, Direct3DCreate9Ex %p, IDirect3D9 %p, isEx %u",
        *(void**)0x0126F0DC, *(void**)0x0126F0E0, *(void**)0x0126F0D8, *(BYTE*)0x0126F0D0);
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    char path[MAX_PATH] = "";
    if (d3d9) GetModuleFileNameA(d3d9, path, MAX_PATH);
    Log("at Load: d3d9.dll %p %s", d3d9, path);
    FindSettings();
    LogSettings("at Load");
}

void ReconEngineInstall()
{
    static const BYTE kSetCameraFov[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xCC, 0x00, 0x00, 0x00};
    s_setCameraFov = (SetCameraFovFn)InstallDetour(0x00C52020, kSetCameraFov, sizeof(kSetCameraFov), (void*)Hook_SetCameraFov,
                                                   "SetCameraFOV");
    static const BYTE kFovComp[] = {0x83, 0xEC, 0x08, 0xD9, 0x44, 0x24, 0x0C};
    s_fovComp = (FovCompFn)InstallDetour(0x00B54000, kFovComp, sizeof(kFovComp), (void*)Hook_FovComp, "FovComp 0xB54000");
    InstallStamps();
}

void ReconEngineDeferredInit()
{
    BYTE* rd = Renderer();
    void* dev = P(rd, 0x288);
    Log("DeferredInit: renderer %p, TESMain renderer %p (%s), device at +0x288 %p, hooked device %p", rd, *(void**)0x011C73B4,
        rd == *(BYTE**)0x011C73B4 ? "same" : "DIFFERENT", dev, D3DDevice());
    if (!D3DDevice()) Log("DeferredInit: device creation was not seen; hooking the renderer's device now");
    D3DAttach((IDirect3DDevice9*)dev, "renderer +0x288");
    BYTE* os = OsGlobals();
    HWND wnd = os ? *(HWND*)(os + 8) : nullptr;
    char cls[64] = "";
    GetClassNameA(wnd, cls, sizeof(cls));
    RECT wr = {}, cr = {};
    GetWindowRect(wnd, &wr);
    GetClientRect(wnd, &cr);
    Log("DeferredInit: window %p class \"%s\" rect %ld,%ld-%ld,%ld client %ldx%ld style %08lx exstyle %08lx; renderer windows %p %p, size fields %lu x %lu",
        wnd, cls, wr.left, wr.top, wr.right, wr.bottom, cr.right, cr.bottom, (DWORD)GetWindowLongA(wnd, GWL_STYLE),
        (DWORD)GetWindowLongA(wnd, GWL_EXSTYLE), P(rd, 0x3BC), P(rd, 0x3C0), rd ? *(DWORD*)(rd + 0xA98) : 0,
        rd ? *(DWORD*)(rd + 0xA9C) : 0);
    LogSettings("DeferredInit");
    // Key bindings: FalloutPrefs.ini [Controls] (written on exit) is the reliable source.
}
