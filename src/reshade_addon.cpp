// ReShade: with supersampling, ReShade only sees the window-sized back buffer, after the mod has
// averaged the frame down, so its effects are missing from the mod's full-size screenshots. When
// ReShade is loaded with full add-on support, the mod registers as a ReShade add-on and has ReShade
// render its effects into the supersampled frame at Present, before the resolve and the screenshot;
// ReShade then skips its own pass for that frame (effect_runtime::render_effects). rowe.ini
// reshadefirst=0 turns this off. ReShade's standard build refuses add-on events ("limited add-on
// functionality"): then nothing changes.
#include <windows.h>
#include <d3d9.h>
#include <reshade.hpp>
#include "fnvfov.h"

static bool s_registered, s_loaded;
static reshade::api::effect_runtime* s_runtime;
static DWORD s_renderedFrames;

static void OnInitRuntime(reshade::api::effect_runtime* rt)
{
    s_runtime = rt;
    uint32_t w = 0, h = 0;
    rt->get_screenshot_width_and_height(&w, &h);
    Log("reshade: effect runtime %p ready (back buffer %ux%u)", rt, w, h);
}

static void OnDestroyRuntime(reshade::api::effect_runtime* rt)
{
    if (s_runtime == rt) s_runtime = nullptr;
    Log("reshade: effect runtime %p gone", rt);
}

void ReshadeAddonInit()
{
    HMODULE reshade = reshade::internal::get_reshade_module_handle();
    if (!reshade) {
        Log("reshade: not loaded");
        return;
    }
    s_loaded = true;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ReshadeAddonInit, &self);
    if (!reshade::register_addon(self, reshade)) {
        Log("reshade: found (%p), registering as an add-on failed (API version %d)", reshade, RESHADE_API_VERSION);
        return;
    }
    s_registered = true;
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitRuntime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
    Log("reshade: registered as an add-on (%p); effects are rendered into the supersampled frame once the runtime exists%s", reshade,
        g_settings.reshadeFirst ? "" : " (off: reshadefirst=0)");
}

bool ReshadeRenderInto(IDirect3DDevice9* dev, IDirect3DSurface9* target)
{
    if (!s_runtime || !g_settings.reshadeFirst || !target) return false;
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return false;
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    dev->GetRenderTarget(0, &rt);
    dev->GetDepthStencilSurface(&ds);
    reshade::api::command_list* cmd = s_runtime->get_command_queue()->get_immediate_command_list();
    reshade::api::resource_view view = {(uint64_t)(uintptr_t)target};  // D3D9: views are the surfaces
    s_runtime->render_effects(cmd, view, view);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(ds);
    sb->Apply();
    sb->Release();
    if (rt) rt->Release();
    if (ds) ds->Release();
    if (++s_renderedFrames == 1) {
        D3DSURFACE_DESC d;
        target->GetDesc(&d);
        Log("reshade: first frame with effects rendered into the %ux%u frame", d.Width, d.Height);
    }
    return true;
}

int ReshadeState() { return s_registered && s_runtime ? 2 : s_loaded ? 1 : 0; }

void ReshadeWriteState(FILE* f)
{
    fprintf(f, "reshade: add-on %d, runtime %p, reshadefirst %d, frames rendered %lu\n", s_registered ? 1 : 0, s_runtime,
            g_settings.reshadeFirst ? 1 : 0, s_renderedFrames);
}
