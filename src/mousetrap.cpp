// Mouse trap: rowe.ini mousetrap=0 off (the game's own behaviour: it captures the mouse through
// DirectInput in gameplay and menus alike), 1 captured in gameplay and free in menus, 2 captured
// everywhere.
//
// Captured: the Windows cursor is also confined to the window's client area (ClipCursor), only
// while the game is in front, re-applied every frame (Windows drops the clip on focus changes)
// and released as soon as the game is switched away from, and when the game crashes.
// Free (mode 1 in menus): the game's DirectInput mouse is switched to non-exclusive and the
// game's menu cursor follows the Windows cursor (dinput.cpp), so the mouse can leave the window
// like in any other program.
#include <windows.h>
#include "fnvfov.h"

static bool s_managing;   // the clip is ours
static HWND s_window;
static WNDPROC s_origWndProc;
static RECT s_clip;

static void Release()
{
    if (!s_managing) return;
    s_managing = false;
    ClipCursor(nullptr);
}

// Lets go as soon as the game is switched away from (subclass of the game window).
static LRESULT CALLBACK MouseTrapWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if ((msg == WM_ACTIVATEAPP && !w) || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE)) Release();
    OverlayWndProc(h, msg, w, l);  // the settings menu's mouse and keyboard
    return CallWindowProcA(s_origWndProc, h, msg, w, l);
}

// The client area on screen, kept on its monitor.
static bool ClientOnScreen(RECT* out)
{
    RECT rc;
    if (!GetClientRect(s_window, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
    POINT tl = {0, 0}, br = {rc.right, rc.bottom};
    ClientToScreen(s_window, &tl);
    ClientToScreen(s_window, &br);
    SetRect(out, tl.x, tl.y, br.x, br.y);
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoA(MonitorFromWindow(s_window, MONITOR_DEFAULTTONEAREST), &mi)) IntersectRect(out, out, &mi.rcMonitor);
    return !IsRectEmpty(out);
}

// The render size per client pixel (the virtual size with supersampling).
static bool RenderScale(float* sx, float* sy)
{
    RECT rc;
    int w = 0, h = 0;
    D3DBackBufferSize(&w, &h);
    if (!GetClientRect(s_window, &rc) || rc.right <= 0 || rc.bottom <= 0 || w <= 0 || h <= 0) return false;
    *sx = (float)w / rc.right;
    *sy = (float)h / rc.bottom;
    return true;
}

// Mode 1 in menus: the mouse is free (non-exclusive) and the game's cursor follows the Windows
// cursor; the Windows cursor starts where the game's cursor is.
static void FreeMouse(bool free)
{
    if (free == DinputMouseFree()) return;
    DinputSetMouseFree(free);
    float x, y, sx, sy;
    if (free && DinputGameCursor(&x, &y) && RenderScale(&sx, &sy)) {
        POINT p = {(LONG)(x / sx), (LONG)(y / sy)};
        ClientToScreen(s_window, &p);
        SetCursorPos(p.x, p.y);
    }
}

static void SteerGameCursor()
{
    float sx, sy;
    POINT p;
    RECT rc;
    if (!RenderScale(&sx, &sy) || !GetCursorPos(&p) || !GetClientRect(s_window, &rc)) return;
    ScreenToClient(s_window, &p);
    if (p.x < 0) p.x = 0;
    if (p.y < 0) p.y = 0;
    if (p.x >= rc.right) p.x = rc.right - 1;
    if (p.y >= rc.bottom) p.y = rc.bottom - 1;
    // With the Pip-Boy warped after the UI the game's cursor is warped too: aim it at the spot
    // whose warped image is under the Windows cursor.
    float x = p.x * sx, y = p.y * sy;
    LensMapToRender(&x, &y);
    DinputSetCursorTarget(x, y, rc.right * sx, rc.bottom * sy);
}

void MouseTrapFrame()
{
    HWND window = GameWindow();
    if (!window) return;
    if (s_window != window) {
        s_window = window;
        s_origWndProc = (WNDPROC)SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)MouseTrapWndProc);
    }
    int mode = g_settings.mousetrap;
    bool front = GameInFront() && !IsIconic(window);
    bool gameplay = InGameplay();

    // The game itself captures the mouse (DirectInput, exclusive) in gameplay and menus alike.
    // The settings menu frees the mouse too (Windows' mouse messages reach it then).
    if (front) FreeMouse(OverlayOpen() || (mode == kMouseTrapGameplay && !gameplay));
    // After a drag on the Pip-Boy (the game got the mouse's own motion) the Windows cursor goes to
    // where the game's cursor is shown, so steering does not pull it back.
    float x, y, sx, sy;
    if (DinputTakeDragEnded() && DinputMouseFree() && RenderScale(&sx, &sy) && (LensCursorShown(&x, &y) || DinputGameCursor(&x, &y))) {
        POINT p = {(LONG)(x / sx), (LONG)(y / sy)};
        ClientToScreen(s_window, &p);
        SetCursorPos(p.x, p.y);
    }
    if (DinputMouseFree()) SteerGameCursor();

    bool want = mode != kMouseTrapOff && front && (mode == kMouseTrapAlways || gameplay) && !OverlayOpen();
    RECT r;
    if (!want || !ClientOnScreen(&r)) {
        Release();
        return;
    }
    RECT cur;
    s_managing = true;
    s_clip = r;
    if (GetClipCursor(&cur) && EqualRect(&cur, &r)) return;
    ClipCursor(&r);
}

void MouseTrapWriteState(FILE* f)
{
    fprintf(f, "mousetrap: mode %d, clipping %d (%ld,%ld-%ld,%ld), in gameplay %d\n", g_settings.mousetrap, s_managing, s_clip.left,
            s_clip.top, s_clip.right, s_clip.bottom, InGameplay());
}
