#define COBJMACROS
#include "screenshotservice.h"
#include "flacenc.h"
#include "flac_core.h"
#include "log.h"

#include <mmreg.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>

static const GUID FLAC_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID FLAC_SUBTYPE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

typedef union {
    WAVEFORMATEXTENSIBLE ext;
    BYTE bytes[256];
} WaveFormatStorage;

static BOOL WriteBytes(HANDLE f, const BYTE *p, DWORD bytes)
{
    DWORD wrote = 0;
    return WriteFile(f, p, bytes, &wrote, NULL) && wrote == bytes;
}

static BOOL ReadAt(HANDLE f, UINT64 offset, BYTE *p, DWORD bytes, DWORD *got)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f, pos, NULL, FILE_BEGIN))
        return FALSE;
    return ReadFile(f, p, bytes, got, NULL);
}

static BOOL IsSubtype(const WAVEFORMATEX *f, const GUID *subtype)
{
    const WAVEFORMATEXTENSIBLE *ext;
    if (f->wFormatTag == WAVE_FORMAT_PCM)
        return IsEqualGUID(subtype, &FLAC_SUBTYPE_PCM);
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return IsEqualGUID(subtype, &FLAC_SUBTYPE_FLOAT);
    if (f->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        f->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return FALSE;
    ext = (const WAVEFORMATEXTENSIBLE *)f;
    return IsEqualGUID(&ext->SubFormat, subtype);
}

static UINT32 ValidBits(const WAVEFORMATEX *f, UINT32 bytesAvailable)
{
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
        bytesAvailable >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)f;
        if (e->Samples.wValidBitsPerSample)
            return e->Samples.wValidBitsPerSample;
    }
    return f->wBitsPerSample;
}

static INT32 FloatToS32(double v)
{
    double scaled;
    if (v <= -1.0)
        return (INT32)0x80000000u;
    if (v >= 1.0)
        return INT32_MAX;
    scaled = v * 2147483648.0;
    if (scaled < 0.0)
        scaled -= 0.5;
    else
        scaled += 0.5;
    if (scaled <= -2147483648.0)
        return (INT32)0x80000000u;
    if (scaled >= 2147483647.0)
        return INT32_MAX;
    return (INT32)scaled;
}

static INT32 ReadIntegerSample(const BYTE *p, UINT32 containerBits,
                               UINT32 validBits)
{
    INT32 sample;
    if (containerBits == 8) {
        sample = (INT32)p[0] - 128;
    } else if (containerBits == 16) {
        INT16 v;
        CopyMemory(&v, p, sizeof(v));
        sample = v;
    } else if (containerBits == 24) {
        UINT32 v = (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16);
        if (v & 0x00800000u)
            v |= 0xff000000u;
        sample = (INT32)v;
    } else {
        CopyMemory(&sample, p, sizeof(sample));
    }
    if (validBits < containerBits)
        sample >>= (containerBits - validBits);
    return sample;
}

static BOOL ConvertChunk(const BYTE *raw, UINT32 frames, const WAVEFORMATEX *f,
                         UINT32 blockAlign, UINT32 validBits, BOOL isFloat,
                         UINT32 outBits, INT32 *out)
{
    UINT32 frame, ch;
    UINT32 containerBits = f->wBitsPerSample;
    UINT32 bytesPerSample = (containerBits + 7u) / 8u;
    if (blockAlign < bytesPerSample * f->nChannels || f->nChannels > 8)
        return FALSE;
    for (frame = 0; frame < frames; frame++) {
        const BYTE *row = raw + (SIZE_T)frame * blockAlign;
        for (ch = 0; ch < f->nChannels; ch++) {
            const BYTE *p = row + (SIZE_T)ch * bytesPerSample;
            INT32 v;
            if (isFloat) {
                if (containerBits == 32) {
                    float x;
                    CopyMemory(&x, p, sizeof(x));
                    v = FloatToS32((double)x);
                } else if (containerBits == 64) {
                    double x;
                    CopyMemory(&x, p, sizeof(x));
                    v = FloatToS32(x);
                } else {
                    return FALSE;
                }
                /* FloatToS32 returns signed 32-bit full scale. Reduce its
                   precision by rescaling, never by clamping it to a 16/24-bit
                   numeric range (which would turn nearly every sample into
                   full scale). The PCM quantization is therefore correct. */
                if (outBits < 32u)
                    v = (INT32)((INT64)v / ((INT64)1 << (32u - outBits)));
            } else {
                v = ReadIntegerSample(p, containerBits, validBits);
            }
            out[(SIZE_T)frame * f->nChannels + ch] = v;
        }
    }
    return TRUE;
}

static BOOL WriteFlacHeader(HANDLE out, const BYTE info[34])
{
    BYTE header[FLAC_NATIVE_HEADER_BYTES];
    memcpy(header, "fLaC", 4);
    header[4] = 0x80; /* last metadata block, STREAMINFO type */
    header[5] = 0; header[6] = 0; header[7] = 34;
    memcpy(header + 8, info, 34);
    return WriteBytes(out, header, sizeof(header));
}

void Flac_FreeTrack(FlacEncodedTrack *track)
{
    if (!track)
        return;
    if (track->frames)
        HeapFree(GetProcessHeap(), 0, track->frames);
    ZeroMemory(track, sizeof(*track));
}

BOOL Flac_EncodeRaw(const wchar_t *rawPath,
                    const BYTE *formatBytes, UINT32 formatByteCount,
                    UINT64 startSample, UINT64 sampleCount,
                    UINT32 preferredFloatBits,
                    const wchar_t *flacPath, FlacEncodedTrack *outTrack,
                    FlacProgressFn progress, void *progressContext)
{
    HANDLE raw = INVALID_HANDLE_VALUE, out = INVALID_HANDLE_VALUE;
    LARGE_INTEGER rawSize, pos;
    WaveFormatStorage storage;
    WAVEFORMATEX *fmt;
    BOOL isPcm, isFloat, ok = FALSE;
    UINT32 validBits, outBits, blockAlign, bytesPerSample;
    UINT64 availableBytes, frameNo;
    UINT32 frameCount, minBlock, lastBlock;
    BYTE *rawBuf = NULL;
    INT32 *samples = NULL;
    UINT64 outputOffset = 0;
    DWORD got;
    FlacEncodedTrack track;

    ZeroMemory(&track, sizeof(track));
    ZeroMemory(&storage, sizeof(storage));
    if (!rawPath || !formatBytes || formatByteCount < sizeof(WAVEFORMATEX) ||
        !flacPath || !outTrack || sampleCount == 0 || sampleCount > 0xfffffffffu)
        return FALSE;
    if (formatByteCount > sizeof(storage))
        return FALSE;
    CopyMemory(&storage, formatBytes, formatByteCount);
    fmt = &storage.ext.Format;
    if ((UINT32)sizeof(WAVEFORMATEX) + fmt->cbSize > formatByteCount)
        return FALSE;
    isPcm = IsSubtype(fmt, &FLAC_SUBTYPE_PCM);
    isFloat = IsSubtype(fmt, &FLAC_SUBTYPE_FLOAT);
    if ((!isPcm && !isFloat) || !fmt->nChannels || fmt->nChannels > 8 ||
        !fmt->nSamplesPerSec || fmt->nSamplesPerSec > 0xfffffu || !fmt->nBlockAlign)
        goto done;
    validBits = ValidBits(fmt, formatByteCount);
    if (isFloat) {
        if (fmt->wBitsPerSample != 32 && fmt->wBitsPerSample != 64)
            goto done;
        /* IEEE float has no PCM bit depth to copy into FLAC metadata. Use the
           endpoint's configured shared-mode PCM precision when available, not
           the 32-bit storage width of the audio engine's float samples. */
        outBits = (preferredFloatBits >= 4u && preferredFloatBits <= 32u)
            ? preferredFloatBits : 24u;
    } else {
        if (fmt->wBitsPerSample != 8 && fmt->wBitsPerSample != 16 &&
            fmt->wBitsPerSample != 24 && fmt->wBitsPerSample != 32)
            goto done;
        /* FLAC carries any 4-32-bit integer depth. WAVEFORMATEXTENSIBLE can
           advertise useful depths such as 20-bit in a 24/32-bit container. */
        if (validBits < 4 || validBits > 32)
            goto done;
        if (validBits > fmt->wBitsPerSample)
            goto done;
        outBits = validBits;
    }
    blockAlign = fmt->nBlockAlign;
    bytesPerSample = (fmt->wBitsPerSample + 7u) / 8u;
    if (blockAlign < bytesPerSample * fmt->nChannels)
        goto done;
    if (sampleCount > UINT32_MAX * (UINT64)FLAC_FRAME_SAMPLES)
        goto done;
    frameCount = (UINT32)((sampleCount + FLAC_FRAME_SAMPLES - 1u) / FLAC_FRAME_SAMPLES);
    /* sampleCount is capped to FLAC STREAMINFO's 36-bit limit above, so
       frameCount * sizeof(FlacFrameIndex) stays below 256 MiB. */
    if (!frameCount)
        goto done;
    track.frames = (FlacFrameIndex *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                               (SIZE_T)frameCount * sizeof(FlacFrameIndex));
    if (!track.frames)
        goto done;

    raw = CreateFileW(rawPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (raw == INVALID_HANDLE_VALUE) {
        Log_Printf(L"flac: cannot open raw source %s (%lu)", rawPath, GetLastError());
        goto done;
    }
    if (!GetFileSizeEx(raw, &rawSize))
        goto done;
    if (startSample > UINT64_MAX / blockAlign)
        goto done;
    availableBytes = (UINT64)rawSize.QuadPart;
    pos.QuadPart = (LONGLONG)(startSample * (UINT64)blockAlign);
    if (!SetFilePointerEx(raw, pos, NULL, FILE_BEGIN))
        goto done;

    out = CreateFileW(flacPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        Log_Printf(L"flac: cannot create %s (%lu)", flacPath, GetLastError());
        goto done;
    }

    lastBlock = (UINT32)(sampleCount % FLAC_FRAME_SAMPLES);
    if (!lastBlock)
        lastBlock = FLAC_FRAME_SAMPLES;
    if (frameCount > 1) {
        /* All non-final frames use the fixed 4096-sample block size.
           STREAMINFO minimum excludes the final (possibly short) frame. */
        minBlock = FLAC_FRAME_SAMPLES;
    } else {
        /* STREAMINFO block-size fields must be at least 16 even when the one
           and only final frame contains fewer samples. */
        minBlock = lastBlock < 16u ? 16u : lastBlock;
    }
    track.sampleRate = fmt->nSamplesPerSec;
    track.channels = fmt->nChannels;
    track.channelMask = 0;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
        formatByteCount >= sizeof(WAVEFORMATEXTENSIBLE)) {
        track.channelMask = ((const WAVEFORMATEXTENSIBLE *)fmt)->dwChannelMask;
    }
    track.bitsPerSample = outBits;
    track.blockAlign = blockAlign;
    track.totalSamples = sampleCount;
    track.frameCount = frameCount;
    FlacCore_MakeStreamInfo(track.sampleRate, track.channels, track.bitsPerSample,
                            minBlock, frameCount > 1 ? FLAC_FRAME_SAMPLES : minBlock,
                            sampleCount, track.streamInfo);
    if (!WriteFlacHeader(out, track.streamInfo))
        goto done;

    rawBuf = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
        (SIZE_T)FLAC_FRAME_SAMPLES * blockAlign);
    samples = (INT32 *)HeapAlloc(GetProcessHeap(), 0,
        (SIZE_T)FLAC_FRAME_SAMPLES * fmt->nChannels * sizeof(INT32));
    if (!rawBuf || !samples)
        goto done;

    for (frameNo = 0; frameNo < frameCount; frameNo++) {
        UINT64 leftFrames = sampleCount - frameNo * FLAC_FRAME_SAMPLES;
        UINT32 frames = leftFrames > FLAC_FRAME_SAMPLES ? FLAC_FRAME_SAMPLES : (UINT32)leftFrames;
        UINT64 wantBytes = (UINT64)frames * blockAlign;
        UINT64 sourcePos = (startSample + frameNo * FLAC_FRAME_SAMPLES) * (UINT64)blockAlign;
        UINT64 haveBytes = 0;
        unsigned char *encoded = NULL;
        size_t encodedBytes = 0;

        if (sourcePos < availableBytes) {
            UINT64 canRead = availableBytes - sourcePos;
            DWORD request = (DWORD)(canRead < wantBytes ? canRead : wantBytes);
            if (!ReadAt(raw, sourcePos, rawBuf, request, &got))
                goto done;
            haveBytes = got;
        }
        if (haveBytes < wantBytes)
            ZeroMemory(rawBuf + (SIZE_T)haveBytes, (SIZE_T)(wantBytes - haveBytes));
        if (!ConvertChunk(rawBuf, frames, fmt, blockAlign, validBits, isFloat,
                          outBits, samples))
            goto done;
        if (!FlacCore_EncodeVerbatimFrame(samples, frames, track.channels,
                                          track.bitsPerSample, frameNo,
                                          &encoded, &encodedBytes))
            goto done;
        if (encodedBytes > UINT32_MAX || outputOffset > UINT64_MAX - encodedBytes) {
            free(encoded);
            goto done;
        }
        track.frames[frameNo].offset = outputOffset;
        track.frames[frameNo].size = (UINT32)encodedBytes;
        track.frames[frameNo].sampleCount = frames;
        if (!WriteBytes(out, encoded, (DWORD)encodedBytes)) {
            free(encoded);
            goto done;
        }
        outputOffset += encodedBytes;
        free(encoded);
        if (progress) progress(progressContext, (UINT64)frameNo + 1u, frameCount);
    }

    StringCchCopyW(track.path, ARRAYSIZE(track.path), flacPath);
    *outTrack = track;
    ZeroMemory(&track, sizeof(track));
    ok = TRUE;
    Log_Printf(L"flac: encoded %s: %lu Hz, %u ch, %u-bit, %lu frames",
               flacPath, (unsigned long)outTrack->sampleRate, outTrack->channels,
               outTrack->bitsPerSample, outTrack->frameCount);

done:
    if (rawBuf) HeapFree(GetProcessHeap(), 0, rawBuf);
    if (samples) HeapFree(GetProcessHeap(), 0, samples);
    if (raw != INVALID_HANDLE_VALUE) CloseHandle(raw);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    Flac_FreeTrack(&track);
    if (!ok)
        DeleteFileW(flacPath);
    return ok;
}
