"""Writes out\\ROWE - MCM.esp: the optional plugin that lists ROWE in The Mod Configuration Menu.

MCM lists one "dummy" item per mod (an item added to its form list) and tells mods apart by the
plugin that item comes from, so an MCM page needs a form from a plugin; the DLL has none. This
plugin holds only that item (a MISC record, form 0x800, named as the list shows it). No scripts:
rowe.dll adds the item to MCM's list and answers MCM's menu itself (mcm.cpp).

Usage: python tools/make_mcm_esp.py [<out file>]
"""
import os
import struct
import sys

NAME = "ROWE for Fallout New Vegas"  # MCM's list cuts names at about 26 characters


def sub(stype, data):
    return stype.encode() + struct.pack("<H", len(data)) + data


def record(rtype, form_id, data, flags=0):
    # type, data size, flags, form ID, version control, form version 15 (FNV), unknown
    return rtype.encode() + struct.pack("<IIIIHH", len(data), flags, form_id, 0, 15, 0) + data


def main(out=None):
    out = out or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "ROWE - MCM.esp")
    misc = record("MISC", 0x01000800,
                  sub("EDID", b"fnvfovMCMDummy\0") + sub("OBND", bytes(12)) + sub("FULL", NAME.encode("latin-1") + b"\0") +
                  sub("DATA", bytes(8)))
    group = b"GRUP" + struct.pack("<I4sIII", 24 + len(misc), b"MISC", 0, 0, 0) + misc
    header = record("TES4", 0,
                    sub("HEDR", struct.pack("<fII", 1.34, 1, 0x801)) + sub("CNAM", b"ROWE\0") +
                    sub("SNAM", b"Lists ROWE in The Mod Configuration Menu (optional; rowe.dll does the rest).\0") +
                    sub("MAST", b"FalloutNV.esm\0") + sub("DATA", bytes(8)))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(header + group)
    print("wrote", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main(*sys.argv[1:])
