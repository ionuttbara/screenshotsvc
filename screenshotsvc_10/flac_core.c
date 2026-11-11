#include "flac_core.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
    unsigned char *data;
    size_t capacity;
    size_t bitPosition;
} BitWriter;

static int ReserveBits(BitWriter *w, size_t moreBits)
{
    size_t needBits, needBytes, cap;
    unsigned char *p;
    if (moreBits > SIZE_MAX - w->bitPosition)
        return 0;
    needBits = w->bitPosition + moreBits;
    if (needBits > SIZE_MAX - 7)
        return 0;
    needBytes = (needBits + 7) / 8;
    if (needBytes <= w->capacity)
        return 1;
    cap = w->capacity ? w->capacity : 256;
    while (cap < needBytes) {
        if (cap > SIZE_MAX / 2) {
            cap = needBytes;
            break;
        }
        cap *= 2;
    }
    p = (unsigned char *)realloc(w->data, cap);
    if (!p)
        return 0;
    if (cap > w->capacity)
        memset(p + w->capacity, 0, cap - w->capacity);
    w->data = p;
    w->capacity = cap;
    return 1;
}

static int WriteBits(BitWriter *w, uint32_t value, unsigned count)
{
    unsigned i;
    if (!ReserveBits(w, count))
        return 0;
    for (i = 0; i < count; i++) {
        unsigned bit = (value >> (count - i - 1)) & 1u;
        size_t at = w->bitPosition++;
        unsigned char mask = (unsigned char)(1u << (7u - (unsigned)(at & 7u)));
        if (bit)
            w->data[at >> 3] |= mask;
        else
            w->data[at >> 3] &= (unsigned char)~mask;
    }
    return 1;
}

static unsigned Crc8(const unsigned char *p, size_t n)
{
    unsigned crc = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned bit;
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 0x80u) ? ((crc << 1) ^ 0x07u) : (crc << 1);
    }
    return crc & 0xffu;
}

static unsigned Crc16(const unsigned char *p, size_t n)
{
    unsigned crc = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned bit;
        crc ^= (unsigned)p[i] << 8;
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x8005u) : (crc << 1);
    }
    return crc & 0xffffu;
}

static unsigned Utf8Int(uint64_t value, unsigned char out[7])
{
    unsigned count, i;
    uint64_t max;
    if (value < 0x80u) {
        out[0] = (unsigned char)value;
        return 1;
    }
    for (count = 2; count <= 7; count++) {
        max = ((uint64_t)1 << (5u * count + 1u)) - 1u;
        if (value <= max)
            break;
    }
    if (count > 7) {
        out[0] = 0xfe;
        for (i = 0; i < 6; i++)
            out[6 - i] = (unsigned char)(0x80u | ((value >> (i * 6u)) & 0x3fu));
        return 7;
    }
    for (i = count - 1; i > 0; i--) {
        out[i] = (unsigned char)(0x80u | (value & 0x3fu));
        value >>= 6;
    }
    out[0] = (unsigned char)(((0xffu << (8u - count)) & 0xffu) |
                             (unsigned char)value);
    return count;
}

int FlacCore_EncodeVerbatimFrame(const int32_t *interleaved, uint32_t frames,
                                 uint32_t channels, uint32_t bitsPerSample,
                                 uint64_t frameNumber, unsigned char **outBytes,
                                 size_t *outSize)
{
    BitWriter w;
    unsigned char numBytes[7];
    unsigned nNum, c, crc;
    uint32_t f;
    size_t headerBytes, bodyBytes;
    uint64_t samplesNeed;

    if (!interleaved || !outBytes || !outSize || frames == 0 || frames > 65535u ||
        channels == 0 || channels > 8 ||
        (bitsPerSample < 4 || bitsPerSample > 32))
        return 0;
    samplesNeed = (uint64_t)frames * channels;
    if (samplesNeed > SIZE_MAX / sizeof(*interleaved))
        return 0;

    memset(&w, 0, sizeof(w));
    /* Fixed-blocking strategy; block size is explicitly present, the sample
       rate and sample depth are taken from STREAMINFO. */
    if (!WriteBits(&w, 0xfff8u, 16) || !WriteBits(&w, 0x70u, 8) ||
        !WriteBits(&w, (channels - 1u) << 4, 8))
        goto fail;
    nNum = Utf8Int(frameNumber, numBytes);
    for (c = 0; c < nNum; c++)
        if (!WriteBits(&w, numBytes[c], 8))
            goto fail;
    if (!WriteBits(&w, (frames - 1u) >> 8, 8) ||
        !WriteBits(&w, (frames - 1u) & 0xffu, 8))
        goto fail;
    headerBytes = w.bitPosition / 8;
    crc = Crc8(w.data, headerBytes);
    if (!WriteBits(&w, crc, 8))
        goto fail;
    headerBytes = w.bitPosition / 8;

    for (c = 0; c < channels; c++) {
        /* Subframe header: zero padding, VERBATIM type (000001), no wasted bits. */
        if (!WriteBits(&w, 0x02u, 8))
            goto fail;
        for (f = 0; f < frames; f++) {
            uint32_t sample = (uint32_t)interleaved[(size_t)f * channels + c];
            if (!WriteBits(&w, sample, bitsPerSample))
                goto fail;
        }
    }
    if (w.bitPosition & 7u) {
        unsigned pad = 8u - (unsigned)(w.bitPosition & 7u);
        if (!WriteBits(&w, 0, pad))
            goto fail;
    }
    bodyBytes = w.bitPosition / 8;
    crc = Crc16(w.data, bodyBytes);
    if (!WriteBits(&w, (crc >> 8) & 0xffu, 8) ||
        !WriteBits(&w, crc & 0xffu, 8))
        goto fail;

    *outBytes = w.data;
    *outSize = w.bitPosition / 8;
    (void)headerBytes;
    return 1;

fail:
    free(w.data);
    return 0;
}

static void PutBE16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void PutBE24(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 16);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)v;
}

static void PutBE64(unsigned char *p, uint64_t v)
{
    unsigned i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> ((7u - i) * 8u));
}

void FlacCore_MakeStreamInfo(uint32_t sampleRate, uint32_t channels,
                             uint32_t bitsPerSample, uint32_t minBlockSize,
                             uint32_t maxBlockSize, uint64_t totalSamples,
                             unsigned char streamInfo[34])
{
    uint64_t packed;
    memset(streamInfo, 0, 34);
    PutBE16(streamInfo, minBlockSize);
    PutBE16(streamInfo + 2, maxBlockSize);
    PutBE24(streamInfo + 4, 0); /* min frame size unknown */
    PutBE24(streamInfo + 7, 0); /* max frame size unknown */
    packed = ((uint64_t)(sampleRate & 0xfffffu) << 44) |
             ((uint64_t)((channels - 1u) & 7u) << 41) |
             ((uint64_t)((bitsPerSample - 1u) & 31u) << 36) |
             (totalSamples & 0xfffffffffu);
    PutBE64(streamInfo + 10, packed);
    /* An all-zero MD5 marks the checksum as unavailable. FLAC frame CRCs still
       protect the individual frames, and samples remain losslessly encoded. */
    memset(streamInfo + 18, 0, 16);
}
