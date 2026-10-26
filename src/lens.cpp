// Lens correction: very wide FOVs without stretched edges (Panini or stereographic projection,
// "auto": rectilinear up to 90 degrees, blending into Panini by 160, or fisheye: equidistant, up to
// 160 degrees across with black corners), after flex-fov and gowfov.
//
// 1. FOV (fov.cpp -> LensRenderFov): the slider's angle becomes a lens FOV; the world is rendered
//    at the wider rectilinear FOV that covers every ray the lens shows. Fisheye "full circle": the
//    same render, all of it shown as a circle fitted to the screen's shorter side.
// 2. Viewmodel (fov.cpp -> LensViewmodelFov): the gun is drawn into the same image as the world,
//    so the warp would shrink it. Its FOV is widened by tanR / C (C = the lens' scale at the
//    centre) so it keeps its vanilla size.
// 3. World draw B (0x00870E18): the world camera's frustum is the rendered FOV of this frame
//    (before the viewmodel pass changes it).
// 4. Pre-UI (the call to InterfaceManager::Render at 0x007144D3): the image-spaced frame is in the
//    back buffer and no HUD has been drawn. The warp copies it and resamples it into the lens
//    projection; the lens FOV is derived from the frustum of step 3, so frames rendered with a
//    different FOV (aiming transitions, menus) are still warped consistently.
// 5. With the Pip-Boy or the pause menu open the game stops drawing the world: it draws a stored
//    copy of the last world frame (unwarped) and the Pip-Boy over it. Those frames are warped with
//    the last world frame's FOV. A loading screen invalidates it (the main menu is not warped).
// 6. While the Pip-Boy screen is the active menu the warp runs after the UI instead (at Present),
//    so the game's hit-testing matches what is shown (see LensPreUI). The cursor itself is then cut out of
//    the warped frame and drawn unwarped where its hit point is shown (see LensPresent).
//
// Units: the engine's FOV angles are horizontal at a 4:3 reference (tan = true tan / (0.75 *
// aspect)); everything here works in true horizontal angles and tangents.
#include <windows.h>
#include <math.h>
#include "fnvfov.h"
#include "lens_vs.h"
#include "lens_ps.h"
#include "cursor_ps.h"

static const float kPi = 3.14159265f;
static const float kMaxRenderFov = 160.f;  // single-view limit; centre sharpness collapses beyond this
static const float kRef = 0.75f;           // the engine's 4:3 reference (double at 0x0101DE30)

// ---- lens math (mirrors lens.hlsl) -----------------------------------------------------------

struct V3 { float x, y, z; };

static V3 Norm(V3 v)
{
    float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return {v.x / l, v.y / l, v.z / l};
}

static V3 RayRectilinear(float sx, float sy, float half)
{
    float t = tanf(half < 1.5690f ? half : 1.5690f);
    return Norm({sx * t, sy * t, 1});
}

static V3 RayPanini(float sx, float sy, float half)
{
    float edge = 2 * sinf(half) / (1 + cosf(half));
    float px = sx * edge, py = sy * edge;
    float k = px * px / 4;
    float clon = (1 - k) / (1 + k);
    float S = 1 + k;
    float lon = atan2f(px, S * clon);
    float lat = atan2f(py, S);
    return {cosf(lat) * sinf(lon), sinf(lat), cosf(lat) * cosf(lon)};
}

static V3 RayStereographic(float sx, float sy, float half)
{
    float t = tanf(half * 0.5f);
    float px = sx * t, py = sy * t;
    float r = sqrtf(px * px + py * py);
    if (r < 1e-6f) return {0, 0, 1};
    float th = 2 * atanf(r);
    return {px / r * sinf(th), py / r * sinf(th), cosf(th)};
}

// Fisheye (equidistant, r = theta): the angle from the centre grows evenly with the distance on
// screen. Up to 160 degrees across (one rendered view's limit); a frame's corners then look past
// 90 degrees, where nothing was rendered, and stay black like a fisheye lens' vignette.
static V3 RayFisheye(float sx, float sy, float half)
{
    float px = sx * half, py = sy * half;
    float r = sqrtf(px * px + py * py);
    if (r < 1e-6f) return {0, 0, 1};
    return {px / r * sinf(r), py / r * sinf(r), cosf(r)};
}

// Rectilinear up to 90 degrees, full Panini from 160 (ease-out in between).
static float AutoBlend(float fovDeg)
{
    float t = (fovDeg - 90.f) / 70.f;
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return 1 - (t - 1) * (t - 1);
}

static V3 Ray(int mode, float sx, float sy, float half, float blend)
{
    if (mode == kLensStereographic) return RayStereographic(sx, sy, half);
    if (mode == kLensFisheye) return RayFisheye(sx, sy, half);
    if (mode == kLensPanini) return RayPanini(sx, sy, half);
    V3 a = RayRectilinear(sx, sy, half), b = RayPanini(sx, sy, half);
    return Norm({a.x + (b.x - a.x) * blend, a.y + (b.y - a.y) * blend, a.z + (b.z - a.z) * blend});
}

// Smallest tan(render half-FOV) whose rectilinear image contains every ray the lens needs.
static float CoverageTan(int mode, float fovDeg, float aspect)
{
    float half = fovDeg * kPi / 360.f, blend = AutoBlend(fovDeg);
    float need = 0;
    const int n = 48;
    for (int i = 0; i <= n; ++i) {
        float t = -1 + 2.f * i / n;
        float pts[4][2] = {{t, 1 / aspect}, {t, -1 / aspect}, {1, t / aspect}, {-1, t / aspect}};
        for (auto& p : pts) {
            V3 r = Ray(mode, p[0], p[1], half, blend);
            if (r.z <= 1e-4f) return 1e9f;
            float x = fabsf(r.x / r.z), y = fabsf(r.y / r.z) * aspect;
            if (x > need) need = x;
            if (y > need) need = y;
        }
    }
    return need;
}

// The lens' scale at the centre: a ray at a small angle theta lands at screen x = theta / C.
static float CentreScale(int mode, float fovDeg)
{
    const float d = 1e-3f;
    V3 r = Ray(mode, d, 0, fovDeg * kPi / 360.f, AutoBlend(fovDeg));
    return atan2f(r.x, r.z) / d;
}

// CoverageTan for lens FOVs 0..179 in half-degree steps, per (mode, aspect). FOV -> render goes
// through it, rendered frustum -> lens FOV goes through its inverse, so the round trip is exact.
static const float kTableStep = 0.5f;
static const int kTableSize = 359;
static float s_table[kTableSize];
static int s_tableMode = -1;
static float s_tableAspect;

static void EnsureTable(int mode, float aspect)
{
    if (mode == s_tableMode && aspect == s_tableAspect) return;
    float prev = 0;
    for (int i = 0; i < kTableSize; ++i) {
        float t = CoverageTan(mode, i * kTableStep, aspect);
        if (t < prev) t = prev;  // keep it monotonic so it can be inverted
        s_table[i] = prev = t;
    }
    s_tableMode = mode;
    s_tableAspect = aspect;
}

static float TableCoverage(float lensFov)
{
    float f = lensFov / kTableStep;
    if (f <= 0) return s_table[0];
    int i = (int)f;
    if (i >= kTableSize - 1) return s_table[kTableSize - 1];
    float a = s_table[i], b = s_table[i + 1];
    if (b >= 1e9f) return b;
    return a + (b - a) * (f - i);
}

static float TableLens(float tanR)
{
    int lo = 0, hi = kTableSize - 1;
    if (s_table[hi] <= tanR) return hi * kTableStep;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (s_table[mid] <= tanR) lo = mid;
        else hi = mid;
    }
    float a = s_table[lo], b = s_table[hi];
    float t = b > a && b < 1e9f ? (tanR - a) / (b - a) : 0.f;
    return (lo + t) * kTableStep;
}

// ---- per-frame state -------------------------------------------------------------------------

static float s_aspect = 16.f / 9.f;
static float s_lensFov;     // lens FOV (true degrees) the latest world FOV call asked for; 0 = none
static float s_renderTan;   // tan(true render half-FOV) that went with it
static float s_snapTan;     // tan(true half-FOV) of the world as drawn this frame (world draw B)
static DWORD s_snapFrame = (DWORD)-1;
static bool s_snapValid;    // the last world frame is what menus show behind them
static float s_warpFov;     // lens FOV of the last warp (true degrees)
static DWORD s_warpFrame = (DWORD)-1;
static const char* s_skip = "";  // why the last frame was not warped

static void UpdateAspect()
{
    int w = 0, h = 0;
    D3DBackBufferSize(&w, &h);
    if (w > 0 && h > 0) s_aspect = (float)w / h;
}

// While scoped with scopelens=0 the scope overlay is not bent: the warp runs before the viewmodel
// pass (which draws the scope model, LensPostImageSpace) instead of before the UI, and that pass
// keeps the game's FOV, so the overlay is drawn flat at its normal size over the lensed world.
static bool ScopeFlat()
{
    return !g_settings.scopeLens && ScopeView();
}

static void RememberPair(float tanR, float lensFov);

// Fisheye full circle: at the widest FOV (the slider's maximum), everything the fill view renders is
// shown as a circle fitted to the screen's shorter side. That fixes a zoom-out factor against the
// fill view (the circle's angle x the shorter side / the fill's angle across the width), which then
// applies at every FOV: a lower FOV setting, aiming or a scope magnify the picture and its circle
// together, so the circle grows past the screen's edges like the fill's black corners do.
static float CircleFit() { return s_aspect > 1.f ? s_aspect : 1.f; }
static bool Circle() { return g_settings.lens == kLensFisheye && g_settings.fisheyeCircle && g_settings.fov > 0; }

// The circle's half angle (radians; 0 if off) and the fill's lens FOV at the widest FOV (degrees).
static float CircleTheta(float* baseFov = nullptr)
{
    if (!Circle()) return 0.f;
    EnsureTable(kLensFisheye, s_aspect);
    float base = 2 * atanf(kRef * s_aspect * tanf(kFovMax * kPi / 360.f)) * 180.f / kPi;
    if (base > kMaxRenderFov) base = kMaxRenderFov;
    float maxTan = tanf(kMaxRenderFov * kPi / 360.f), t = TableCoverage(base);
    if (t > maxTan) t = maxTan;
    if (baseFov) *baseFov = base;
    return atanf(t / CircleFit());
}

float LensRenderFov(float engineFov)
{
    int mode = g_settings.lens;
    s_lensFov = 0;
    if (mode == kLensOff || g_settings.fov <= 0) return engineFov;  // "game default" leaves the image alone
    UpdateAspect();
    float tanTrue = kRef * s_aspect * tanf(engineFov * kPi / 360.f);
    float lensFov = 2 * atanf(tanTrue) * 180.f / kPi;
    if (mode == kLensAuto && lensFov <= 90.f) return engineFov;
    EnsureTable(mode, s_aspect);
    float maxTan = tanf(kMaxRenderFov * kPi / 360.f);
    float tanR = TableCoverage(lensFov);
    if (Circle()) {
        // The fill view's lens FOV, zoomed out by the circle's factor; the render covers what the
        // screen then shows, at most the circle (tan(theta) x aspect across the height).
        float base;
        float theta = CircleTheta(&base), fit = CircleFit();
        float fill = lensFov > kMaxRenderFov ? kMaxRenderFov : lensFov;
        lensFov = fill * (2.f * theta * fit * 180.f / kPi) / base;
        tanR = TableCoverage(lensFov);
        float most = tanf(theta) * fit;
        if (tanR > most) tanR = most;
    } else if (mode == kLensFisheye) {  // up to 160 across; what the corners need beyond one view stays black
        if (lensFov > kMaxRenderFov) lensFov = kMaxRenderFov;
        tanR = TableCoverage(lensFov);
        if (tanR > maxTan) tanR = maxTan;
    } else if (tanR > maxTan) {  // shrink the lens FOV to what one view can cover
        lensFov = TableLens(maxTan);
        tanR = maxTan;
    }
    s_lensFov = lensFov;
    s_renderTan = tanR;
    RememberPair(tanR, lensFov);
    return 2 * atanf(tanR / (kRef * s_aspect)) * 180.f / kPi;
}

static float LensForRender(float tanR);

// Menus over the stored world frame (Pip-Boy, pause menu): that frame is warped with the lens it was
// rendered for (Decide), not the one the slider asks for now, so a viewmodel drawn over it (the
// Pip-Boy) must be widened for that lens too, or a FOV change while the Pip-Boy is up would size
// it for a lens the frame is not warped with until the Pip-Boy is opened again.
static bool ShowsStoredFrame()
{
    typedef bool(__cdecl * IsMenuModeFn)();
    return s_snapValid && FrameCount() - s_snapFrame > 1 && ((IsMenuModeFn)0x00702360)();
}

float LensViewmodelFov(float engineFov)
{
    if (s_lensFov <= 0 || ScopeFlat()) return engineFov;
    float lensFov = s_lensFov, renderTan = s_renderTan;
    if (ShowsStoredFrame() && s_snapTan > 0) {
        EnsureTable(g_settings.lens, s_aspect);
        renderTan = s_snapTan;
        lensFov = LensForRender(renderTan);
        if (g_settings.lens == kLensAuto && lensFov <= 90.5f) return engineFov;  // that frame is not warped
    }
    float c = CentreScale(g_settings.lens, lensFov);
    if (c <= 1e-4f) return engineFov;
    float t = tanf(engineFov * kPi / 360.f) * renderTan / c;
    return 2 * atanf(t) * 180.f / kPi;
}

float LensEffectiveFov()
{
    if (s_warpFov <= 0 || FrameCount() - s_warpFrame > 5) return 0;
    float half = Circle() ? CircleTheta() : s_warpFov * kPi / 360.f;  // the circle's angle
    if (half >= 1.5690f) half = 1.5690f;
    return 2 * atanf(tanf(half) / (kRef * s_aspect)) * 180.f / kPi;  // engine units
}

bool LensStats(float* lensFov, float* renderFov, float* autoBlend, const char** skip)
{
    *skip = s_skip;
    if (s_warpFov <= 0 || FrameCount() - s_warpFrame > 5) return false;
    *lensFov = Circle() ? CircleTheta() * 360.f / kPi : s_warpFov;
    *renderFov = 2 * atanf(s_snapTan) * 180.f / kPi;
    *autoBlend = g_settings.lens == kLensAuto ? AutoBlend(s_warpFov) : 0.f;
    return true;
}

void LensWorldDraw(float frustumRight)
{
    s_snapTan = frustumRight;
    s_snapFrame = FrameCount();
    s_snapValid = true;
}

// ---- warp pass -------------------------------------------------------------------------------

static IDirect3DVertexShader9* s_vs;
static IDirect3DPixelShader9* s_ps;
static IDirect3DPixelShader9* s_cursorPS;
static IDirect3DTexture9* s_copy;
static IDirect3DSurface9* s_copySurface;
static IDirect3DTexture9* s_pre;          // Pip-Boy frames: the frame before the UI
static IDirect3DSurface9* s_preSurface;
static bool s_preValid;  // s_pre holds this frame's pre-UI image (set at pre-UI, used at Present)
static DWORD s_preUsedFrame;  // s_pre exists only while the Pip-Boy uses it (full size: 430 MB at 64x)
static UINT s_copyW, s_copyH;
static D3DFORMAT s_copyFormat;
static bool s_failed;

static void ReleasePre()
{
    if (s_preSurface) { s_preSurface->Release(); s_preSurface = nullptr; }
    if (s_pre) { s_pre->Release(); s_pre = nullptr; }
    s_preValid = false;
}

void LensLost()
{
    if (s_copySurface) { s_copySurface->Release(); s_copySurface = nullptr; }
    if (s_copy) { s_copy->Release(); s_copy = nullptr; }
    ReleasePre();
}

static bool Prepare(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d)
{
    if (!s_vs && FAILED(dev->CreateVertexShader((const DWORD*)g_lensVS, &s_vs))) return false;
    if (!s_ps && FAILED(dev->CreatePixelShader((const DWORD*)g_lensPS, &s_ps))) return false;
    if (!s_cursorPS && FAILED(dev->CreatePixelShader((const DWORD*)g_cursorPS, &s_cursorPS))) return false;
    if (s_copy && s_copyW == d.Width && s_copyH == d.Height && s_copyFormat == d.Format) return true;
    LensLost();
    if (FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &s_copy, nullptr)))
        return false;
    s_copy->GetSurfaceLevel(0, &s_copySurface);
    s_copyW = d.Width;
    s_copyH = d.Height;
    s_copyFormat = d.Format;
    Log("lens: copy texture %ux%u fmt %d", d.Width, d.Height, d.Format);
    return true;
}

// The Pip-Boy's pre-UI copy (same size and format as the copy texture), created when first needed.
static bool PreparePre(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d)
{
    if (!Prepare(dev, d)) return false;
    s_preUsedFrame = FrameCount();
    if (s_pre) return true;
    if (FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &s_pre, nullptr)))
        return false;
    s_pre->GetSurfaceLevel(0, &s_preSurface);
    return true;
}

// The lens FOV each recent render FOV was asked for: with the fisheye several lens FOVs share the
// capped render FOV, so the table cannot be inverted there.
struct LensPair { float tanR, lensFov; int mode; };
static LensPair s_pairs[16];
static int s_pairNext;

static void RememberPair(float tanR, float lensFov)
{
    const LensPair& last = s_pairs[(s_pairNext + 15) % 16];
    if (last.tanR == tanR && last.lensFov == lensFov && last.mode == g_settings.lens) return;
    s_pairs[s_pairNext] = {tanR, lensFov, g_settings.lens};
    s_pairNext = (s_pairNext + 1) % 16;
}

// The lens FOV a frame rendered with tan(half FOV) = tanR shows: the most recently asked one with
// that render FOV (the engine's frustum differs from the asked tangent by rounding).
static float LensForRender(float tanR)
{
    for (int k = 1; k <= 16; ++k) {
        const LensPair& p = s_pairs[(s_pairNext - k + 16) % 16];
        if (p.lensFov > 0 && p.mode == g_settings.lens && fabsf(p.tanR - tanR) <= 2e-3f * tanR) return p.lensFov;
    }
    return TableLens(tanR);
}

// Whether this frame is warped, and with which lens FOV (true degrees); 0 = not warped.
static float Decide()
{
    int mode = g_settings.lens;
    if (mode == kLensOff) { s_skip = "lens off"; return 0; }
    if (g_settings.fov <= 0) { s_skip = "FOV at game default"; return 0; }
    if (FlatMenuView()) { s_skip = "terminal or casino game open"; return 0; }
    if (((BYTE*)0x011F308F)[1007]) s_snapValid = false;  // loading screen
    typedef bool(__cdecl * IsMenuModeFn)();
    bool stored = s_snapFrame != FrameCount();  // the world was not drawn: a menu shows the stored frame
    if (stored && !(s_snapValid && ((IsMenuModeFn)0x00702360)())) { s_skip = "world not drawn this frame"; return 0; }
    if (s_failed) { s_skip = "resources failed"; return 0; }
    UpdateAspect();
    EnsureTable(mode, s_aspect);
    float lensFov = LensForRender(s_snapTan);
    if (mode == kLensAuto && lensFov <= 90.5f) { s_skip = "rectilinear (auto, <= 90)"; return 0; }
    return lensFov;
}

// Resamples `rt` into the lens projection. ownScene: called outside the engine's BeginScene.
struct HoleRect { float x0, y0, x1, y1; };  // source pixels

static void Warp(IDirect3DDevice9* dev, IDirect3DSurface9* rt, float lensFov, bool ownScene, const HoleRect* hole = nullptr)
{
    int mode = g_settings.lens;
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE || !Prepare(dev, d)) {
        if (d.MultiSampleType == D3DMULTISAMPLE_NONE) {
            s_failed = true;
            Log("lens: resource creation failed");
        }
        s_skip = "unsupported target";
        return;
    }
    dev->StretchRect(rt, nullptr, s_copySurface, nullptr, D3DTEXF_NONE);

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
    IDirect3DSurface9 *oldRT = nullptr, *ds = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&ds);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.f, 1.f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(s_vs);
    dev->SetPixelShader(s_ps);
    dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    float half = lensFov * kPi / 360.f;
    float c0[4] = {half, s_snapTan, s_aspect, AutoBlend(lensFov)};
    float c1[4] = {(float)mode, CircleTheta(), 0, 0};
    float c2[4] = {-1.f / d.Width, 1.f / d.Height, 0, 0};
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetVertexShaderConstantF(2, c2, 1);
    float c3[4] = {1, 1, 0, 0};  // empty
    if (hole) {
        c3[0] = hole->x0 / d.Width;
        c3[1] = hole->y0 / d.Height;
        c3[2] = hole->x1 / d.Width;
        c3[3] = hole->y1 / d.Height;
    }
    dev->SetPixelShaderConstantF(3, c3, 1);
    dev->SetTexture(0, s_copy);
    dev->SetTexture(1, hole ? s_pre : nullptr);
    for (DWORD i = 1; i < 2; ++i) {
        dev->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(i, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    struct V { float x, y, z, u, v; };
    const V quad[4] = {{-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}};
    if (!ownScene || SUCCEEDED(dev->BeginScene())) {
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        if (ownScene) dev->EndScene();
    }
    dev->SetTexture(0, nullptr);
    dev->SetTexture(1, nullptr);

    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(ds);
    sb->Apply();
    sb->Release();
    if (oldRT) oldRT->Release();
    if (ds) ds->Release();
    s_warpFov = lensFov;
    s_warpFrame = FrameCount();
    s_skip = "";
}

// With the Pip-Boy up its cursor is a 2D element over the 3D Pip-Boy screen: warping before the
// UI would show the screen somewhere else than where the game hit-tests the cursor. Those frames
// are warped after the UI instead (LensPresent), so the cursor is warped together with the
// screen it points at.
static float s_deferredFov;   // lens FOV to warp at Present, 0 = none
static DWORD s_postFrame = (DWORD)-1000;  // last frame warped after the UI
static int s_postW, s_postH;

// The Pip-Boy is fully open (InterfaceManager pipBoyMode 3) and its screen is what the cursor
// works on: every visible menu is one of the Pip-Boy's own screens (Inventory 1002, Stats 1003,
// Map 1023, Repair 1035, Item Mod 1061), the HUD (1004) or one of the always-visible helper menus
// (1086 and up). Any other menu (pause menu, message box, "how many?", tutorial tooltip, ...) is
// flat and hit-tested where it is drawn, so then the frame is warped before the UI as usual and
// the cursor needs no mapping. Menu ids from JIP LN.
// Plugins can add menus that stay visible all the time like the HUD (Tale of Two Wastelands lists
// show 1085 everywhere). Menus seen visible during gameplay (no menu mode) are noted here and do
// not count as being over the Pip-Boy. The message box, pause menu and "how many?" box never do.
static bool s_alwaysVisible[1086];

static void NoteAlwaysVisibleMenus()
{
    typedef bool(__cdecl * IsMenuModeFn)();
    if (((IsMenuModeFn)0x00702360)()) return;
    const BYTE* visible = (const BYTE*)0x011F308F;
    for (int id = 1001; id < 1086; ++id) {
        if (!visible[id] || s_alwaysVisible[id] || id == 1001 || id == 1013 || id == 1016) continue;
        s_alwaysVisible[id] = true;
        if (id != 1004) Log("lens: menu %d is visible during gameplay; not counted as over the Pip-Boy", id);
    }
}

bool PipboyScreenActive()
{
    BYTE* im = *(BYTE**)0x011D8A80;
    if (!im || *(DWORD*)(im + 0x4BC) != 3) return false;
    const BYTE* visible = (const BYTE*)0x011F308F;
    for (int id = 1001; id < 1086; ++id) {
        if (!visible[id] || s_alwaysVisible[id]) continue;
        if (id == 1002 || id == 1003 || id == 1004 || id == 1023 || id == 1035 || id == 1061) continue;
        return false;
    }
    return true;
}

static DWORD s_earlyFrame = (DWORD)-1;  // FrameCount() of a warp before the viewmodel pass

void LensPostImageSpace(IDirect3DDevice9* dev)
{
    if (!ScopeFlat()) return;
    float lensFov = Decide();
    if (lensFov <= 0) return;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) { s_skip = "no render target"; return; }
    Warp(dev, rt, lensFov, false);
    rt->Release();
    s_earlyFrame = FrameCount();
}

// The Pip-Boy's frame before the UI, from render target 0: where the cursor is cut out, and what
// tells cursor pixels apart.
static void CapturePre(IDirect3DDevice9* dev)
{
    s_preValid = false;
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) {
        D3DSURFACE_DESC d;
        rt->GetDesc(&d);
        if (d.MultiSampleType == D3DMULTISAMPLE_NONE && PreparePre(dev, d) &&
            SUCCEEDED(dev->StretchRect(rt, nullptr, s_preSurface, nullptr, D3DTEXF_NONE)))
            s_preValid = true;
        rt->Release();
    }
}

// After the pixelated look moved the interface to the window's resolution: the Pip-Boy's copy is
// taken again there (the warp after the UI works on that frame).
void LensPreUIRetarget(IDirect3DDevice9* dev)
{
    if (s_deferredFov > 0 && SupersampleSplitThisFrame()) CapturePre(dev);
}

void LensPreUI(IDirect3DDevice9* dev)
{
    s_deferredFov = 0;
    NoteAlwaysVisibleMenus();
    if (s_pre && FrameCount() - s_preUsedFrame > 300) ReleasePre();  // the Pip-Boy has been closed a while
    if (s_earlyFrame == FrameCount()) return;  // warped before the scope overlay (LensPostImageSpace)
    float lensFov = Decide();
    if (lensFov <= 0) return;
    if (PipboyScreenActive()) {
        s_deferredFov = lensFov;
        CapturePre(dev);
        return;
    }
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) { s_skip = "no render target"; return; }
    Warp(dev, rt, lensFov, false);
    rt->Release();
}

// The warp's mapping (as in the shader): output pixel -> source pixel, for a w x h frame.
static bool MapToSource(float lensFov, int w, int h, float x, float y, float* sx, float* sy)
{
    float half = lensFov * kPi / 360.f;
    float u = x / w * 2 - 1, v = (1 - y / h * 2) / s_aspect;
    V3 r = Ray(g_settings.lens, u, v, half, AutoBlend(lensFov));
    if (r.z <= 1e-4f) return false;
    if (Circle() && sqrtf(u * u + v * v) * half > CircleTheta()) return false;
    float rx = r.x / r.z / s_snapTan, ry = r.y / r.z / (s_snapTan / s_aspect);
    *sx = (0.5f + 0.5f * rx) * w;
    *sy = (0.5f - 0.5f * ry) * h;
    return true;
}

// The inverse: where source pixel (qx, qy) is shown (Newton's method on MapToSource).
static bool MapToOutput(float lensFov, int w, int h, float qx, float qy, float* px, float* py)
{
    float x = qx, y = qy;
    // Start from the centre-scaled guess: the warp scales the centre by tanR / C.
    float c = CentreScale(g_settings.lens, lensFov);
    if (c > 1e-4f) {
        float k = c / s_snapTan;  // source offset per output offset at the centre
        x = w * 0.5f + (qx - w * 0.5f) / k;
        y = h * 0.5f + (qy - h * 0.5f) / k;
    }
    for (int i = 0; i < 12; ++i) {
        float fx, fy, ax, ay, bx, by;
        if (!MapToSource(lensFov, w, h, x, y, &fx, &fy) || !MapToSource(lensFov, w, h, x + 1, y, &ax, &ay) ||
            !MapToSource(lensFov, w, h, x, y + 1, &bx, &by))
            return false;
        float ex = fx - qx, ey = fy - qy;
        if (fabsf(ex) < 0.05f && fabsf(ey) < 0.05f) break;
        float j00 = ax - fx, j01 = bx - fx, j10 = ay - fy, j11 = by - fy;  // d source / d output
        float det = j00 * j11 - j01 * j10;
        if (fabsf(det) < 1e-6f) return false;
        x -= (j11 * ex - j01 * ey) / det;
        y -= (-j10 * ex + j00 * ey) / det;
    }
    *px = x;
    *py = y;
    return true;
}

// Draws the cursor's pixels (src rect of the frame with the UI, where it differs from the frame
// before the UI) 1:1 at dst, outside the engine's scene.
static void DrawCursor(IDirect3DDevice9* dev, IDirect3DSurface9* rt, const HoleRect& src, float dx, float dy)
{
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
    IDirect3DSurface9 *oldRT = nullptr, *ds = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&ds);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.f, 1.f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(s_vs);
    dev->SetPixelShader(s_cursorPS);
    dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    float c2[4] = {-1.f / d.Width, 1.f / d.Height, 0, 0};
    dev->SetVertexShaderConstantF(2, c2, 1);
    dev->SetTexture(0, s_copy);
    dev->SetTexture(1, s_pre);
    for (DWORD i = 0; i < 2; ++i) {
        dev->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(i, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    float W = (float)d.Width, H = (float)d.Height;
    float x0 = (src.x0 + dx) / W * 2 - 1, x1 = (src.x1 + dx) / W * 2 - 1;
    float y0 = 1 - (src.y0 + dy) / H * 2, y1 = 1 - (src.y1 + dy) / H * 2;
    float u0 = src.x0 / W, u1 = src.x1 / W, v0 = src.y0 / H, v1 = src.y1 / H;
    struct V { float x, y, z, u, v; };
    const V quad[4] = {{x0, y0, 0, u0, v0}, {x1, y0, 0, u1, v0}, {x0, y1, 0, u0, v1}, {x1, y1, 0, u1, v1}};
    if (SUCCEEDED(dev->BeginScene())) {
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        dev->EndScene();
    }
    dev->SetTexture(0, nullptr);
    dev->SetTexture(1, nullptr);
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(ds);
    sb->Apply();
    sb->Release();
    if (oldRT) oldRT->Release();
    if (ds) ds->Release();
}

static bool s_cursorDrawn;
static float s_cursorOutX, s_cursorOutY;

void LensPresent(IDirect3DDevice9* dev, IDirect3DSurface9* frame)
{
    s_cursorDrawn = false;
    bool preValid = s_preValid;
    s_preValid = false;
    if (s_deferredFov <= 0 || !frame) return;
    D3DSURFACE_DESC d;
    frame->GetDesc(&d);
    // Frame pixels per render pixel: 1, except for the pixelated look's interface at the window's
    // resolution (the cursor's position and the mapping for the mouse stay in render pixels).
    int rw = 0, rh = 0;
    D3DBackBufferSize(&rw, &rh);
    float kx = rw > 0 ? (float)d.Width / rw : 1.f, ky = rh > 0 ? (float)d.Height / rh : 1.f;
    // The cursor's area around its hit point (the cursor image extends right and down from it).
    float qx, qy;
    HoleRect hole;
    // (FrameCount advances at the start of Present, so a flag ties the pre-UI copy to this frame.)
    bool cut = preValid && s_pre && s_copyW == d.Width && s_copyH == d.Height && DinputGameCursor(&qx, &qy);
    if (cut) {
        qx *= kx;
        qy *= ky;
    }
    // Where the game draws its cursor: at its position.
    float dx = qx, dy = qy;
    if (cut) {
        float size = d.Height * 0.07f, pad = d.Height * 0.01f;
        hole = {dx - pad, dy - pad, dx + size, dy + size};
    }
    Warp(dev, frame, s_deferredFov, true, cut ? &hole : nullptr);
    float px, py;
    if (cut && MapToOutput(s_deferredFov, d.Width, d.Height, qx, qy, &px, &py)) {
        DrawCursor(dev, frame, hole, px - dx, py - dy);
        s_cursorDrawn = true;
        s_cursorOutX = px / kx;
        s_cursorOutY = py / ky;
    }
    s_postW = (int)(d.Width / kx + 0.5f);
    s_postH = (int)(d.Height / ky + 0.5f);
    s_postFrame = FrameCount();
    s_deferredFov = 0;
}

// Where the game's cursor must be so that it is shown at render pixel (x, y) of a frame warped
// after the UI (the shader's mapping). False if no such frame.
bool LensMapToRender(float* x, float* y)
{
    // Only for this frame: as soon as frames are warped before the UI again, the cursor maps 1:1.
    if (s_postFrame != FrameCount() || s_postW <= 0 || s_postH <= 0 || s_warpFov <= 0) return false;
    return MapToSource(s_warpFov, s_postW, s_postH, *x, *y, x, y);
}

// While the Pip-Boy is warped after the UI the cursor is shown where its hit point lands, and the
// lens magnifies the centre (about 3x at FOV 135 with the stereographic lens): mouse moves would
// carry the shown cursor that much further than in flat menus. Render pixels per shown pixel at
// the shown cursor, for scaling the mouse's motion; false when the cursor is not drawn that way.
bool LensCursorScale(float* kx, float* ky)
{
    if (!s_cursorDrawn || FrameCount() - s_postFrame > 2 || s_postW <= 0 || s_postH <= 0 || s_warpFov <= 0) return false;
    float x = s_cursorOutX, y = s_cursorOutY, x0, y0, x1, y1, x2, y2;
    if (!MapToSource(s_warpFov, s_postW, s_postH, x, y, &x0, &y0) || !MapToSource(s_warpFov, s_postW, s_postH, x + 1, y, &x1, &y1) ||
        !MapToSource(s_warpFov, s_postW, s_postH, x, y + 1, &x2, &y2))
        return false;
    *kx = x1 - x0;
    *ky = y2 - y0;
    return *kx > 0.05f && *ky > 0.05f;
}

// Where the game's cursor was last shown (render pixels) while the Pip-Boy is warped after the UI.
bool LensCursorShown(float* x, float* y)
{
    if (!s_cursorDrawn || FrameCount() - s_postFrame > 2) return false;
    *x = s_cursorOutX;
    *y = s_cursorOutY;
    return true;
}

void LensWriteState(FILE* f)
{
    float kx, ky;
    if (LensCursorScale(&kx, &ky)) fprintf(f, "lens: Pip-Boy mouse scale %.3f %.3f\n", kx, ky);
    fprintf(f, "lens: mode %d, asked lens %.2f (render tan %.4f), frame drawn with tan %.4f (frame %lu), last warp %.2f at frame %lu%s%s\n",
            g_settings.lens, s_lensFov, s_renderTan, s_snapTan, s_snapFrame, s_warpFov, s_warpFrame,
            s_warpFrame == FrameCount() ? " (this frame)" : "", s_skip[0] ? ", skipped: " : "");
    if (s_skip[0]) fprintf(f, "lens skip reason: %s\n", s_skip);
    if (s_cursorDrawn) fprintf(f, "lens: Pip-Boy cursor drawn unwarped at %.1f %.1f\n", s_cursorOutX, s_cursorOutY);
}
