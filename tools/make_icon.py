# Writes res\rowe.ico: an amber lens ring with a centre dot on a dark rounded tile (the HUD's
# colour), as PNG images of 16, 24, 32, 48, 64 and 256 pixels. Plain Python (zlib), no imaging library.
#   python tools\make_icon.py
import os
import struct
import zlib

AMBER = (255, 182, 66)
DARK = (24, 22, 18)


def coverage(x, y, size, shape):
    # 4x4 supersampling of one pixel against shape(px, py) in unit coordinates.
    hit = 0
    for i in range(4):
        for j in range(4):
            if shape((x + (i + 0.5) / 4) / size, (y + (j + 0.5) / 4) / size):
                hit += 1
    return hit / 16.0


def tile(px, py):
    r = 0.18  # rounded corners
    dx = max(abs(px - 0.5) - (0.5 - r), 0)
    dy = max(abs(py - 0.5) - (0.5 - r), 0)
    return dx * dx + dy * dy <= r * r


def ring(px, py):
    d = ((px - 0.5) ** 2 + (py - 0.5) ** 2) ** 0.5
    return 0.27 <= d <= 0.37


def dot(px, py):
    return ((px - 0.5) ** 2 + (py - 0.5) ** 2) ** 0.5 <= 0.09


def image(size):
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            a = coverage(x, y, size, tile)
            c = max(coverage(x, y, size, ring), coverage(x, y, size, dot))
            rgb = [int(DARK[k] * (1 - c) + AMBER[k] * c) for k in range(3)]
            row += bytes(rgb + [int(255 * a)])
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    sizes = [16, 24, 32, 48, 64, 256]
    pngs = [image(s) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, p in zip(sizes, pngs):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(p), offset)
        offset += len(p)
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "res", "rowe.ico")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(pngs))
    print("wrote", out)


if __name__ == "__main__":
    main()
