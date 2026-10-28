// Interface at aspect ratios narrower than 4:3 (5:4, 1:1, portrait).
//
// The game lays out its interface in a virtual screen from InterfaceManager::GetScreenWidth /
// GetScreenHeight (0x00715D40 / 0x00715DA0): wider than tall: 960 high and 960 * aspect wide;
// square or taller: 1280 wide and 1280 / aspect high. Its menus, HUD, console and Pip-Boy screen
// need at least 1280 x 960, and its interface camera (set up at 0x00713031) is always 960 high,
// so below 4:3 parts of the interface fall off the screen or are drawn in the wrong place (even
// at 1:1, the game's own tall case).
//
// Below 4:3 the mod letterboxes the interface instead: the layout size is exactly 4:3
// (1280 x 960, the size every menu works with), and the camera shows 1280 x (1280 / aspect)
// around it, so the 4:3 interface sits in the middle of the window with free bands above and
// below. The camera keeps its 4:3 position and the frustum grows by the same amount up and down:
// the Pip-Boy screen is rendered with the same camera and a temporary 1280 x 960 frustum, which
// then lands where it does on a 4:3 screen. The game's "widescreen" flag (0x011C70EB, set when
// the aspect is anything but 4:3) is cleared while letterboxed: it picks the widescreen menu
// layouts (&widescreen;), safe zones, and the console's horizontal stretch and wrap width (x1.333,
// which puts the console off the left edge below 4:3). Clipped tiles (scrolling text such as the
// tutorial box) get a scissor rectangle in pixels from Tile::Update (0x00A04640), which scales y
// by iSize H / 960 from the window's top; below 4:3 it uses the band's scale and offset instead.
// 4:3 and wider is unchanged.
//
// The menu cursor lives in interface units (InterfaceManager +0x2C / +0x34); its pixel position
// (+0x38 / +0x40), which the hit test turns back into interface units through the interface
// camera, is derived with GetScreenHeight (960). Below 4:3 that uses the camera's height instead,
// so the pixel position is where the cursor is drawn and the hit test agrees with it.
//
// The HUD and VATS are the exception to the 4:3 band: their setup (0x0076BFE0, 0x00770430, VATS
// 0x007E9200 / 0x007EC810) puts elements at
// 2 * safe zone Y from the top or bottom of the layout; below 4:3 its safe zone Y is reduced by half
// the band above the layout, so they sit at the window's top and bottom edges instead.
//
// Full-screen pictures are scaled up to cover the window instead of the band (UiFrame): the pause
// menu's vignette (StartMenu's pause_background, a NIF, stretched vertically), the loading screen's
// images (scaled up about the centre, cropping the sides) and the scope overlay (the world is not
// clipped to the scope's sides, ScopeScissorAspect).
//
// On the Pip-Boy (0x007F8720) the game casts a ray through the 3D camera at the cursor's pixel
// position: the three buttons are picked with it directly, and a hit on the screen gives a texture
// coordinate (u, v) that becomes a pixel point (u * H * 4/3, v * H) for the menu pick through the
// interface camera (InterfaceManager +0x4AC / +0x4B0). Below 4:3 that point is (u * W, band top
// + v * band height), where the letterboxed camera shows the 1280 x 960 layout.
#include <windows.h>
#include "fnvfov.h"

typedef int(__cdecl* SizeFn)();
static const SizeFn GetSizeW = (SizeFn)0x004DC1F0;  // iSize W (what the engine renders at)
static const SizeFn GetSizeH = (SizeFn)0x004DC200;  // iSize H

static float s_lastW, s_lastH;

static bool Narrow(float* w, float* h)
{
    *w = (float)GetSizeW();
    *h = (float)GetSizeH();
    return *w > 0 && *h > 0 && *w * 3 < *h * 4;
}

// The layout size (what GetScreenWidth / GetScreenHeight return).
static void UiSize(float* uw, float* uh)
{
    float w, h;
    if (Narrow(&w, &h)) {
        *uw = 1280.f;
        *uh = 960.f;
    } else if (w > 0 && h > 0) {
        *uw = w / h * 960.f;
        *uh = 960.f;
    } else {
        *uw = 1280.f;
        *uh = 960.f;
    }
    s_lastW = *uw;
    s_lastH = *uh;
}

static float __cdecl Hook_GetScreenWidth()
{
    float w, h;
    UiSize(&w, &h);
    return w;
}

static float __cdecl Hook_GetScreenHeight()
{
    float w, h;
    UiSize(&w, &h);
    return h;
}

// Called in place of the frustum setup at 0x00713031: writes right / top of the camera frustum
// being built (the engine function's locals [ebp-24h] / [ebp-20h]); 0x00713049 writes its bottom
// ([ebp-1Ch]), 0x0071306F the camera's vertical position (-top / 2 in the game; -480, as on a 4:3
// screen, when letterboxed).
static float s_camW, s_camH;
static float s_frustumTop = 960, s_frustumBottom = 0;
static float s_cameraZ = -480.f;

// interfaceManager: the object being set up ([ebp-0B4h], in eax at the patched code). Its +0x4D0
// (the screen's aspect relative to 4:3, computed just before) is read by other interface code;
// with the 4:3 layout it reads 1, as on a 4:3 screen.
static void __cdecl CameraSize(BYTE* interfaceManager)
{
    float w, h;
    if (Narrow(&w, &h)) {
        s_camW = 1280.f;
        s_camH = 1280.f * h / w;
        float extra = (s_camH - 960.f) * 0.5f;
        s_frustumTop = 960.f + extra;
        s_frustumBottom = -extra;
        s_cameraZ = -480.f;
        if (interfaceManager && !IsBadWritePtr(interfaceManager + 0x4D0, 4)) {
            Log("ui: interface aspect factor %.4f set to 1 (4:3 layout)", *(float*)(interfaceManager + 0x4D0));
            *(float*)(interfaceManager + 0x4D0) = 1.f;
        }
    } else {
        UiSize(&s_camW, &s_camH);
        s_frustumTop = s_camH;
        s_frustumBottom = 0;
        s_cameraZ = -s_camH * 0.5f;
    }
    Log("ui: interface camera %.1f x %.1f", s_camW, s_camH);
}

static __declspec(naked) void CameraFrustum()
{
    __asm {
        pushad
        push eax
        call CameraSize
        add esp, 4
        popad
        push eax
        mov eax, s_camW
        mov [ebp - 0x24], eax
        mov eax, s_frustumTop
        mov [ebp - 0x20], eax
        pop eax
        ret
    }
}

static __declspec(naked) void FrustumBottom()
{
    __asm {
        push eax
        mov eax, s_frustumBottom
        mov [ebp - 0x1C], eax
        pop eax
        ret
    }
}

// Replaces fld [ebp-20h]; fchs; fmul [0.5]: leaves the camera's vertical position on the FPU stack.
static __declspec(naked) void CameraZ()
{
    __asm {
        fld dword ptr [s_cameraZ]
        ret
    }
}

// Replaces mov byte ptr [0x011C70EB], 1 where the renderer sets the widescreen flag (at start
// and on a resolution change; iSize is current there).
static void __cdecl SetWidescreenFlag()
{
    float w, h;
    bool narrow = Narrow(&w, &h);
    *(BYTE*)0x011C70EB = narrow ? 0 : 1;
    static int s_logged;
    if (s_logged++ < 4) Log("ui: widescreen flag %d (%.0f x %.0f%s)", narrow ? 0 : 1, w, h, narrow ? ", letterboxed 4:3" : "");
}

static __declspec(naked) void WidescreenFlag()
{
    __asm {
        pushad
        call SetWidescreenFlag
        popad
        ret
    }
}

// Tile::Update's scissor rectangle (two copies, 0x00A063C6 and 0x00A065C3): pixels = interface
// units * (iSize / GetScreenWidth|Height), against the screen or, for tiles on the Pip-Boy screen,
// the Pip-Boy texture (1280 x 960 units there, left alone).
// The game's own check (0x00A03C60: the tile's menu, 0x00A03C90, byte +0x1C) is done here directly:
// HDPipboy ("High Resolution Screens") makes 0x00A03C60 always return false, which would put the
// Pip-Boy's clip rectangles in the letterbox band below 4:3 and crop its screens.
typedef BYTE*(__thiscall* TileMenuFn)(void*);
static bool TileOnPipboy(void* tile)
{
    BYTE* menu = ((TileMenuFn)0x00A03C90)(tile);
    return menu && menu[0x1C] != 0;
}
typedef float(__thiscall* TileYFn)(void*);
static TileYFn s_tileY;  // 0x00A01440: the tile's y on the screen, in interface units

static bool LetterboxedTile(void* tile, float* w, float* h)
{
    return Narrow(w, h) && tile && !TileOnPipboy(tile);
}

// Replaces the call to GetScreenHeight (0x00706DF0) for the vertical scale: the camera's height
// (1280 / aspect) instead of 960, so y uses the same pixels per unit as x.
static float __cdecl ClipScreenHeight(void* tile)
{
    float w, h;
    if (LetterboxedTile(tile, &w, &h)) return 1280.f * h / w;
    return ((float(__cdecl*)())0x00706DF0)();
}

static __declspec(naked) void ClipScreenHeightStub()
{
    __asm {
        push dword ptr [ebp - 0x3E8]  // the tile being updated
        call ClipScreenHeight
        add esp, 4
        ret
    }
}

// Tile y for the rectangle: plus the band above the 4:3 interface (in interface units).
static float __fastcall ClipTileY(void* tile, void*)
{
    float y = s_tileY(tile);
    float w, h;
    if (LetterboxedTile(tile, &w, &h)) y += (1280.f * h / w - 960.f) * 0.5f;
    return y;
}

// Replaces the call to GetScreenHeight at 0x00711CF8 (InterfaceManager cursor update: pixel y =
// iSize H / 2 - y * iSize H / GetScreenHeight).
static float __cdecl CursorScreenHeight()
{
    float w, h;
    if (Narrow(&w, &h)) return 1280.f * h / w;
    return Hook_GetScreenHeight();
}

// The menu pick (0x007126C0) casts a ray through a camera at the cursor's pixel position
// (NiCamera::WindowPointToRay, 0x00A71080, called at 0x00712926). Recorded for the state file.
typedef bool(__thiscall* RayFn)(void*, int, int, void*, void*, int);
static RayFn s_windowPointToRay;
static struct { BYTE* cam; int x, y; float frustum[4], viewport[4]; DWORD frame; } s_pick;

static bool __fastcall PickRay(BYTE* cam, void*, int x, int y, void* origin, void* dir, int a5)
{
    s_pick.cam = cam;
    s_pick.x = x;
    s_pick.y = y;
    if (cam && !IsBadReadPtr(cam + 0xDC, 0x34)) {
        memcpy(s_pick.frustum, cam + 0xDC, 16);   // left, right, top, bottom
        memcpy(s_pick.viewport, cam + 0x100, 16); // left, right, top, bottom
    }
    s_pick.frame = FrameCount();
    return s_windowPointToRay(cam, x, y, origin, dir, a5);
}

// Replaces 0x007F88E3-0x007F890D: the Pip-Boy screen's pick point from the texture coordinate
// (locals u [ebp-7Ch], v [ebp-78h]; output pointers [ebp+10h], [ebp+14h]).
static void __cdecl PipboyPickPoint(BYTE* frame)
{
    float u = *(float*)(frame - 0x7C), v = *(float*)(frame - 0x78);
    float* outX = *(float**)(frame + 0x10);
    float* outY = *(float**)(frame + 0x14);
    float w, h;
    bool narrow = Narrow(&w, &h);
    float scaleX = h * (float)*(double*)0x01078128;  // H * 4/3
    *(float*)(frame - 0x70) = h;
    *(float*)(frame - 0x74) = scaleX;
    if (narrow) {
        float band = w * 0.75f;
        *outX = u * w;
        *outY = (h - band) * 0.5f + v * band;
    } else {
        *outX = u * scaleX;
        *outY = v * h;
    }
}

static __declspec(naked) void PipboyPickPointStub()
{
    __asm {
        pushad
        push ebp
        call PipboyPickPoint
        add esp, 4
        popad
        ret
    }
}

// Replaces calls to the safe zone Y getter (0x00717820, iSafeZoneY / iSafeZoneYWide) in the HUD.
static float __cdecl HudSafeZoneY()
{
    float y = ((float(__cdecl*)())0x00717820)();
    float w, h;
    if (Narrow(&w, &h)) y -= (1280.f * h / w - 960.f) * 0.25f;
    return y;
}

static float __cdecl ScopeScissorAspect();

void UiInstall()
{
    // push ebp; mov ebp, esp; sub esp, 14h; call GetSizeW (0x004DC1F0)
    static const BYTE kWidth[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0xE8, 0xA5, 0x64, 0xDC, 0xFF};
    static const BYTE kHeight[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0xE8, 0x45, 0x64, 0xDC, 0xFF};
    // The originals are never called (the hooks replace them), so the copied call needs no fixup.
    InstallDetour(0x00715D40, kWidth, sizeof(kWidth), (void*)Hook_GetScreenWidth, "InterfaceManager::GetScreenWidth");
    InstallDetour(0x00715DA0, kHeight, sizeof(kHeight), (void*)Hook_GetScreenHeight, "InterfaceManager::GetScreenHeight");
    // fld [eax+4D0h]; fmul [1280.0]; fstp [ebp-24h]; fld [960.0]; fstp [ebp-20h]
    static const BYTE kFrustum[] = {0xD9, 0x80, 0xD0, 0x04, 0x00, 0x00, 0xDC, 0x0D, 0x60, 0xE9, 0x06, 0x01,
                                    0xD9, 0x5D, 0xDC, 0xD9, 0x05, 0xDC, 0xF2, 0x06, 0x01, 0xD9, 0x5D, 0xE0};
    if (PatchCallOver(0x00713031, kFrustum, sizeof(kFrustum), (void*)CameraFrustum, "interface camera frustum")) {
        static const BYTE kBottom[] = {0xD9, 0xEE, 0xD9, 0x5D, 0xE4};  // fldz; fstp [ebp-1Ch]
        PatchCallOver(0x00713049, kBottom, sizeof(kBottom), (void*)FrustumBottom, "interface camera frustum bottom");
        // fld [ebp-20h]; fchs; fmul [0.5]
        static const BYTE kZ[] = {0xD9, 0x45, 0xE0, 0xD9, 0xE0, 0xDC, 0x0D, 0x88, 0x15, 0x01, 0x01};
        PatchCallOver(0x0071306F, kZ, sizeof(kZ), (void*)CameraZ, "interface camera position");
    }
    static const BYTE kFlag[] = {0xC6, 0x05, 0xEB, 0x70, 0x1C, 0x01, 0x01};  // mov byte ptr [0x011C70EB], 1
    PatchCallOver(0x004DB550, kFlag, sizeof(kFlag), (void*)WidescreenFlag, "widescreen flag (start)");
    PatchCallOver(0x004DC3DC, kFlag, sizeof(kFlag), (void*)WidescreenFlag, "widescreen flag (resolution change)");
    PatchCall(0x00A06415, 0x00706DF0, (void*)ClipScreenHeightStub, "tile scissor height (1)");
    PatchCall(0x00A06612, 0x00706DF0, (void*)ClipScreenHeightStub, "tile scissor height (2)");
    PatchCall(0x00870D3C, 0x00706E70, (void*)ScopeScissorAspect, "scope scissor width");
    PatchCall(0x0076C11E, 0x00717820, (void*)HudSafeZoneY, "HUD safe zone Y (setup)");
    PatchCall(0x00770623, 0x00717820, (void*)HudSafeZoneY, "HUD safe zone Y (layout)");
    // VATS has its own HP / AP / enemy health bars, placed the same way (0x007E9200; 0x007EC810
    // keeps the values for later).
    PatchCall(0x007EA107, 0x00717820, (void*)HudSafeZoneY, "VATS safe zone Y (setup)");
    PatchCall(0x007ED566, 0x00717820, (void*)HudSafeZoneY, "VATS safe zone Y (cached)");
    static const BYTE kPipboyPoint[] = {0xE8, 0x38, 0xE5, 0xF0, 0xFF, 0xD9, 0x5D, 0x90, 0xD9, 0x45, 0x90, 0xDC, 0x0D, 0x28,
                                        0x81, 0x07, 0x01, 0xD9, 0x5D, 0x8C, 0xD9, 0x45, 0x84, 0xD8, 0x4D, 0x8C, 0x8B, 0x4D,
                                        0x10, 0xD9, 0x19, 0xD9, 0x45, 0x88, 0xD8, 0x4D, 0x90, 0x8B, 0x55, 0x14, 0xD9, 0x1A};
    PatchCallOver(0x007F88E3, kPipboyPoint, sizeof(kPipboyPoint), (void*)PipboyPickPointStub, "Pip-Boy screen pick point");
    s_windowPointToRay = (RayFn)PatchCall(0x00712926, 0x00A71080, (void*)PickRay, "menu pick ray (state only)");
    PatchCall(0x00711CF8, 0x00715DA0, (void*)CursorScreenHeight, "menu cursor pixel position");
    s_tileY = (TileYFn)PatchCall(0x00A06469, 0x00A01440, (void*)ClipTileY, "tile scissor y (1)");
    if (s_tileY) PatchCall(0x00A06666, 0x00A01440, (void*)ClipTileY, "tile scissor y (2)");
}

// A tile's name (Tile +0x20: String {char* data; UInt16 len; ...}), or "" when unreadable.
static const char* TileName(BYTE* tile)
{
    if (!tile || IsBadReadPtr(tile, 0x2C)) return "";
    const char* name = *(const char**)(tile + 0x20);
    return name && !IsBadStringPtrA(name, 64) ? name : "";
}

// Scene nodes (NiAVObject): name +0x08, local rotate +0x34 (3x3), translate +0x58, scale +0x64;
// NiNode children: NiTArray at +0x9C (data +0xA0, capacity +0xA4, size +0xA8, words).
static BYTE** Children(BYTE* node, WORD* count)
{
    *count = 0;
    if (!node || IsBadReadPtr(node, 0xAC)) return nullptr;
    BYTE** data = *(BYTE***)(node + 0xA0);
    WORD capacity = *(WORD*)(node + 0xA4), size = *(WORD*)(node + 0xA8);
    // Geometry (not a NiNode) has other fields there.
    if (!data || size > capacity || capacity > 1024 || IsBadReadPtr(data, capacity * sizeof(BYTE*))) return nullptr;
    *count = size;
    return data;
}

static BYTE* FindNode(BYTE* node, const char* name, int depth)
{
    if (!node || IsBadReadPtr(node, 0xAC)) return nullptr;
    const char* n = *(const char**)(node + 0x08);
    if (n && !IsBadStringPtrA(n, 64) && lstrcmpiA(n, name) == 0) return node;
    if (depth <= 0) return nullptr;
    WORD count;
    BYTE** children = Children(node, &count);
    for (WORD i = 0; i < count; ++i)
        if (BYTE* found = FindNode(children[i], name, depth - 1)) return found;
    return nullptr;
}

static void DumpNodes(BYTE* node, int level, int depth)
{
    if (!node || IsBadReadPtr(node, 0xAC)) return;
    const char* n = *(const char**)(node + 0x08);
    Log("ui: node %*s%p '%s' children %u", level * 2, "", node, n && !IsBadStringPtrA(n, 64) ? n : "", *(WORD*)(node + 0xA8));
    if (level >= depth) return;
    WORD count;
    BYTE** children = Children(node, &count);
    for (WORD i = 0; i < count; ++i) DumpNodes(children[i], level + 1, depth);
}

// NiAVObject::Update (0x00A59C60, as the cursor update calls it): recomputes the world transforms
// below the node from its local one.
static void UpdateNode(BYTE* node)
{
    struct { float time; BYTE flags[12]; } data = {};
    ((void(__thiscall*)(void*, void*))0x00A59C60)(node, &data);
}

static int s_pauseLogged, s_loadingLogged;
static BYTE* s_scopeModel;
static float s_scopeScale;
static BYTE* s_pauseNif;
static int s_loadingImages;

bool ScopeView()
{
    BYTE* hud = *(BYTE**)0x011D96C0;  // HUDMainMenu; +0x1FC: scope overlay shown (0x00709CC0)
    return hud && !IsBadReadPtr(hud + 0x1FC, 1) && hud[0x1FC];
}

// Logs the interface scene below the node named `name` ("-": its root, "world": the world scene
// graph), for the nodes command.
void UiDumpNodes(const char* name, int depth)
{
    BYTE* im = *(BYTE**)0x011D8A80;
    if (!im || IsBadReadPtr(im + 4, 4)) return;
    BYTE* root = *(BYTE**)(im + 4);
    BYTE* world = *(BYTE**)0x011DEB7C;  // the world scene graph (a node)
    BYTE* start = lstrcmpA(name, "-") == 0 ? root : lstrcmpA(name, "world") == 0 ? world : FindNode(root, name, 6);
    Log("ui: nodes below '%s' (%p), depth %d:", name, start, depth);
    DumpNodes(start, 0, depth);
}

// Scope view (0x00870BD0): the world is clipped to x = W * fScopeScissorAmount * aspect factor / 2
// from each side, for the scope model to cover the rest. Below 4:3 the model is scaled up to cover
// the window (UiFrame), so the world is not clipped at the sides.
static float __cdecl ScopeScissorAspect()
{
    float w, h;
    if (Narrow(&w, &h)) return 0.f;
    return ((float(__cdecl*)())0x00706E70)();
}

// Once per frame (at Present: takes effect from the next frame's scene update).
void UiFrame()
{
    float w, h;
    if (!Narrow(&w, &h)) return;
    float k = 1280.f * h / w / 960.f;  // camera height / layout height
    BYTE* im = *(BYTE**)0x011D8A80;
    if (!im || IsBadReadPtr(im + 4, 4)) return;
    BYTE* root = *(BYTE**)(im + 4);
    const BYTE* visible = (const BYTE*)0x011F308F;
    if (visible[1013]) {
        // The vignette model's root (child of the tile's node): vertical axis (z) scaled.
        BYTE* menu = FindNode(root, "StartMenu", 3);
        BYTE* tile = nullptr;
        BYTE* nif = FindNode(menu, "PauseScreen01", 4);
        s_pauseNif = nif;
        if (s_pauseLogged < 2 && !nif) {
            ++s_pauseLogged;
            Log("ui: pause vignette not found (menu %p, tile node %p)", menu, tile);
            DumpNodes(menu, 0, 5);
        }
        if (nif && !IsBadWritePtr(nif + 0x34, 36)) {
            // Its own rotation with the parent's vertical axis (row z) scaled.
            static BYTE* s_origNif;
            static float s_orig[9];
            float* r = (float*)(nif + 0x34);
            if (nif != s_origNif) {
                s_origNif = nif;
                memcpy(s_orig, r, sizeof(s_orig));
            }
            for (int i = 0; i < 6; ++i) r[i] = s_orig[i];
            for (int i = 6; i < 9; ++i) r[i] = s_orig[i] * k;
            UpdateNode(nif);
            if (s_pauseLogged++ < 2) Log("ui: pause vignette %p '%s' stretched x%.3f vertically", nif, *(const char**)(nif + 8), k);
        }
    }
    // Scope overlay: the HUD's scoped flag (+0x1FC); the weapon's scope model is the child of the
    // container node at 0x011D9770, a mask plane through its origin, drawn in front of the camera.
    // Below 4:3 the view shows more vertically than the model covers (it is made for 4:3 and wider):
    // scaled up, it covers the height as at 4:3, with the circle the same share of the height.
    BYTE* container = *(BYTE**)0x011D9770;
    s_scopeModel = nullptr;
    if (ScopeView() && container) {
        WORD count;
        BYTE** children = Children(container, &count);
        BYTE* model = count ? children[0] : nullptr;
        if (model && !IsBadWritePtr(model + 0x64, 4)) {
            s_scopeScale = 4.f / 3.f * h / w;
            *(float*)(model + 0x64) = s_scopeScale;
            UpdateNode(model);
            s_scopeModel = model;
        }
    }
    if (visible[1007]) {
        // Loading images (at 0, 0, 1280 x 960 in the layout): scaled about the layout's centre.
        BYTE* menu = FindNode(root, "LoadingMenu", 3);
        s_loadingImages = 0;
        static const char* kImages[] = {"loading_world_image", "loading_tile_slide_01", "loading_tile_slide_02"};
        for (const char* name : kImages) {
            BYTE* node = FindNode(menu, name, 4);
            if (!node || IsBadWritePtr(node + 0x58, 16)) continue;
            float* t = (float*)(node + 0x58);
            t[0] = 640.f * (1.f - k);   // x of the top-left corner
            t[2] = -480.f * (1.f - k);  // z (up) of the top-left corner
            *(float*)(node + 0x64) = k;
            UpdateNode(node);
            ++s_loadingImages;
            if (s_loadingLogged++ < 3) Log("ui: loading image %s scaled x%.3f", name, k);
        }
    }
}

void UiWriteState(FILE* f)
{
    BYTE* im = *(BYTE**)0x011D8A80;
    if (im && !IsBadReadPtr(im + 0xCC, 4)) {
        // The tile under the menu cursor (InterfaceManager +0xCC, JIP LN's activeTile) and its parents.
        BYTE* tile = *(BYTE**)(im + 0xCC);
        fprintf(f, "ui: tile under cursor %p", tile);
        for (int i = 0; tile && i < 4; ++i, tile = IsBadReadPtr(tile + 0x28, 4) ? nullptr : *(BYTE**)(tile + 0x28))
            fprintf(f, " %s'%s'", i ? "< " : "", TileName(tile));
        fprintf(f, "\n");
        // Its node and parents: world translate (+0x8C) and scale (+0x98), names (+0x08).
        BYTE* t = *(BYTE**)(im + 0xCC);
        BYTE* node = t && !IsBadReadPtr(t + 0x2C, 4) ? *(BYTE**)(t + 0x2C) : nullptr;
        for (int i = 0; node && i < 8 && !IsBadReadPtr(node, 0x9C); ++i, node = *(BYTE**)(node + 0x18)) {
            const char* n = *(const char**)(node + 0x08);
            float* wt = (float*)(node + 0x8C);
            float* lt = (float*)(node + 0x58);
            fprintf(f, "  node %p '%s' world %.1f %.1f %.1f scale %.4f local %.1f %.1f %.1f scale %.4f\n", node,
                    n && !IsBadStringPtrA(n, 64) ? n : "", wt[0], wt[1], wt[2], *(float*)(node + 0x98), lt[0], lt[1], lt[2],
                    *(float*)(node + 0x64));
        }
    }
    if (s_scopeModel) fprintf(f, "ui: scope model %p scaled x%.3f\n", s_scopeModel, s_scopeScale);
    if (s_pick.cam)
        fprintf(f, "ui: pick ray camera %p at %d %d (frame %lu): frustum l %.4f r %.4f t %.4f b %.4f, viewport %.3f %.3f %.3f %.3f\n",
                s_pick.cam, s_pick.x, s_pick.y, s_pick.frame, s_pick.frustum[0], s_pick.frustum[1], s_pick.frustum[2],
                s_pick.frustum[3], s_pick.viewport[0], s_pick.viewport[1], s_pick.viewport[2], s_pick.viewport[3]);
    fprintf(f, "ui: interface size %.1f x %.1f, camera %.1f x %.1f (frustum top %.1f bottom %.1f, z %.1f)\n", s_lastW, s_lastH,
            s_camW, s_camH, s_frustumTop, s_frustumBottom, s_cameraZ);
}
