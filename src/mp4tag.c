/*
 * mp4tag.c - inserting a 'colr' box into a finished MP4.
 */

#include "nitshot.h"
#include "mp4tag.h"
#include "log.h"

#define COLR_SIZE 19        /* 8 header + 'nclx' + 3 u16 + 1 flags byte     */
#define MDCV_SIZE 32        /* 8 header + 6 u16 primaries + 2 u16 white + 2 u32 */
#define CLLI_SIZE 12        /* 8 header + 2 u16                              */
#define MAX_BOXES (COLR_SIZE + MDCV_SIZE + CLLI_SIZE)

static unsigned RdU32(const BYTE *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static void WrU32(BYTE *p, unsigned v)
{
    p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16);
    p[2] = (BYTE)(v >> 8);  p[3] = (BYTE)v;
}

static void WrU16(BYTE *p, UINT16 v)
{
    p[0] = (BYTE)(v >> 8); p[1] = (BYTE)v;
}

static BOOL IsType(const BYTE *p, const char *t)
{
    return p[4] == (BYTE)t[0] && p[5] == (BYTE)t[1] &&
           p[6] == (BYTE)t[2] && p[7] == (BYTE)t[3];
}

/*
 * Depth-first search for the video sample entry, remembering the chain of
 * ancestors on the way down so their sizes can be grown afterwards. Returns
 * the offset of the sample entry within 'moov', or 0.
 *
 * 'chain' receives the offsets of every enclosing box, 'depth' their count.
 */
static unsigned FindSampleEntry(const BYTE *buf, unsigned base, unsigned end,
                                unsigned *chain, int *depth, int maxDepth,
                                BOOL *alreadyTagged)
{
    unsigned p = base;

    while (p + 8 <= end) {
        unsigned size = RdU32(buf + p);
        unsigned body = p + 8;

        /* Compared as a difference: 'p + size' wraps for a corrupt size near
           4 GB and would sail past the check it is meant to be. */
        if (size < 8 || size > end - p)
            return 0;

        if (IsType(buf + p, "hvc1") || IsType(buf + p, "hev1") ||
            IsType(buf + p, "avc1")) {
            /* VisualSampleEntry is 78 bytes before its child boxes start. */
            unsigned c = body + 78;
            unsigned e = p + size;           /* in range: checked above */
            while (c < e && e - c >= 8) {
                unsigned csz = RdU32(buf + c);
                if (csz < 8 || csz > e - c)
                    break;
                if (IsType(buf + c, "colr")) {
                    *alreadyTagged = TRUE;
                    return 0;
                }
                c += csz;
            }
            return p;
        }

        if (IsType(buf + p, "moov") || IsType(buf + p, "trak") ||
            IsType(buf + p, "mdia") || IsType(buf + p, "minf") ||
            IsType(buf + p, "stbl") || IsType(buf + p, "stsd")) {
            unsigned inner = IsType(buf + p, "stsd") ? body + 8 : body;
            unsigned hit;

            if (*depth >= maxDepth)
                return 0;
            chain[*depth] = p;
            (*depth)++;
            hit = FindSampleEntry(buf, inner, p + size, chain, depth, maxDepth,
                                  alreadyTagged);
            if (hit)
                return hit;
            (*depth)--;
        }
        p += size;
    }
    return 0;
}

/* CIE xy is carried in units of 0.00002, luminance in units of 0.0001 cd/m2. */
static UINT16 Xy(float v)
{
    double u = (double)v * 50000.0 + 0.5;
    if (u < 0.0)     u = 0.0;
    if (u > 65535.0) u = 65535.0;
    return (UINT16)u;
}

static UINT32 Lum(float nits)
{
    double u = (double)nits * 10000.0 + 0.5;
    if (u < 0.0)          u = 0.0;
    if (u > 4294967295.0) u = 4294967295.0;
    return (UINT32)u;
}

static UINT16 Nits16(float nits)
{
    double u = (double)nits + 0.5;
    if (u < 0.0)     u = 0.0;
    if (u > 65535.0) u = 65535.0;
    return (UINT16)u;
}

/* Builds the boxes to append, and returns how many bytes they take. */
static unsigned BuildBoxes(const Mp4ColourInfo *info, BYTE *out)
{
    unsigned n = 0;

    WrU32(out + n, COLR_SIZE);
    out[n + 4] = 'c'; out[n + 5] = 'o'; out[n + 6] = 'l'; out[n + 7] = 'r';
    out[n + 8] = 'n'; out[n + 9] = 'c'; out[n + 10] = 'l'; out[n + 11] = 'x';
    WrU16(out + n + 12, info->primaries);
    WrU16(out + n + 14, info->transfer);
    WrU16(out + n + 16, info->matrix);
    out[n + 18] = info->fullRange ? 0x80 : 0x00;
    n += COLR_SIZE;

    if (info->greenX > 0.0f && info->whiteX > 0.0f) {
        WrU32(out + n, MDCV_SIZE);
        out[n + 4] = 'm'; out[n + 5] = 'd'; out[n + 6] = 'c'; out[n + 7] = 'v';
        /* Green, blue, red - that is the order this box and the matching SEI
           use, and getting it wrong swaps two of the primaries silently. */
        WrU16(out + n + 8,  Xy(info->greenX)); WrU16(out + n + 10, Xy(info->greenY));
        WrU16(out + n + 12, Xy(info->blueX));  WrU16(out + n + 14, Xy(info->blueY));
        WrU16(out + n + 16, Xy(info->redX));   WrU16(out + n + 18, Xy(info->redY));
        WrU16(out + n + 20, Xy(info->whiteX)); WrU16(out + n + 22, Xy(info->whiteY));
        WrU32(out + n + 24, Lum(info->maxMasteringNits));
        WrU32(out + n + 28, Lum(info->minMasteringNits));
        n += MDCV_SIZE;
    }

    if (info->maxCLL > 0.0f) {
        WrU32(out + n, CLLI_SIZE);
        out[n + 4] = 'c'; out[n + 5] = 'l'; out[n + 6] = 'l'; out[n + 7] = 'i';
        WrU16(out + n + 8,  Nits16(info->maxCLL));
        WrU16(out + n + 10, Nits16(info->maxFALL));
        n += CLLI_SIZE;
    }
    return n;
}

/* ------------------------------------------------------- moov in memory */

/*
 * Reads the 'moov' box of a file whose 'moov' is the last box. That layout is
 * what makes growing it safe: inserting bytes inside it leaves every 'stco'
 * chunk offset - which all point into the earlier 'mdat' - still correct.
 */
static BOOL LoadMoov(HANDLE f, UINT64 *moovAt, BYTE **moov, unsigned *moovLen)
{
    LARGE_INTEGER fileSize, seek;
    BYTE          header[16];
    UINT64        pos = 0, at = 0, len = 0, mdatAt = 0;
    DWORD         got = 0;

    *moov = NULL;
    if (!GetFileSizeEx(f, &fileSize))
        return FALSE;

    /* Walk the top level only; the boxes of interest are both there. */
    while (pos + 8 <= (UINT64)fileSize.QuadPart) {
        UINT64 size;

        seek.QuadPart = (LONGLONG)pos;
        if (!SetFilePointerEx(f, seek, NULL, FILE_BEGIN) ||
            !ReadFile(f, header, 16, &got, NULL) || got < 8)
            return FALSE;

        size = RdU32(header);
        if (size == 1) {
            if (got < 16)
                return FALSE;
            size = ((UINT64)RdU32(header + 8) << 32) | RdU32(header + 12);
        } else if (size == 0) {
            size = (UINT64)fileSize.QuadPart - pos;
        }
        if (size < 8)
            return FALSE;

        if (IsType(header, "moov")) { at = pos; len = size; }
        else if (IsType(header, "mdat")) mdatAt = pos;

        pos += size;
    }

    if (!at || !len) {
        Log_Printf(L"mp4tag: no moov box");
        return FALSE;
    }
    /*
     * Both conditions matter. If moov came first, growing it would push mdat
     * along and every chunk offset in stco would be wrong. If anything follows
     * moov, rewriting the tail would destroy it.
     */
    if (mdatAt > at || at + len != (UINT64)fileSize.QuadPart) {
        Log_Printf(L"mp4tag: moov is not the last box, leaving the file alone");
        return FALSE;
    }
    if (len > 64u * 1024u * 1024u)
        return FALSE;

    *moov = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)len);
    if (!*moov)
        return FALSE;
    seek.QuadPart = (LONGLONG)at;
    if (!SetFilePointerEx(f, seek, NULL, FILE_BEGIN) ||
        !ReadFile(f, *moov, (DWORD)len, &got, NULL) || got != len) {
        HeapFree(GetProcessHeap(), 0, *moov);
        *moov = NULL;
        return FALSE;
    }
    *moovAt  = at;
    *moovLen = (unsigned)len;
    return TRUE;
}

static BOOL StoreMoov(HANDLE f, UINT64 moovAt, const BYTE *moov, unsigned len)
{
    LARGE_INTEGER seek;
    DWORD         got = 0;

    seek.QuadPart = (LONGLONG)moovAt;
    if (!SetFilePointerEx(f, seek, NULL, FILE_BEGIN))
        return FALSE;
    if (!WriteFile(f, moov, len, &got, NULL) || got != len)
        return FALSE;
    return SetEndOfFile(f);
}

static HANDLE OpenForEdit(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        Log_Printf(L"mp4tag: cannot open %s (%lu)", path, GetLastError());
    return f;
}

/* -------------------------------------------------------------- colour */

BOOL Mp4Tag_WriteColour(const wchar_t *path, const Mp4ColourInfo *info)
{
    HANDLE        f;
    UINT64        moovAt = 0;
    unsigned      moovLen = 0;
    BYTE         *moov = NULL;
    BYTE         *out = NULL;
    unsigned      chain[8], entry, added = 0;
    int           depth = 0, i;
    BOOL          tagged = FALSE, ok = FALSE;

    if (!info)
        return FALSE;

    f = OpenForEdit(path);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!LoadMoov(f, &moovAt, &moov, &moovLen))
        goto done;

    entry = FindSampleEntry(moov, 0, moovLen, chain, &depth,
                            (int)ARRAYSIZE(chain), &tagged);
    if (tagged) {
        Log_Printf(L"mp4tag: colr already present");
        ok = TRUE;
        goto done;
    }
    if (!entry) {
        Log_Printf(L"mp4tag: no video sample entry found");
        goto done;
    }

    out = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)moovLen + MAX_BOXES);
    if (!out)
        goto done;

    {
        unsigned entrySize = RdU32(moov + entry);
        unsigned insertAt  = entry + entrySize;   /* after the sample entry's
                                                     existing children */
        BYTE     boxes[MAX_BOXES];

        added = BuildBoxes(info, boxes);

        memcpy(out, moov, insertAt);
        memcpy(out + insertAt, boxes, added);
        memcpy(out + insertAt + added, moov + insertAt,
               (SIZE_T)(moovLen - insertAt));

        /* The sample entry itself, then every box that contains it. */
        WrU32(out + entry, entrySize + added);
        for (i = 0; i < depth; i++)
            WrU32(out + chain[i], RdU32(out + chain[i]) + added);
    }

    if (!StoreMoov(f, moovAt, out, moovLen + added))
        goto done;
    ok = TRUE;
    Log_Printf(L"mp4tag: colr (%u/%u/%u)%s%s written",
               info->primaries, info->transfer, info->matrix,
               (info->greenX > 0.0f && info->whiteX > 0.0f) ? L" + mdcv" : L"",
               info->maxCLL > 0.0f ? L" + clli" : L"");
    if (info->maxCLL > 0.0f)
        Log_Printf(L"mp4tag: MaxCLL %.0f nits, MaxFALL %.0f nits, mastering "
                   L"%.0f..%.4f nits",
                   info->maxCLL, info->maxFALL,
                   info->maxMasteringNits, info->minMasteringNits);

done:
    if (out)  HeapFree(GetProcessHeap(), 0, out);
    if (moov) HeapFree(GetProcessHeap(), 0, moov);
    CloseHandle(f);
    return ok;
}

/* ---------------------------------------------------------- track names */

#define NAME_UTF8_MAX 400   /* 128 UTF-16 units as UTF-8, with room to spare */

/* The first child of the given type directly inside [at, end), or 0. */
static unsigned FindChild(const BYTE *buf, unsigned at, unsigned end, const char *type)
{
    while (at + 8 <= end) {
        unsigned size = RdU32(buf + at);
        if (size < 8 || size > end - at)
            return 0;
        if (IsType(buf + at, type))
            return at;
        at += size;
    }
    return 0;
}

static BOOL IsSoundTrak(const BYTE *buf, unsigned trak)
{
    unsigned end  = trak + RdU32(buf + trak);
    unsigned mdia = FindChild(buf, trak + 8, end, "mdia");
    unsigned hdlr = mdia ? FindChild(buf, mdia + 8, mdia + RdU32(buf + mdia), "hdlr") : 0;
    /* hdlr: header, version/flags, pre_defined, then the handler type. */
    return hdlr && RdU32(buf + hdlr) >= 20 && memcmp(buf + hdlr + 16, "soun", 4) == 0;
}

static unsigned PutBox(BYTE *out, unsigned n, const char *type, const BYTE *body,
                       unsigned len)
{
    WrU32(out + n, 8 + len);
    memcpy(out + n + 4, type, 4);
    memcpy(out + n + 8, body, len);
    return n + 8 + len;
}

/*
 * Copies a sound 'trak' with its name set in two places, because players look
 * in one or the other: the handler name in 'mdia/hdlr' (what LAV, MPC and
 * ffmpeg's handler_name show) and a 'udta/name' box (what VLC shows). Returns
 * the bytes written to 'out'.
 */
static unsigned CopyNamedTrak(const BYTE *buf, unsigned trak, const char *name,
                              unsigned nameLen, BYTE *out)
{
    unsigned end = trak + RdU32(buf + trak);
    unsigned c = trak + 8, n = 8;

    memcpy(out, buf + trak, 8);
    while (c + 8 <= end) {
        unsigned size = RdU32(buf + c);
        if (size < 8 || size > end - c)
            break;
        if (IsType(buf + c, "mdia")) {
            unsigned mEnd = c + size, m = c + 8, start = n;
            memcpy(out + n, buf + c, 8);
            n += 8;
            while (m + 8 <= mEnd) {
                unsigned ms = RdU32(buf + m);
                if (ms < 8 || ms > mEnd - m)
                    break;
                if (IsType(buf + m, "hdlr") && ms >= 32) {
                    /* Everything up to the name - version/flags, pre_defined,
                       handler type, three reserved words - then the new name
                       and its terminator. */
                    BYTE body[24 + NAME_UTF8_MAX + 1];
                    memcpy(body, buf + m + 8, 24);
                    memcpy(body + 24, name, nameLen);
                    body[24 + nameLen] = 0;
                    n = PutBox(out, n, "hdlr", body, 24 + nameLen + 1);
                } else {
                    memcpy(out + n, buf + m, ms);
                    n += ms;
                }
                m += ms;
            }
            WrU32(out + start, n - start);
        } else if (!IsType(buf + c, "udta")) {
            memcpy(out + n, buf + c, size);
            n += size;
        }
        /* An existing udta is replaced: the sink writes none, and keeping one
           would mean merging two 'name' boxes. */
        c += size;
    }
    {
        BYTE     udta[8 + NAME_UTF8_MAX];
        unsigned u = PutBox(udta, 0, "name", (const BYTE *)name, nameLen);
        n = PutBox(out, n, "udta", udta, u);
    }
    WrU32(out, n);
    return n;
}

BOOL Mp4Tag_NameAudioTracks(const wchar_t *path, const wchar_t *const *names, int count)
{
    HANDLE   f;
    UINT64   moovAt = 0;
    unsigned moovLen = 0, c, n, named = 0;
    BYTE    *moov = NULL, *out = NULL;
    BOOL     ok = FALSE;

    if (!path || !names || count <= 0)
        return FALSE;
    f = OpenForEdit(path);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!LoadMoov(f, &moovAt, &moov, &moovLen))
        goto done;
    out = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                            (SIZE_T)moovLen + (SIZE_T)count * (2 * NAME_UTF8_MAX + 64));
    if (!out)
        goto done;

    memcpy(out, moov, 8);
    n = 8;
    for (c = 8; c + 8 <= moovLen; ) {
        unsigned size = RdU32(moov + c);
        if (size < 8 || size > moovLen - c)
            goto done;
        if (IsType(moov + c, "trak") && (int)named < count && IsSoundTrak(moov, c)) {
            char utf8[NAME_UTF8_MAX];
            int  len = WideCharToMultiByte(CP_UTF8, 0, names[named], -1, utf8,
                                           (int)sizeof(utf8), NULL, NULL);
            if (len <= 1)
                len = 1;                 /* unconvertible: an empty name */
            n += CopyNamedTrak(moov, c, utf8, (unsigned)(len - 1), out + n);
            ++named;
        } else {
            memcpy(out + n, moov + c, size);
            n += size;
        }
        c += size;
    }
    WrU32(out, n);
    if (!StoreMoov(f, moovAt, out, n))
        goto done;
    ok = TRUE;
    Log_Printf(L"mp4tag: %u audio track(s) named", named);

done:
    if (out)  HeapFree(GetProcessHeap(), 0, out);
    if (moov) HeapFree(GetProcessHeap(), 0, moov);
    CloseHandle(f);
    return ok;
}
