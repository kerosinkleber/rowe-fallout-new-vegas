// The look of the game's HUD for Dear ImGui (the F8 menu and the launcher): colours from the HUD
// colour, thick glowing frame lines with chamfered joints, sliders as segmented meters.
#pragma once
#include "imgui.h"

// Sets ImGui's style for a font size and HUD colour (0..1). bgAlpha: the window background's opacity.
void HudApplyStyle(float fontPx, const float color[3], float bgAlpha = 0.82f);
float HudFontPx();
bool HudStyleIs(float fontPx, const float color[3]);  // the style was applied with these

ImU32 HudCol(float k = 1.f, float a = 1.f);  // the HUD colour times k, with alpha a
float HudT();                                 // frame line thickness
void HudPath(ImDrawList* dl, const ImVec2* pts, int n);  // a frame line with its glow

void HudSeparator();
void HudTableFrame(const ImVec2& tmin, const ImVec2& tmax, float labelW);
void HudWindowFrame();
bool HudSliderInt(const char* id, int* v, int lo, int hi, const char* fmt = "%d");
bool HudSliderFloat(const char* id, float* v, float lo, float hi, const char* fmt);
bool HudCheckbox(const char* label, bool* v);
bool HudOption(const char* label, bool on);
void HudTip(const char* text);  // tooltip for the last item

// The game's UI font as an ImGui font (gamefont.cpp): the .fnt data (line height, texture names,
// 256 glyphs) and the alpha of its texture. GameFontSet copies both.
bool GameFontSet(const void* fntData, unsigned fntSize, const unsigned char* alpha, int texW, int texH);
bool GameFontReady();
ImFont* GameFontAdd(ImFontAtlas* atlas, float sizePx);
