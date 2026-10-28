// fnvbridge provider API, version 1 (design section 5). Copy this header into a plugin that wants to
// give Claude Code's bridge its own state, commands and capture points.
//
// How a plugin becomes a provider:
//   1. In NVSEPlugin_Load: messaging->RegisterListener(handle, "fnvbridge", OnBridgeMessage).
//   2. At the bridge's PostLoad, OnBridgeMessage gets a message of type FNVBRIDGE_MESSAGE_API whose
//      data is a const FNVBridgeAPI*. Call api->register_provider(&provider) there. The provider
//      struct and everything it points to must stay valid for the whole game.
//   3. Without fnvbridge installed nothing arrives and nothing else is needed.
//
// Everything is plain C (no exceptions, no STL), so any compiler works. All callbacks run on the
// game's main thread.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct IDirect3DDevice9;
struct IDirect3DSurface9;

#define FNVBRIDGE_API_VERSION 1
#define FNVBRIDGE_MESSAGE_API 0x46425231  // 'FBR1'

// A JSON writer. Write exactly one value per call you get one in; commas are added for you.
typedef struct FNVBridgeJson {
    void* ctx;
    void (*begin_object)(void* ctx);
    void (*end_object)(void* ctx);
    void (*begin_array)(void* ctx);
    void (*end_array)(void* ctx);
    void (*key)(void* ctx, const char* name);  // inside an object, before each value
    void (*str)(void* ctx, const char* text);
    void (*num)(void* ctx, double value);
    void (*boolean)(void* ctx, int value);
    void (*null_value)(void* ctx);
} FNVBridgeJson;

typedef struct FNVBridgeCommand {
    const char* name;         // the MCP tool becomes "<provider>_<name>"
    const char* description;  // shown to Claude
    const char* args_schema;  // JSON schema "properties" object of the arguments, or NULL
    int settle_ticks;         // the bridge replies this many frames after the handler, with the provider's state
    // args_json: the arguments as a JSON object. Write one value (the result) to out and return 1,
    // or fill error and return 0.
    int (*handler)(const char* args_json, FNVBridgeJson* out, char* error, int error_size);
} FNVBridgeCommand;

typedef struct FNVBridgeProvider {
    unsigned version;  // FNVBRIDGE_API_VERSION
    const char* name;  // short, lower case: "rowe"
    // Writes the provider's state as one value (an object). Called about four times a second.
    void (*write_state)(FNVBridgeJson* out);
    const FNVBridgeCommand* commands;
    unsigned command_count;
    const char* const* capture_points;  // "prelens", "scene", ...: screenshot point "<provider>.<point>"
    unsigned capture_point_count;
    // A screenshot at this point is wanted: return 1, then call api->deliver_capture when the
    // frame passes that point (normally in the next frame).
    int (*request_capture)(const char* point);
} FNVBridgeProvider;

typedef struct FNVBridgeAPI {
    unsigned version;
    int (*register_provider)(const FNVBridgeProvider* provider);  // 1: registered
    // An event in the bridge's journal, as "<provider>.<type>"; data_json is an object's text or NULL.
    void (*emit_event)(const char* provider, const char* type, const char* data_json);
    void (*warn)(const char* text);  // added to the reply of the command being run
    // The frame at a capture point: the bridge copies the surface at once (render target or back
    // buffer, any size, MSAA or not, X8R8G8B8 or A8R8G8B8).
    void (*deliver_capture)(const char* provider, const char* point, IDirect3DDevice9* device, IDirect3DSurface9* surface);
} FNVBridgeAPI;

#ifdef __cplusplus
}
#endif
