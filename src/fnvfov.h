// ROWE: declarations shared between the modules, grouped by source file.
//
// The game runs everything this mod touches on one thread (window, input, game loop, rendering
// and Present all run on the main thread; verified in phase 0), so module state is plain globals.
//
// All engine addresses are for FalloutNV.exe 1.4.0.525 (image base 0x00400000). Code patches
// check the bytes they replace first and skip themselves on a mismatch.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <d3d9.h>

// main.cpp
void Log(const char* fmt, ...);  // appends a timestamped line (with thread id) to rowe.log
const char* ModDir();            // folder of the DLL, with trailing backslash
DWORD MainThreadId();            // the engine's main thread (from OSGlobals), 0 if not known yet
HWND GameWindow();               // the main window (OSGlobals), null before it exists
bool GameInFront();              // a window of this process is the foreground window
bool InGameplay();               // no menu, not loading, the world drawn in the last few frames

// detour.cpp
// Returns the trampoline (call it to run the original), or null if not installed.
void* InstallDetour(DWORD targetVA, const BYTE* expected, int len, void* hook, const char* name);
// Replaces one function in the exe's import table, found by name. Chains to whatever is in the
// slot now (another mod may have replaced it); returns that previous value, or null.
void* PatchExeImport(const char* dll, const char* name, void* hook);
// Retargets the `call rel32` at siteVA. The site must hold E8; if its target is not
// `expectedTarget` (another mod hooked it), the hook chains to that target. A jump (E9) or other
// bytes are refused. Returns the target the hook must call, or null.
void* PatchCall(DWORD siteVA, DWORD expectedTarget, void* hook, const char* name);
bool ExeCodeRange(DWORD* begin, DWORD* end);  // the exe's .text section
void PatchWriteStatus(FILE* f);                // every patch attempt and its result
bool PatchCallOver(DWORD siteVA, const BYTE* expected, int len, void* fn, const char* name);  // code -> call fn + NOPs

void CrashLogInstall(HMODULE self);  // crashlog.cpp: logs access violations to rowe.log

// settings.cpp: rowe.ini (see the file header for the keys).
enum { kLensOff = 0, kLensAuto = 1, kLensPanini = 2, kLensStereographic = 3, kLensFisheye = 4, kLensModeCount = 5 };
enum { kMouseTrapOff = 0, kMouseTrapGameplay = 1, kMouseTrapAlways = 2, kMouseTrapModeCount = 3 };
static const float kFovMin = 60.f, kFovMax = 160.f;
static const float kPipboyFovMin = 20.f, kPipboyFovMax = 90.f;
static const float kZoomFovMin = 10.f;
struct Settings {
    float fov = 0;           // 0 = game default
    int lens = kLensAuto;
    float pipboyFov = 0;     // 0 = game default (fPipboy1stPersonFOV)
    float zoomFov = 0;       // zoom without a weapon drawn; 0 = game default (fIronSightsZoomDefault)
    float supersample = 1;   // pixel-count multiplier; applies at the next start
    int mousetrap = kMouseTrapOff;
    bool skipIntros = true;  // applies at the next start
    bool scopeLens = true;   // the lens also bends the scope overlay
    bool fisheyeCircle = false;  // fisheye: the whole image circle on screen (smaller, more black) instead of filling the width
    char screenshotDir[MAX_PATH] = "";  // screenshot folder; empty = next to the DLL
    int screenshotKey = VK_F12;          // virtual key of the screenshot key
    int menuKey = VK_F8;                 // virtual key that opens and closes the settings menu
    bool screenshotFull = true;          // at the supersampled size (false: the window's)
    bool screenshotJpg = true;           // JPEG (false: PNG)
    int screenshotQuality = 95;          // JPEG quality 1-100
    bool reshadeFirst = true;            // ReShade's effects into the supersampled frame (needs ReShade's add-on build)
    bool stats = false;                  // the stats window (lens, supersampling), also with the menu closed
    int statsSize = 80;                  // its text size, percent of the menu's
    float statsX = -1, statsY = -1;      // its top-left corner, fraction of the screen; -1 = top right
    int devFullscreen = -1;  // [dev] fullscreen: -1 = the game's own bFull Screen
    int devWidth = 0, devHeight = 0;  // [dev] size: window size for this start, 0 = the game's own
    bool devVatsShrink = true;  // [dev] vatsshrink=0: VATS image space at the game's size (comparison)
};
extern Settings g_settings;
void SettingsLoad();
void SettingsSave();
#include "keynames.h"  // KeyFromName, KeyName

bool WritePng(const char* path, int w, int h, const BYTE* rgb);  // png.cpp: rows top first

// d3d9hook.cpp: the engine's D3D9 device. Device methods are hooked in its vtable; calls the mod
// makes itself inside a D3DScope bypass the hooks (no tracing, no supersampling redirection).
struct D3DScope { D3DScope(); ~D3DScope(); };
void* D3DWrapProc(const char* name, void* real);  // GetProcAddress filter for Direct3DCreate9(Ex)
void D3DAttach(IDirect3DDevice9* device, const char* how);
IDirect3DDevice9* D3DDevice();                  // the hooked device, or null
HRESULT D3DRealBackBuffer(IDirect3DSurface9** out);  // the swap chain's own back buffer
void D3DSimulateDeviceLost();                    // test: the game resets its device as after a loss
void D3DReportLiveResources(const char* when);   // logs the default-pool resources still alive, by creating module
void D3DBackBufferSize(int* w, int* h);         // the size the engine renders at (virtual with SS)
void D3DTraceStamp(const char* what);           // marks an engine event in the frame trace
bool D3DTracing();
void D3DDescribeRT(char* out, int size);        // currently bound render target 0

// frame.cpp: per-frame work at Present (in this order: supersampling resolve, screenshot,
// overlay, mouse trap) and device loss.
void FramePresent(IDirect3DDevice9* dev);
void FrameDeviceLost();
void ModWriteState(FILE* f);  // the mod's settings and per-feature decisions, for rowe_state.txt

// screenshot.cpp: F12 or the `shot` command. Captures the frame as PNG next to the DLL, with a
// state snapshot of the same frame (.txt).
enum { kShotFrame, kShotScene, kShotWithMenu, kShotUser };
void ScreenshotRequest(int kind);
void ScreenshotPresent(IDirect3DDevice9* dev, IDirect3DSurface9* frame);  // final frame, before the overlay
void ScreenshotAfterOverlay(IDirect3DDevice9* dev, IDirect3DSurface9* frame);  // with the settings menu
void ScreenshotPreUI(IDirect3DDevice9* dev);                               // scene only, before the HUD
void ScreenshotLost();
void ScreenshotFolder(char* out);  // the screenshot folder (created if missing), trailing backslash

// fov.cpp: the FOV slider (remaps the angles BSSceneGraph::SetCameraFOV is called with).
float FovRemapSetter(void* ret, void* sg, float fov);  // per call site (return address)
float FovRemapComp(float fov);                         // FovComp (0x00B54000) follows the world calls
float FovBase();                                       // fDefaultWorldFOV
int FlatMenuView();                                    // a terminal or casino game is open (lens off): its menu id, or 0
float FovZoomDefault();                                // the game's own zoom target as shown (slider units)
void FovWriteState(FILE* f);

// lens.cpp: lens correction (see the file header for the sequence of hooks).
float LensRenderFov(float engineFov);      // world: FOV to render so the lens is covered
float LensViewmodelFov(float engineFov);   // viewmodel: FOV that keeps the gun's vanilla size
void LensWorldDraw(float frustumRight);    // world draw B: true tan(half FOV) of this frame
void LensPreUI(IDirect3DDevice9* dev);     // the warp, before the HUD
void LensPostImageSpace(IDirect3DDevice9* dev);  // the warp before the viewmodel pass (flat scope overlay)
void LensPresent(IDirect3DDevice9* dev, IDirect3DSurface9* frame);  // Pip-Boy frames: the warp after the UI
bool LensMapToRender(float* x, float* y);  // shown at render pixel (x, y) -> where the game's cursor must be
bool LensCursorScale(float* kx, float* ky);  // Pip-Boy warped after the UI: render px per shown px at the cursor
bool LensCursorShown(float* x, float* y);   // Pip-Boy warped after the UI: where the cursor was shown (render px)
void LensLost();
float LensEffectiveFov();                  // lens FOV in use (engine units), 0 when not warping
// For the stats window: the latest warp's lens FOV and the FOV it was rendered at (true horizontal
// degrees), Auto's Panini share (0..1); false when no recent frame was warped (*skip says why).
bool LensStats(float* lensFov, float* renderFov, float* autoBlend, const char** skip);
void LensWriteState(FILE* f);

// supersample.cpp: renders at a virtual resolution, averaged down at Present (see the file header).
// reshade_addon.cpp: ReShade's effects rendered into the supersampled frame (ReShade add-on build).
void ReshadeAddonInit();
bool ReshadeRenderInto(IDirect3DDevice9* dev, IDirect3DSurface9* target);
void ReshadeWriteState(FILE* f);
int ReshadeState();  // 0 not loaded, 1 loaded without add-on support, 2 add-on registered and runtime ready

void SupersampleInit();
bool SupersampleActive();
void SupersampleSyncSettings();  // before the first window: adopt an iSize override (FalloutCustom.ini)
bool SupersampleIsVirtual(IUnknown* r);  // r is the virtual back buffer (texture or surface)
bool SupersampleOverridden();  // the game size changed after the iSize reads: supersampling off this start
// borderless.cpp: fullscreen at a size the monitor has no display mode for runs borderless instead.
void BorderlessDecide();
bool BorderlessActive();
void BorderlessWindowCreated(HWND wnd, HWND parent, DWORD style);
bool BorderlessFixAdjust(LPRECT rc);
bool BorderlessFixPos(HWND wnd, int* x, int* y, int* cx, int* cy, UINT* flags);
bool SupersampleSizes(int* nativeW, int* nativeH, int* virtW, int* virtH);  // false when off
bool SupersampleVirtualSize(int* nativeW, int* nativeH, int* virtW, int* virtH);  // once the INI is read
UINT SupersampleIniInt(LPCSTR app, LPCSTR key, UINT value, UINT (*readOther)(bool width));  // GetPrivateProfileIntA result
LPCSTR SupersampleIniWrite(LPCSTR app, LPCSTR key, LPCSTR value, char* buf);  // WritePrivateProfileStringA value
UINT SupersampleIniMsaa(LPCSTR app, LPCSTR key, UINT value);              // iMultiSample: capped at high factors
bool SupersampleFixSize(int* w, int* h);                                  // window sizes: virtual -> native
bool SupersampleFixPresentParams(D3DPRESENT_PARAMETERS* pp);              // CreateDevice/Reset: virtual -> native
void SupersampleRestorePresentParams(D3DPRESENT_PARAMETERS* pp);
void SupersampleDeviceReady(IDirect3DDevice9* dev);                       // creates and binds the virtual back buffer
bool SupersampleBackBuffer(IDirect3DSurface9** out);                      // GetBackBuffer replacement
IDirect3DSurface9* SupersampleReplaceTarget(IDirect3DSurface9* s);        // SetRenderTarget: real -> virtual
void SupersampleResolve(IDirect3DDevice9* dev, IDirect3DSurface9* realBB);
void SupersampleLost();
void SupersampleWriteState(FILE* f);

// dinput.cpp: the game's DirectInput mouse (see the file header).
void DinputInstall();
void DinputSetMouseFree(bool free);   // non-exclusive, movement hidden from the game
bool DinputMouseFree();
bool DinputTakeDragEnded();  // a drag on the Pip-Boy with the free mouse just ended (once)
bool DinputGameCursor(float* x, float* y);
void DinputSetGameCursor(float x, float y);
void DinputSetCursorTarget(float x, float y, float renderW, float renderH);  // free mode: where the game cursor should go
void DinputWriteState(FILE* f);

// ui.cpp: interface layout at aspect ratios narrower than 4:3.
void UiInstall();
void UiWriteState(FILE* f);
void UiFrame();                 // from Present
void UiDumpNodes(const char* name, int depth);
bool ScopeView();               // the HUD shows a scope overlay (aiming a scoped weapon)
// lens.cpp: the Pip-Boy screen is the active menu (fully open, nothing over it).
bool PipboyScreenActive();

// mousetrap.cpp / intros.cpp
void MouseTrapFrame();          // from Present
void MouseTrapWriteState(FILE* f);
void IntrosInit();              // at plugin load: finds the settings
void IntrosApply();             // after the INI files are read (first CreateWindowExA)
void IntrosWriteState(FILE* f);

// overlay.cpp: the F8 settings menu.
void OverlayPresent(IDirect3DDevice9* dev, IDirect3DSurface9* target);
void OverlayCommand(const char* arg);  // tools: "open", "close", "up", "down", "left", "right", "home"
bool OverlayOpen();
// gameui.cpp: the game's UI font and HUD colour for the settings menu.
bool GameFontCapture(int fontIndex);  // copies the game's loaded font fontIndex (0-based); false if not readable
bool GameHudColor(float rgb[3]);
bool OverlayWndProc(HWND h, UINT msg, WPARAM w, LPARAM l);  // the game window's messages (menu input)
void OverlayLost();

// recon_win32.cpp: logs the exe's window, INI and D3D9 lookup calls (startup timing, sizes).
void ReconWin32Install();
// recon_engine.cpp: engine hooks (FOV setter, render call sites) and the state file.
void ReconEngineInstall();
void ReconEngineDeferredInit();
void ReconEngineMainLoop();    // xNVSE MainGameLoop, game thread
void ReconEngineOnPresent();   // from the Present hook
void ReconEngineAtLoad();      // logs what already exists when the plugin loads
void WriteStateTo(const char* path);  // a state snapshot (the state file's contents)
BYTE* FindSettingObject(const char* name);  // a game Setting {vtable, value, name} by its name, or null
void FovTraceStart(int frames);  // records every FOV setter call for `frames` presents (rowe_fovtrace.csv)
void FovTraceFinish();           // writes a trace still recording (at exit)
DWORD FrameCount();              // presents so far
DWORD LastWorldDrawFrame();      // FrameCount() when the world was last drawn

// gamecmd.cpp: rowe_cmd.txt command queue (see the file header), game thread.
void GameCmdInit(void* consoleInterface);
void GameCmdQueue(const char* line);  // appends a command (the mod's own sequences)
void GameCmdNow(const char* line);    // runs a command on the next tick, ahead of the queue
// restart.cpp: save, start the game again the same way, load the save (settings that apply at start).
void RestartGame();
void RestartNow(bool saved);
void RestartAtLoad();
bool RestartGameLoaded();
bool RestartPending();
void GameCmdTick();              // once per MainGameLoop
// mcm.cpp: the settings as a page of The Mod Configuration Menu (with ROWE - MCM.esp; optional).
void McmInit(void* scriptInterface);  // xNVSE's NVSEScriptInterface (null: no MCM page)
void McmTick();                       // once per MainGameLoop
void GameCmdWriteState(FILE* f);
