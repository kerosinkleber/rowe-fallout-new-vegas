// ROWE as a provider for fnvbridge, Claude Code's bridge to the game (B:\fnvbridge, design section
// 5): ROWE's settings and rowe_state.txt in the bridge's get_state, its commands as MCP tools
// (rowe_set, rowe_reload, ...), and capture points for screenshots inside its pipeline. Without
// fnvbridge installed the bridge never calls in and nothing here runs.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fnvfov.h"
#include "nvse.h"
#include "fnvbridge_api.h"

static const FNVBridgeAPI* s_api;

// The capture points, in frame order: before the lens warp, the scene before the HUD (after the
// lens and the supersampling downscale), the presented frame (before the settings overlay), and
// the frame with the settings overlay.
static const char* kPoints[kBridgeCaptureCount] = {"prelens", "scene", "final", "overlay"};
static volatile LONG s_want[kBridgeCaptureCount];

// --- arguments: the bridge sends an object of strings and numbers ---

static bool ArgString(const char* json, const char* key, char* out, int size)
{
    char pattern[64];
    _snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    int n = 0;
    for (; *p && *p != '"' && n < size - 1; ++p) out[n++] = *p == '\\' && p[1] ? *++p : *p;
    out[n] = 0;
    return true;
}

static bool ArgNumber(const char* json, const char* key, double* out)
{
    char pattern[64];
    _snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    char* end;
    *out = strtod(p + strlen(pattern), &end);
    return end != p + strlen(pattern);
}

// --- state ---

static void WriteState(FNVBridgeJson* j)
{
    void* c = j->ctx;
    j->begin_object(c);
    j->key(c, "frame"), j->num(c, FrameCount());
    j->key(c, "settings");
    j->begin_object(c);
    j->key(c, "fov"), j->num(c, g_settings.fov);
    j->key(c, "lens"), j->num(c, g_settings.lens);
    j->key(c, "pipboyfov"), j->num(c, g_settings.pipboyFov);
    j->key(c, "zoomfov"), j->num(c, g_settings.zoomFov);
    j->key(c, "supersample"), j->num(c, g_settings.supersample);
    j->key(c, "pixelsize"), j->num(c, g_settings.pixelSize);
    j->key(c, "pixelhud"), j->boolean(c, g_settings.pixelHud);
    j->key(c, "mousetrap"), j->num(c, g_settings.mousetrap);
    j->key(c, "scopelens"), j->boolean(c, g_settings.scopeLens);
    j->key(c, "fisheyecircle"), j->boolean(c, g_settings.fisheyeCircle);
    j->key(c, "skipintros"), j->boolean(c, g_settings.skipIntros);
    j->end_object(c);
    // rowe_state.txt (written four times a second): hook counters, patch results, gate decisions.
    j->key(c, "state_file");
    j->begin_array(c);
    char path[MAX_PATH];
    wsprintfA(path, "%srowe_state.txt", ModDir());
    FILE* f = fopen(path, "r");
    if (f) {
        char line[1024];
        for (int n = 0; n < 400 && fgets(line, sizeof(line), f); ++n) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            j->str(c, line);
        }
        fclose(f);
    }
    j->end_array(c);
    j->end_object(c);
}

// --- commands: queued for ROWE's command runner, which runs them on the next tick ---

static int Queue(const char* line, FNVBridgeJson* out)
{
    GameCmdNow(line);
    void* c = out->ctx;
    out->begin_object(c);
    out->key(c, "queued"), out->str(c, line);
    out->end_object(c);
    return 1;
}

static int CmdSet(const char* args, FNVBridgeJson* out, char* error, int size)
{
    static const char* kKeys[] = {"fov", "lens", "pipboyfov", "zoomfov", "supersample", "pixelsize", "pixelhud", "mousetrap", "skipintros",
                                  "scopelens", "fisheyecircle"};
    char key[32];
    double value;
    if (!ArgString(args, "key", key, sizeof(key)) || !ArgNumber(args, "value", &value)) {
        _snprintf(error, size, "set needs key (a string) and value (a number)");
        return 0;
    }
    bool known = false;
    for (const char* k : kKeys) known |= !lstrcmpiA(k, key);
    if (!known) {
        _snprintf(error, size, "unknown key %s (fov lens pipboyfov zoomfov supersample pixelsize pixelhud mousetrap skipintros scopelens fisheyecircle)", key);
        return 0;
    }
    char line[96];
    _snprintf(line, sizeof(line), "set %s %g", key, value);
    line[sizeof(line) - 1] = 0;
    return Queue(line, out);
}

static int CmdReload(const char*, FNVBridgeJson* out, char*, int) { return Queue("reload", out); }
static int CmdPhoto(const char*, FNVBridgeJson* out, char*, int) { return Queue("photo", out); }
static int CmdDeviceLost(const char*, FNVBridgeJson* out, char*, int) { return Queue("devicelost", out); }

// Any line of ROWE's command runner (overlay, wheel, trace, cursor, wincursor, consolepos, nodes,
// restart, usershot, ...), queued as is.
static int CmdLine(const char* args, FNVBridgeJson* out, char* error, int size)
{
    char line[256];
    if (!ArgString(args, "line", line, sizeof(line)) || !line[0]) {
        _snprintf(error, size, "line is required");
        return 0;
    }
    return Queue(line, out);
}

static int CmdFovTrace(const char* args, FNVBridgeJson* out, char*, int)
{
    double frames = 300;
    ArgNumber(args, "frames", &frames);
    char line[48];
    _snprintf(line, sizeof(line), "fovtrace %d", (int)frames);
    line[sizeof(line) - 1] = 0;
    return Queue(line, out);
}

static const FNVBridgeCommand kCommands[] = {
    {"set",
     "Change a ROWE setting and save it to rowe.ini, as the settings menu does: fov (60-160, 0 = game default), lens (0 off, 1 Panini, ...), pipboyfov, "
     "zoomfov, mousetrap, scopelens, fisheyecircle; supersample, pixelsize and skipintros apply at the next start. Replies with "
     "ROWE's state three frames later.",
     "{\"key\": {\"type\": \"string\"}, \"value\": {\"type\": \"number\"}}", 3, CmdSet},
    {"reload", "Read rowe.ini again. Replies with ROWE's state three frames later.", nullptr, 3, CmdReload},
    {"photo", "Turn photo mode on or off. Replies with ROWE's state ten frames later.", nullptr, 10, CmdPhoto},
    {"fovtrace", "Record every SetCameraFOV call for some frames to rowe_fovtrace.csv.", "{\"frames\": {\"type\": \"integer\"}}", 1, CmdFovTrace},
    {"devicelost", "Simulate a lost D3D device (tests ROWE's Reset handling).", nullptr, 5, CmdDeviceLost},
    {"cmd",
     "Run a line of ROWE's own command runner: overlay open|close|tab <name>, wheel <delta>, trace (one-frame D3D9 trace), "
     "cursor <x> <y>, wincursor <x> <y>, consolepos <x> <y>, nodes <name> <depth>, usershot, restart. Replies two frames later.",
     "{\"line\": {\"type\": \"string\"}}", 2, CmdLine},
};

static int RequestCapture(const char* point)
{
    for (int i = 0; i < kBridgeCaptureCount; ++i)
        if (!strcmp(point, kPoints[i])) {
            InterlockedExchange(&s_want[i], 1);
            return 1;
        }
    return 0;
}

static const FNVBridgeProvider kProvider = {
    FNVBRIDGE_API_VERSION, "rowe", WriteState, kCommands, sizeof(kCommands) / sizeof(*kCommands), kPoints, kBridgeCaptureCount, RequestCapture,
};

// --- capture points, called from ROWE's pipeline ---

void BridgeCaptureSurface(int point, IDirect3DDevice9* dev, IDirect3DSurface9* surface)
{
    if (!s_api || point < 0 || point >= kBridgeCaptureCount || !s_want[point]) return;
    InterlockedExchange(&s_want[point], 0);
    s_api->deliver_capture("rowe", kPoints[point], dev, surface);
}

void BridgeCaptureRT(int point, IDirect3DDevice9* dev)
{
    if (!s_api || point < 0 || point >= kBridgeCaptureCount || !s_want[point]) return;
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) {
        BridgeCaptureSurface(point, dev, rt);
        rt->Release();
    }
}

static void OnBridgeMessage(NVSEMessagingInterface::Message* msg)
{
    if (msg->type != FNVBRIDGE_MESSAGE_API || !msg->data) return;
    const FNVBridgeAPI* api = (const FNVBridgeAPI*)msg->data;
    if (api->version != FNVBRIDGE_API_VERSION) {
        Log("bridge: fnvbridge API version %u, ROWE speaks %u: not registered", api->version, FNVBRIDGE_API_VERSION);
        return;
    }
    if (api->register_provider(&kProvider)) {
        s_api = api;
        Log("bridge: registered with fnvbridge as provider \"rowe\"");
    }
}

void BridgeProviderInit(void* messaging, UINT32 handle)
{
    NVSEMessagingInterface* m = (NVSEMessagingInterface*)messaging;
    if (m) m->RegisterListener(handle, "fnvbridge", OnBridgeMessage);
}
