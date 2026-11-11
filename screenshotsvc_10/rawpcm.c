/*
 * rawpcm.c - a small sequential raw-PCM sink with gap filling from WASAPI's
 * packet QPC timestamps. The callback does no codec work; only cached file
 * writes happen here, and encoding is deferred until capture has stopped.
 */
#include "screenshotservice.h"
#include "rawpcm.h"
#include "log.h"
#include <string.h>
#include <strsafe.h>

#define RAW_ZERO_BYTES 16384u

struct RawPcmSink {
    HANDLE  file;
    UINT32  rate;
    UINT32  blockAlign;
    UINT64  origin100ns;
    UINT64  nextFrame;
    BOOL    failed;
    BYTE    silenceByte;
    wchar_t path[MAX_PATH];
};

static BOOL WriteAll(RawPcmSink *s, const BYTE *data, DWORD bytes)
{
    DWORD wrote = 0;
    if (!bytes)
        return TRUE;
    if (!WriteFile(s->file, data, bytes, &wrote, NULL) || wrote != bytes) {
        if (!s->failed)
            Log_Printf(L"raw audio: write failed for %s (%lu)", s->path, GetLastError());
        s->failed = TRUE;
        return FALSE;
    }
    return TRUE;
}

static BOOL WriteSilence(RawPcmSink *s, UINT64 frames)
{
    static const BYTE zero[RAW_ZERO_BYTES] = { 0 };
    BYTE filled[RAW_ZERO_BYTES];
    const BYTE *silence = zero;
    UINT64 bytes = frames * (UINT64)s->blockAlign;
    if (s->silenceByte) {
        memset(filled, s->silenceByte, sizeof(filled));
        silence = filled;
    }
    while (bytes) {
        DWORD take = bytes > RAW_ZERO_BYTES ? RAW_ZERO_BYTES : (DWORD)bytes;
        /* A silence block should end on a sample-frame boundary. */
        take -= take % s->blockAlign;
        if (!take)
            take = s->blockAlign;
        if (!WriteAll(s, silence, take))
            return FALSE;
        bytes -= take;
    }
    s->nextFrame += frames;
    return TRUE;
}

RawPcmSink *RawPcm_Open(const wchar_t *path, UINT32 sampleRate,
                        UINT32 blockAlign, UINT64 origin100ns)
{
    RawPcmSink *s;
    if (!path || !sampleRate || !blockAlign)
        return NULL;
    s = (RawPcmSink *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    if (!s)
        return NULL;
    s->file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (s->file == INVALID_HANDLE_VALUE) {
        Log_Printf(L"raw audio: cannot create %s (%lu)", path, GetLastError());
        HeapFree(GetProcessHeap(), 0, s);
        return NULL;
    }
    s->rate = sampleRate;
    s->blockAlign = blockAlign;
    s->origin100ns = origin100ns;
    StringCchCopyW(s->path, ARRAYSIZE(s->path), path);
    Log_Printf(L"raw audio: opened %s at %lu Hz, block align %u",
               path, (unsigned long)sampleRate, blockAlign);
    return s;
}

BOOL RawPcm_WriteTimed(RawPcmSink *s, const BYTE *data, UINT32 frames,
                       BOOL silent, UINT64 packetQpc100ns)
{
    UINT32 skip = 0;
    UINT64 target = 0;
    UINT64 bytes;

    if (!s || s->failed)
        return FALSE;
    if (!frames)
        return TRUE;

    if (packetQpc100ns && s->origin100ns) {
        if (packetQpc100ns < s->origin100ns) {
            UINT64 lead = s->origin100ns - packetQpc100ns;
            UINT64 leadFrames = (lead * s->rate + 9999999u) / 10000000u;
            if (leadFrames >= frames)
                return TRUE;
            skip = (UINT32)leadFrames;
            packetQpc100ns += (leadFrames * 10000000u) / s->rate;
        }
        target = ((packetQpc100ns - s->origin100ns) * s->rate + 5000000u) / 10000000u;

        if (target > s->nextFrame + 2u) {
            if (!WriteSilence(s, target - s->nextFrame))
                return FALSE;
        } else if (target + frames < s->nextFrame && s->nextFrame - (target + frames) > 2u) {
            /* A duplicate/late packet is safer to discard than to insert at the
               wrong point and move everything after it. */
            return TRUE;
        } else if (target < s->nextFrame && s->nextFrame - target > 2u) {
            UINT64 overlap = s->nextFrame - target;
            if (overlap >= frames)
                return TRUE;
            skip += (UINT32)overlap;
        }
        /* Ignore one- or two-frame timestamp rounding noise. */
    }

    if (skip >= frames)
        return TRUE;
    frames -= skip;
    if (!silent) {
        if (!data) {
            s->failed = TRUE;
            return FALSE;
        }
        data += (SIZE_T)skip * s->blockAlign;
    }
    bytes = (UINT64)frames * s->blockAlign;
    if (bytes > MAXDWORD) {
        s->failed = TRUE;
        return FALSE;
    }
    if (silent) {
        if (!WriteSilence(s, frames))
            return FALSE;
    } else {
        if (!WriteAll(s, data, (DWORD)bytes))
            return FALSE;
        s->nextFrame += frames;
    }
    return TRUE;
}

BOOL RawPcm_PadTo(RawPcmSink *s, UINT64 targetFrames)
{
    if (!s || s->failed)
        return FALSE;
    if (targetFrames <= s->nextFrame)
        return TRUE;
    return WriteSilence(s, targetFrames - s->nextFrame);
}

void RawPcm_SetSilenceByte(RawPcmSink *s, BYTE value)
{
    if (s)
        s->silenceByte = value;
}

BOOL RawPcm_Close(RawPcmSink *s, UINT64 *framesWritten)
{
    BOOL ok;
    if (!s)
        return FALSE;
    if (framesWritten)
        *framesWritten = s->nextFrame;
    ok = !s->failed;
    if (!CloseHandle(s->file))
        ok = FALSE;
    HeapFree(GetProcessHeap(), 0, s);
    return ok;
}
