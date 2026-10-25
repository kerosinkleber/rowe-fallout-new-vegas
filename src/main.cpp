// ROWE: xNVSE plugin for Fallout: New Vegas (FalloutNV.exe 1.4.0.525): FOV slider, lens
// correction, supersampling (also windowed), mouse trap, intro skip, settings menu, screenshots.
// Loaded by xNVSE from Data\NVSE\Plugins. Its files (rowe.log, rowe_state.txt, request files)
// live next to the DLL.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "fnvfov.h"
#include "nvse.h"

static char g_dir[MAX_PATH];
static PluginHandle g_handle;
static NVSEMessagingInterface* g_messaging;
static CRITICAL_SECTION g_logLock;

// The file is opened for every line, so everything logged before a crash is on disk.
void Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe.log", g_dir);
    EnterCriticalSection(&g_logLock);
    FILE* f = fopen(path, "a");
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

const char* ModDir() { return g_dir; }

// OSGlobals (*0x011DEA0C): window at +0x08, main thread id at +0x10 (xNVSE GameOSDepend.h).
DWORD MainThreadId()
{
    BYTE* os = *(BYTE**)0x011DEA0C;
    return os ? *(DWORD*)(os + 0x10) : 0;
}

HWND GameWindow()
{
    BYTE* os = *(BYTE**)0x011DEA0C;
    return os ? *(HWND*)(os + 0x08) : nullptr;
}

// No menu, not loading, a player, and the world was drawn in the last few frames.
bool InGameplay()
{
    BYTE* im = *(BYTE**)0x011D8A80;  // InterfaceManager: currentMode +0x0C (1 = gameplay)
    typedef bool(__cdecl * IsMenuModeFn)();
    return im && *(DWORD*)(im + 0x0C) == 1 && !((IsMenuModeFn)0x00702360)() && !((BYTE*)0x011F308F)[1007] &&
           *(BYTE**)0x011DEA3C && FrameCount() - LastWorldDrawFrame() < 5;
}

bool GameInFront()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void OnMessage(NVSEMessagingInterface::Message* msg)
{
    switch (msg->type) {
    case NVSEMessagingInterface::kMessage_PostLoad: Log("message: PostLoad"); break;
    case NVSEMessagingInterface::kMessage_PostPostLoad: Log("message: PostPostLoad"); break;
    case NVSEMessagingInterface::kMessage_DeferredInit:
        Log("message: DeferredInit");
        ReconEngineDeferredInit();
        break;
    case NVSEMessagingInterface::kMessage_MainGameLoop: ReconEngineMainLoop(); break;
    case NVSEMessagingInterface::kMessage_PostLoadGame: AutosaveReset("game loaded"); break;
    case NVSEMessagingInterface::kMessage_NewGame: AutosaveReset("new game"); break;
    case NVSEMessagingInterface::kMessage_SaveGame: AutosaveReset("game saved"); break;
    case NVSEMessagingInterface::kMessage_ExitGame:
    case NVSEMessagingInterface::kMessage_ExitGame_Console:
        Log("message: ExitGame%s", msg->type == NVSEMessagingInterface::kMessage_ExitGame_Console ? " (console qqq)" : "");
        FovTraceFinish();
        break;
    }
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name = "ROWE";
    info->version = 1;
    if (nvse->isEditor) return false;
    Log("Query: xNVSE %08x, runtime %08x, nogore %u", nvse->nvseVersion, nvse->runtimeVersion, nvse->isNogore);
    if (nvse->runtimeVersion != kRuntime_1_4_0_525 || nvse->isNogore) {
        Log("Query: unsupported runtime, not loading");
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
    Log("Load: begin");
    SettingsLoad();
    SupersampleInit();
    IntrosInit();
    g_handle = nvse->GetPluginHandle();
    g_messaging = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
    if (g_messaging) g_messaging->RegisterListener(g_handle, "NVSE", OnMessage);
    else Log("Load: no messaging interface");
    GameCmdInit(nvse->QueryInterface(kInterface_Console));
    McmInit(nvse->QueryInterface(kInterface_Script));
    RestartAtLoad();
    ReconEngineAtLoad();
    ReconWin32Install();
    DinputInstall();
    UiInstall();
    ReconEngineInstall();
    ReshadeAddonInit();
    Log("Load: done");
    return true;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logLock);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        char* slash = strrchr(g_dir, '\\');
        if (slash) slash[1] = 0;
        // One log per start: the previous one is kept as rowe.log.1.
        char log[MAX_PATH], prev[MAX_PATH];
        wsprintfA(log, "%srowe.log", g_dir);
        wsprintfA(prev, "%srowe.log.1", g_dir);
        MoveFileExA(log, prev, MOVEFILE_REPLACE_EXISTING);
        Log("---- ROWE loaded ----");
        CrashLogInstall(inst);
    }
    return TRUE;
}
