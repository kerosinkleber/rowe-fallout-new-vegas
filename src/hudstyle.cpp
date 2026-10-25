// The look of the game's HUD for Dear ImGui (see hudstyle.h). Shared by the F8 menu (overlay.cpp)
// and the launcher.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include "hudstyle.h"

// The game's HUD colour on near-black (Pip-Boy amber if the setting is not found).
static float s_color[3] = {-1, -1, -1};
static float s_fontPx;

static ImVec4 Shade(const float c[3], float k, float a = 1.f) { return ImVec4(c[0] * k, c[1] * k, c[2] * k, a); }

void HudApplyStyle(float fontPx, const float c[3], float bgAlpha)
{
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    ImVec4* col = st.Colors;
    col[ImGuiCol_Text] = Shade(c, 1.f);
    col[ImGuiCol_TextDisabled] = Shade(c, 0.6f);
    col[ImGuiCol_WindowBg] = ImVec4(c[0] * 0.04f, c[1] * 0.04f, c[2] * 0.04f, bgAlpha);
    col[ImGuiCol_PopupBg] = ImVec4(c[0] * 0.07f, c[1] * 0.07f, c[2] * 0.07f, 0.97f);
    col[ImGuiCol_Border] = Shade(c, 0.62f);
    col[ImGuiCol_FrameBg] = Shade(c, 0.20f, 0.80f);
    col[ImGuiCol_FrameBgHovered] = Shade(c, 0.36f, 0.90f);
    col[ImGuiCol_FrameBgActive] = Shade(c, 0.48f);
    col[ImGuiCol_TitleBg] = Shade(c, 0.20f);
    col[ImGuiCol_TitleBgActive] = Shade(c, 0.36f);
    col[ImGuiCol_CheckMark] = Shade(c, 1.f);
    col[ImGuiCol_SliderGrab] = Shade(c, 0.62f);
    col[ImGuiCol_SliderGrabActive] = Shade(c, 1.f);
    col[ImGuiCol_Button] = Shade(c, 0.30f, 0.90f);
    col[ImGuiCol_ButtonHovered] = Shade(c, 0.48f);
    col[ImGuiCol_ButtonActive] = Shade(c, 0.62f);
    col[ImGuiCol_Header] = Shade(c, 0.30f, 0.90f);
    col[ImGuiCol_HeaderHovered] = Shade(c, 0.48f);
    col[ImGuiCol_HeaderActive] = Shade(c, 0.62f);
    col[ImGuiCol_Separator] = Shade(c, 0.62f);
    col[ImGuiCol_Tab] = Shade(c, 0.20f);
    col[ImGuiCol_TabHovered] = Shade(c, 0.48f);
    col[ImGuiCol_TabSelected] = Shade(c, 0.36f);
    col[ImGuiCol_NavCursor] = Shade(c, 1.f);
    col[ImGuiCol_TextSelectedBg] = Shade(c, 0.40f, 0.60f);
    // The rest of ImGui's blue and grey defaults.
    col[ImGuiCol_CheckboxSelectedBg] = Shade(c, 0.20f, 0.80f);
    col[ImGuiCol_ScrollbarBg] = Shade(c, 0.04f, 0.50f);
    col[ImGuiCol_ScrollbarGrab] = Shade(c, 0.36f);
    col[ImGuiCol_ScrollbarGrabHovered] = Shade(c, 0.55f);
    col[ImGuiCol_ScrollbarGrabActive] = Shade(c, 0.75f);
    col[ImGuiCol_SeparatorHovered] = Shade(c, 0.80f);
    col[ImGuiCol_SeparatorActive] = Shade(c, 1.f);
    col[ImGuiCol_ResizeGrip] = Shade(c, 0.30f, 0.40f);
    col[ImGuiCol_ResizeGripHovered] = Shade(c, 0.60f, 0.70f);
    col[ImGuiCol_ResizeGripActive] = Shade(c, 1.f, 0.90f);
    col[ImGuiCol_InputTextCursor] = Shade(c, 1.f);
    col[ImGuiCol_TabSelectedOverline] = Shade(c, 1.f);
    col[ImGuiCol_TabDimmed] = Shade(c, 0.15f);
    col[ImGuiCol_TabDimmedSelected] = Shade(c, 0.30f);
    col[ImGuiCol_TabDimmedSelectedOverline] = Shade(c, 0.60f);
    col[ImGuiCol_TableHeaderBg] = Shade(c, 0.20f);
    col[ImGuiCol_TableBorderStrong] = Shade(c, 0.50f);
    col[ImGuiCol_TableBorderLight] = Shade(c, 0.30f);
    col[ImGuiCol_TextLink] = Shade(c, 1.f);
    col[ImGuiCol_TreeLines] = Shade(c, 0.50f);
    col[ImGuiCol_DragDropTarget] = Shade(c, 1.f);
    col[ImGuiCol_NavWindowingHighlight] = Shade(c, 1.f, 0.70f);
    st.WindowRounding = 0.f;
    st.FrameRounding = 0.f;
    st.GrabRounding = 0.f;
    st.TabRounding = 0.f;
    st.WindowBorderSize = 0.f;  // the HUD bracket is drawn instead (HudWindowFrame)
    st.FrameBorderSize = 0.f;
    st.PopupBorderSize = 1.f;
    st.WindowPadding = ImVec2(16.f, 10.f);
    st.ScaleAllSizes(fontPx / 13.f);  // ImGui's sizes are made for a 13 px font
    st.FontSizeBase = fontPx;
    s_fontPx = fontPx;
    for (int i = 0; i < 3; ++i) s_color[i] = c[i];
}

float HudFontPx() { return s_fontPx; }

bool HudStyleIs(float fontPx, const float c[3])
{
    return fontPx == s_fontPx && c[0] == s_color[0] && c[1] == s_color[1] && c[2] == s_color[2];
}

// ---- HUD look ----------------------------------------------------------------------------------
// The HUD's frame lines (HP / AP brackets): thick lines in the HUD colour with a soft glow, L-shaped
// brackets on the outer side, 45-degree chamfers where a divider leaves the baseline, and meters
// made of vertical segments (bright where filled, faint where empty).

ImU32 HudCol(float k, float a)
{
    return ImGui::GetColorU32(ImVec4(s_color[0] * k, s_color[1] * k, s_color[2] * k, a));
}

float HudT() { return fmaxf(2.f, floorf(s_fontPx / 8.f + 0.5f)); }  // line thickness (3 px at 24 px text)

void HudPath(ImDrawList* dl, const ImVec2* pts, int n)
{
    float t = HudT();
    dl->AddPolyline(pts, n, HudCol(1.f, 0.16f), ImDrawFlags_None, t * 3.2f);  // glow
    dl->AddPolyline(pts, n, HudCol(), ImDrawFlags_None, t);
}

// A separator like the HUD's: a line with short chamfered drops at both ends.
void HudSeparator()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float t = HudT(), c = s_fontPx * 0.35f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x0 = p.x, x1 = p.x + ImGui::GetContentRegionAvail().x, y = p.y + t;
    ImVec2 pts[] = {ImVec2(x0, y + c * 1.6f), ImVec2(x0, y + c), ImVec2(x0 + c, y), ImVec2(x1 - c, y), ImVec2(x1, y + c), ImVec2(x1, y + c * 1.6f)};
    HudPath(dl, pts, 6);
    ImGui::Dummy(ImVec2(0, t * 2 + c * 1.6f));
}

// Under a tab's rows: the baseline across, and the divider between the label and value columns
// dropping from it with a chamfer (the HUD's CND | 13/7 joint).
void HudTableFrame(const ImVec2& tmin, const ImVec2& tmax, float labelW)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float c = s_fontPx * 0.35f, y = tmin.y - ImGui::GetStyle().ItemSpacing.y * 0.5f;
    float x = tmin.x + labelW + ImGui::GetStyle().CellPadding.x;
    ImVec2 base[] = {ImVec2(tmin.x, y), ImVec2(tmax.x, y)};
    HudPath(dl, base, 2);
    ImVec2 div[] = {ImVec2(x - c, y), ImVec2(x, y + c), ImVec2(x, tmax.y)};
    HudPath(dl, div, 3);
}

// The window's bracket: the line down the left (with the title next to it, like "HP") into the
// baseline along the bottom, which ends in a short rise on the right.
void HudWindowFrame()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wmin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    float t = HudT(), c = s_fontPx * 0.45f;
    float x0 = wmin.x + t * 1.5f, x1 = wmin.x + size.x - t * 1.5f, y0 = wmin.y + t * 1.5f, y1 = wmin.y + size.y - t * 1.5f;
    ImVec2 pts[] = {ImVec2(x0, y0), ImVec2(x0, y1), ImVec2(x1 - c, y1), ImVec2(x1, y1 - c), ImVec2(x1, y1 - c * 2.2f)};
    HudPath(dl, pts, 5);
}

// A slider drawn as a HUD meter: segments over the width, the value at the right.
bool HudSliderInt(const char* id, int* v, int lo, int hi, const char* fmt)
{
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, HudCol(1.f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, HudCol(1.f, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(0, 0, 0, 0));
    bool changed = ImGui::SliderInt(id, v, lo, hi, "");
    ImGui::PopStyleColor(5);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    char num[16];
    _snprintf(num, sizeof(num) - 1, fmt, *v);
    num[sizeof(num) - 1] = 0;
    ImVec2 ts = ImGui::CalcTextSize(num);
    float numW = fmaxf(ImGui::CalcTextSize("000").x, ts.x);
    float mx0 = a.x, mx1 = b.x - numW - s_fontPx * 0.4f;
    float step = fmaxf(4.f, floorf(s_fontPx * 0.38f + 0.5f)), segW = fmaxf(2.f, floorf(step * 0.62f + 0.5f));
    int n = (int)((mx1 - mx0) / step);
    float fill = hi > lo ? (float)(*v - lo) / (hi - lo) : 0.f;
    float sy0 = a.y + (b.y - a.y) * 0.22f, sy1 = b.y - (b.y - a.y) * 0.22f;
    for (int i = 0; i < n; ++i) {
        float x = mx0 + i * step;
        bool on = (i + 0.5f) / n <= fill;
        dl->AddRectFilled(ImVec2(x, sy0), ImVec2(x + segW, sy1), on ? HudCol() : HudCol(0.5f, 0.55f));
    }
    // The segments stand on the meter's baseline, as on the HUD.
    float by = sy1 + HudT() * 0.5f;
    ImVec2 base[] = {ImVec2(mx0, by), ImVec2(mx0 + (n > 0 ? (n - 1) * step + segW : 0), by)};
    HudPath(dl, base, 2);
    dl->AddText(ImVec2(b.x - ts.x, a.y + (b.y - a.y - ts.y) * 0.5f), HudCol(), num);
    return changed;
}

// A checkbox as an outlined box in the HUD colour.
bool HudCheckbox(const char* label, bool* v)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, fmaxf(1.f, floorf(HudT() * 0.6f)));
    ImGui::PushStyleColor(ImGuiCol_Border, HudCol());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, HudCol(0.2f, 0.5f));
    bool changed = ImGui::Checkbox(label, v);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    return changed;
}

// One of a few choices, like a Pip-Boy list entry: the chosen one is boxed, the others dimmer.
bool HudOption(const char* label, bool on)
{
    ImVec2 pad = ImGui::GetStyle().FramePadding, ts = ImGui::CalcTextSize(label);
    ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    bool clicked = ImGui::InvisibleButton(label, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (on || ImGui::IsItemHovered()) dl->AddRectFilled(a, b, HudCol(1.f, on ? 0.18f : 0.10f));
    if (on) dl->AddRect(a, b, HudCol(), 0.f, 0, fmaxf(1.f, floorf(HudT() * 0.6f)));
    dl->AddText(ImVec2(a.x + pad.x, a.y + pad.y), on ? HudCol() : HudCol(0.65f), label);
    return clicked;
}

// The float version (the value shown with fmt).
bool HudSliderFloat(const char* id, float* v, float lo, float hi, const char* fmt)
{
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, HudCol(1.f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, HudCol(1.f, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(0, 0, 0, 0));
    bool changed = ImGui::SliderFloat(id, v, lo, hi, "");
    ImGui::PopStyleColor(5);
    char num[32];
    _snprintf(num, sizeof(num) - 1, fmt, *v);
    num[sizeof(num) - 1] = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImVec2 ts = ImGui::CalcTextSize(num);
    float numW = fmaxf(ImGui::CalcTextSize("00000").x, ts.x);
    float mx0 = a.x, mx1 = b.x - numW - s_fontPx * 0.4f;
    float step = fmaxf(4.f, floorf(s_fontPx * 0.38f + 0.5f)), segW = fmaxf(2.f, floorf(step * 0.62f + 0.5f));
    int n = (int)((mx1 - mx0) / step);
    float fill = hi > lo ? (*v - lo) / (hi - lo) : 0.f;
    float sy0 = a.y + (b.y - a.y) * 0.22f, sy1 = b.y - (b.y - a.y) * 0.22f;
    for (int i = 0; i < n; ++i) {
        float x = mx0 + i * step;
        bool on = (i + 0.5f) / n <= fill;
        dl->AddRectFilled(ImVec2(x, sy0), ImVec2(x + segW, sy1), on ? HudCol() : HudCol(0.5f, 0.55f));
    }
    float by = sy1 + HudT() * 0.5f;
    ImVec2 base[] = {ImVec2(mx0, by), ImVec2(mx0 + (n > 0 ? (n - 1) * step + segW : 0), by)};
    HudPath(dl, base, 2);
    dl->AddText(ImVec2(b.x - ts.x, a.y + (b.y - a.y - ts.y) * 0.5f), HudCol(), num);
    return changed;
}

void HudTip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

