// Per-frame work at Present and on device loss, in a fixed order: supersampling resolve (the
// frame reaches the real back buffer), screenshot (finished frame, no menu), settings menu, mouse
// trap. Also the mod's section of rowe_state.txt.
#include <windows.h>
#include "fnvfov.h"

void FramePresent(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(D3DRealBackBuffer(&bb)) || !bb) return;
    // The frame the engine rendered (the virtual back buffer with supersampling).
    IDirect3DSurface9* engineFrame = nullptr;
    if (!SupersampleBackBuffer(&engineFrame)) {
        engineFrame = bb;
        bb->AddRef();
    }
    LensPresent(dev, engineFrame);
    if (engineFrame != bb) ReshadeRenderInto(dev, engineFrame);  // ReShade's effects at the full size, before the resolve
    engineFrame->Release();
    SupersampleResolve(dev, bb);
    ScreenshotPresent(dev, bb);
    OverlayPresent(dev, bb);
    ScreenshotAfterOverlay(dev, bb);
    bb->Release();
    MouseTrapFrame();
    UiFrame();
}

void FrameDeviceLost()
{
    OverlayLost();
    ScreenshotLost();
    LensLost();
    SupersampleLost();
}

void ModWriteState(FILE* f)
{
    fprintf(f, "mod: fov %g lens %d pipboyfov %g supersample %g mousetrap %d skipintros %d scopelens %d | overlay %s\n",
            g_settings.fov, g_settings.lens, g_settings.pipboyFov, g_settings.supersample, g_settings.mousetrap, g_settings.skipIntros,
            g_settings.scopeLens, OverlayOpen() ? "open" : "closed");
    FovWriteState(f);
    LensWriteState(f);
    SupersampleWriteState(f);
    ReshadeWriteState(f);
    MouseTrapWriteState(f);
    DinputWriteState(f);
    UiWriteState(f);
    IntrosWriteState(f);
}
