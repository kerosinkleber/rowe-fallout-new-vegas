// Code patching primitives. Every patch checks the image base and the bytes it replaces, so a
// different exe build (or a site another mod already owns) fails safe and is logged.
#include <windows.h>
#include <string.h>
#include "fnvfov.h"

// Every patch attempt and its result, for rowe_state.txt.
struct PatchStatus { const char* name; DWORD va; const char* result; };
static PatchStatus s_patches[64];
static int s_patchCount;

static void Record(const char* name, DWORD va, const char* result)
{
    if (s_patchCount < 64) s_patches[s_patchCount++] = {name, va, result};
}

void PatchWriteStatus(FILE* f)
{
    fprintf(f, "patches:\n");
    for (int i = 0; i < s_patchCount; ++i) fprintf(f, "  %08lx %-44s %s\n", s_patches[i].va, s_patches[i].name, s_patches[i].result);
}

static bool ImageBaseOk(const char* name)
{
    if ((DWORD)GetModuleHandleA(nullptr) == 0x00400000) return true;
    Log("%s: skipped, unexpected image base %p", name, GetModuleHandleA(nullptr));
    Record(name, 0, "SKIPPED (image base)");
    return false;
}

static void WriteCode(BYTE* at, const BYTE* bytes, int len)
{
    DWORD old;
    VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, len);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
}

bool ExeCodeRange(DWORD* begin, DWORD* end)
{
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            *begin = (DWORD)base + s->VirtualAddress;
            *end = *begin + s->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// The slot is found through the import names (OriginalFirstThunk), not by comparing against
// GetProcAddress, so a slot another mod already replaced is still found and chained.
void* PatchExeImport(const char* dll, const char* name, void* hook)
{
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (lstrcmpiA((char*)(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        void** slots = (void**)(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
            if (strcmp((char*)ibn->Name, name) != 0) continue;
            void* prev = slots[i];
            DWORD old;
            VirtualProtect(&slots[i], sizeof(void*), PAGE_READWRITE, &old);
            slots[i] = hook;
            VirtualProtect(&slots[i], sizeof(void*), old, &old);
            void* real = (void*)GetProcAddress(GetModuleHandleA(dll), name);
            Log("import %s!%s: hooked (slot %p, previous %p%s)", dll, name, &slots[i], prev,
                prev == real ? "" : ", not the system function: chained");
            Record(name, (DWORD)&slots[i], prev == real ? "ok (import)" : "ok (import, chained)");
            return prev;
        }
    }
    Log("import %s!%s: not found", dll, name);
    Record(name, 0, "NOT FOUND (import)");
    return nullptr;
}

// Detours the exe function at targetVA. `expected` holds its first `len` bytes (whole,
// position-independent instructions); nothing is patched unless they match. The trampoline runs
// the copied bytes and jumps back to targetVA + len, i.e. calling it runs the original function.
void* InstallDetour(DWORD targetVA, const BYTE* expected, int len, void* hook, const char* name)
{
    if (!ImageBaseOk(name)) return nullptr;
    BYTE* target = (BYTE*)targetVA;
    if (len < 5 || IsBadReadPtr(target, len) || memcmp(target, expected, len) != 0) {
        Log("%s: skipped, bytes at %08lx do not match (first byte %02x; other mod or exe build?)",
            name, targetVA, IsBadReadPtr(target, 1) ? 0 : target[0]);
        Record(name, targetVA, "SKIPPED (bytes differ)");
        return nullptr;
    }
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, target, len);
    tramp[len] = 0xE9;
    *(int*)(tramp + len + 1) = (int)((target + len) - (tramp + len + 5));

    BYTE patch[32];
    patch[0] = 0xE9;
    *(int*)(patch + 1) = (int)((BYTE*)hook - (target + 5));
    for (int i = 5; i < len; ++i) patch[i] = 0x90;
    WriteCode(target, patch, len);
    Log("%s: detour installed at %08lx", name, targetVA);
    Record(name, targetVA, "ok (detour)");
    return tramp;
}

// Replaces `len` bytes of whole instructions at siteVA (checked against `expected`) with a call
// to fn followed by NOPs. fn runs in the middle of the engine function, with its registers and
// frame (a naked function that preserves what the replaced code preserved).
bool PatchCallOver(DWORD siteVA, const BYTE* expected, int len, void* fn, const char* name)
{
    if (!ImageBaseOk(name)) return false;
    BYTE* site = (BYTE*)siteVA;
    if (len < 5 || len > 64 || IsBadReadPtr(site, len) || memcmp(site, expected, len) != 0) {
        Log("%s: skipped, bytes at %08lx do not match (first byte %02x)", name, siteVA, IsBadReadPtr(site, 1) ? 0 : site[0]);
        Record(name, siteVA, "SKIPPED (bytes differ)");
        return false;
    }
    BYTE patch[64];
    patch[0] = 0xE8;
    *(int*)(patch + 1) = (int)((BYTE*)fn - (site + 5));
    for (int i = 5; i < len; ++i) patch[i] = 0x90;
    WriteCode(site, patch, len);
    Log("%s: code at %08lx replaced (%d bytes)", name, siteVA, len);
    Record(name, siteVA, "ok (code)");
    return true;
}

void* PatchCall(DWORD siteVA, DWORD expectedTarget, void* hook, const char* name)
{
    if (!ImageBaseOk(name)) return nullptr;
    BYTE* site = (BYTE*)siteVA;
    if (IsBadReadPtr(site, 5)) return nullptr;
    if (site[0] != 0xE8) {
        Log("%s: skipped, %08lx holds %02x, not a call (%s)", name, siteVA, site[0],
            site[0] == 0xE9 ? "another mod jump-hooked it" : "different exe build?");
        Record(name, siteVA, site[0] == 0xE9 ? "SKIPPED (jump hook present)" : "SKIPPED (not a call)");
        return nullptr;
    }
    DWORD current = siteVA + 5 + *(int*)(site + 1);
    BYTE patch[5] = {0xE8};
    *(int*)(patch + 1) = (int)((BYTE*)hook - (site + 5));
    WriteCode(site, patch, 5);
    if (current == expectedTarget) Log("%s: call at %08lx hooked", name, siteVA);
    else Log("%s: call at %08lx hooked, chaining to %08lx (vanilla target %08lx; another mod's hook)",
             name, siteVA, current, expectedTarget);
    Record(name, siteVA, current == expectedTarget ? "ok (call)" : "ok (call, chained to another mod)");
    return (void*)current;
}
