"""Static checks of FalloutNV.exe against the addresses the mod relies on.

Usage: python -I tools/pecheck.py "<path to FalloutNV.exe>"
Prints the PE layout, the imports the mod hooks and the bytes at each patch site.
"""
import struct
import sys


def main(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", data, opt + 28)[0]
    dllchar = struct.unpack_from("<H", data, opt + 70)[0]
    print(f"image base {base:#x}  dll characteristics {dllchar:#x}"
          f" (dynamic base {'on' if dllchar & 0x40 else 'off'}, large address aware "
          f"{'on' if struct.unpack_from('<H', data, pe + 22)[0] & 0x20 else 'off'})")
    secs = []
    for i in range(nsec):
        s = pe + 24 + optsz + 40 * i
        name = data[s:s + 8].rstrip(b"\0").decode(errors="replace")
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", data, s + 8)
        secs.append((name, va, vsz, rptr, rsz))
        print(f"  section {name:8} va {base + va:#010x} vsize {vsz:#x} raw {rptr:#x}/{rsz:#x}")

    def off(rva):
        for name, va, vsz, rptr, rsz in secs:
            if va <= rva < va + max(vsz, rsz):
                return rptr + rva - va
        return None

    def at(va, n):
        o = off(va - base)
        return data[o:o + n] if o is not None else None

    def cstr(rva):
        o = off(rva)
        return data[o:data.index(b"\0", o)].decode()

    imp_rva = struct.unpack_from("<I", data, opt + 104)[0]
    wanted = {"Direct3DCreate9", "Direct3DCreate9Ex", "DirectInput8Create", "CreateWindowExA",
              "CreateWindowExW", "SetWindowPos", "AdjustWindowRect", "AdjustWindowRectEx",
              "MoveWindow", "ClipCursor", "GetClientRect", "GetWindowRect", "GetSystemMetrics",
              "GetPrivateProfileStringA", "GetPrivateProfileIntA", "WritePrivateProfileStringA",
              "ShowCursor", "SetCursorPos", "GetCursorPos", "LoadLibraryA", "GetProcAddress",
              "ChangeDisplaySettingsA", "EnumDisplaySettingsA"}
    print("imports of interest (IAT slot address):")
    d = off(imp_rva)
    while True:
        oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", data, d)
        if not name_rva:
            break
        dll = cstr(name_rva)
        t = off(oft or ft)
        i = 0
        while True:
            thunk = struct.unpack_from("<I", data, t + 4 * i)[0]
            if not thunk:
                break
            if not thunk & 0x80000000:
                fn = cstr(thunk + 2)
                if fn in wanted or dll.lower().startswith("d3d9"):
                    print(f"  {dll:14} {fn:28} {base + ft + 4 * i:#010x}")
            i += 1
        d += 20

    # Patch sites from the research report (expected bytes where the notes give them).
    sites = [
        ("xNVSE main-loop hook site (mov eax,[0x11F4748])", 0x0086B386, 5),
        ("SetCameraFOV prologue", 0x00C52020, 16),
        ("SetCameraFOV companion 0xB54000", 0x00B54000, 16),
        ("call InterfaceManager::Render (lens site)", 0x007144D3, 5),
        ("post-image-space call (JG fallback)", 0x008703F1, 5),
        ("image-space + splatter call (RTM)", 0x00870994, 5),
        ("CreateDevice call site", 0x00E731FF, 6),
        ("main loop SetCameraFOV call", 0x0086EB8B, 5),
        ("RenderWorldSceneGraph call 1", 0x00870AE8, 5),
        ("RenderWorldSceneGraph call 2", 0x00870E18, 5),
        ("WorldToScreen SetCameraFOV call", 0x00C5257F, 5),
        ("screenshot popup call", 0x00878E5C, 5),
        ("OnFramePresent call (main loop)", 0x0087055E, 5),
        ("double 0.75 constant", 0x0101DE30, 8),
        ("IsMenuMode", 0x00702360, 8),
        ("Main::Render", 0x008706B0, 8),
        ("InterfaceManager::Render", 0x007134D0, 8),
    ]
    print("patch sites:")
    for label, va, n in sites:
        b = at(va, n)
        desc = b.hex(" ") if b else "outside image"
        extra = ""
        if b and b[0] in (0xE8, 0xE9) and n >= 5:
            tgt = va + 5 + struct.unpack_from("<i", b, 1)[0]
            extra = f"  -> {'call' if b[0] == 0xE8 else 'jmp'} {tgt:#010x}"
        if label.startswith("double") and b:
            extra = f"  = {struct.unpack('<d', b)[0]}"
        print(f"  {va:#010x} {label:48} {desc}{extra}")


if __name__ == "__main__":
    main(sys.argv[1])
