// FOV slider: remaps the angle BSSceneGraph::SetCameraFOV (0x00C52020) is called with.
//
// The engine calls the setter several times per frame (phase 0, run 4): four world call sites
// with the world FOV (already eased while aiming), three viewmodel call sites with the
// first-person FOV, an over-shoulder site in third person and a pair around the Pip-Boy. Only the
// world sites are remapped, so the stored FOV values stay vanilla (nothing to "reset" after VATS,
// kill cams or POV changes) and the weapon keeps its size. FovComp (0x00B54000), which follows
// every world call with the same angle, gets the same remapped angle.
//
// Units: the engine's FOV is a horizontal FOV at a 4:3 reference (the frustum's right edge is
// 0.75 * aspect * tan(fov / 2), Hor+), and so is the slider, like fDefaultWorldFOV.
//
// Remap: tan(fov / 2) is scaled by k = tan(slider / 2) / tan(base / 2), base = fDefaultWorldFOV.
// That keeps aiming's magnification (the ratio of the tangents) as authored; between 55 and 35
// degrees the scaling fades out, so scopes (below 35) keep their authored zoom too. Then the lens (lens.cpp)
// may ask for a wider render FOV than that, which its warp maps back.
#include <windows.h>
#include <math.h>
#include "fnvfov.h"

static const float kPi = 3.14159265f;
// Below kScopeLo (scopes) the angle is left alone; from kScopeHi up (iron sights and the normal
// view) the full scaling applies, so iron sights keep their vanilla magnification.
static const float kScopeLo = 35.f, kScopeHi = 55.f;

// Return addresses of the world call sites (the call's next instruction).
static const DWORD kWorldSites[] = {
    0x0086EB90,  // main loop (also in menus), setLOD 1
    0x00872B70,  // before the world draw, setLOD 1
    0x0094AF33,  // PlayerCharacter::UpdateCamera
    0x008759F9,  // restore after the viewmodel pass
    0x00875E9C,  // around the Pip-Boy (world value)
    0x00950642,  // once after loading
    0x00874874,  // rare pair (world value)
    0x00C52584,  // NiCamera::WorldToScreen helper (quest markers must match the render)
};
// Viewmodel sites (first-person FOV, Pip-Boy FOV): widened only to compensate the lens warp.
static const DWORD kViewmodelSites[] = {0x00874D2D, 0x008752BF, 0x00875B97, 0x00875F14, 0x008747E4};
// Sites that are never remapped: over-shoulder, menu scene graph, scene graph setup.
static const DWORD kOtherSites[] = {0x0094B4C3, 0x00713375, 0x00C51D6C};

static float s_lastIn = -1, s_lastOut = -1;  // the latest world remap, for FovComp
static DWORD s_lastWorldFrame;
static float s_lastWorldIn, s_lastWorldOut;

static float SmoothStep(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t * t * (3 - 2 * t);
}

float FovBase()
{
    float base = *(float*)0x01203160;  // fDefaultWorldFOV:Display value
    return base > 10 && base < 170 ? base : 75.f;
}

// The slider's FOV for an engine angle (before the lens), in engine units.
static float SliderRemap(float fov)
{
    float s = g_settings.fov;
    if (s <= 0) return fov;
    float base = FovBase();
    float hi = base < kScopeHi ? base : kScopeHi;
    float w = SmoothStep((fov - kScopeLo) / (hi - kScopeLo));
    float k = tanf(s * kPi / 360.f) / tanf(base * kPi / 360.f);
    float t = tanf(fov * kPi / 360.f) * powf(k, w);
    float out = atanf(t) * 360.f / kPi;
    return out > kFovMax ? kFovMax : out;
}

static bool IsWorldSite(DWORD ret, void* sg, float fov)
{
    for (DWORD a : kWorldSites)
        if (a == ret) return true;
    for (DWORD a : kViewmodelSites)
        if (a == ret) return false;
    for (DWORD a : kOtherSites)
        if (a == ret) return false;
    // Unknown site: world if it targets the world scene graph with the player's world FOV.
    BYTE* player = *(BYTE**)0x011DEA3C;
    bool world = sg == *(void**)0x011DEB7C && player && fabsf(*(float*)(player + 0x670) - fov) < 0.01f;
    static DWORD logged[32];
    static int count;
    bool seen = false;
    for (int i = 0; i < count; ++i) seen |= logged[i] == ret;
    if (!seen && count < 32) {
        logged[count++] = ret;
        Log("fov: unknown SetCameraFOV call site %08lx (fov %.2f) treated as %s", ret, fov, world ? "world" : "other");
    }
    return world;
}

// While the Pip-Boy is up (InterfaceManager +0x4BC pipBoyMode != 0) the viewmodel FOV eases from
// fDefault1stPersonFOV to fPipboy1stPersonFOV; the Pip-Boy FOV setting moves the end of that
// easing, so opening and closing still animate.
static float PipboyRemap(float fov)
{
    float target = g_settings.pipboyFov;
    BYTE* im = *(BYTE**)0x011D8A80;
    if (target <= 0 || !im || !*(DWORD*)(im + 0x4BC)) return fov;
    float first = *(float*)0x0120316C, pipboy = *(float*)0x01203178;  // fDefault1stPersonFOV, fPipboy1stPersonFOV
    if (!(first > pipboy + 0.5f)) return target;
    float t = (first - fov) / (first - pipboy);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return fov + (target - pipboy) * t;
}

// Zoom with the weapon holstered: the aiming code (0x0095DE30) eases the world FOV from
// fDefaultWorldFOV to fIronSightsZoomDefault:Combat (setting 0x011E0970, value at +4, default 50;
// bIronSightsZoomEnable on). Through the slider remap that zoom is small (130 -> about 96), and a
// target below 55 would cross the remap's fade to the scope range, where the zoom races and then
// crawls. With zoomfov set, the game's progress along its own easing is mapped straight onto the
// slider FOV -> zoomfov, evenly in log tan (a constant zoom speed); the game's timing is kept.
static const DWORD kZoomDefaultSetting = 0x011E0970;

static float ZoomTarget()
{
    float* value = (float*)(kZoomDefaultSetting + 4);
    return IsBadReadPtr(value, 4) ? 0.f : *value;
}

// The player's weapon is drawn (BaseProcess::IsWeaponOut, vtable +0x454, as the aiming code asks).
static bool WeaponOut()
{
    BYTE* player = *(BYTE**)0x011DEA3C;
    if (!player || IsBadReadPtr(player + 0x68, 4)) return true;
    BYTE* process = *(BYTE**)(player + 0x68);
    if (!process || IsBadReadPtr(process, 4)) return true;
    void** vtbl = *(void***)process;
    if (!vtbl || IsBadReadPtr(&vtbl[0x454 / 4], 4)) return true;
    typedef bool(__thiscall * IsWeaponOutFn)(void*);
    return ((IsWeaponOutFn)vtbl[0x454 / 4])(process);
}

// The shown FOV during the holstered zoom, or 0 when this world angle is not that zoom.
static float HolsteredZoom(float fov)
{
    float zoom = g_settings.zoomFov;
    if (zoom <= 0) return 0;
    float base = FovBase(), target = ZoomTarget();
    if (!(base - target > 1.f) || fov >= base || fov < target - 0.01f || WeaponOut()) return 0;
    float p = (base - fov) / (base - target);
    if (p > 1) p = 1;
    float from = logf(tanf(SliderRemap(base) * kPi / 360.f)), to = logf(tanf(zoom * kPi / 360.f));
    return atanf(expf(from + (to - from) * p)) * 360.f / kPi;
}

// Menus that show a model right in front of the camera, made for a flat image: terminals
// (ComputersMenu 1057, its hacking screen 1055; the screen is drawn over the stored, blurred world
// frame by its own camera) and the casino games (slot machine 1080, blackjack 1081, roulette 1082,
// Caravan 1083: the table). While one is open the lens is off (lens.cpp). Returns its id, or 0.
int FlatMenuView()
{
    static const int kMenus[] = {1055, 1057, 1080, 1081, 1082, 1083};
    const BYTE* visible = (const BYTE*)0x011F308F;  // indexed by menu id
    for (int id : kMenus)
        if (visible[id]) return id;
    return 0;
}

float FovZoomDefault()
{
    return SliderRemap(ZoomTarget());
}

float FovRemapSetter(void* ret, void* sg, float fov)
{
    for (DWORD a : kViewmodelSites)
        if (a == (DWORD)ret) return LensViewmodelFov(PipboyRemap(fov));
    if (!IsWorldSite((DWORD)ret, sg, fov)) return fov;
    float shown = HolsteredZoom(fov);
    float out = LensRenderFov(shown > 0 ? shown : SliderRemap(fov));
    s_lastIn = fov;
    s_lastOut = out;
    s_lastWorldFrame = FrameCount();
    s_lastWorldIn = fov;
    s_lastWorldOut = out;
    return out;
}

float FovRemapComp(float fov)
{
    return fov == s_lastIn ? s_lastOut : fov;
}

void FovWriteState(FILE* f)
{
    fprintf(f, "fov: zoom without weapon: zoomfov %g, game target %.3f, weapon out %d\n", g_settings.zoomFov, ZoomTarget(),
            WeaponOut() ? 1 : 0);
    fprintf(f, "fov: lens-off menu open %d\n", FlatMenuView());
    fprintf(f, "fov: slider %g base %g, last world call frame %lu: %.3f -> %.3f\n", g_settings.fov, FovBase(), s_lastWorldFrame,
            s_lastWorldIn, s_lastWorldOut);
}
