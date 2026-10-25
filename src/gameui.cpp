// The game's own UI look for the settings menu:
// - Font: the font the game loaded as its UI font (font 2, Textures\Fonts\Monofonto_Large.fnt by
//   default, or whatever a UI mod put there). The game's FontManager (0x011F33F8) holds a FontInfo
//   per font; FontInfo +0x38 is the font data (the .fnt file's content) and +0x0C the texturing
//   property, whose base map's texture renderer data (+0x24) holds the IDirect3DTexture9 at +0x64.
//   Both are copied once and handed to gamefont.cpp, which serves them to Dear ImGui.
// - Colour: the HUD colour setting (uHUDColor:Interface), read live.
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "fnvfov.h"
#include "hudstyle.h"

static const unsigned kFontDataSize = 296 + 256 * 56;  // the .fnt layout (gamefont.cpp)

static bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }

bool GameFontCapture(int fontIndex)
{
    if (GameFontReady()) return true;
    BYTE* fm = *(BYTE**)0x011F33F8;
    if (!Readable(fm, 4 * 8)) return false;
    BYTE* fi = *(BYTE**)(fm + 4 * fontIndex);
    if (!Readable(fi, 0x40) || !fi[0]) return false;
    BYTE* fd = *(BYTE**)(fi + 0x38);
    BYTE* texProp = *(BYTE**)(fi + 0x0C);
    if (!Readable(fd, kFontDataSize) || !Readable(texProp, 0x24)) return false;
    BYTE** maps = *(BYTE***)(texProp + 0x20);
    if (!Readable(maps, 4)) return false;
    BYTE* map = maps[0];
    if (!Readable(map, 0x0C)) return false;
    BYTE* tex = *(BYTE**)(map + 0x08);
    if (!Readable(tex, 0x28)) return false;
    BYTE* rdata = *(BYTE**)(tex + 0x24);
    if (!Readable(rdata, 0x68)) return false;
    IDirect3DTexture9* d3dtex = *(IDirect3DTexture9**)(rdata + 0x64);
    if (!Readable(d3dtex, 4)) return false;
    D3DSURFACE_DESC d;
    if (FAILED(d3dtex->GetLevelDesc(0, &d)) || (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_A8)) {
        Log("gameui: font %d texture not usable (format %d)", fontIndex, Readable(d3dtex, 4) ? (int)d.Format : -1);
        return false;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(d3dtex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
        Log("gameui: font %d texture %ux%u cannot be read (pool %d)", fontIndex, d.Width, d.Height, d.Pool);
        return false;
    }
    BYTE* alpha = (BYTE*)malloc((size_t)d.Width * d.Height);
    if (alpha) {
        for (UINT y = 0; y < d.Height; ++y) {
            const BYTE* row = (const BYTE*)lr.pBits + (size_t)y * lr.Pitch;
            for (UINT x = 0; x < d.Width; ++x) alpha[y * d.Width + x] = d.Format == D3DFMT_A8 ? row[x] : row[x * 4 + 3];
        }
    }
    d3dtex->UnlockRect(0);
    if (!alpha) return false;
    bool ok = GameFontSet(fd, kFontDataSize, alpha, (int)d.Width, (int)d.Height);
    free(alpha);
    if (!ok) return false;
    const char* path = Readable(*(const char**)(fi + 4), 1) ? *(const char**)(fi + 4) : "?";
    Log("gameui: using the game's font %d (%s): texture %ux%u", fontIndex, path, d.Width, d.Height);
    return true;
}

// ---- colour -------------------------------------------------------------------------------------

// The HUD colour the game uses now (0..1): the setting uHUDColor:Interface (0xRRGGBBAA), which the
// game's colour list uses for &hudmain; (the iSystemColorHUDMain... keys in old prefs are unused).
bool GameHudColor(float rgb[3])
{
    static BYTE* obj;
    static bool searched;
    if (!searched) {
        searched = true;
        obj = FindSettingObject("uHUDColor:Interface");
        Log("gameui: HUD colour setting %p", obj);
    }
    if (!obj) return false;
    DWORD v = *(DWORD*)(obj + 4);
    rgb[0] = ((v >> 24) & 0xFF) / 255.f;
    rgb[1] = ((v >> 16) & 0xFF) / 255.f;
    rgb[2] = ((v >> 8) & 0xFF) / 255.f;
    return true;
}
