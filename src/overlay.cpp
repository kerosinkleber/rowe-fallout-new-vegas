// The settings menu (F8, or rowe.ini menukey), built with Dear ImGui (third_party/imgui, MIT) and drawn into the real back
// buffer at Present, after supersampling's resolve, so it is sharp at any supersampling and not
// bent by the lens. Font: Share Tech Mono (third_party/fonts, SIL OFL), sized to the window.
//
// Input: while the menu is open the game's DirectInput mouse is set free (non-exclusive, as for
// mouse trap mode 1) so Windows' mouse messages reach the menu through the window procedure
// (OverlayWndProc, called from the mouse trap's subclass), and the game's DirectInput mouse and
// keyboard read nothing (dinput.cpp). ImGui draws its own cursor. The menu key is read with GetAsyncKeyState.
//
// Every change applies at once and is saved to rowe.ini. Tools: the `overlay open` / `overlay
// close` commands (OverlayCommand).
//
// The stats window (stats=1) is drawn by the same ImGui frame, with the menu closed too; it only
// takes the mouse (to be dragged) while the menu is open.
#include <windows.h>
#include <shellapi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fnvfov.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "sharetechmono_ttf.h"
#include "hudstyle.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool s_open, s_ready, s_failed;
static HWND s_window;
static int s_capturingKey;     // 1: the next key pressed becomes *s_captureInto (the screenshot or menu key)
static int* s_captureInto;
static bool s_menuKeyDown;     // the menu key was down last frame (it toggles on the press)
static char s_dirEdit[MAX_PATH];
static bool s_dirEditing;

static const float kSupersampleSteps[] = {1.f, 2.f, 2.25f, 3.f, 4.f, 6.f, 8.f, 9.f, 10.f, 12.f, 16.f, 25.f, 36.f, 49.f, 64.f};
static const int kSupersampleStepCount = sizeof(kSupersampleSteps) / sizeof(kSupersampleSteps[0]);

// ---- look ------------------------------------------------------------------------------------

// The colour to use: the game's HUD colour, else amber.
static void MenuColor(float c[3])
{
    if (!GameHudColor(c)) { c[0] = 1.00f; c[1] = 0.71f; c[2] = 0.26f; }
}

// The render window (the device's, a child of the game's main window) gets the mouse messages: the
// menu listens there too (the main window's messages arrive through the mouse trap's subclass).
static WNDPROC s_renderOrigProc;
static LRESULT CALLBACK RenderWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    OverlayWndProc(h, msg, w, l);
    return CallWindowProcA(s_renderOrigProc, h, msg, w, l);
}

static bool Init(IDirect3DDevice9* dev, int height)
{
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    HWND w = SUCCEEDED(dev->GetCreationParameters(&cp)) && cp.hFocusWindow ? cp.hFocusWindow : GameWindow();
    if (!w) return false;
    if (w != GameWindow()) s_renderOrigProc = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)RenderWndProc);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true;  // the game hides the Windows cursor
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    float px = floorf(height / 45.f + 0.5f);
    if (px < 14) px = 14;
    if (px > 64) px = 64;
    // The game's UI font (font 2), else the built-in Share Tech Mono.
    if (!(GameFontCapture(1) && GameFontAdd(io.Fonts, px))) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF((void*)g_shareTechMono, (int)g_shareTechMonoSize, px, &cfg);
        Log("overlay: the game's font is not available, using Share Tech Mono");
    }
    float c[3];
    MenuColor(c);
    HudApplyStyle(px, c);
    if (!ImGui_ImplWin32_Init(w) || !ImGui_ImplDX9_Init(dev)) {
        Log("overlay: ImGui initialisation failed");
        return false;
    }
    s_window = w;
    Log("overlay: Dear ImGui %s ready (font %.0f px, window %p%s)", IMGUI_VERSION, px, w, s_renderOrigProc ? ", render window" : "");
    return true;
}

// ---- widgets ---------------------------------------------------------------------------------

static bool s_dirty;  // a change to save
static void Changed() { s_dirty = true; }

// A FOV-like setting where 0 means the game's own value: a "game" checkbox and a slider.
// rightW: room left free at the row's right for a widget the caller adds (SameLine) after it.
static void AngleRow(const char* label, const char* id, float* value, float gameValue, float lo, float hi, const char* tip,
                     float rightW = 0)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    HudTip(tip);
    ImGui::TableNextColumn();
    ImGui::PushID(id);
    bool game = *value <= 0;
    if (HudCheckbox("game", &game)) {
        *value = game ? 0.f : floorf(gameValue + 0.5f);
        if (*value < lo) *value = lo;
        if (*value > hi) *value = hi;
        Changed();
    }
    HudTip("Use the game's own value.");
    ImGui::SameLine();
    int v = (int)floorf((game ? gameValue : *value) + 0.5f);
    ImGui::BeginDisabled(game);
    ImGui::SetNextItemWidth(rightW > 0 ? -(rightW + ImGui::GetStyle().ItemSpacing.x) : -FLT_MIN);
    if (HudSliderInt("##v", &v, (int)lo, (int)hi)) *value = (float)v;
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    ImGui::EndDisabled();
    ImGui::PopID();
}

static void LabelCell(const char* label, const char* tip)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    HudTip(tip);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

static void FactorLabel(char* out, int size, float f, int nativeW, int nativeH)
{
    if (f <= 1.01f) { _snprintf(out, size, "off"); return; }
    float s = sqrtf(f);
    if (nativeW > 0) _snprintf(out, size, "%gx  (%dx%d)", f, (int)(nativeW * s + 0.5f) & ~1, (int)(nativeH * s + 0.5f) & ~1);
    else _snprintf(out, size, "%gx", f);
}

// ---- folder browser (like ReShade's path picker) ---------------------------------------------

static bool s_browseOpen;
static char s_browsePath[MAX_PATH];  // empty: the list of drives
static char s_browseDirs[512][MAX_PATH];
static int s_browseCount;

static void BrowseList()
{
    s_browseCount = 0;
    if (!s_browsePath[0]) {
        char drives[256];
        DWORD n = GetLogicalDriveStringsA(sizeof(drives), drives);
        for (char* d = drives; d < drives + n && *d && s_browseCount < 512; d += lstrlenA(d) + 1)
            lstrcpynA(s_browseDirs[s_browseCount++], d, MAX_PATH);
        return;
    }
    char pattern[MAX_PATH + 4];
    wsprintfA(pattern, "%s*", s_browsePath);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (!lstrcmpA(fd.cFileName, ".") || !lstrcmpA(fd.cFileName, "..")) continue;
        if (s_browseCount < 512) lstrcpynA(s_browseDirs[s_browseCount++], fd.cFileName, MAX_PATH);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(s_browseDirs, s_browseCount, MAX_PATH, [](const void* a, const void* b) { return lstrcmpiA((const char*)a, (const char*)b); });
}

static void BrowseTo(const char* path)
{
    lstrcpynA(s_browsePath, path, MAX_PATH);
    int n = lstrlenA(s_browsePath);
    if (n && s_browsePath[n - 1] != '\\' && n < MAX_PATH - 1) lstrcatA(s_browsePath, "\\");
    BrowseList();
}

static void BrowseUp()
{
    int n = lstrlenA(s_browsePath);
    if (n <= 3) { s_browsePath[0] = 0; BrowseList(); return; }  // "C:\" -> drives
    s_browsePath[n - 1] = 0;
    char* slash = strrchr(s_browsePath, '\\');
    if (slash) slash[1] = 0;
    BrowseList();
}

static void DrawBrowser()
{
    if (s_browseOpen) {
        ImGui::OpenPopup("Choose the screenshot folder");
        s_browseOpen = false;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(HudFontPx() * 34, io.DisplaySize.y * 0.6f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Choose the screenshot folder", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (ImGui::Button("up")) BrowseUp();
    ImGui::SameLine();
    ImGui::TextUnformatted(s_browsePath[0] ? s_browsePath : "(drives)");
    float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("dirs", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
        char next[MAX_PATH] = "";
        for (int i = 0; i < s_browseCount; ++i) {
            char label[MAX_PATH + 8];
            wsprintfA(label, "%s%s", s_browseDirs[i], s_browsePath[0] ? "\\" : "");
            if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick))
                wsprintfA(next, "%s%s", s_browsePath, s_browseDirs[i]);
        }
        if (!s_browseCount) ImGui::TextDisabled(s_browsePath[0] ? "no folders here" : "no drives");
        if (next[0]) BrowseTo(next);
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(!s_browsePath[0]);
    if (ImGui::Button("use this folder")) {
        lstrcpynA(g_settings.screenshotDir, s_browsePath, sizeof(g_settings.screenshotDir));
        Changed();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---- restart -----------------------------------------------------------------------------------

static int s_startSkipIntros = -1;  // skipintros as this start read it

// Settings changed since the start that only apply at the next one.
static bool RestartNeeded(char* why, int size)
{
    why[0] = 0;
    int nw, nh, vw, vh;
    float active = SupersampleSizes(&nw, &nh, &vw, &vh) ? (float)vw * vh / ((float)nw * nh) : 1.f;
    bool ss = !SupersampleOverridden() && fabsf(active - (g_settings.supersample > 1.01f ? g_settings.supersample : 1.f)) > 0.05f &&
              !(active > 1.01f && g_settings.supersample > 1.01f && fabsf(sqrtf(active) - sqrtf(g_settings.supersample)) < 0.01f);
    bool intros = s_startSkipIntros >= 0 && (s_startSkipIntros != 0) != g_settings.skipIntros;
    if (ss && intros) lstrcpynA(why, "supersampling and intro videos change after a restart", size);
    else if (ss) lstrcpynA(why, "supersampling changes after a restart", size);
    else if (intros) lstrcpynA(why, "intro videos change after a restart", size);
    return ss || intros;
}

static void DrawRestart()
{
    char why[96];
    bool needed = RestartNeeded(why, sizeof(why));
    ImGui::BeginDisabled(RestartPending());
    if (ImGui::Button(RestartPending() ? "restarting..." : "restart game")) ImGui::OpenPopup("Restart the game?");
    ImGui::EndDisabled();
    HudTip("Saves the game, starts it again the same way and loads the save: for the settings that only apply at start "
        "(supersampling, intro videos).");
    ImGui::SameLine();
    if (needed) ImGui::TextUnformatted(why);
    else ImGui::TextDisabled("nothing waiting for a restart");
    if (ImGui::BeginPopupModal("Restart the game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (RestartGameLoaded())
            ImGui::TextUnformatted("The game is saved as \"rowe_restart\", started again\nand that save is loaded.");
        else ImGui::TextUnformatted("The game is started again (no game is loaded,\nso nothing is saved).");
        if (ImGui::Button("restart")) {
            SettingsSave();
            RestartGame();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---- stats window ------------------------------------------------------------------------------
// What the mod is doing now: the lens (its mode, the FOV it shows and the FOV rendered for it) and
// supersampling (the size rendered and the size shown), in a small window in the menu's look. Its
// text size is statssize percent of the menu's; a drag (menu open) moves it, kept as fractions of
// the screen (statsx, statsy; -1 = the top right corner).

static ImVec2 s_statsLast(-1, -1);  // where the stats window was last frame (top-left), before a drag
static bool s_statsPlace;           // put it where the settings say on the next frame (reset position)

static void StatsRow(const char* label, const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(buf);
}

static bool RestartNeeded(char* why, int size);

static void DrawStats(float menuPx, int backW, int backH)
{
    ImGuiIO& io = ImGui::GetIO();
    float px = floorf(menuPx * g_settings.statsSize / 100.f + 0.5f), c[3];
    MenuColor(c);
    HudApplyStyle(px, c);
    ImGui::PushFont(nullptr, px);
    bool placed = g_settings.statsX >= 0;
    ImVec2 pos = placed ? ImVec2(floorf(g_settings.statsX * io.DisplaySize.x), floorf(g_settings.statsY * io.DisplaySize.y))
                        : ImVec2(floorf(io.DisplaySize.x * 0.98f), floorf(io.DisplaySize.y * 0.03f));
    // Pinned while the menu is closed; with it open ImGui keeps the position, so it can be dragged.
    ImGui::SetNextWindowPos(pos, s_open && !s_statsPlace ? ImGuiCond_Appearing : ImGuiCond_Always, placed ? ImVec2(0, 0) : ImVec2(1, 0));
    s_statsPlace = false;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNav;
    if (!s_open) flags |= ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("ROWE STATS", nullptr, flags)) {
        ImGui::TextUnformatted("ROWE STATS");
        if (s_open) HudTip("Drag to move it (while this menu is open). Turn it off in Other, Stats.");
        if (ImGui::BeginTable("stats", 2, ImGuiTableFlags_SizingFixedFit)) {
            static const char* kLensNames[kLensModeCount] = {"off", "auto", "Panini", "stereographic", "fisheye"};
            float lensFov = 0, renderFov = 0, blend = 0;
            const char* skip = "";
            if (LensStats(&lensFov, &renderFov, &blend, &skip)) {
                if (g_settings.lens == kLensAuto) StatsRow("Lens", "auto: %d%% Panini", (int)(blend * 100 + 0.5f));
                else if (g_settings.lens == kLensFisheye) StatsRow("Lens", "fisheye, %s", g_settings.fisheyeCircle ? "full circle" : "fill");
                else StatsRow("Lens", "%s", kLensNames[g_settings.lens]);
                StatsRow("Lens FOV", "%.0f deg across", lensFov);
                StatsRow("Rendered", "%.0f deg across", renderFov);
            } else {
                StatsRow("Lens", "%s", kLensNames[g_settings.lens]);
                if (skip && *skip && g_settings.lens != kLensOff) StatsRow("", "not warping: %s", skip);
            }
            if (g_settings.fov > 0) StatsRow("FOV", "%.0f (at 4:3)", g_settings.fov);
            else StatsRow("FOV", "game's own (%.0f)", FovBase());
            if (ScopeView()) StatsRow("Scope overlay", "%s", g_settings.scopeLens ? "bent with the lens" : "flat");
            int nw = 0, nh = 0, vw = 0, vh = 0;
            if (SupersampleSizes(&nw, &nh, &vw, &vh)) {
                StatsRow("Supersampling", "%.4gx", (float)vw * vh / ((float)nw * nh));
                StatsRow("Renders at", "%dx%d", vw, vh);
                StatsRow("Shown at", "%dx%d", nw, nh);
            } else {
                StatsRow("Supersampling", SupersampleOverridden() ? "off this start (size overridden)" : "off");
                StatsRow("Renders at", "%dx%d", backW, backH);
            }
            char why[96];
            if (RestartNeeded(why, sizeof(why)) && strstr(why, "supersampling"))
                StatsRow("Next start", g_settings.supersample > 1.01f ? "%.4gx" : "off", g_settings.supersample);
            ImGui::EndTable();
        }
        HudWindowFrame();
        // Where a drag (menu open) left it: kept as fractions of the screen.
        ImVec2 p = ImGui::GetWindowPos();
        if (s_open && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && s_statsLast.x >= 0 &&
            (fabsf(p.x - s_statsLast.x) > 0.5f || fabsf(p.y - s_statsLast.y) > 0.5f) && io.DisplaySize.x > 0 && io.DisplaySize.y > 0) {
            g_settings.statsX = fminf(fmaxf(p.x / io.DisplaySize.x, 0.f), 1.f);
            g_settings.statsY = fminf(fmaxf(p.y / io.DisplaySize.y, 0.f), 1.f);
            Changed();
        }
        if (!s_open || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) s_statsLast = p;
    }
    ImGui::End();
    ImGui::PopFont();
    HudApplyStyle(menuPx, c);
}

// ---- the window ------------------------------------------------------------------------------

static void DrawMenu(int backW, int backH)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.03f, io.DisplaySize.y * 0.05f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(HudFontPx() * 30, 0), ImVec2(io.DisplaySize.x * 0.9f, io.DisplaySize.y * 0.9f));
    if (!ImGui::Begin("ROWE SETTINGS", &s_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("ROWE SETTINGS");  // like the HUD's "HP" beside its bracket
    HudTip("ROWE for Fallout New Vegas: Reinhold Oberst\xC3\xBC" "rcks wunderbare Erweiterungen");
    ImGui::SameLine();
    float closeW = ImGui::CalcTextSize("close").x + ImGui::GetStyle().FramePadding.x * 2;
    float right = ImGui::GetWindowContentRegionMax().x - closeW;
    if (right > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right);
    if (ImGui::SmallButton("close")) s_open = false;
    const ImGuiTableFlags tf = ImGuiTableFlags_SizingStretchProp;
    float labelW = HudFontPx() * 11;

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("View")) {
            if (ImGui::BeginTable("view", 2, tf)) {
                ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, labelW);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
                AngleRow("FOV", "fov", &g_settings.fov, FovBase(), kFovMin, kFovMax,
                         "Horizontal field of view, in degrees at 4:3 like the game's own setting (wider screens show more).");
                int lensFov = (int)(LensEffectiveFov() + 0.5f);
                if (lensFov > 0 && g_settings.fov > 0 && lensFov + 1 < (int)(g_settings.fov + 0.5f)) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled(g_settings.lens == kLensFisheye && g_settings.fisheyeCircle ? "the circle shows at most %d"
                                                                                                  : "the lens shows at most %d",
                                        lensFov);
                }
                static const char* kLens[kLensModeCount] = {"Off", "Auto", "Panini", "Stereographic", "Fisheye"};
                LabelCell("Lens", "How very wide views are drawn without stretched edges.\n"
                                  "Auto: normal up to 90 degrees, then Panini. Panini: keeps vertical lines straight. "
                                  "Stereographic: keeps shapes round. Fisheye: up to 160 degrees across, black corners.");
                if (ImGui::Combo("##lens", &g_settings.lens, kLens, kLensModeCount)) Changed();
                if (g_settings.lens == kLensFisheye) {
                    LabelCell("Fisheye", "Fill: the picture fills the screen's width, the circle's edge is cut off at the sides.\n"
                                         "Full circle: at the widest FOV (160) everything that is rendered is shown, as a circle fitted to the "
                                         "shorter side, with black around it. A lower FOV or zooming in magnifies the picture and its circle "
                                         "together, so the circle grows past the screen's edges.");
                    if (HudOption("fill", !g_settings.fisheyeCircle) && g_settings.fisheyeCircle) { g_settings.fisheyeCircle = false; Changed(); }
                    ImGui::SameLine();
                    if (HudOption("full circle", g_settings.fisheyeCircle) && !g_settings.fisheyeCircle) { g_settings.fisheyeCircle = true; Changed(); }
                }
                LabelCell("Scope overlay", "Whether the lens also bends the scope's crosshair and black mask.");
                int flat = g_settings.scopeLens ? 0 : 1;
                static const char* kScope[] = {"bent with the lens", "flat"};
                if (ImGui::Combo("##scope", &flat, kScope, 2)) { g_settings.scopeLens = flat == 0; Changed(); }
                AngleRow("Zoom FOV", "zoom", &g_settings.zoomFov, FovZoomDefault(), kZoomFovMin, kFovMax,
                         "Where the zoom with no weapon drawn (holding the aim button) ends.");
                {
                    // Take the Pip-Boy out or put it away (a tap of its control, as the player's key does)
                    // to see the Pip-Boy FOV while setting it. pipBoyMode: InterfaceManager +0x4BC, 0 = put away.
                    BYTE* im = *(BYTE**)0x011D8A80;
                    bool out = im && *(DWORD*)(im + 0x4BC) != 0;
                    float padX = ImGui::GetStyle().FramePadding.x * 2;
                    float btnW = fmaxf(ImGui::CalcTextSize("take out").x, ImGui::CalcTextSize("put away").x) + padX;
                    AngleRow("Pip-Boy FOV", "pipboy", &g_settings.pipboyFov, *(float*)0x01203178 > 0 ? *(float*)0x01203178 : 47.f,
                             kPipboyFovMin, kPipboyFovMax, "The Pip-Boy's field of view: lower makes the Pip-Boy appear bigger.", btnW);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!RestartGameLoaded());
                    if (ImGui::Button(out ? "put away##pipboy" : "take out##pipboy", ImVec2(btnW, 0))) {
                        Log("overlay: Pip-Boy %s", out ? "put away" : "taken out");
                        GameCmdNow("con TapControl 14");  // control 14: the Pip-Boy (MenuMode)
                    }
                    HudTip(out ? "Put the Pip-Boy away." : "Take the Pip-Boy out, to see the Pip-Boy FOV while you set it.");
                    ImGui::EndDisabled();
                }
                ImGui::EndTable();
                HudTableFrame(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), labelW);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Image")) {
            if (ImGui::BeginTable("image", 2, tf)) {
                ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, labelW);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
                int nw = 0, nh = 0, vw = 0, vh = 0;
                bool active = SupersampleSizes(&nw, &nh, &vw, &vh);
                if (!active) { nw = backW; nh = backH; }
                char cur[64];
                FactorLabel(cur, sizeof(cur), g_settings.supersample, nw, nh);
                LabelCell("Supersampling", "Renders at a higher resolution and averages it down to the window: smoother edges "
                                           "and finer detail, at a large cost in speed and video memory. Applies at the next start.");
                if (ImGui::BeginCombo("##ss", cur)) {
                    for (int i = 0; i < kSupersampleStepCount; ++i) {
                        char lab[64];
                        FactorLabel(lab, sizeof(lab), kSupersampleSteps[i], nw, nh);
                        bool sel = fabsf(kSupersampleSteps[i] - g_settings.supersample) < 0.01f;
                        if (ImGui::Selectable(lab, sel)) { g_settings.supersample = kSupersampleSteps[i]; Changed(); }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                if (SupersampleOverridden()) ImGui::TextDisabled("off this start: the game's size was changed by another file");
                else if (active && fabsf((float)vw * vh / ((float)nw * nh) - g_settings.supersample) < 0.05f)
                    ImGui::TextDisabled("rendering at %dx%d", vw, vh);
                else ImGui::TextDisabled("applies at the next start");
                LabelCell("ReShade first", "With ReShade's add-on build: ReShade's effects are applied to the supersampled frame, "
                                           "so they are in the full-size screenshots.");
                if (HudCheckbox("##reshade", &g_settings.reshadeFirst)) Changed();
                ImGui::SameLine();
                int rs = ReshadeState();
                ImGui::TextDisabled("%s", rs == 2 ? "ReShade add-on active" : rs == 1 ? "standard ReShade: no effect" : "ReShade not loaded");
                ImGui::EndTable();
                HudTableFrame(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), labelW);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Screenshots")) {
            if (ImGui::BeginTable("shots", 2, tf)) {
                ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, labelW);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
                LabelCell("Key", "Click, then press the key to use. Esc cancels.");
                char keyName[32];
                KeyName(g_settings.screenshotKey, keyName, sizeof(keyName));
                char btn[64];
                bool capturing = s_capturingKey && s_captureInto == &g_settings.screenshotKey;
                _snprintf(btn, sizeof(btn), capturing ? "press a key...###key" : "%s###key", keyName);
                if (ImGui::Button(btn, ImVec2(-FLT_MIN, 0))) { s_capturingKey = 1; s_captureInto = &g_settings.screenshotKey; }
                LabelCell("Folder", "Where screenshots are saved. %VARIABLES% work, relative paths start at the game folder; "
                                    "empty saves next to the mod's DLL. Press Enter to apply.");
                if (!s_dirEditing) lstrcpynA(s_dirEdit, g_settings.screenshotDir, sizeof(s_dirEdit));
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##dir", "next to the DLL", s_dirEdit, sizeof(s_dirEdit));
                s_dirEditing = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    lstrcpynA(g_settings.screenshotDir, s_dirEdit, sizeof(g_settings.screenshotDir));
                    Changed();
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                if (ImGui::Button("browse")) {
                    char dir[MAX_PATH];
                    ScreenshotFolder(dir);
                    BrowseTo(dir);
                    s_browseOpen = true;
                }
                HudTip("Choose the folder in a list (like ReShade's path picker).");
                ImGui::SameLine();
                if (ImGui::Button("open")) {
                    char dir[MAX_PATH];
                    ScreenshotFolder(dir);
                    ShellExecuteA(nullptr, "open", dir, nullptr, nullptr, SW_SHOWNORMAL);
                }
                HudTip("Open the folder in Explorer.");
                LabelCell("Size", "Full: the size the game renders at (with supersampling, e.g. 11520x6480). Window: the window's size.");
                int full = g_settings.screenshotFull ? 0 : 1;
                if (HudOption("full", full == 0)) { g_settings.screenshotFull = true; Changed(); }
                ImGui::SameLine();
                if (HudOption("window", full == 1)) { g_settings.screenshotFull = false; Changed(); }
                LabelCell("Format", "JPEG: about 5x smaller and much faster to write, visually lossless at quality 90+. "
                                    "PNG: lossless, for editing.");
                int fmt = g_settings.screenshotJpg ? 0 : 1;
                if (HudOption("JPEG", fmt == 0)) { g_settings.screenshotJpg = true; Changed(); }
                ImGui::SameLine();
                if (HudOption("PNG", fmt == 1)) { g_settings.screenshotJpg = false; Changed(); }
                LabelCell("JPEG quality", "1-100. 95 is a good default.");
                ImGui::BeginDisabled(!g_settings.screenshotJpg);
                HudSliderInt("##q", &g_settings.screenshotQuality, 50, 100);
                if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
                ImGui::EndDisabled();
                ImGui::EndTable();
                HudTableFrame(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), labelW);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Other")) {
            if (ImGui::BeginTable("other", 2, tf)) {
                ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, labelW);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
                static const char* kTrap[kMouseTrapModeCount] = {"off", "in gameplay (free in menus)", "always"};
                LabelCell("Mouse trap", "Keeps the mouse inside the window. In gameplay: the mouse is free in menus "
                                        "(and moves the game's cursor directly).");
                if (ImGui::Combo("##trap", &g_settings.mousetrap, kTrap, kMouseTrapModeCount)) Changed();
                LabelCell("Intro videos", "Skip the intro videos at start (applies at the next start).");
                bool skip = g_settings.skipIntros;
                if (HudCheckbox("skip##intros", &skip)) { g_settings.skipIntros = skip; Changed(); }
                LabelCell("Menu key", "The key that opens and closes this menu. Click, then press the key to use. Esc cancels.");
                char menuKey[32], menuBtn[64];
                KeyName(g_settings.menuKey, menuKey, sizeof(menuKey));
                bool capturing = s_capturingKey && s_captureInto == &g_settings.menuKey;
                _snprintf(menuBtn, sizeof(menuBtn), capturing ? "press a key...###menukey" : "%s###menukey", menuKey);
                if (ImGui::Button(menuBtn, ImVec2(-FLT_MIN, 0))) { s_capturingKey = 1; s_captureInto = &g_settings.menuKey; }
                LabelCell("Stats", "A small window with the active lens (mode, the FOV it shows and renders) and the supersampling "
                                   "size, shown also with this menu closed. Drag it to move it while this menu is open.");
                if (HudCheckbox("show##stats", &g_settings.stats)) Changed();
                ImGui::SameLine();
                ImGui::BeginDisabled(!g_settings.stats || g_settings.statsX < 0);
                if (ImGui::Button("reset position")) { g_settings.statsX = g_settings.statsY = -1; s_statsLast = ImVec2(-1, -1); s_statsPlace = true; Changed(); }
                ImGui::EndDisabled();
                HudTip("Back to the top right corner.");
                LabelCell("Stats text size", "The stats window's text size, in percent of this menu's.");
                ImGui::BeginDisabled(!g_settings.stats);
                HudSliderInt("##statssize", &g_settings.statsSize, 50, 200, "%d%%");
                if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
                ImGui::EndDisabled();
                ImGui::EndTable();
                HudTableFrame(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), labelW);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    HudSeparator();
    DrawRestart();
    DrawBrowser();
    char closeKey[32];
    KeyName(g_settings.menuKey, closeKey, sizeof(closeKey));
    ImGui::TextDisabled("%s closes. Changes apply at once and are saved.", closeKey);
    HudWindowFrame();
    ImGui::End();
}

// The screenshot or menu key: the first key pressed after "press a key..." (Esc cancels; the
// other of the two keys is refused, so the screenshot key never toggles the menu).
static void CaptureKey()
{
    if (!s_capturingKey) return;
    if (s_capturingKey == 1) {  // wait until the click's mouse button and all keys are up
        for (int vk = 1; vk < 256; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000) return;
        s_capturingKey = 2;
        return;
    }
    for (int vk = 8; vk < 256; ++vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU)) continue;
        if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
        int other = s_captureInto == &g_settings.menuKey ? g_settings.screenshotKey : g_settings.menuKey;
        if (vk != VK_ESCAPE && vk != other) {
            *s_captureInto = vk;
            if (s_captureInto == &g_settings.menuKey) s_menuKeyDown = true;  // still held: not a press yet
            Changed();
        }
        s_capturingKey = 0;
        return;
    }
}

// ---- interface ---------------------------------------------------------------------------------

void OverlayCommand(const char* arg)
{
    if (!lstrcmpiA(arg, "open")) s_open = true;
    else if (!lstrcmpiA(arg, "close")) s_open = false;
    else Log("overlay: unknown command '%s' (open, close)", arg);
}

bool OverlayOpen() { return s_open; }

bool OverlayWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (!s_ready || !s_open) return false;
    ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
    return false;
}

void OverlayLost()
{
    if (s_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void OverlayPresent(IDirect3DDevice9* dev, IDirect3DSurface9* target)
{
    bool down = GameInFront() && (GetAsyncKeyState(g_settings.menuKey) & 0x8000) != 0;
    if (down && !s_menuKeyDown && !s_capturingKey) s_open = !s_open;
    s_menuKeyDown = down;
    if (s_startSkipIntros < 0) s_startSkipIntros = g_settings.skipIntros ? 1 : 0;
    if (!s_open) s_capturingKey = 0;
    if ((!s_open && !g_settings.stats) || !target || s_failed) {
        if (s_dirty) { SettingsSave(); s_dirty = false; }
        return;
    }
    D3DSURFACE_DESC d;
    if (FAILED(target->GetDesc(&d))) return;
    D3DScope scope;
    if (!s_ready) {
        s_ready = Init(dev, (int)d.Height);
        if (!s_ready) { s_failed = true; return; }
    }
    float px = floorf(d.Height / 45.f + 0.5f);
    if (px < 14) px = 14;
    if (px > 64) px = 64;
    float c[3];
    MenuColor(c);
    if (!HudStyleIs(px, c)) HudApplyStyle(px, c);
    CaptureKey();
    ImGui::GetIO().MouseDrawCursor = s_open;  // the stats window alone takes no mouse
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (g_settings.stats) DrawStats(px, (int)d.Width, (int)d.Height);
    if (s_open) DrawMenu((int)d.Width, (int)d.Height);
    ImGui::EndFrame();
    ImGui::Render();
    if (s_dirty && !ImGui::IsAnyItemActive()) { SettingsSave(); s_dirty = false; }

    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    D3DVIEWPORT9 oldVp;
    dev->GetViewport(&oldVp);
    dev->SetRenderTarget(0, target);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.f, 1.f};
    dev->SetViewport(&vp);
    if (SUCCEEDED(dev->BeginScene())) {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());  // saves and restores the device state itself
        dev->EndScene();
    }
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    dev->SetViewport(&oldVp);
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
}
