// The game's DirectInput mouse. FNV reads the mouse through DirectInput in exclusive mode, which
// captures it in gameplay and in menus alike, and moves its own menu cursor from the mouse
// deltas. For mouse trap mode 1 ("gameplay only") the mouse is set free in menus:
// - the mouse device is switched to non-exclusive, so Windows owns the cursor again (it can leave
//   the window; over the window the game keeps it hidden);
// - the game's menu cursor follows the Windows cursor: the X/Y movement the game reads is
//   replaced by the movement that takes its cursor (InterfaceManager +0x38 / +0x40, JIP LN's
//   layout, in render pixels; writing there does not move it) to the Windows cursor's position,
//   through the inverse of the game's cursor acceleration (see Steer).
// Back in gameplay the game's own cooperative level is restored.
//
// Hooks: the exe's DirectInput8Create import (xNVSE replaces it first; this chains),
// IDirectInput8::CreateDevice, and the mouse device's SetCooperativeLevel / GetDeviceState /
// GetDeviceData (by vtable index; xNVSE's wrapper devices share one vtable, so calls are told
// apart by device pointer).
#define CINTERFACE
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <math.h>
#include <stdlib.h>
#include "fnvfov.h"

typedef HRESULT(WINAPI* DI8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceFn)(void* di, REFGUID guid, void** out, void* outer);
typedef HRESULT(STDMETHODCALLTYPE* AcquireFn)(void* dev);
typedef HRESULT(STDMETHODCALLTYPE* GetDeviceStateFn)(void* dev, DWORD size, void* data);
typedef HRESULT(STDMETHODCALLTYPE* GetDeviceDataFn)(void* dev, DWORD cb, DIDEVICEOBJECTDATA* data, DWORD* inout, DWORD flags);
typedef HRESULT(STDMETHODCALLTYPE* SetCooperativeLevelFn)(void* dev, HWND hwnd, DWORD flags);

// IDirectInput8 / IDirectInputDevice8 vtable slots.
enum { kSlotCreateDevice = 3, kSlotAcquire = 7, kSlotUnacquire = 8, kSlotGetDeviceState = 9, kSlotGetDeviceData = 10,
       kSlotSetCooperativeLevel = 13 };

static DI8CreateFn s_create;
static CreateDeviceFn s_createDevice;
static AcquireFn s_acquire, s_unacquire;
static GetDeviceStateFn s_getState;
static GetDeviceDataFn s_getData;
static SetCooperativeLevelFn s_setCoop;

static void* s_mouse;
static void* s_keyboard;  // only told apart: while the settings menu is open the game reads nothing
static HWND s_mouseWindow;
static DWORD s_gameFlags;   // the cooperative level the game asked for
static bool s_free;         // non-exclusive, movement hidden from the game
static LONG s_stateCalls, s_dataCalls;
static LONG s_keyStateCalls, s_keyDataCalls, s_escKept;  // keyboard reads; Esc kept from the game in photo mode
// The game's menu cursor moves g(m) render pixels per mouse count for a move of m counts: it
// accelerates (measured ~1 px/count for small moves and ~0.05 * m for large ones at 3840x2160),
// and the pixels scale with the render size (~9 px/count at 11520 across). g is learned per
// size class of the move (up to 15 counts, up to 60, more) from the game's response, so any render
// size, supersampling and mouse setting fits.
static const float kSteerBins[2] = {15.f, 60.f};
static float s_gain[3] = {1.f, 1.5f, 5.f};
static float s_damp = 1.f;               // < 1 after the cursor crossed the target (overshoot)
static float s_lastErrX, s_lastErrY;     // the distance left at the previous correction
static float s_targetX, s_targetY;      // where the game cursor should go (render pixels)
static float s_renderW, s_renderH;
static bool s_haveTarget;
static DWORD s_steerFrame = (DWORD)-1;  // one correction per frame
static LONG s_sentX, s_sentY;           // counts sent with the last correction
static float s_lastX, s_lastY;          // game cursor when they were sent

bool DinputGameCursor(float* x, float* y);

static bool NearEdge(float x, float y)
{
    return x < 2 || y < 2 || x > s_renderW - 3 || y > s_renderH - 3;
}

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int SteerBin(float counts) { return counts <= kSteerBins[0] ? 0 : counts <= kSteerBins[1] ? 1 : 2; }

// Learns the pixels per count of the last move's size class (skipping moves that hit a screen edge).
static void Learn(float x, float y)
{
    float sent = sqrtf((float)s_sentX * s_sentX + (float)s_sentY * s_sentY);
    float dx = x - s_lastX, dy = y - s_lastY;
    float moved = sqrtf(dx * dx + dy * dy);
    if (sent < 1 || moved < 0.5f || NearEdge(x, y) || NearEdge(s_lastX, s_lastY)) return;
    float& g = s_gain[SteerBin(sent)];
    g = Clamp(g * 0.5f + moved / sent * 0.5f, 0.05f, 200.f);
}

// The mouse movement the game reads in free mode: towards the target, once per frame. 80% of
// the distance is closed per frame through the inverse of the acceleration curve.
// Steering only while the game uses mouse movement for its cursor. While the Pip-Boy is being
// raised or lowered (pipBoyMode 2 / 5; 3 = open) the game turns the camera with it instead, so
// nothing is sent then. As a safety net, if movement that was sent did not move the cursor, the
// game is using it for something else: steering pauses for a while instead of turning the view.
static DWORD s_pauseUntil;

static bool CursorLive()
{
    BYTE* im = *(BYTE**)0x011D8A80;
    DWORD pipboy = im ? *(DWORD*)(im + 0x4BC) : 0;
    return pipboy == 0 || pipboy == 3;
}

static void Steer(LONG* lx, LONG* ly)
{
    *lx = *ly = 0;
    float x, y;
    if (!s_haveTarget || !DinputGameCursor(&x, &y)) return;
    DWORD frame = FrameCount();
    if (frame == s_steerFrame) return;
    if (!CursorLive() || frame < s_pauseUntil) {
        s_steerFrame = (DWORD)-1;  // nothing in flight to learn from
        return;
    }
    if (s_steerFrame != (DWORD)-1) {
        if (labs(s_sentX) + labs(s_sentY) >= 8 && fabsf(x - s_lastX) + fabsf(y - s_lastY) < 0.5f && !NearEdge(x, y)) {
            Log("dinput: sent movement did not move the cursor (%ld %ld); steering paused", s_sentX, s_sentY);
            s_pauseUntil = frame + 30;
            s_steerFrame = (DWORD)-1;
            return;
        }
        Learn(x, y);
    }
    s_steerFrame = frame;
    float dx = s_targetX - x, dy = s_targetY - y;
    float dist = sqrtf(dx * dx + dy * dy);
    // Crossing the target means the last move was too large: damp the next ones until it settles.
    if (dx * s_lastErrX + dy * s_lastErrY < 0) s_damp = s_damp * 0.5f < 0.25f ? 0.25f : s_damp * 0.5f;
    else s_damp = s_damp * 1.25f > 1.f ? 1.f : s_damp * 1.25f;
    s_lastErrX = dx;
    s_lastErrY = dy;
    if (dist >= 0.75f) {
        // 70% of the distance per frame, in counts of the size class that move falls in.
        float d = dist * 0.7f * s_damp;
        float m = d / s_gain[0];
        if (m > kSteerBins[0]) m = d / s_gain[1];
        if (m > kSteerBins[1]) m = d / s_gain[2];
        if (m > 3000) m = 3000;
        // Less than half a count would overshoot by more than it closes (a count can move the
        // cursor several pixels at high render sizes): stay.
        if (m >= 0.5f) {
            if (m < 1) m = 1;
            float cx = dx / dist * m, cy = dy / dist * m;
            *lx = (LONG)(cx >= 0 ? cx + 0.5f : cx - 0.5f);
            *ly = (LONG)(cy >= 0 ? cy + 0.5f : cy - 0.5f);
        }
    }
    s_sentX = *lx;
    s_sentY = *ly;
    s_lastX = x;
    s_lastY = y;
    // Steering trace: the first frames after each target change.
    // (Only the first few target changes per session, so menu use does not fill the log.)
    static float traceX = -1, traceY = -1;
    static int traceLeft, traces;
    if ((fabsf(s_targetX - traceX) > 10 || fabsf(s_targetY - traceY) > 10) && traces < 10) {
        ++traces;
        traceX = s_targetX;
        traceY = s_targetY;
        traceLeft = 8;
    }
    if (traceLeft > 0) {
        --traceLeft;
        Log("dinput: steer frame %lu cursor %.1f %.1f target %.1f %.1f sent %ld %ld px/count %.2f %.2f %.2f damp %.2f", frame, x, y,
            s_targetX, s_targetY, *lx, *ly, s_gain[0], s_gain[1], s_gain[2], s_damp);
    }
}

void DinputSetCursorTarget(float x, float y, float renderW, float renderH)
{
    s_targetX = x;
    s_targetY = y;
    s_renderW = renderW;
    s_renderH = renderH;
    s_haveTarget = true;
}

static void SwapSlot(void** vtable, int slot, void* hook, void** orig)
{
    if (vtable[slot] == hook) return;
    if (!*orig) *orig = vtable[slot];
    DWORD old;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old);
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
}

static HRESULT STDMETHODCALLTYPE H_SetCooperativeLevel(void* dev, HWND hwnd, DWORD flags)
{
    if (dev == s_mouse) {
        Log("dinput: mouse SetCooperativeLevel(%p, %08lx)%s", hwnd, flags, s_free ? " (kept non-exclusive)" : "");
        s_mouseWindow = hwnd;
        s_gameFlags = flags;
        if (s_free) flags = DISCL_NONEXCLUSIVE | DISCL_FOREGROUND;
    }
    return s_setCoop(dev, hwnd, flags);
}

// Transition trace: every mouse read for a while after the mouse is freed or captured again.
static int s_switchTrace;
static bool s_dragging, s_dragEnded;

// A drag on the Pip-Boy with the free mouse just ended (once).
bool DinputTakeDragEnded()
{
    bool ended = s_dragEnded;
    s_dragEnded = false;
    return ended;
}

static HRESULT STDMETHODCALLTYPE H_GetDeviceState(void* dev, DWORD size, void* data)
{
    HRESULT hr = s_getState(dev, size, data);
    // The settings menu has the mouse and keyboard: the game reads no movement, buttons or keys.
    if ((dev == s_mouse || dev == s_keyboard) && OverlayOpen()) {
        if (SUCCEEDED(hr) && data) memset(data, 0, size);
        return hr;
    }
    // Photo mode keeps Esc from the game: the pause menu would end it (the shortcut ends it).
    if (dev == s_keyboard) InterlockedIncrement(&s_keyStateCalls);
    if (dev == s_keyboard && PhotoActive() && SUCCEEDED(hr) && data && size > DIK_ESCAPE && (((BYTE*)data)[DIK_ESCAPE] & 0x80)) {
        ((BYTE*)data)[DIK_ESCAPE] = 0;
        InterlockedIncrement(&s_escKept);
    }
    if (dev == s_mouse) {
        InterlockedIncrement(&s_stateCalls);
        // Photo mode zooms with the wheel; the game does not get it (in third person it would move
        // the camera and switch to the first person).
        if (PhotoActive() && SUCCEEDED(hr) && data && size >= 3 * sizeof(LONG)) {
            if (((LONG*)data)[2]) PhotoWheel(((LONG*)data)[2]);
            ((LONG*)data)[2] = 0;
        }
        LONG rawX = 0, rawY = 0;
        if (SUCCEEDED(hr) && data && size >= 2 * sizeof(LONG)) {
            rawX = ((LONG*)data)[0];
            rawY = ((LONG*)data)[1];
        }
        // Dragging on the Pip-Boy (left button held: panning the map, a scroll bar): the game pans by the
        // mouse's own motion, so it gets it unchanged, as without the mod (no steering, no lens scale).
        bool drag = SUCCEEDED(hr) && data && size >= 16 && (((BYTE*)data)[12] & 0x80) && PipboyScreenActive();
        if (drag != s_dragging) {
            s_dragging = drag;
            Log("dinput: Pip-Boy drag %s (free mouse %d)", drag ? "started" : "ended", s_free ? 1 : 0);
            if (!drag && s_free) s_dragEnded = true;  // the Windows cursor is moved to the game's (mousetrap.cpp)
        }
        if (s_free && !drag && SUCCEEDED(hr) && data && size >= 2 * sizeof(LONG)) Steer(&((LONG*)data)[0], &((LONG*)data)[1]);
        // Pip-Boy warped after the UI: the shown cursor moves at the normal speed (see LensCursorScale).
        static float s_restX, s_restY;
        float kx, ky;
        if (!s_free && !drag && SUCCEEDED(hr) && data && size >= 2 * sizeof(LONG) && LensCursorScale(&kx, &ky)) {
            float fx = ((LONG*)data)[0] * kx + s_restX, fy = ((LONG*)data)[1] * ky + s_restY;
            ((LONG*)data)[0] = (LONG)fx;
            ((LONG*)data)[1] = (LONG)fy;
            s_restX = fx - (LONG)fx;
            s_restY = fy - (LONG)fy;
        } else {
            s_restX = s_restY = 0;
        }
        if (s_switchTrace > 0) {
            --s_switchTrace;
            BYTE* im = *(BYTE**)0x011D8A80;
            BYTE* pl = *(BYTE**)0x011DEA3C;
            float cx = 0, cy = 0;
            DinputGameCursor(&cx, &cy);
            typedef bool(__cdecl * IsMenuModeFn)();
            Log("dinput: switch trace frame %lu hr %08lx free %d pipBoyMode %lu menumode %d rot %.4f %.4f cursor %.1f %.1f raw %ld %ld sent %ld %ld",
                FrameCount(), hr, s_free, im ? *(DWORD*)(im + 0x4BC) : 0, ((IsMenuModeFn)0x00702360)(), pl ? *(float*)(pl + 0x24) : 0,
                pl ? *(float*)(pl + 0x2C) : 0, cx, cy, rawX, rawY, data ? ((LONG*)data)[0] : 0, data ? ((LONG*)data)[1] : 0);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_GetDeviceData(void* dev, DWORD cb, DIDEVICEOBJECTDATA* data, DWORD* inout, DWORD flags)
{
    HRESULT hr = s_getData(dev, cb, data, inout, flags);
    if ((dev == s_mouse || dev == s_keyboard) && OverlayOpen()) {
        if (SUCCEEDED(hr) && inout && !(flags & DIGDD_PEEK)) *inout = 0;
        return hr;
    }
    if (dev == s_keyboard) InterlockedIncrement(&s_keyDataCalls);
    if (dev == s_keyboard && PhotoActive() && SUCCEEDED(hr) && data && inout && cb >= sizeof(DWORD) * 2) {  // no Esc (as above)
        DWORD n = *inout, kept = 0;
        for (DWORD i = 0; i < n; ++i) {
            DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((BYTE*)data + i * cb);
            if (e->dwOfs == DIK_ESCAPE) {
                InterlockedIncrement(&s_escKept);
                continue;
            }
            if (kept != i) memcpy((BYTE*)data + kept * cb, e, cb);
            ++kept;
        }
        *inout = kept;
    }
    if (dev == s_mouse) {
        InterlockedIncrement(&s_dataCalls);
        bool photo = PhotoActive();
        if ((s_free || photo) && SUCCEEDED(hr) && data && inout && cb >= sizeof(DWORD) * 2) {
            DWORD n = *inout, kept = 0;
            for (DWORD i = 0; i < n; ++i) {
                DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((BYTE*)data + i * cb);
                if (s_free && (e->dwOfs == DIMOFS_X || e->dwOfs == DIMOFS_Y)) continue;
                if (photo && e->dwOfs == DIMOFS_Z) {  // the wheel zooms in photo mode (as in GetDeviceState)
                    if (!(flags & DIGDD_PEEK)) PhotoWheel((LONG)e->dwData);
                    continue;
                }
                if (kept != i) memcpy((BYTE*)data + kept * cb, e, cb);
                ++kept;
            }
            *inout = kept;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateDevice(void* di, REFGUID guid, void** out, void* outer)
{
    HRESULT hr = s_createDevice(di, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(guid, GUID_SysKeyboard)) {
        s_keyboard = *out;
        void** vt = *(void***)s_keyboard;
        SwapSlot(vt, kSlotGetDeviceState, (void*)H_GetDeviceState, (void**)&s_getState);
        SwapSlot(vt, kSlotGetDeviceData, (void*)H_GetDeviceData, (void**)&s_getData);
        Log("dinput: keyboard device %p (vtable %p) hooked", s_keyboard, vt);
    }
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(guid, GUID_SysMouse)) {
        s_mouse = *out;
        void** vt = *(void***)s_mouse;
        s_acquire = (AcquireFn)vt[kSlotAcquire];
        s_unacquire = (AcquireFn)vt[kSlotUnacquire];
        SwapSlot(vt, kSlotGetDeviceState, (void*)H_GetDeviceState, (void**)&s_getState);
        SwapSlot(vt, kSlotGetDeviceData, (void*)H_GetDeviceData, (void**)&s_getData);
        SwapSlot(vt, kSlotSetCooperativeLevel, (void*)H_SetCooperativeLevel, (void**)&s_setCoop);
        Log("dinput: mouse device %p (vtable %p) hooked", s_mouse, vt);
    }
    return hr;
}

static HRESULT WINAPI My_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    HRESULT hr = s_create(inst, version, riid, out, outer);
    Log("dinput: DirectInput8Create(%08lx) -> %08lx %p", version, hr, SUCCEEDED(hr) && out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        void** vt = *(void***)*out;
        SwapSlot(vt, kSlotCreateDevice, (void*)H_CreateDevice, (void**)&s_createDevice);
    }
    return hr;
}

void DinputInstall()
{
    s_create = (DI8CreateFn)PatchExeImport("DINPUT8.dll", "DirectInput8Create", (void*)My_DirectInput8Create);
}

// Switches the mouse between the game's own cooperative level and non-exclusive.
void DinputSetMouseFree(bool free)
{
    if (free == s_free || !s_mouse || !s_setCoop || !s_mouseWindow) return;
    s_free = free;
    s_switchTrace = 120;
    s_haveTarget = false;
    s_steerFrame = (DWORD)-1;
    s_unacquire(s_mouse);
    HRESULT hr = s_setCoop(s_mouse, s_mouseWindow, free ? DISCL_NONEXCLUSIVE | DISCL_FOREGROUND : s_gameFlags);
    HRESULT ha = s_acquire(s_mouse);
    Log("dinput: mouse %s (SetCooperativeLevel %08lx, Acquire %08lx)", free ? "set free (non-exclusive)" : "captured again (game's level)",
        hr, ha);
}

bool DinputMouseFree() { return s_free; }

// The game's menu cursor (InterfaceManager +0x38 / +0x40).
bool DinputGameCursor(float* x, float* y)
{
    BYTE* im = *(BYTE**)0x011D8A80;
    if (!im) return false;
    *x = *(float*)(im + 0x38);
    *y = *(float*)(im + 0x40);
    return true;
}

void DinputSetGameCursor(float x, float y)
{
    BYTE* im = *(BYTE**)0x011D8A80;
    if (!im) return;
    *(float*)(im + 0x38) = x;
    *(float*)(im + 0x40) = y;
}

void DinputWriteState(FILE* f)
{
    float x = 0, y = 0;
    DinputGameCursor(&x, &y);
    POINT p = {};
    GetCursorPos(&p);
    if (GameWindow()) ScreenToClient(GameWindow(), &p);
    fprintf(f, "cursor: game %.1f %.1f, windows (client) %ld %ld\n", x, y, p.x, p.y);
    fprintf(f, "dinput: mouse %p, game cooperative level %08lx, free %d, GetDeviceState %ld, GetDeviceData %ld, px/count %.2f %.2f %.2f "
               "damp %.2f, target %.1f %.1f\n",
            s_mouse, s_gameFlags, s_free, s_stateCalls, s_dataCalls, s_gain[0], s_gain[1], s_gain[2], s_damp, s_targetX, s_targetY);
    fprintf(f, "dinput: keyboard %p, GetDeviceState %ld, GetDeviceData %ld, Esc kept from the game (photo mode) %ld\n", s_keyboard,
            s_keyStateCalls, s_keyDataCalls, s_escKept);
}
