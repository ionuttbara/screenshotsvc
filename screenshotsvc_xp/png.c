#include "classic.h"

static DWORD g_crcTable[256];
static BOOL g_crcReady = FALSE;
static void InitCrcTable(void)
{
    DWORD n;
    int k;
    if (g_crcReady) return;
    for (n = 0; n < 256; ++n) {
        DWORD c = n;
        for (k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320UL : (c >> 1);
        g_crcTable[n] = c;
    }
    g_crcReady = TRUE;
}
static DWORD CrcUpdate(DWORD c, const BYTE *p, DWORD n)
{
    DWORD i;
    InitCrcTable();
    for (i = 0; i < n; ++i) c = g_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static DWORD Adler32(const BYTE *p, DWORD n)
{
    DWORD a = 1, b = 0, pos = 0;
    while (pos < n) {
        DWORD end = pos + 5552UL;
        DWORD i;
        if (end > n) end = n;
        for (i = pos; i < end; ++i) { a += p[i]; b += a; }
        a %= 65521UL; b %= 65521UL; pos = end;
    }
    return (b << 16) | a;
}
static void PutBE32(BYTE *p, DWORD n)
{
    p[0] = (BYTE)(n >> 24); p[1] = (BYTE)(n >> 16);
    p[2] = (BYTE)(n >> 8);  p[3] = (BYTE)n;
}
static BOOL WriteChunk(FILE *f, const char type[4], const BYTE *data, DWORD size)
{
    BYTE len[4], crcBuf[4];
    DWORD crc;
    PutBE32(len, size);
    if (fwrite(len, 1, 4, f) != 4 || fwrite(type, 1, 4, f) != 4) return FALSE;
    if (size && fwrite(data, 1, size, f) != size) return FALSE;
    crc = CrcUpdate(0xFFFFFFFFUL, (const BYTE *)type, 4);
    if (size) crc = CrcUpdate(crc, data, size);
    PutBE32(crcBuf, crc ^ 0xFFFFFFFFUL);
    return fwrite(crcBuf, 1, 4, f) == 4;
}

/* Dependency-free PNG writer. DEFLATE stored blocks trade file size for compatibility
   with Windows 2000, where WIC is not guaranteed to exist. */
BOOL SavePNG(const char *path, const BYTE *bgra, int width, int height)
{
    static const BYTE sig[8] = {137,80,78,71,13,10,26,10};
    BYTE ihdr[13], *raw = NULL, *z = NULL;
    DWORD rawSize, zSize, pos, inPos;
    FILE *f = NULL;
    int x, y;
    BOOL ok = FALSE;
    if (!path || !bgra || width <= 0 || height <= 0) return FALSE;
    if ((unsigned long)width > 0x3FFFFFFFUL || (unsigned long)height > 0x3FFFFFFFUL) return FALSE;
    rawSize = (DWORD)((size_t)width * 4U + 1U) * (DWORD)height;
    if ((size_t)rawSize != ((size_t)width * 4U + 1U) * (size_t)height) return FALSE;
    raw = (BYTE *)malloc(rawSize);
    if (!raw) goto done;
    pos = 0;
    for (y = 0; y < height; ++y) {
        const BYTE *src = bgra + (size_t)y * (size_t)width * 4U;
        raw[pos++] = 0; /* PNG filter: None */
        for (x = 0; x < width; ++x) {
            raw[pos++] = src[x * 4 + 2];
            raw[pos++] = src[x * 4 + 1];
            raw[pos++] = src[x * 4 + 0];
            raw[pos++] = 255;
        }
    }
    /* zlib header + uncompressed DEFLATE blocks, max 65535 bytes each + Adler32 */
    zSize = 2 + rawSize + ((rawSize + 65534UL) / 65535UL) * 5 + 4;
    z = (BYTE *)malloc(zSize);
    if (!z) goto done;
    pos = 0; z[pos++] = 0x78; z[pos++] = 0x01;
    inPos = 0;
    while (inPos < rawSize) {
        DWORD block = rawSize - inPos;
        WORD n, inv;
        if (block > 65535UL) block = 65535UL;
        z[pos++] = (BYTE)((inPos + block == rawSize) ? 1 : 0);
        n = (WORD)block; inv = (WORD)~n;
        z[pos++] = (BYTE)n; z[pos++] = (BYTE)(n >> 8);
        z[pos++] = (BYTE)inv; z[pos++] = (BYTE)(inv >> 8);
        memcpy(z + pos, raw + inPos, block);
        pos += block; inPos += block;
    }
    PutBE32(z + pos, Adler32(raw, rawSize)); pos += 4;
    zSize = pos;
    f = fopen(path, "wb");
    if (!f) goto done;
    if (fwrite(sig, 1, 8, f) != 8) goto done;
    PutBE32(ihdr, (DWORD)width); PutBE32(ihdr + 4, (DWORD)height);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    if (!WriteChunk(f, "IHDR", ihdr, 13)) goto done;
    /* IDAT chunk length is bounded by DWORD. */
    if (!WriteChunk(f, "IDAT", z, zSize)) goto done;
    if (!WriteChunk(f, "IEND", NULL, 0)) goto done;
    ok = TRUE;
done:
    if (f) fclose(f);
    if (raw) free(raw);
    if (z) free(z);
    if (!ok && path) DeleteFileA(path);
    return ok;
}

BOOL SaveBMP(const char *path, const BYTE *bgra, int width, int height)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f;
    int y;
    DWORD rowBytes, imageBytes;
    BYTE *row;
    if (!path || !bgra || width <= 0 || height <= 0) return FALSE;
    rowBytes = (DWORD)(((width * 3L) + 3L) & ~3L);
    imageBytes = rowBytes * (DWORD)height;
    f = fopen(path, "wb"); if (!f) return FALSE;
    ZeroMemory(&fh, sizeof(fh)); fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + imageBytes;
    ZeroMemory(&ih, sizeof(ih)); ih.biSize = sizeof(ih); ih.biWidth = width; ih.biHeight = height; ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB; ih.biSizeImage = imageBytes;
    if (fwrite(&fh, sizeof(fh), 1, f) != 1 || fwrite(&ih, sizeof(ih), 1, f) != 1) { fclose(f); return FALSE; }
    row = (BYTE *)malloc(rowBytes); if (!row) { fclose(f); return FALSE; }
    for (y = height - 1; y >= 0; --y) {
        int x;
        const BYTE *p = bgra + (size_t)y * (size_t)width * 4U;
        for (x = 0; x < width; ++x) { row[x * 3] = p[x * 4]; row[x * 3 + 1] = p[x * 4 + 1]; row[x * 3 + 2] = p[x * 4 + 2]; }
        memset(row + width * 3, 0, rowBytes - (DWORD)width * 3U);
        if (fwrite(row, 1, rowBytes, f) != rowBytes) { free(row); fclose(f); return FALSE; }
    }
    free(row); fclose(f); return TRUE;
}
