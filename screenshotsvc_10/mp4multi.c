/*
 * mp4multi.c - native ISO Base Media File Format muxing of FLAC audio tracks.
 *
 * The video and combined AAC tracks are produced by Media Foundation first.
 * This module appends complete FLAC frames to mdat, writes sample tables for
 * each frame, and adds two fLaC tracks to moov. It never decodes or re-encodes
 * video and has no FFmpeg dependency.
 */
#include "screenshotservice.h"
#include "mp4multi.h"
#include "log.h"

#include <strsafe.h>
#include <shlwapi.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MAX_MOOV_BYTES (64u * 1024u * 1024u)
#define FLAC_HEADER_BYTES 42u
#define COPY_BUFFER_BYTES (4u * 1024u * 1024u)

typedef struct {
    BYTE *data;
    SIZE_T size;
    SIZE_T capacity;
} BoxBuffer;

typedef struct {
    UINT64 offset;
    UINT64 size;
    UINT32 headerSize;
    BOOL found;
} TopBox;

typedef struct {
    UINT32 trakAt;
    UINT32 trakSize;
    UINT32 mdiaAt;
    UINT32 mdiaSize;
    UINT32 hdlrAt;
    UINT32 hdlrSize;
    UINT32 tkhdAt;
    UINT32 tkhdSize;
    UINT32 trackId;
} AudioTrackLocation;

static UINT32 Rd32(const BYTE *p)
{
    return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) |
           ((UINT32)p[2] << 8) | (UINT32)p[3];
}
static UINT64 Rd64(const BYTE *p)
{
    return ((UINT64)Rd32(p) << 32) | (UINT64)Rd32(p + 4);
}
static void Wr32(BYTE *p, UINT32 v)
{
    p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16);
    p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v;
}
static void Wr64(BYTE *p, UINT64 v)
{
    Wr32(p, (UINT32)(v >> 32)); Wr32(p + 4, (UINT32)v);
}
static BOOL TypeIs(const BYTE *p, const char *s)
{
    return p[4] == (BYTE)s[0] && p[5] == (BYTE)s[1] &&
           p[6] == (BYTE)s[2] && p[7] == (BYTE)s[3];
}

static BOOL BufReserve(BoxBuffer *b, SIZE_T extra)
{
    SIZE_T need, cap;
    BYTE *p;
    if (extra > SIZE_MAX - b->size)
        return FALSE;
    need = b->size + extra;
    if (need <= b->capacity)
        return TRUE;
    cap = b->capacity ? b->capacity : 1024;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    p = (BYTE *)HeapReAlloc(GetProcessHeap(), 0, b->data, cap);
    if (!p) {
        p = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cap);
        if (!p) return FALSE;
        if (b->data) {
            CopyMemory(p, b->data, b->size);
            HeapFree(GetProcessHeap(), 0, b->data);
        }
    }
    b->data = p;
    b->capacity = cap;
    return TRUE;
}

static BOOL BufBytes(BoxBuffer *b, const void *data, SIZE_T n)
{
    if (!BufReserve(b, n))
        return FALSE;
    if (n && data)
        CopyMemory(b->data + b->size, data, n);
    else if (n)
        ZeroMemory(b->data + b->size, n);
    b->size += n;
    return TRUE;
}
static BOOL BufU8(BoxBuffer *b, BYTE v) { return BufBytes(b, &v, 1); }
static BOOL BufU16(BoxBuffer *b, UINT16 v)
{
    BYTE x[2] = { (BYTE)(v >> 8), (BYTE)v };
    return BufBytes(b, x, sizeof(x));
}
static BOOL BufU32(BoxBuffer *b, UINT32 v)
{
    BYTE x[4]; Wr32(x, v); return BufBytes(b, x, sizeof(x));
}
static BOOL BufU64(BoxBuffer *b, UINT64 v)
{
    BYTE x[8]; Wr64(x, v); return BufBytes(b, x, sizeof(x));
}
static SIZE_T BeginBox(BoxBuffer *b, const char type[4])
{
    SIZE_T at = b->size;
    if (!BufU32(b, 0) || !BufBytes(b, type, 4))
        return SIZE_MAX;
    return at;
}
static BOOL EndBox(BoxBuffer *b, SIZE_T at)
{
    SIZE_T size;
    if (at == SIZE_MAX || at > b->size || b->size - at > UINT32_MAX)
        return FALSE;
    size = b->size - at;
    Wr32(b->data + at, (UINT32)size);
    return TRUE;
}
static BOOL BeginFullBox(BoxBuffer *b, BYTE version, BYTE flags0,
                         BYTE flags1, BYTE flags2)
{
    BYTE f[4] = { version, flags0, flags1, flags2 };
    return BufBytes(b, f, sizeof(f));
}
static void BufFree(BoxBuffer *b)
{
    if (b->data) HeapFree(GetProcessHeap(), 0, b->data);
    ZeroMemory(b, sizeof(*b));
}

static BOOL ReadAt(HANDLE f, UINT64 off, BYTE *data, DWORD bytes)
{
    LARGE_INTEGER pos;
    DWORD got = 0;
    pos.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(f, pos, NULL, FILE_BEGIN) &&
           ReadFile(f, data, bytes, &got, NULL) && got == bytes;
}
static BOOL WriteAll(HANDLE f, const BYTE *data, DWORD bytes)
{
    DWORD wrote = 0;
    return WriteFile(f, data, bytes, &wrote, NULL) && wrote == bytes;
}
static BOOL CopyRange(HANDLE src, HANDLE dst, UINT64 off, UINT64 length)
{
    BYTE *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, COPY_BUFFER_BYTES);
    BOOL ok = FALSE;
    if (!buf) return FALSE;
    while (length) {
        DWORD take = length > COPY_BUFFER_BYTES ? COPY_BUFFER_BYTES : (DWORD)length;
        if (!ReadAt(src, off, buf, take) || !WriteAll(dst, buf, take))
            goto done;
        off += take;
        length -= take;
    }
    ok = TRUE;
done:
    HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

static BOOL ReadTopBoxes(HANDLE f, UINT64 fileSize, TopBox *mdat, TopBox *moov)
{
    UINT64 at = 0;
    BYTE h[16];
    ZeroMemory(mdat, sizeof(*mdat));
    ZeroMemory(moov, sizeof(*moov));
    while (at + 8 <= fileSize) {
        UINT64 size;
        UINT32 headerSize = 8;
        if (!ReadAt(f, at, h, 8)) return FALSE;
        size = Rd32(h);
        if (size == 1) {
            if (at + 16 > fileSize || !ReadAt(f, at, h, 16)) return FALSE;
            size = Rd64(h + 8);
            headerSize = 16;
        } else if (size == 0) {
            size = fileSize - at;
        }
        if (size < headerSize || size > fileSize - at)
            return FALSE;
        if (TypeIs(h, "mdat")) {
            if (mdat->found) return FALSE; /* only one media-data box supported */
            mdat->offset = at; mdat->size = size;
            mdat->headerSize = headerSize; mdat->found = TRUE;
        } else if (TypeIs(h, "moov")) {
            if (moov->found) return FALSE;
            moov->offset = at; moov->size = size;
            moov->headerSize = headerSize; moov->found = TRUE;
        }
        at += size;
    }
    return at == fileSize && mdat->found && moov->found &&
           moov->offset + moov->size == fileSize && mdat->offset < moov->offset;
}

/* `parent` points at a normal 32-bit box header and is wholly inside moov. */
static BOOL FindChild(const BYTE *buf, UINT32 parentAt, UINT32 parentSize,
                      const char type[4], UINT32 *childAt, UINT32 *childSize)
{
    UINT64 p = (UINT64)parentAt + 8;
    UINT64 end = (UINT64)parentAt + parentSize;
    while (p + 8 <= end) {
        UINT32 sz = Rd32(buf + (SIZE_T)p);
        if (sz < 8 || sz > end - p) return FALSE;
        if (TypeIs(buf + (SIZE_T)p, type)) {
            if (childAt) *childAt = (UINT32)p;
            if (childSize) *childSize = sz;
            return TRUE;
        }
        p += sz;
    }
    return FALSE;
}

static UINT32 GetTrackId(const BYTE *moov, UINT32 tkhdAt, UINT32 tkhdSize)
{
    BYTE version;
    if (tkhdSize < 32) return 0;
    version = moov[tkhdAt + 8];
    if (version == 1) {
        if (tkhdSize < 32) return 0;
        return Rd32(moov + tkhdAt + 28);
    }
    return Rd32(moov + tkhdAt + 20);
}

static BOOL FindAudioTrack(const BYTE *moov, UINT32 moovSize,
                           AudioTrackLocation *loc, UINT32 *maxTrackId)
{
    UINT32 p = 8;
    BOOL foundAudio = FALSE;
    *maxTrackId = 0;
    ZeroMemory(loc, sizeof(*loc));
    while (p + 8 <= moovSize) {
        UINT32 trakSize = Rd32(moov + p);
        if (trakSize < 8 || trakSize > moovSize - p) return FALSE;
        if (TypeIs(moov + p, "trak")) {
            UINT32 tkhdAt, tkhdSize, mdiaAt, mdiaSize, hdlrAt, hdlrSize;
            if (FindChild(moov, p, trakSize, "tkhd", &tkhdAt, &tkhdSize)) {
                UINT32 id = GetTrackId(moov, tkhdAt, tkhdSize);
                if (id > *maxTrackId) *maxTrackId = id;
                if (FindChild(moov, p, trakSize, "mdia", &mdiaAt, &mdiaSize) &&
                    FindChild(moov, mdiaAt, mdiaSize, "hdlr", &hdlrAt, &hdlrSize) &&
                    hdlrSize >= 24 && TypeIs(moov + hdlrAt, "hdlr") &&
                    moov[hdlrAt + 16] == 's' && moov[hdlrAt + 17] == 'o' &&
                    moov[hdlrAt + 18] == 'u' && moov[hdlrAt + 19] == 'n') {
                    if (!foundAudio) {
                        loc->trakAt = p; loc->trakSize = trakSize;
                        loc->mdiaAt = mdiaAt; loc->mdiaSize = mdiaSize;
                        loc->hdlrAt = hdlrAt; loc->hdlrSize = hdlrSize;
                        loc->tkhdAt = tkhdAt; loc->tkhdSize = tkhdSize;
                        loc->trackId = id;
                        foundAudio = TRUE;
                    }
                }
            }
        }
        p += trakSize;
    }
    return foundAudio;
}

static BOOL BuildHandlerName(BoxBuffer *b, const BYTE *oldMoov,
                             UINT32 oldHdlrAt, const wchar_t *name)
{
    char ascii[96];
    int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, ascii, sizeof(ascii), NULL, NULL);
    SIZE_T box;
    if (n <= 0 || n > (int)sizeof(ascii)) return FALSE;
    box = BeginBox(b, "hdlr");
    if (box == SIZE_MAX || !BufBytes(b, oldMoov + oldHdlrAt + 8, 24) ||
        !BufBytes(b, ascii, (SIZE_T)n))
        return FALSE;
    return EndBox(b, box);
}

static BOOL AddMatrix(BoxBuffer *b)
{
    return BufU32(b, 0x00010000u) && BufU32(b, 0) && BufU32(b, 0) &&
           BufU32(b, 0) && BufU32(b, 0x00010000u) && BufU32(b, 0) &&
           BufU32(b, 0) && BufU32(b, 0) && BufU32(b, 0x40000000u);
}

/* AudioSampleEntry uses an unsigned 16.16 sample rate. FLAC can carry
   higher rates; ISO BMFF readers must consult STREAMINFO for the exact rate.
   For common rates, write the greatest regular division that fits. */
static UINT32 FlacSampleEntryRate(UINT32 sampleRate)
{
    if (sampleRate <= 65535u)
        return sampleRate;
    if (sampleRate % 48000u == 0)
        return 48000u;
    if (sampleRate % 44100u == 0)
        return 44100u;
    return 65535u;
}

static BOOL BuildFlacTrack(BoxBuffer *b, const FlacEncodedTrack *t,
                           const char *handlerName, UINT32 trackId,
                           UINT32 movieTimescale, UINT64 *dataBytes)
{
    SIZE_T trak, tkhd, mdia, mdhd, hdlr, minf, smhd, dinf, dref, url, stbl;
    SIZE_T stsd, entry, dfla, chan, stts, stsc, stsz, co64;
    UINT32 i, runCount, lastCount;
    UINT64 duration;
    UINT64 bytes = 0;
    BOOL ok = FALSE;

    if (!t || !t->sampleRate || !t->channels || !t->bitsPerSample ||
        !t->frameCount || !t->frames || !movieTimescale)
        return FALSE;
    for (i = 0; i < t->frameCount; i++) {
        if (!t->frames[i].size || UINT64_MAX - bytes < t->frames[i].size)
            return FALSE;
        bytes += t->frames[i].size;
    }
    *dataBytes = bytes;
    duration = t->totalSamples * (UINT64)movieTimescale / t->sampleRate;
    if (t->totalSamples && duration == 0) duration = 1;

    trak = BeginBox(b, "trak");
    if (trak == SIZE_MAX) goto done;
    tkhd = BeginBox(b, "tkhd");
    if (tkhd == SIZE_MAX || !BeginFullBox(b, 1, 0, 0, 7) ||
        !BufU64(b, 0) || !BufU64(b, 0) || !BufU32(b, trackId) ||
        !BufU32(b, 0) || !BufU64(b, duration) ||
        !BufU64(b, 0) || !BufU16(b, 0) || !BufU16(b, 1) ||
        !BufU16(b, 0x0100) || !BufU16(b, 0) || !AddMatrix(b) ||
        !BufU32(b, 0) || !BufU32(b, 0) || !EndBox(b, tkhd)) goto done;

    mdia = BeginBox(b, "mdia");
    if (mdia == SIZE_MAX) goto done;
    mdhd = BeginBox(b, "mdhd");
    if (mdhd == SIZE_MAX || !BeginFullBox(b, 1, 0, 0, 0) ||
        !BufU64(b, 0) || !BufU64(b, 0) || !BufU32(b, t->sampleRate) ||
        !BufU64(b, t->totalSamples) || !BufU16(b, 0x55c4) ||
        !BufU16(b, 0) || !EndBox(b, mdhd)) goto done;
    hdlr = BeginBox(b, "hdlr");
    if (hdlr == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) ||
        !BufU32(b, 0) || !BufBytes(b, "soun", 4) ||
        !BufU32(b, 0) || !BufU32(b, 0) || !BufU32(b, 0) ||
        !BufBytes(b, handlerName, strlen(handlerName) + 1u) || !EndBox(b, hdlr)) goto done;

    minf = BeginBox(b, "minf");
    if (minf == SIZE_MAX) goto done;
    smhd = BeginBox(b, "smhd");
    if (smhd == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) ||
        !BufU16(b, 0) || !BufU16(b, 0) || !EndBox(b, smhd)) goto done;

    dinf = BeginBox(b, "dinf");
    if (dinf == SIZE_MAX) goto done;
    dref = BeginBox(b, "dref");
    if (dref == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) || !BufU32(b, 1)) goto done;
    url = BeginBox(b, "url ");
    if (url == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 1) ||
        !EndBox(b, url) || !EndBox(b, dref) || !EndBox(b, dinf)) goto done;

    stbl = BeginBox(b, "stbl");
    if (stbl == SIZE_MAX) goto done;
    stsd = BeginBox(b, "stsd");
    if (stsd == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) || !BufU32(b, 1)) goto done;
    entry = BeginBox(b, "fLaC");
    if (entry == SIZE_MAX || !BufBytes(b, "\0\0\0\0\0\0", 6) ||
        !BufU16(b, 1) || /* data_reference_index */
        !BufU16(b, 0) || !BufU16(b, 0) || !BufU32(b, 0) ||
        !BufU16(b, (UINT16)t->channels) || !BufU16(b, (UINT16)t->bitsPerSample) ||
        !BufU16(b, 0) || !BufU16(b, 0) ||
        !BufU32(b, FlacSampleEntryRate(t->sampleRate) << 16)) goto done;
    dfla = BeginBox(b, "dfLa");
    if (dfla == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) ||
        !BufU8(b, 0x80) || !BufU8(b, 0) || !BufU8(b, 0) || !BufU8(b, 34) ||
        !BufBytes(b, t->streamInfo, 34) || !EndBox(b, dfla)) goto done;

    /* Preserve WAVEFORMATEXTENSIBLE speaker placement in the MP4 track. The
       common Windows/CoreAudio speaker bitmap bits are compatible; the FLAC
       stream itself stores the channels independently in their source order. */
    if (t->channelMask) {
        chan = BeginBox(b, "chan");
        if (chan == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) ||
            !BufU32(b, 1u) || /* use channel bitmap */
            !BufU32(b, t->channelMask) || !BufU32(b, 0) ||
            !EndBox(b, chan)) goto done;
    }
    if (!EndBox(b, entry) || !EndBox(b, stsd)) goto done;

    stts = BeginBox(b, "stts");
    lastCount = t->frames[t->frameCount - 1].sampleCount;
    runCount = (t->frameCount > 1 && lastCount != 4096u) ? 2u : 1u;
    if (stts == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) || !BufU32(b, runCount)) goto done;
    if (runCount == 2) {
        if (!BufU32(b, t->frameCount - 1u) || !BufU32(b, 4096u) ||
            !BufU32(b, 1) || !BufU32(b, lastCount)) goto done;
    } else {
        if (!BufU32(b, t->frameCount) || !BufU32(b, lastCount)) goto done;
    }
    if (!EndBox(b, stts)) goto done;

    stsc = BeginBox(b, "stsc");
    if (stsc == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) ||
        !BufU32(b, 1) || !BufU32(b, 1) || !BufU32(b, 1) ||
        !BufU32(b, 1) || !EndBox(b, stsc)) goto done;

    stsz = BeginBox(b, "stsz");
    if (stsz == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) ||
        !BufU32(b, 0) || !BufU32(b, t->frameCount)) goto done;
    for (i = 0; i < t->frameCount; i++)
        if (!BufU32(b, t->frames[i].size)) goto done;
    if (!EndBox(b, stsz)) goto done;

    co64 = BeginBox(b, "co64");
    if (co64 == SIZE_MAX || !BeginFullBox(b, 0, 0, 0, 0) ||
        !BufU32(b, t->frameCount)) goto done;
    for (i = 0; i < t->frameCount; i++)
        if (!BufU64(b, 0)) goto done; /* patched once mdat's position is known */
    if (!EndBox(b, co64) || !EndBox(b, stbl) || !EndBox(b, minf) ||
        !EndBox(b, mdia) || !EndBox(b, trak)) goto done;

    /* Store co64 location indirectly as the start of its first offset table. */
    (void)co64;
    ok = TRUE;
done:
    if (!ok) {
        /* The caller discards the entire partially-built buffer on failure. */
    }
    return ok;
}

/* A 32-bit mdat can cross 4 GiB after the FLAC payloads are appended.
   In that case we promote the box header to the ISO-BMFF 64-bit largesize
   form. Every existing chunk offset into the original mdat payload must then
   move by eight bytes because the header grew from 8 to 16 bytes. */
static BOOL PatchChunkOffsetBox(BYTE *box, UINT32 boxSize,
                                UINT64 dataStart, UINT64 dataEnd,
                                UINT64 delta, UINT32 *patchedTables)
{
    UINT32 count, i;
    BOOL isStco = TypeIs(box, "stco");
    BOOL isCo64 = TypeIs(box, "co64");
    UINT64 expected;

    if ((!isStco && !isCo64) || boxSize < 16u)
        return FALSE;
    count = Rd32(box + 12);
    expected = 16ull + (UINT64)count * (isStco ? 4ull : 8ull);
    if (!count || expected != boxSize)
        return FALSE;

    for (i = 0; i < count; ++i) {
        BYTE *entry = box + 16u + (SIZE_T)i * (isStco ? 4u : 8u);
        UINT64 offset = isStco ? (UINT64)Rd32(entry) : Rd64(entry);
        if (offset < dataStart || offset >= dataEnd || UINT64_MAX - offset < delta)
            return FALSE;
        offset += delta;
        if (isStco) {
            if (offset > UINT32_MAX)
                return FALSE;
            Wr32(entry, (UINT32)offset);
        } else {
            Wr64(entry, offset);
        }
    }
    (*patchedTables)++;
    return TRUE;
}

static BOOL PatchChunkOffsetsInBoxes(BYTE *parent, UINT32 parentSize,
                                     UINT64 dataStart, UINT64 dataEnd,
                                     UINT64 delta, UINT32 *patchedTables,
                                     unsigned depth)
{
    UINT32 at = 8u;
    if (!parent || parentSize < 8u || depth > 8u)
        return FALSE;

    while (at + 8u <= parentSize) {
        BYTE *child = parent + at;
        UINT64 childSize64 = Rd32(child);
        UINT32 headerSize = 8u;
        UINT32 childSize;

        if (childSize64 == 1u) {
            if (at + 16u > parentSize)
                return FALSE;
            childSize64 = Rd64(child + 8u);
            headerSize = 16u;
        } else if (childSize64 == 0u) {
            childSize64 = parentSize - at;
        }
        if (childSize64 < headerSize || childSize64 > parentSize - at ||
            childSize64 > UINT32_MAX)
            return FALSE;
        childSize = (UINT32)childSize64;

        if (TypeIs(child, "stco") || TypeIs(child, "co64")) {
            if (!PatchChunkOffsetBox(child, childSize, dataStart, dataEnd,
                                     delta, patchedTables))
                return FALSE;
        } else if (TypeIs(child, "trak") || TypeIs(child, "mdia") ||
                   TypeIs(child, "minf") || TypeIs(child, "stbl")) {
            /* These sample-table containers are expected to use ordinary
               8-byte headers. If an unusual extended container is present,
               fail safely rather than interpreting its largesize as a child. */
            if (headerSize != 8u ||
                !PatchChunkOffsetsInBoxes(child, childSize, dataStart, dataEnd,
                                          delta, patchedTables, depth + 1u))
                return FALSE;
        }
        at += childSize;
    }
    return at == parentSize;
}

static BOOL PatchExistingChunkOffsets(BYTE *moov, UINT32 moovSize,
                                      UINT64 dataStart, UINT64 dataEnd,
                                      UINT64 delta)
{
    UINT32 patchedTables = 0;
    if (!PatchChunkOffsetsInBoxes(moov, moovSize, dataStart, dataEnd,
                                  delta, &patchedTables, 0u))
        return FALSE;
    return patchedTables != 0;
}

static BOOL PatchCo64Track(BYTE *trak, UINT32 trakSize, UINT64 firstDataOffset,
                           const FlacEncodedTrack *t)
{
    /* All boxes here are 32-bit sized and generated by BuildFlacTrack. */
    UINT32 mdiaAt, mdiaSize, minfAt, minfSize, stblAt, stblSize, co64At, co64Size;
    UINT64 offset = firstDataOffset;
    UINT32 i;
    if (!FindChild(trak, 0, trakSize, "mdia", &mdiaAt, &mdiaSize) ||
        !FindChild(trak, mdiaAt, mdiaSize, "minf", &minfAt, &minfSize) ||
        !FindChild(trak, minfAt, minfSize, "stbl", &stblAt, &stblSize) ||
        !FindChild(trak, stblAt, stblSize, "co64", &co64At, &co64Size) ||
        co64Size < 16u || Rd32(trak + co64At + 12) != t->frameCount ||
        co64Size != 16u + (UINT64)t->frameCount * 8u)
        return FALSE;
    for (i = 0; i < t->frameCount; i++) {
        Wr64(trak + co64At + 16u + i * 8u, offset);
        if (UINT64_MAX - offset < t->frames[i].size)
            return FALSE;
        offset += t->frames[i].size;
    }
    return TRUE;
}

static BOOL GetMovieTimescale(const BYTE *moov, UINT32 moovSize, UINT32 *timescale)
{
    UINT32 at, size;
    BYTE version;
    if (!FindChild(moov, 0, moovSize, "mvhd", &at, &size) || size < 24)
        return FALSE;
    version = moov[at + 8];
    if (version == 1) {
        if (size < 32) return FALSE;
        *timescale = Rd32(moov + at + 28);
    } else {
        *timescale = Rd32(moov + at + 20);
    }
    return *timescale != 0;
}

static BOOL PatchTrackNextId(BYTE *moov, UINT32 moovSize, UINT32 nextId)
{
    UINT32 mvhdAt, mvhdSize;
    if (!FindChild(moov, 0, moovSize, "mvhd", &mvhdAt, &mvhdSize) || mvhdSize < 12)
        return FALSE;
    Wr32(moov + mvhdAt + mvhdSize - 4, nextId);
    return TRUE;
}

static BOOL CopyFlacFrames(HANDLE src, HANDLE dst, const FlacEncodedTrack *t)
{
    LARGE_INTEGER size;
    UINT64 sum = 0;
    UINT32 i;
    if (!GetFileSizeEx(src, &size) || size.QuadPart < FLAC_HEADER_BYTES)
        return FALSE;
    for (i = 0; i < t->frameCount; i++) {
        if (t->frames[i].offset != sum || UINT64_MAX - sum < t->frames[i].size)
            return FALSE;
        sum += t->frames[i].size;
    }
    if ((UINT64)size.QuadPart != FLAC_HEADER_BYTES + sum)
        return FALSE;
    return CopyRange(src, dst, FLAC_HEADER_BYTES, sum);
}

static BOOL ContainsAscii(const BYTE *data, SIZE_T bytes, const char *needle)
{
    SIZE_T n = strlen(needle), i;
    if (!data || !needle || !n || bytes < n) return FALSE;
    for (i = 0; i <= bytes - n; ++i)
        if (memcmp(data + i, needle, n) == 0)
            return TRUE;
    return FALSE;
}

/* Returns TRUE when at least one of our named FLAC tracks already exists.
   Treat a partially labelled file as occupied too: never append duplicate
   recovery tracks automatically to a file that may need manual inspection. */
BOOL Mp4Multi_HasFlacTracks(const wchar_t *mp4)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    TopBox mdat, moov;
    BYTE *data = NULL;
    BOOL hasMic, hasSystem, result = FALSE;

    ZeroMemory(&mdat, sizeof(mdat));
    ZeroMemory(&moov, sizeof(moov));
    if (!mp4 || !mp4[0]) return FALSE;
    file = CreateFileW(mp4, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                       FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart < 16)
        goto done;
    if (!ReadTopBoxes(file, (UINT64)size.QuadPart, &mdat, &moov) ||
        moov.size > MAX_MOOV_BYTES)
        goto done;
    data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)moov.size);
    if (!data || !ReadAt(file, moov.offset, data, (DWORD)moov.size))
        goto done;
    hasMic = ContainsAscii(data, (SIZE_T)moov.size, "Microphone (FLAC)");
    hasSystem = ContainsAscii(data, (SIZE_T)moov.size, "System Audio (FLAC)");
    result = hasMic && hasSystem;
    if (hasMic != hasSystem)
        Log_Printf(L"mp4multi: only one recovered FLAC track label exists in %s; bundle is incomplete", mp4);

done:
    if (data) HeapFree(GetProcessHeap(), 0, data);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return result;
}

BOOL Mp4Multi_AddFlacTracks(const wchar_t *mp4,
                            const FlacEncodedTrack *microphone,
                            const FlacEncodedTrack *systemAudio)
{
    HANDLE src = INVALID_HANDLE_VALUE, tmp = INVALID_HANDLE_VALUE;
    HANDLE micFile = INVALID_HANDLE_VALUE, sysFile = INVALID_HANDLE_VALUE;
    LARGE_INTEGER fileSize;
    TopBox mdat, moovBox;
    BYTE *oldMoov = NULL, *newMoov = NULL;
    wchar_t temp[MAX_PATH];
    BoxBuffer micTrack, sysTrack, newHeader;
    AudioTrackLocation audioLoc;
    UINT32 maxTrackId = 0, movieTimescale = 0;
    UINT32 oldMoovSize, deltaHandler, handlerNewSize;
    UINT64 micBytes = 0, sysBytes = 0, mediaExtra;
    UINT64 intermediateBytes, newMoovLen64;
    DWORD newMoovLen;
    BOOL ok = FALSE;
    BOOL promoteMdat = FALSE;
    ULONGLONG started = GetTickCount64();

    ZeroMemory(&mdat, sizeof(mdat)); ZeroMemory(&moovBox, sizeof(moovBox));
    ZeroMemory(&micTrack, sizeof(micTrack)); ZeroMemory(&sysTrack, sizeof(sysTrack));
    ZeroMemory(&newHeader, sizeof(newHeader)); ZeroMemory(&audioLoc, sizeof(audioLoc));

    if (!mp4 || !microphone || !systemAudio ||
        !microphone->frameCount || !systemAudio->frameCount ||
        !PathFileExistsW(mp4))
        return FALSE;
    if (FAILED(StringCchPrintfW(temp, ARRAYSIZE(temp), L"%s.mt.tmp", mp4)))
        return FALSE;
    DeleteFileW(temp);

    src = CreateFileW(mp4, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (src == INVALID_HANDLE_VALUE || !GetFileSizeEx(src, &fileSize) || fileSize.QuadPart < 16)
        goto done;
    if (!ReadTopBoxes(src, (UINT64)fileSize.QuadPart, &mdat, &moovBox) ||
        moovBox.size > MAX_MOOV_BYTES || moovBox.headerSize != 8)
        goto done;
    oldMoovSize = (UINT32)moovBox.size;
    if (oldMoovSize < 16) goto done;
    oldMoov = (BYTE *)HeapAlloc(GetProcessHeap(), 0, oldMoovSize);
    if (!oldMoov) goto done;
    if (!ReadAt(src, moovBox.offset, oldMoov, oldMoovSize)) goto done;

    if (!FindAudioTrack(oldMoov, oldMoovSize, &audioLoc, &maxTrackId) ||
        !GetMovieTimescale(oldMoov, oldMoovSize, &movieTimescale) ||
        maxTrackId > UINT32_MAX - 3u || audioLoc.hdlrSize < 32u)
        goto done;

    if (!BuildFlacTrack(&micTrack, microphone, "Microphone (FLAC)",
                        maxTrackId + 1u, movieTimescale, &micBytes) ||
        !BuildFlacTrack(&sysTrack, systemAudio, "System Audio (FLAC)",
                        maxTrackId + 2u, movieTimescale, &sysBytes))
        goto done;

    /* Rebase sample offsets into the new mdat payload. */
    if (UINT64_MAX - micBytes < sysBytes) goto done;
    mediaExtra = micBytes + sysBytes;
    if (mdat.size > UINT64_MAX - mediaExtra) goto done;
    promoteMdat = (mdat.headerSize == 8u &&
                   mdat.size + mediaExtra > UINT32_MAX);
    if (promoteMdat) {
        UINT64 oldDataStart = mdat.offset + mdat.headerSize;
        UINT64 oldDataEnd = mdat.offset + mdat.size;
        if (mdat.size + mediaExtra > UINT64_MAX - 8u ||
            !PatchExistingChunkOffsets(oldMoov, oldMoovSize,
                                       oldDataStart, oldDataEnd, 8u)) {
            Log_Printf(L"mp4multi: could not safely promote 32-bit mdat to 64-bit size");
            goto done;
        }
        Log_Printf(L"mp4multi: promoting mdat to 64-bit size for payload above 4 GiB");
    }
    {
        UINT64 flacDataOffset = mdat.offset + mdat.size + (promoteMdat ? 8u : 0u);
        if (!PatchCo64Track(micTrack.data, (UINT32)micTrack.size,
                            flacDataOffset, microphone) ||
            !PatchCo64Track(sysTrack.data, (UINT32)sysTrack.size,
                            flacDataOffset + micBytes, systemAudio))
            goto done;
    }

    micFile = CreateFileW(microphone->path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    sysFile = CreateFileW(systemAudio->path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (micFile == INVALID_HANDLE_VALUE || sysFile == INVALID_HANDLE_VALUE)
        goto done;

    /* Replace the AAC handler name with a descriptive track label. */
    if (!BuildHandlerName(&newHeader, oldMoov, audioLoc.hdlrAt,
                          L"Combined (System + Microphone)"))
        goto done;
    handlerNewSize = (UINT32)newHeader.size;
    /* Keep box-size arithmetic bounded even if another muxer writes an
       unusually long existing handler name. */
    if (handlerNewSize < audioLoc.hdlrSize)
        goto done;
    deltaHandler = handlerNewSize - audioLoc.hdlrSize;

    newMoovLen64 = (UINT64)oldMoovSize + deltaHandler + micTrack.size + sysTrack.size;
    if (newMoovLen64 > MAX_MOOV_BYTES || newMoovLen64 > UINT32_MAX)
        goto done;
    newMoovLen = (DWORD)newMoovLen64;
    newMoov = (BYTE *)HeapAlloc(GetProcessHeap(), 0, newMoovLen);
    if (!newMoov) goto done;
    CopyMemory(newMoov, oldMoov, audioLoc.hdlrAt);
    CopyMemory(newMoov + audioLoc.hdlrAt, newHeader.data, newHeader.size);
    CopyMemory(newMoov + audioLoc.hdlrAt + newHeader.size,
               oldMoov + audioLoc.hdlrAt + audioLoc.hdlrSize,
               oldMoovSize - audioLoc.hdlrAt - audioLoc.hdlrSize);
    CopyMemory(newMoov + oldMoovSize + deltaHandler, micTrack.data, micTrack.size);
    CopyMemory(newMoov + oldMoovSize + deltaHandler + micTrack.size,
               sysTrack.data, sysTrack.size);

    /* All ancestors begin before hdlr, so their starting offsets did not move. */
    Wr32(newMoov + audioLoc.hdlrAt, handlerNewSize);
    if ((UINT64)Rd32(oldMoov + audioLoc.mdiaAt) + deltaHandler > UINT32_MAX ||
        (UINT64)Rd32(oldMoov + audioLoc.trakAt) + deltaHandler > UINT32_MAX)
        goto done;
    Wr32(newMoov + audioLoc.mdiaAt, Rd32(oldMoov + audioLoc.mdiaAt) + deltaHandler);
    Wr32(newMoov + audioLoc.trakAt, Rd32(oldMoov + audioLoc.trakAt) + deltaHandler);
    Wr32(newMoov, newMoovLen);
    /* Audio tracks are alternatives in the container's selection metadata. */
    if (newMoov[audioLoc.tkhdAt + 8] == 1) {
        if (audioLoc.tkhdSize < 56) goto done;
        newMoov[audioLoc.tkhdAt + 54] = 0;
        newMoov[audioLoc.tkhdAt + 55] = 1;
    } else {
        if (audioLoc.tkhdSize < 44) goto done;
        newMoov[audioLoc.tkhdAt + 42] = 0;
        newMoov[audioLoc.tkhdAt + 43] = 1;
    }
    if (!PatchTrackNextId(newMoov, newMoovLen, maxTrackId + 3u))
        goto done;

    tmp = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (tmp == INVALID_HANDLE_VALUE) goto done;

    /* Preserve all pre-mdat bytes. Promote an 8-byte header to largesize when
       the appended FLAC frames would exceed the 32-bit box-size limit. */
    if (!CopyRange(src, tmp, 0, mdat.offset)) goto done;
    {
        BYTE hdr[16];
        if (promoteMdat) {
            Wr32(hdr, 1u);
            CopyMemory(hdr + 4, "mdat", 4);
            Wr64(hdr + 8, mdat.size + mediaExtra + 8u);
            if (!WriteAll(tmp, hdr, (DWORD)sizeof(hdr))) goto done;
        } else {
            if (!ReadAt(src, mdat.offset, hdr, mdat.headerSize)) goto done;
            if (mdat.headerSize == 8u)
                Wr32(hdr, (UINT32)(mdat.size + mediaExtra));
            else
                Wr64(hdr + 8, mdat.size + mediaExtra);
            if (!WriteAll(tmp, hdr, mdat.headerSize)) goto done;
        }
    }
    if (!CopyRange(src, tmp, mdat.offset + mdat.headerSize,
                   mdat.size - mdat.headerSize)) goto done;
    if (!CopyFlacFrames(micFile, tmp, microphone) ||
        !CopyFlacFrames(sysFile, tmp, systemAudio)) goto done;

    intermediateBytes = moovBox.offset - (mdat.offset + mdat.size);
    if (!CopyRange(src, tmp, mdat.offset + mdat.size, intermediateBytes)) goto done;
    if (!WriteAll(tmp, newMoov, newMoovLen)) goto done;

    CloseHandle(tmp); tmp = INVALID_HANDLE_VALUE;
    CloseHandle(src); src = INVALID_HANDLE_VALUE;
    CloseHandle(micFile); micFile = INVALID_HANDLE_VALUE;
    CloseHandle(sysFile); sysFile = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temp, mp4, MOVEFILE_REPLACE_EXISTING)) {
        Log_Printf(L"mp4multi: cannot replace MP4 %s (%lu)", mp4, GetLastError());
        goto done;
    }
    ok = TRUE;
    Log_Printf(L"mp4multi: embedded labeled Combined AAC, Microphone FLAC, System Audio FLAC tracks in %llu ms",
               (unsigned long long)(GetTickCount64() - started));

done:
    if (src != INVALID_HANDLE_VALUE) CloseHandle(src);
    if (tmp != INVALID_HANDLE_VALUE) CloseHandle(tmp);
    if (micFile != INVALID_HANDLE_VALUE) CloseHandle(micFile);
    if (sysFile != INVALID_HANDLE_VALUE) CloseHandle(sysFile);
    if (oldMoov) HeapFree(GetProcessHeap(), 0, oldMoov);
    if (newMoov) HeapFree(GetProcessHeap(), 0, newMoov);
    BufFree(&micTrack); BufFree(&sysTrack); BufFree(&newHeader);
    if (!ok) {
        DeleteFileW(temp);
        Log_Printf(L"mp4multi: failed to add FLAC tracks; original MP4 kept intact");
    }
    return ok;
}
