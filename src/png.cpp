// Minimal PNG writer: 8-bit RGB, no filtering, zlib "stored" (uncompressed) blocks. Files are
// about as large as a BMP but open everywhere, including in tools that do not read BMP.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "fnvfov.h"

static DWORD s_crcTable[256];

static DWORD Crc(DWORD crc, const BYTE* p, size_t n)
{
    if (!s_crcTable[1]) {
        for (DWORD i = 0; i < 256; ++i) {
            DWORD c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            s_crcTable[i] = c;
        }
    }
    for (size_t i = 0; i < n; ++i) crc = s_crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void Be32(BYTE* p, DWORD v) { p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v; }

static void Chunk(FILE* f, const char* type, const BYTE* data, DWORD len)
{
    BYTE hdr[8];
    Be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    DWORD crc = Crc(0xFFFFFFFF, (const BYTE*)type, 4);
    crc = Crc(crc, data, len) ^ 0xFFFFFFFF;
    BYTE c[4];
    Be32(c, crc);
    fwrite(c, 1, 4, f);
}

// rows: `h` rows of `w` RGB triples, top row first.
bool WritePng(const char* path, int w, int h, const BYTE* rgb)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    static const BYTE sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    BYTE ihdr[13];
    Be32(ihdr, w);
    Be32(ihdr + 4, h);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 2;   // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    Chunk(f, "IHDR", ihdr, 13);

    // Raw scanlines (filter byte 0 + pixels), wrapped in stored deflate blocks of <= 65535 bytes.
    size_t rowLen = (size_t)w * 3 + 1, rawLen = rowLen * h;
    size_t blocks = (rawLen + 65534) / 65535;
    size_t zLen = 2 + rawLen + blocks * 5 + 4;
    BYTE* z = (BYTE*)malloc(zLen);
    if (!z) { fclose(f); return false; }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    DWORD a = 1, b = 0;  // Adler-32 of the raw data
    size_t done = 0, rowPos = 0;
    int y = 0;
    while (done < rawLen) {
        size_t n = rawLen - done < 65535 ? rawLen - done : 65535;
        z[o++] = done + n == rawLen ? 1 : 0;
        z[o++] = (BYTE)n;
        z[o++] = (BYTE)(n >> 8);
        z[o++] = (BYTE)~n;
        z[o++] = (BYTE)(~n >> 8);
        for (size_t i = 0; i < n; ++i) {
            BYTE v = rowPos == 0 ? 0 : rgb[(size_t)y * w * 3 + rowPos - 1];
            if (++rowPos == rowLen) { rowPos = 0; ++y; }
            z[o++] = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        done += n;
    }
    Be32(z + o, (b << 16) | a);
    o += 4;
    Chunk(f, "IDAT", z, (DWORD)o);
    free(z);
    Chunk(f, "IEND", nullptr, 0);
    fclose(f);
    return true;
}
