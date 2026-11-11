#include "giflegacy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define GIF_HASH_SIZE 8192
#define GIF_MAX_CODES 4096

typedef struct BitWriter {
    unsigned char *data;
    size_t size, capacity;
    unsigned int bits;
    int bitCount;
} BitWriter;

static FILE *g_gifFile = NULL;
static int g_gifWidth = 0, g_gifHeight = 0;
static unsigned long g_gifFrameNo = 0;
static int g_gifFps = 15;

static int PutByte(FILE *f, unsigned char c) { return f && fputc((int)c, f) != EOF; }
static int PutWord(FILE *f, unsigned int w)
{
    return PutByte(f, (unsigned char)(w & 255U)) && PutByte(f, (unsigned char)((w >> 8) & 255U));
}

static int BW_Reserve(BitWriter *bw, size_t need)
{
    unsigned char *p;
    size_t cap;
    if (need <= bw->capacity) return 1;
    cap = bw->capacity ? bw->capacity : 4096;
    while (cap < need) {
        if (cap > (size_t)-1 / 2U) return 0;
        cap *= 2;
    }
    p = (unsigned char *)realloc(bw->data, cap);
    if (!p) return 0;
    bw->data = p; bw->capacity = cap; return 1;
}

static int BW_Code(BitWriter *bw, unsigned int code, int codeSize)
{
    bw->bits |= (code << bw->bitCount);
    bw->bitCount += codeSize;
    while (bw->bitCount >= 8) {
        if (!BW_Reserve(bw, bw->size + 1)) return 0;
        bw->data[bw->size++] = (unsigned char)(bw->bits & 255U);
        bw->bits >>= 8; bw->bitCount -= 8;
    }
    return 1;
}

static int BW_Finish(BitWriter *bw)
{
    if (bw->bitCount > 0) {
        if (!BW_Reserve(bw, bw->size + 1)) return 0;
        bw->data[bw->size++] = (unsigned char)(bw->bits & 255U);
        bw->bits = 0; bw->bitCount = 0;
    }
    return 1;
}

static int WriteSubBlocks(FILE *f, const unsigned char *data, size_t size)
{
    size_t at = 0;
    while (at < size) {
        size_t n = size - at;
        if (n > 255) n = 255;
        if (!PutByte(f, (unsigned char)n) || fwrite(data + at, 1, n, f) != n) return 0;
        at += n;
    }
    return PutByte(f, 0);
}

/* Quantize to a global 3-3-2 palette, a practical compromise for an XP-era
   dependency-free GIF encoder. It preserves frame dimensions and timing. */
static unsigned char Quantize(const unsigned char *p)
{
    return (unsigned char)((p[2] & 0xE0U) | ((p[1] & 0xE0U) >> 3) | (p[0] >> 6));
}

static int LZW_EncodeFrame(const unsigned char *bgra, int width, int height)
{
    int *hashKey = NULL, *hashCode = NULL;
    BitWriter bw;
    int codeSize = 9, nextCode = 258, prefix, k, i, total, slot, key, code;
    unsigned int clearCode = 256, endCode = 257;
    size_t pixelCount;
    int ok = 0;
    memset(&bw, 0, sizeof(bw));
    if (!g_gifFile || !bgra || width != g_gifWidth || height != g_gifHeight || width < 1 || height < 1) return 0;
    if ((size_t)width > (size_t)-1 / (size_t)height) return 0;
    pixelCount = (size_t)width * (size_t)height;
    if (pixelCount == 0 || pixelCount > 100000000U) return 0;
    hashKey = (int *)malloc(sizeof(int) * GIF_HASH_SIZE);
    hashCode = (int *)malloc(sizeof(int) * GIF_HASH_SIZE);
    if (!hashKey || !hashCode) goto done;
    for (i = 0; i < GIF_HASH_SIZE; ++i) hashKey[i] = -1;

    /* GIF image descriptor. */
    if (!PutByte(g_gifFile, 0x2C) || !PutWord(g_gifFile, 0) || !PutWord(g_gifFile, 0) ||
        !PutWord(g_gifFile, (unsigned int)width) || !PutWord(g_gifFile, (unsigned int)height) || !PutByte(g_gifFile, 0)) goto done;
    if (!PutByte(g_gifFile, 8)) goto done; /* minimum LZW code size */
    if (!BW_Code(&bw, clearCode, codeSize)) goto done;

    prefix = Quantize(bgra);
    total = (int)pixelCount;
    for (i = 1; i < total; ++i) {
        unsigned char *px = (unsigned char *)(bgra + (size_t)i * 4U);
        k = (int)Quantize(px);
        key = (prefix << 8) | k;
        slot = ((prefix * 33) ^ k) & (GIF_HASH_SIZE - 1);
        while (hashKey[slot] != -1 && hashKey[slot] != key) slot = (slot + 1) & (GIF_HASH_SIZE - 1);
        if (hashKey[slot] == key) {
            prefix = hashCode[slot];
            continue;
        }
        if (!BW_Code(&bw, (unsigned int)prefix, codeSize)) goto done;
        if (nextCode < GIF_MAX_CODES) {
            hashKey[slot] = key; hashCode[slot] = nextCode++;
            if (nextCode > (1 << codeSize) && codeSize < 12) ++codeSize;
        } else {
            if (!BW_Code(&bw, clearCode, codeSize)) goto done;
            for (code = 0; code < GIF_HASH_SIZE; ++code) hashKey[code] = -1;
            codeSize = 9; nextCode = 258;
        }
        prefix = k;
    }
    if (!BW_Code(&bw, (unsigned int)prefix, codeSize) || !BW_Code(&bw, endCode, codeSize) || !BW_Finish(&bw)) goto done;
    if (!WriteSubBlocks(g_gifFile, bw.data, bw.size)) goto done;
    ok = 1;
done:
    if (hashKey) free(hashKey);
    if (hashCode) free(hashCode);
    if (bw.data) free(bw.data);
    return ok;
}

int GIF_Start(const char *path, int width, int height, int fps)
{
    int i;
    if (g_gifFile) GIF_Stop();
    if (!path || width < 1 || height < 1 || width > 65535 || height > 65535) return 0;
    g_gifFile = fopen(path, "wb");
    if (!g_gifFile) return 0;
    g_gifWidth = width; g_gifHeight = height; g_gifFrameNo = 0;
    if (fps < 1) fps = 15;
    g_gifFps = fps;
    if (fwrite("GIF89a", 1, 6, g_gifFile) != 6 || !PutWord(g_gifFile, (unsigned int)width) || !PutWord(g_gifFile, (unsigned int)height) ||
        !PutByte(g_gifFile, 0xF7) || !PutByte(g_gifFile, 0) || !PutByte(g_gifFile, 0)) { GIF_Stop(); return 0; }
    for (i = 0; i < 256; ++i) {
        unsigned char r = (unsigned char)(((i >> 5) & 7) * 255 / 7);
        unsigned char g = (unsigned char)(((i >> 2) & 7) * 255 / 7);
        unsigned char b = (unsigned char)((i & 3) * 255 / 3);
        if (!PutByte(g_gifFile, r) || !PutByte(g_gifFile, g) || !PutByte(g_gifFile, b)) { GIF_Stop(); return 0; }
    }
    return 1;
}

int GIF_WriteFrameTimed(const unsigned char *bgra, int width, int height, unsigned int delayCentiseconds)
{
    if (!g_gifFile) return 0;
    if (delayCentiseconds < 1U) delayCentiseconds = 1U;
    if (delayCentiseconds > 65535U) delayCentiseconds = 65535U;
    if (!PutByte(g_gifFile, 0x21) || !PutByte(g_gifFile, 0xF9) || !PutByte(g_gifFile, 4) ||
        !PutByte(g_gifFile, 0) || !PutWord(g_gifFile, delayCentiseconds) || !PutByte(g_gifFile, 0) || !PutByte(g_gifFile, 0)) return 0;
    if (!LZW_EncodeFrame(bgra, width, height)) return 0;
    ++g_gifFrameNo;
    return 1;
}

int GIF_WriteFrame(const unsigned char *bgra, int width, int height)
{
    unsigned int delay;
    if (g_gifFps >= 1 && g_gifFps != 15) delay = (unsigned int)((100 + g_gifFps / 2) / g_gifFps);
    else delay = (g_gifFrameNo % 3UL == 2UL) ? 6U : 7U;
    return GIF_WriteFrameTimed(bgra, width, height, delay);
}

void GIF_Stop(void)
{
    if (!g_gifFile) return;
    PutByte(g_gifFile, 0x3B);
    fflush(g_gifFile); fclose(g_gifFile); g_gifFile = NULL;
    g_gifWidth = g_gifHeight = 0; g_gifFrameNo = 0; g_gifFps = 15;
}
