// The game's UI font for Dear ImGui: the .fnt data (line height, texture names, 256 glyphs of 14
// floats: texture, four UV corners, width, height, left / right spacing, ascent) and the alpha of
// its texture, served by a font loader that scales each glyph to the size asked for. The F8 menu
// takes them from the running game (gameui.cpp), the launcher from the game's files.
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hudstyle.h"
#include "imgui_internal.h"

struct GameGlyph { float tex, u0, v0, u1, v1, u2, v2, u3, v3, width, height, left, right, ascent; };
struct GameFontData { float lineHeight; DWORD numTextures; BYTE textures[8][0x24]; GameGlyph glyphs[256]; };
static_assert(sizeof(GameFontData) == 296 + 256 * 56, "the .fnt layout");

static GameFontData s_font;
static BYTE* s_alpha;          // the font texture's alpha, s_texW x s_texH
static int s_texW, s_texH;
static float s_maxAscent;
static bool s_fontReady;

bool GameFontReady() { return s_fontReady; }

bool GameFontSet(const void* fntData, unsigned fntSize, const unsigned char* alpha, int texW, int texH)
{
    if (fntSize < sizeof(GameFontData) || !alpha || texW <= 0 || texH <= 0) return false;
    BYTE* copy = (BYTE*)malloc((size_t)texW * texH);
    if (!copy) return false;
    memcpy(copy, alpha, (size_t)texW * texH);
    free(s_alpha);
    s_alpha = copy;
    memcpy(&s_font, fntData, sizeof(s_font));
    if (!(s_font.lineHeight > 1.f && s_font.lineHeight < 512.f)) return false;
    s_texW = texW;
    s_texH = texH;
    s_maxAscent = 0;
    for (const GameGlyph& g : s_font.glyphs)
        if (g.ascent > s_maxAscent) s_maxAscent = g.ascent;
    s_fontReady = true;
    return true;
}

static bool HasGlyph(ImWchar c) { return c < 256 && (s_font.glyphs[c].width > 0 || s_font.glyphs[c].right > 0 || c == ' '); }

static bool SrcContainsGlyph(ImFontAtlas*, ImFontConfig*, ImWchar c) { return HasGlyph(c); }

static bool BakedInit(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*)
{
    if (!src->MergeMode) {
        float scale = baked->Size / s_font.lineHeight;
        baked->Ascent = ImCeil(s_maxAscent * scale);
        baked->Descent = -ImCeil((s_font.lineHeight - s_maxAscent) * scale);
    }
    return true;
}

static bool BakedLoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar c, ImFontGlyph* out, float* outAdvance)
{
    if (!HasGlyph(c)) return false;
    const GameGlyph& g = s_font.glyphs[c];
    float scale = baked->Size / s_font.lineHeight;
    float advance = (g.width + g.left + g.right) * scale;
    if (outAdvance) {
        *outAdvance = advance;
        return true;
    }
    out->Codepoint = c;
    out->AdvanceX = advance;
    int sx = (int)floorf(g.u0 * s_texW + 0.5f), sy = (int)floorf(g.v0 * s_texH + 0.5f);
    int sw = (int)floorf((g.u3 - g.u0) * s_texW + 0.5f), sh = (int)floorf((g.v3 - g.v0) * s_texH + 0.5f);
    if (sw <= 0 || sh <= 0) return true;  // blank (space)
    int w = (int)ceilf(sw * scale), h = (int)ceilf(sh * scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    ImFontAtlasRectId id = ImFontAtlasPackAddRect(atlas, w, h);
    if (id == ImFontAtlasRectId_Invalid) return false;
    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, id);
    ImFontAtlasBuilder* b = atlas->Builder;
    b->TempBuffer.resize(w * h);
    unsigned char* px = b->TempBuffer.Data;
    // Box filter down (or bilinear up) from the game's glyph.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float fx0 = sx + x * (float)sw / w, fx1 = sx + (x + 1) * (float)sw / w;
            float fy0 = sy + y * (float)sh / h, fy1 = sy + (y + 1) * (float)sh / h;
            float sum = 0, area = 0;
            for (int yy = (int)fy0; yy < (int)ceilf(fy1); ++yy) {
                float wy = fminf(fy1, yy + 1.f) - fmaxf(fy0, (float)yy);
                if (wy <= 0 || yy < 0 || yy >= s_texH) continue;
                for (int xx = (int)fx0; xx < (int)ceilf(fx1); ++xx) {
                    float wx = fminf(fx1, xx + 1.f) - fmaxf(fx0, (float)xx);
                    if (wx <= 0 || xx < 0 || xx >= s_texW) continue;
                    sum += s_alpha[yy * s_texW + xx] * wx * wy;
                    area += wx * wy;
                }
            }
            px[y * w + x] = (unsigned char)(area > 0 ? fminf(255.f, sum / area + 0.5f) : 0);
        }
    }
    out->X0 = g.left * scale;
    out->Y0 = baked->Ascent - g.ascent * scale;
    out->X1 = out->X0 + w;
    out->Y1 = out->Y0 + h;
    out->Visible = true;
    out->PackId = id;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, px, ImTextureFormat_Alpha8, w);
    return true;
}

static ImFontLoader s_loader;

// Adds the game's font to the atlas (GameFontCapture first); null if it is not available.
ImFont* GameFontAdd(ImFontAtlas* atlas, float sizePx)
{
    if (!s_fontReady) return nullptr;
    s_loader.Name = "ROWE game font";
    s_loader.FontSrcContainsGlyph = SrcContainsGlyph;
    s_loader.FontBakedInit = BakedInit;
    s_loader.FontBakedLoadGlyph = BakedLoadGlyph;
    ImFontConfig cfg;
    cfg.FontLoader = &s_loader;
    cfg.SizePixels = sizePx;
    lstrcpynA(cfg.Name, "Game UI font", sizeof(cfg.Name));
    return atlas->AddFont(&cfg);
}

