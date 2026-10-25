// The parts of the xNVSE plugin API this mod uses (layouts from xNVSE nvse/PluginAPI.h,
// https://github.com/xNVSE/NVSE). Only the leading fields that are read are declared.
#pragma once
#include <windows.h>

typedef UINT32 PluginHandle;

struct NVSEInterface {
    UINT32 nvseVersion;
    UINT32 runtimeVersion;
    UINT32 editorVersion;
    UINT32 isEditor;
    void* RegisterCommand;
    void* SetOpcodeBase;
    void* (*QueryInterface)(UINT32 id);
    PluginHandle (*GetPluginHandle)(void);
    void* RegisterTypedCommand;
    const char* (*GetRuntimeDirectory)();
    UINT32 isNogore;
};

struct PluginInfo {
    enum { kInfoVersion = 1 };
    UINT32 infoVersion;
    const char* name;
    UINT32 version;
};

struct NVSEMessagingInterface {
    struct Message {
        const char* sender;
        UINT32 type;
        UINT32 dataLen;
        void* data;
    };
    typedef void (*EventCallback)(Message* msg);
    enum {
        kMessage_PostLoad = 0,
        kMessage_ExitGame = 1,
        kMessage_ExitToMainMenu = 2,
        kMessage_LoadGame = 3,
        kMessage_SaveGame = 4,
        kMessage_ExitGame_Console = 7,
        kMessage_PostLoadGame = 8,
        kMessage_PostPostLoad = 9,
        kMessage_NewGame = 14,
        kMessage_DeferredInit = 18,
        kMessage_MainGameLoop = 20,
        kMessage_OnFramePresent = 24,
    };
    UINT32 version;
    bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
    bool (*Dispatch)(PluginHandle sender, UINT32 messageType, void* data, UINT32 dataLen, const char* receiver);
};

enum { kInterface_Console = 1, kInterface_Messaging = 2, kInterface_Script = 6 };
static const UINT32 kRuntime_1_4_0_525 = 0x040020D0;
