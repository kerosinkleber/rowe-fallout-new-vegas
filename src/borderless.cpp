// Fullscreen at a size the monitor has no display mode for (1400x1400 on a 2560x1440 screen, say):
// the game would stop with "failed to initialize renderer", because a fullscreen Direct3D 9
// device needs one of the monitor's modes. Instead the game runs windowed in a borderless window
// of its size, centred on the monitor over a black backdrop window that covers the rest.
//
// The decision is made when the game creates its main window (after the INI reads and after any
// FalloutCustom.ini a plugin applied to the settings): bFull Screen:Display set, and the native
// size (iSize, without supersampling's factor) not among the monitor's display modes. Then the
// setting is cleared (the game takes its windowed path), the window frame the game adds is left
// out (AdjustWindowRect), and when the game places its window it is made a borderless popup,
// centred, and owned by the backdrop (so it stays in front of it; it keeps a taskbar button).
#include <windows.h>
#include "fnvfov.h"

static bool s_on, s_decided;
static int s_w, s_h;
static HWND s_main, s_backdrop;
static RECT s_monitor;

static bool IsDisplayMode(const char* device, int w, int h)
{
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(device, i, &dm); ++i)
        if ((int)dm.dmPelsWidth == w && (int)dm.dmPelsHeight == h) return true;
    return false;
}

void BorderlessDecide()
{
    if (s_decided) return;
    s_decided = true;
    BYTE* full = FindSettingObject("bFull Screen:Display");
    if (!full || !full[4]) return;
    int vw, vh;
    if (!SupersampleVirtualSize(&s_w, &s_h, &vw, &vh)) {
        BYTE* ow = FindSettingObject("iSize W:Display");
        BYTE* oh = FindSettingObject("iSize H:Display");
        if (!ow || !oh) return;
        s_w = *(int*)(ow + 4);
        s_h = *(int*)(oh + 4);
    }
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    s_monitor = mi.rcMonitor;
    if (s_w <= 0 || s_h <= 0 || IsDisplayMode(mi.szDevice, s_w, s_h)) return;
    full[4] = 0;
    s_on = true;
    Log("borderless: fullscreen at %dx%d is not a display mode of %s (%ldx%ld): borderless window, centred, on a black backdrop", s_w,
        s_h, mi.szDevice, s_monitor.right - s_monitor.left, s_monitor.bottom - s_monitor.top);
}

bool BorderlessActive() { return s_on; }

void BorderlessWindowCreated(HWND wnd, HWND parent, DWORD style)
{
    if (s_on && !s_main && wnd && !parent && !(style & WS_CHILD)) s_main = wnd;
}

static LRESULT CALLBACK BackdropProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // a click on the black area leaves the game active
    case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
    }
    return DefWindowProcA(h, msg, w, l);
}

static void MakeBackdrop()
{
    if (s_backdrop) return;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = BackdropProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "ROWE backdrop";
    RegisterClassA(&wc);
    s_backdrop = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "", WS_POPUP | WS_VISIBLE, s_monitor.left, s_monitor.top,
                                 s_monitor.right - s_monitor.left, s_monitor.bottom - s_monitor.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (s_backdrop) SetWindowLongPtrA(s_main, GWLP_HWNDPARENT, (LONG_PTR)s_backdrop);  // owned: always in front of it
    Log("borderless: backdrop %p", s_backdrop);
}

// The frame the game adds for its windowed mode: none.
bool BorderlessFixAdjust(LPRECT)
{
    return s_on;
}

// The game placing its main window: borderless popup, centred on the monitor.
bool BorderlessFixPos(HWND wnd, int* x, int* y, int* cx, int* cy, UINT* flags)
{
    if (!s_on || wnd != s_main) return false;
    MakeBackdrop();
    LONG style = GetWindowLongA(wnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME);
    SetWindowLongA(wnd, GWL_STYLE, style | WS_POPUP);
    SetWindowLongA(wnd, GWL_EXSTYLE, (GetWindowLongA(wnd, GWL_EXSTYLE) & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME)) | WS_EX_APPWINDOW);
    *cx = s_w;
    *cy = s_h;
    *x = s_monitor.left + (s_monitor.right - s_monitor.left - s_w) / 2;
    *y = s_monitor.top + (s_monitor.bottom - s_monitor.top - s_h) / 2;
    *flags = (*flags & ~(SWP_NOMOVE | SWP_NOSIZE)) | SWP_FRAMECHANGED;
    return true;
}
