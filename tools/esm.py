"""Reads records from Fallout: New Vegas plugin files (.esm/.esp, TES4 format). Read-only.

As a module: records(path, "WEAP") yields Record objects (form ID, flags, subrecords).
As a script: python tools/esm.py <plugin> <TYPE> [<editor ID filter>] prints records and their
subrecord types.
"""
import struct
import sys
import zlib


class Record:
    def __init__(self, rtype, flags, form_id, data):
        self.type = rtype
        self.flags = flags
        self.form_id = form_id
        self.subs = []  # (type, bytes) in file order
        i = 0
        big = None
        while i + 6 <= len(data):
            stype = data[i:i + 4].decode("latin-1")
            size = struct.unpack_from("<H", data, i + 4)[0]
            i += 6
            if stype == "XXXX":  # the next subrecord's size does not fit 16 bits
                big = struct.unpack_from("<I", data, i)[0]
                i += size
                continue
            if big is not None:
                size, big = big, None
            self.subs.append((stype, data[i:i + size]))
            i += size

    def get(self, stype):
        for t, d in self.subs:
            if t == stype:
                return d
        return None

    def all(self, stype):
        return [d for t, d in self.subs if t == stype]

    def string(self, stype):
        d = self.get(stype)
        return d.split(b"\0")[0].decode("latin-1") if d else ""

    @property
    def edid(self):
        return self.string("EDID")


def masters(path):
    """The plugin's master files, in order (form ID prefixes 0..n-1; n is the plugin itself)."""
    with open(path, "rb") as f:
        head = f.read(24)
        size = struct.unpack_from("<I", head, 4)[0]
        rec = Record("TES4", 0, 0, f.read(size))
    return [d.split(b"\0")[0].decode("latin-1") for d in rec.all("MAST")]


def records(path, wanted):
    with open(path, "rb") as f:
        data = f.read()
    i = 0
    while i + 24 <= len(data):
        rtype = data[i:i + 4].decode("latin-1")
        size, flags, form_id = struct.unpack_from("<III", data, i + 4)
        if rtype == "GRUP":
            label = data[i + 8:i + 12].decode("latin-1")
            group_type = struct.unpack_from("<I", data, i + 12)[0]
            # Top-level groups are labelled with their record type; skip the others whole.
            if group_type == 0 and label != wanted:
                i += size
            else:
                i += 24
            continue
        body = data[i + 24:i + 24 + size]
        i += 24 + size
        if rtype != wanted:
            continue
        if flags & 0x00040000:  # compressed: uncompressed size, then zlib data
            body = zlib.decompress(body[4:])
        yield Record(rtype, flags, form_id, body)


def main(path, rtype, pattern=""):
    for r in records(path, rtype):
        if pattern.lower() in r.edid.lower():
            print(f"{r.form_id:08X} {r.edid:40} {r.string('FULL'):30} {' '.join(t for t, _ in r.subs)}")


if __name__ == "__main__":
    main(*sys.argv[1:])
