"""Lists or extracts files from a Fallout: New Vegas BSA archive (version 104). Read-only.

Usage: python tools/bsa.py <archive.bsa> [<path filter> [<out dir>]]
  Without an out dir, prints the matching paths; with one, writes them there.
"""
import os
import struct
import sys
import zlib


def entries(path):
    f = open(path, "rb")
    magic, version, offset, flags, folders, files, _, names_len, _ = struct.unpack("<4sIIIIIIII", f.read(36))
    if magic != b"BSA\0" or version not in (103, 104):
        raise SystemExit("not a v103/104 BSA")
    f.seek(offset)
    counts = [struct.unpack("<QII", f.read(16))[1] for _ in range(folders)]
    records = []
    for count in counts:
        name = f.read(f.read(1)[0])[:-1].decode("latin-1") if flags & 1 else ""
        for _ in range(count):
            _, size, data_offset = struct.unpack("<QII", f.read(16))
            records.append((name, size, data_offset))
    names = f.read(names_len).split(b"\0") if flags & 2 else [b""] * len(records)
    for (folder, size, data_offset), name in zip(records, names):
        yield f, flags, folder + "\\" + name.decode("latin-1"), size, data_offset


def read(f, flags, size, data_offset):
    compressed = bool(flags & 4) != bool(size & 0x40000000)
    size &= 0x3FFFFFFF
    f.seek(data_offset)
    data = f.read(size)
    if flags & 0x100:  # file names embedded before the data
        data = data[1 + data[0]:]
    return zlib.decompress(data[4:]) if compressed else data


def main(archive, pattern="", out=None):
    for f, flags, name, size, data_offset in entries(archive):
        if pattern.lower() not in name.lower():
            continue
        if out is None:
            print(name)
            continue
        target = os.path.join(out, *name.split("\\"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        try:
            data = read(f, flags, size, data_offset)
        except zlib.error as e:
            print("skipped", name, e)
            continue
        with open(target, "wb") as w:
            w.write(data)


if __name__ == "__main__":
    main(*sys.argv[1:])
