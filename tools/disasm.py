"""Disassembles FalloutNV.exe at a virtual address (32-bit, image base 0x400000).

Usage: python tools/disasm.py <exe> <hex address> [<byte count>]
"""
import struct
import sys

import capstone


def main(exe, addr, count="0x100"):
    data = open(exe, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    va = int(addr, 16)
    n = int(count, 0)
    for i in range(nsec):
        s = pe + 24 + optsz + 40 * i
        vsz, vaddr, rsz, rptr = struct.unpack_from("<IIII", data, s + 8)
        if vaddr <= va - base < vaddr + max(vsz, rsz):
            off = rptr + va - base - vaddr
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
            for ins in md.disasm(data[off:off + n], va):
                print(f"{ins.address:08x}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
            return
    print("address outside the image")


if __name__ == "__main__":
    main(*sys.argv[1:])
