// Reconnaissance: hooks the exe's imports for window creation and sizing, INI access and the
// D3D9 lookup, and logs them with timestamps. Answers when the plugin loads relative to INI
// parsing, window creation and Direct3DCreate9, and which APIs size the window.
#include <windows.h>
#include <intrin.h>
#include <string.h>
#include "fnvfov.h"

typedef HWND(WINAPI* CreateWindowExAFn)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL(WINAPI* SetWindowPosFn)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI* AdjustWindowRectFn)(LPRECT, DWORD, BOOL);
typedef BOOL(WINAPI* AdjustWindowRectExFn)(LPRECT, DWORD, BOOL, DWORD);
typedef BOOL(WINAPI* GetClientRectFn)(HWND, LPRECT);
typedef int(WINAPI* GetSystemMetricsFn)(int);
typedef int(WINAPI* ShowCursorFn)(BOOL);
typedef DWORD(WINAPI* GetPrivateProfileStringAFn)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
typedef UINT(WINAPI* GetPrivateProfileIntAFn)(LPCSTR, LPCSTR, INT, LPCSTR);
typedef BOOL(WINAPI* WritePrivateProfileStringAFn)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
typedef HMODULE(WINAPI* LoadLibraryAFn)(LPCSTR);
typedef void*(WINAPI* GetProcAddressFn)(HMODULE, LPCSTR);

static CreateWindowExAFn s_CreateWindowExA;
static SetWindowPosFn s_SetWindowPos;
static AdjustWindowRectFn s_AdjustWindowRect;
static AdjustWindowRectExFn s_AdjustWindowRectEx;
static GetClientRectFn s_GetClientRect;
static GetSystemMetricsFn s_GetSystemMetrics;
static ShowCursorFn s_ShowCursor;
static GetPrivateProfileStringAFn s_GetPrivateProfileStringA;
static GetPrivateProfileIntAFn s_GetPrivateProfileIntA;
static WritePrivateProfileStringAFn s_WritePrivateProfileStringA;
static LoadLibraryAFn s_LoadLibraryA;
static GetProcAddressFn s_GetProcAddress;

static const char* Str(LPCSTR s) { return !s ? "(null)" : IS_INTRESOURCE(s) ? "(atom)" : s; }

// INI keys worth logging: display size and mode, intros, screenshots, FOV, input.
static bool KeyOfInterest(LPCSTR key)
{
    static const char* const kWords[] = {"isize", "full screen", "bborder", "presentinterval", "introsequence",
        "mainmenumovie", "multisample", "screenshot", "fov", "ilocation", "alwaysactive", "iadapter",
        "background mouse", "bwindowed", "bdo intro"};
    if (!key) return false;
    char low[128];
    lstrcpynA(low, key, sizeof(low));
    CharLowerA(low);
    for (const char* w : kWords)
        if (strstr(low, w)) return true;
    return false;
}

// Logs each distinct INI file the first time it is read.
static void NoteIniFile(LPCSTR file)
{
    static char seen[8][MAX_PATH];
    static int count;
    if (!file) return;
    for (int i = 0; i < count; ++i)
        if (lstrcmpiA(seen[i], file) == 0) return;
    if (count < 8) lstrcpynA(seen[count++], file, MAX_PATH);
    Log("ini: first read of %s", file);
}

static LONG s_iniReads;

static HWND WINAPI Hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    IntrosApply();  // the INI files have been read by now
    SupersampleSyncSettings();  // and any FalloutCustom.ini applied
    BorderlessDecide();         // fullscreen at a size that is no display mode: borderless instead
    int reqW = w, reqH = h;
    if (SupersampleFixSize(&w, &h)) Log("supersample: window %dx%d created at native %dx%d", reqW, reqH, w, h);
    HWND wnd = s_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    BorderlessWindowCreated(wnd, parent, style);
    RECT cr = {};
    if (wnd) GetClientRect(wnd, &cr);
    Log("CreateWindowExA(class \"%s\", title \"%s\", style %08lx, exstyle %08lx, %d,%d %dx%d) -> %p, client %ldx%ld (ini reads so far %ld)",
        Str(cls), Str(title), style, ex, x, y, w, h, wnd, cr.right, cr.bottom, s_iniReads);
    return wnd;
}

static BOOL WINAPI Hook_SetWindowPos(HWND wnd, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if (BorderlessFixPos(wnd, &x, &y, &cx, &cy, &flags)) Log("borderless: main window placed at %d,%d %dx%d", x, y, cx, cy);
    BOOL r = s_SetWindowPos(wnd, after, x, y, cx, cy, flags);
    static LONG n;
    if (InterlockedIncrement(&n) <= 100) {
        RECT cr = {};
        GetClientRect(wnd, &cr);
        Log("SetWindowPos(%p, after %p, %d,%d %dx%d, flags %04x) -> %d, client now %ldx%ld", wnd, after, x, y, cx, cy, flags, r,
            cr.right, cr.bottom);
    }
    return r;
}

// The window's client size: the virtual size (from iSize) becomes the native one.
static void FixRect(LPRECT rc)
{
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    if (SupersampleFixSize(&w, &h)) {
        rc->right = rc->left + w;
        rc->bottom = rc->top + h;
    }
}

static BOOL WINAPI Hook_AdjustWindowRect(LPRECT rc, DWORD style, BOOL menu)
{
    FixRect(rc);
    RECT in = *rc;
    BOOL r = BorderlessFixAdjust(rc) ? TRUE : s_AdjustWindowRect(rc, style, menu);
    Log("AdjustWindowRect(%ld,%ld-%ld,%ld, style %08lx, menu %d) -> %ld,%ld-%ld,%ld", in.left, in.top, in.right, in.bottom,
        style, menu, rc->left, rc->top, rc->right, rc->bottom);
    return r;
}

static BOOL WINAPI Hook_AdjustWindowRectEx(LPRECT rc, DWORD style, BOOL menu, DWORD ex)
{
    FixRect(rc);
    RECT in = *rc;
    BOOL r = BorderlessFixAdjust(rc) ? TRUE : s_AdjustWindowRectEx(rc, style, menu, ex);
    Log("AdjustWindowRectEx(%ld,%ld-%ld,%ld, style %08lx, menu %d, ex %08lx) -> %ld,%ld-%ld,%ld", in.left, in.top, in.right,
        in.bottom, style, menu, ex, rc->left, rc->top, rc->right, rc->bottom);
    return r;
}

// Logged when a window's client size changes, plus the first few calls (with the caller).
static BOOL WINAPI Hook_GetClientRect(HWND wnd, LPRECT rc)
{
    BOOL r = s_GetClientRect(wnd, rc);
    static HWND lastWnd;
    static LONG lastW = -1, lastH = -1, n;
    LONG count = InterlockedIncrement(&n);
    if (r && (count <= 10 || wnd != lastWnd || rc->right != lastW || rc->bottom != lastH)) {
        Log("GetClientRect(%p) -> %ldx%ld (call %ld, from %p)", wnd, rc->right, rc->bottom, count, _ReturnAddress());
        lastWnd = wnd;
        lastW = rc->right;
        lastH = rc->bottom;
    }
    return r;
}

static int WINAPI Hook_GetSystemMetrics(int index)
{
    int r = s_GetSystemMetrics(index);
    static BYTE seen[256];
    if (index >= 0 && index < 256 && !seen[index]) {
        seen[index] = 1;
        Log("GetSystemMetrics(%d) -> %d (first call, from %p)", index, r, _ReturnAddress());
    }
    return r;
}

static int WINAPI Hook_ShowCursor(BOOL show)
{
    int r = s_ShowCursor(show);
    static LONG n;
    if (InterlockedIncrement(&n) <= 20) Log("ShowCursor(%d) -> %d", show, r);
    return r;
}

static DWORD WINAPI Hook_GetPrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file)
{
    DWORD r = s_GetPrivateProfileStringA(app, key, def, out, size, file);
    InterlockedIncrement(&s_iniReads);
    NoteIniFile(file);
    if (KeyOfInterest(key))
        Log("ini read [%s] %s = \"%s\" (default \"%s\", %s)", Str(app), Str(key), out && size ? out : "", Str(def), Str(file));
    return r;
}

// The window size the game asks for, with [dev] size=WxH applied (before supersampling scales it).
static LPCSTR s_sizeFile;

static UINT WindowSize(bool width, UINT value)
{
    if (g_settings.devWidth > 0) return width ? g_settings.devWidth : g_settings.devHeight;
    return value;
}

// FalloutCustom.ini (next to FalloutPrefs.ini) is applied to the game's settings by plugins only
// after the game has read its INI files and listed the display modes; supersampling's extra mode
// and virtual size must already follow it, so its iSize is taken where the game reads the prefs'.
static UINT CustomSize(bool width, UINT value, LPCSTR file)
{
    if (!file || IS_INTRESOURCE(file)) return value;
    const char* name = strrchr(file, '\\');
    if (!name || lstrcmpiA(name + 1, "FalloutPrefs.ini")) return value;
    char custom[MAX_PATH];
    lstrcpynA(custom, file, (int)(name - file) + 2);
    lstrcatA(custom, "FalloutCustom.ini");
    INT v = (INT)s_GetPrivateProfileIntA("Display", width ? "iSize W" : "iSize H", -1, custom);
    if (v <= 0) return value;
    static bool logged[2];
    if (!logged[width]) {
        logged[width] = true;
        Log("ini: iSize %s %u from FalloutCustom.ini instead of %u", width ? "W" : "H", (UINT)v, value);
    }
    return (UINT)v;
}

static UINT ReadOtherSide(bool width)
{
    UINT v = s_GetPrivateProfileIntA("Display", width ? "iSize W" : "iSize H", width ? 640 : 480, s_sizeFile);
    return WindowSize(width, CustomSize(width, v, s_sizeFile));
}

static UINT WINAPI Hook_GetPrivateProfileIntA(LPCSTR app, LPCSTR key, INT def, LPCSTR file)
{
    UINT r = s_GetPrivateProfileIntA(app, key, def, file);
    if (key && !IS_INTRESOURCE(key) && (!lstrcmpiA(key, "iSize W") || !lstrcmpiA(key, "iSize H"))) {
        bool width = !lstrcmpiA(key, "iSize W");
        r = CustomSize(width, r, file);
        UINT game = r;
        r = WindowSize(width, r);
        if (r != game) Log("dev: iSize %s %u overridden to %u", width ? "W" : "H", game, r);
        s_sizeFile = file;
    }
    r = SupersampleIniInt(app, key, r, ReadOtherSide);
    if (g_settings.devMsaa >= 0 && key && !IS_INTRESOURCE(key) && !lstrcmpiA(key, "iMultiSample")) {
        Log("dev: iMultiSample %u overridden to %d", r, g_settings.devMsaa);
        r = g_settings.devMsaa;
    }
    r = SupersampleIniMsaa(app, key, r);
    if (g_settings.devFullscreen >= 0 && key && !IS_INTRESOURCE(key) && !lstrcmpiA(key, "bFull Screen")) {
        Log("dev: bFull Screen %u overridden to %d", r, g_settings.devFullscreen);
        r = g_settings.devFullscreen;
    }
    InterlockedIncrement(&s_iniReads);
    NoteIniFile(file);
    if (KeyOfInterest(key)) Log("ini read int [%s] %s = %u (default %d, %s)", Str(app), Str(key), r, def, Str(file));
    return r;
}

static BOOL WINAPI Hook_WritePrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR value, LPCSTR file)
{
    char buf[16];
    value = SupersampleIniWrite(app, key, value, buf);
    if (g_settings.devMsaa >= 0 && file && key && !IS_INTRESOURCE(key) && !lstrcmpiA(key, "iMultiSample")) {
        UINT prefs = s_GetPrivateProfileIntA("Display", "iMultiSample", 0, file);  // the file keeps its own value
        wsprintfA(buf, "%u", prefs);
        value = buf;
    }
    static LONG n;
    if (KeyOfInterest(key) || InterlockedIncrement(&n) <= 5)
        Log("ini WRITE [%s] %s = \"%s\" (%s)", Str(app), Str(key), Str(value), Str(file));
    return s_WritePrivateProfileStringA(app, key, value, file);
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name)
{
    HMODULE m = s_LoadLibraryA(name);
    static LONG n;
    if (InterlockedIncrement(&n) <= 100) Log("LoadLibraryA(\"%s\") -> %p (from %p)", Str(name), m, _ReturnAddress());
    return m;
}

static FARPROC WINAPI Hook_GetProcAddress(HMODULE mod, LPCSTR name)
{
    FARPROC p = (FARPROC)s_GetProcAddress(mod, name);
    if (!IS_INTRESOURCE(name) && (strstr(name, "Direct3D") || strstr(name, "DirectInput") || strstr(name, "D3DPERF"))) {
        Log("GetProcAddress(%p, \"%s\") -> %p (from %p)", mod, name, p, _ReturnAddress());
        if (p && strncmp(name, "Direct3DCreate9", 15) == 0) p = (FARPROC)D3DWrapProc(name, (void*)p);
    }
    return p;
}

#define HOOK(dll, fn) s_##fn = (fn##Fn)PatchExeImport(dll, #fn, (void*)Hook_##fn)

void ReconWin32Install()
{
    HOOK("USER32.dll", CreateWindowExA);
    HOOK("USER32.dll", SetWindowPos);
    HOOK("USER32.dll", AdjustWindowRect);
    HOOK("USER32.dll", AdjustWindowRectEx);
    HOOK("USER32.dll", GetClientRect);
    HOOK("USER32.dll", GetSystemMetrics);
    HOOK("USER32.dll", ShowCursor);
    HOOK("KERNEL32.dll", GetPrivateProfileStringA);
    HOOK("KERNEL32.dll", GetPrivateProfileIntA);
    HOOK("KERNEL32.dll", WritePrivateProfileStringA);
    HOOK("KERNEL32.dll", LoadLibraryA);
    s_GetProcAddress = (GetProcAddressFn)PatchExeImport("KERNEL32.dll", "GetProcAddress", (void*)Hook_GetProcAddress);
    // A hook whose slot was not found must still have a function to call.
    if (!s_CreateWindowExA) s_CreateWindowExA = CreateWindowExA;
    if (!s_SetWindowPos) s_SetWindowPos = SetWindowPos;
    if (!s_AdjustWindowRect) s_AdjustWindowRect = AdjustWindowRect;
    if (!s_AdjustWindowRectEx) s_AdjustWindowRectEx = AdjustWindowRectEx;
    if (!s_GetClientRect) s_GetClientRect = GetClientRect;
    if (!s_GetSystemMetrics) s_GetSystemMetrics = GetSystemMetrics;
    if (!s_ShowCursor) s_ShowCursor = ShowCursor;
    if (!s_GetPrivateProfileStringA) s_GetPrivateProfileStringA = GetPrivateProfileStringA;
    if (!s_GetPrivateProfileIntA) s_GetPrivateProfileIntA = GetPrivateProfileIntA;
    if (!s_WritePrivateProfileStringA) s_WritePrivateProfileStringA = WritePrivateProfileStringA;
    if (!s_LoadLibraryA) s_LoadLibraryA = LoadLibraryA;
    if (!s_GetProcAddress) s_GetProcAddress = (GetProcAddressFn)GetProcAddress;
}
