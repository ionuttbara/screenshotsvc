/* Offline, timestamp-aligned render of the combined audio programme. */
#include "screenshotservice.h"
#include "rawmix.h"
#include "wavsink.h"
#include "log.h"
#include <mmreg.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define RAWMIX_CACHE_FRAMES 8192u
#define RAWMIX_CHUNK_FRAMES 2048u
#define RAWMIX_RATE 48000u
#define RAWMIX_LIMITER_THRESHOLD 0.88f
#define RAWMIX_LIMITER_RELEASE 0.00025f /* about 80 ms at 48 kHz */
#define RAWMIX_INVALID_FRAME ((UINT64)~(UINT64)0)

static const GUID RAWMIX_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID RAWMIX_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

typedef union { WAVEFORMATEXTENSIBLE ext; BYTE bytes[256]; } FormatStorage;
typedef struct {
    HANDLE file;
    FormatStorage storage;
    WAVEFORMATEX *fmt;
    UINT32 rate, channels, bits, validBits, blockAlign, channelMask;
    BOOL isFloat;
    UINT64 frames, cacheStart;
    UINT32 cacheFrames;
    BYTE *cache;
} RawReader;

static BOOL ReadAt(HANDLE f, UINT64 offset, BYTE *p, DWORD bytes, DWORD *got)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(f, pos, NULL, FILE_BEGIN) && ReadFile(f, p, bytes, got, NULL);
}

static BOOL IsSubtype(const WAVEFORMATEX *f, const GUID *subtype)
{
    if (f->wFormatTag == WAVE_FORMAT_PCM)
        return IsEqualGUID(subtype, &RAWMIX_PCM);
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return IsEqualGUID(subtype, &RAWMIX_FLOAT);
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return IsEqualGUID(&((const WAVEFORMATEXTENSIBLE *)f)->SubFormat, subtype);
    return FALSE;
}

static void ReaderClose(RawReader *r)
{
    if (r->file != INVALID_HANDLE_VALUE) CloseHandle(r->file);
    if (r->cache) HeapFree(GetProcessHeap(), 0, r->cache);
    ZeroMemory(r, sizeof(*r));
    r->file = INVALID_HANDLE_VALUE;
}

static BOOL ReaderOpen(RawReader *r, const wchar_t *path,
                       const BYTE *format, UINT32 formatBytes)
{
    LARGE_INTEGER size;
    UINT32 cb, valid;
    ZeroMemory(r, sizeof(*r));
    r->file = INVALID_HANDLE_VALUE;
    if (!path || !format || formatBytes < sizeof(WAVEFORMATEX) ||
        formatBytes > sizeof(r->storage.bytes)) return FALSE;
    CopyMemory(r->storage.bytes, format, formatBytes);
    r->fmt = &r->storage.ext.Format;
    if ((UINT32)sizeof(WAVEFORMATEX) + r->fmt->cbSize > formatBytes) return FALSE;
    r->isFloat = IsSubtype(r->fmt, &RAWMIX_FLOAT);
    if (!r->isFloat && !IsSubtype(r->fmt, &RAWMIX_PCM)) return FALSE;
    r->rate = r->fmt->nSamplesPerSec;
    r->channels = r->fmt->nChannels;
    r->bits = r->fmt->wBitsPerSample;
    r->blockAlign = r->fmt->nBlockAlign;
    valid = r->bits;
    if (r->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        r->fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
        formatBytes >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)r->fmt;
        if (e->Samples.wValidBitsPerSample) valid = e->Samples.wValidBitsPerSample;
        r->channelMask = e->dwChannelMask;
    }
    r->validBits = valid;
    if (!r->rate || r->rate > 384000u || !r->channels || r->channels > 8u ||
        !r->blockAlign || r->validBits < 4u || r->validBits > r->bits ||
        (r->isFloat && r->bits != 32u && r->bits != 64u) ||
        (!r->isFloat && r->bits != 8u && r->bits != 16u && r->bits != 24u && r->bits != 32u))
        return FALSE;
    cb = (r->bits + 7u) / 8u;
    if (r->blockAlign < cb * r->channels) return FALSE;
    r->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (r->file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx(r->file, &size) || size.QuadPart < 0) { ReaderClose(r); return FALSE; }
    r->frames = (UINT64)size.QuadPart / r->blockAlign;
    r->cache = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)RAWMIX_CACHE_FRAMES * r->blockAlign);
    if (!r->cache) { ReaderClose(r); return FALSE; }
    r->cacheStart = RAWMIX_INVALID_FRAME;
    return TRUE;
}

static BOOL ReaderEnsure(RawReader *r, UINT64 frame)
{
    UINT64 available;
    DWORD want, got = 0;
    if (frame >= r->frames) return TRUE;
    if (r->cacheStart != RAWMIX_INVALID_FRAME && frame >= r->cacheStart &&
        frame - r->cacheStart < r->cacheFrames) return TRUE;
    available = r->frames - frame;
    want = (DWORD)((available > RAWMIX_CACHE_FRAMES ? RAWMIX_CACHE_FRAMES : available) * r->blockAlign);
    if (!ReadAt(r->file, frame * (UINT64)r->blockAlign, r->cache, want, &got)) return FALSE;
    r->cacheStart = frame;
    r->cacheFrames = got / r->blockAlign;
    return r->cacheFrames != 0;
}

static double ReadScalar(const RawReader *r, const BYTE *p)
{
    if (r->isFloat) {
        double v;
        if (r->bits == 32u) { float f; CopyMemory(&f, p, sizeof(f)); v = f; }
        else CopyMemory(&v, p, sizeof(v));
        if (!isfinite(v)) return 0.0;
        if (v < -1.0) v = -1.0;
        if (v > 1.0) v = 1.0;
        return v;
    }
    if (r->bits == 8u)
        return ((double)((INT32)p[0] - 128)) / 128.0;
    if (r->bits == 16u) {
        INT16 v;
        CopyMemory(&v, p, sizeof(v));
        if (r->validBits < 16u) v = (INT16)(v >> (16u - r->validBits));
        return (double)v / (double)(1LL << (r->validBits - 1u));
    }
    if (r->bits == 24u) {
        UINT32 u = (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16);
        INT32 v = (u & 0x00800000u) ? (INT32)(u | 0xff000000u) : (INT32)u;
        if (r->validBits < 24u) v >>= (24u - r->validBits);
        return (double)v / (double)(1LL << (r->validBits - 1u));
    }
    {
        INT32 v;
        CopyMemory(&v, p, sizeof(v));
        if (r->validBits < 32u) v >>= (32u - r->validBits);
        return (double)v / (double)(1LL << (r->validBits - 1u));
    }
}

static double ChannelAt(const RawReader *r, const BYTE *frame, UINT32 ch)
{
    UINT32 bytes = (r->bits + 7u) / 8u;
    return ReadScalar(r, frame + (SIZE_T)ch * bytes);
}

static void AddWeighted(double value, double weight, double *sum, double *sumWeight)
{
    *sum += value * weight;
    *sumWeight += weight;
}

static void DownmixFrame(const RawReader *r, const BYTE *frame, float *left, float *right)
{
    double l = 0.0, rr = 0.0, wl = 0.0, wr = 0.0;
    UINT32 c;
    if (r->channels == 1u) {
        *left = *right = (float)ChannelAt(r, frame, 0);
        return;
    }
    if (r->channels == 2u) {
        *left = (float)ChannelAt(r, frame, 0);
        *right = (float)ChannelAt(r, frame, 1);
        return;
    }
    if (r->channelMask) {
        UINT32 bit, mask = r->channelMask;
        c = 0;
        for (bit = 1u; bit; bit <<= 1u) {
            double v;
            if (!(mask & bit)) continue;
            if (c >= r->channels) break;
            v = ChannelAt(r, frame, c++);
            switch (bit) {
            case 0x00000001u: AddWeighted(v, 1.0, &l, &wl); break; /* front left */
            case 0x00000002u: AddWeighted(v, 1.0, &rr, &wr); break; /* front right */
            case 0x00000004u: /* centre */
                AddWeighted(v, 0.70710678, &l, &wl); AddWeighted(v, 0.70710678, &rr, &wr); break;
            case 0x00000008u: /* LFE */
                AddWeighted(v, 0.31622777, &l, &wl); AddWeighted(v, 0.31622777, &rr, &wr); break;
            case 0x00000010u: case 0x00000200u: case 0x00001000u: case 0x00004000u:
                AddWeighted(v, 0.70710678, &l, &wl); break;
            case 0x00000020u: case 0x00000400u: case 0x00002000u: case 0x00008000u:
                AddWeighted(v, 0.70710678, &rr, &wr); break;
            case 0x00000100u: case 0x00000800u: /* back/top centre */
                AddWeighted(v, 0.5, &l, &wl); AddWeighted(v, 0.5, &rr, &wr); break;
            case 0x00000040u: AddWeighted(v, 0.70710678, &l, &wl); break; /* front left of centre */
            case 0x00000080u: AddWeighted(v, 0.70710678, &rr, &wr); break; /* front right of centre */
            default:
                if (c & 1u) AddWeighted(v, 0.70710678, &rr, &wr);
                else AddWeighted(v, 0.70710678, &l, &wl);
                break;
            }
        }
    } else {
        /* Fallback channel order: L, R, centre, LFE, surround L/R, ... */
        for (c = 0; c < r->channels; c++) {
            double v = ChannelAt(r, frame, c);
            switch (c) {
            case 0: AddWeighted(v, 1.0, &l, &wl); break;
            case 1: AddWeighted(v, 1.0, &rr, &wr); break;
            case 2: AddWeighted(v, 0.70710678, &l, &wl); AddWeighted(v, 0.70710678, &rr, &wr); break;
            case 3: AddWeighted(v, 0.31622777, &l, &wl); AddWeighted(v, 0.31622777, &rr, &wr); break;
            default:
                if (c & 1u) AddWeighted(v, 0.70710678, &rr, &wr);
                else AddWeighted(v, 0.70710678, &l, &wl);
                break;
            }
        }
    }
    /* Keep multichannel downmixing within full-scale without a time-varying limiter. */
    if (wl > 1.0) l /= wl;
    if (wr > 1.0) rr /= wr;
    if (l < -1.0) l = -1.0; else if (l > 1.0) l = 1.0;
    if (rr < -1.0) rr = -1.0; else if (rr > 1.0) rr = 1.0;
    *left = (float)l;
    *right = (float)rr;
}

static BOOL ReaderStereoAt(RawReader *r, UINT64 frame, float *left, float *right)
{
    const BYTE *p;
    if (frame >= r->frames) { *left = *right = 0.0f; return TRUE; }
    if (!ReaderEnsure(r, frame)) return FALSE;
    if (frame < r->cacheStart || frame - r->cacheStart >= r->cacheFrames) {
        *left = *right = 0.0f;
        return TRUE;
    }
    p = r->cache + (SIZE_T)(frame - r->cacheStart) * r->blockAlign;
    DownmixFrame(r, p, left, right);
    return TRUE;
}

static BOOL ResampledStereoAt(RawReader *r, UINT64 startSample,
                              UINT64 outputFrame, UINT32 outputRate,
                              float *left, float *right)
{
    double p;
    UINT64 lo, hi;
    float frac, l0, r0, l1, r1;

    /* The overwhelmingly common 48 kHz endpoint needs no resampling. Avoid
       floating-point phase calculation and, more importantly, the redundant
       second source-frame read on every output sample. */
    if (r->rate == outputRate) {
        if (UINT64_MAX - startSample < outputFrame)
            return FALSE;
        return ReaderStereoAt(r, startSample + outputFrame, left, right);
    }

    p = (double)startSample + (double)outputFrame * (double)r->rate / (double)outputRate;
    lo = (UINT64)p;
    hi = lo == RAWMIX_INVALID_FRAME ? lo : lo + 1u;
    frac = (float)(p - (double)lo);
    if (!ReaderStereoAt(r, lo, &l0, &r0)) return FALSE;
    /* Exact phase hits do not need the following sample. This also avoids
       cache misses for common integer-ratio conversions. */
    if (frac <= 0.0000001f || hi == lo) {
        *left = l0;
        *right = r0;
        return TRUE;
    }
    if (!ReaderStereoAt(r, hi, &l1, &r1)) return FALSE;
    *left = l0 + (l1 - l0) * frac;
    *right = r0 + (r1 - r0) * frac;
    return TRUE;
}

static INT16 ToPcm16(float v)
{
    double scaled;
    INT32 sample;
    if (!isfinite(v)) v = 0.0f;
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
    scaled = (double)v * (v < 0.0f ? 32768.0 : 32767.0);
    sample = (INT32)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    if (sample < -32768) sample = -32768;
    if (sample > 32767) sample = 32767;
    return (INT16)sample;
}

BOOL RawMix_BuildStereoWav(const wchar_t *microphonePath,
                           const BYTE *microphoneFormat, UINT32 microphoneFormatBytes,
                           UINT64 microphoneStartSample,
                           const wchar_t *systemPath,
                           const BYTE *systemFormat, UINT32 systemFormatBytes,
                           UINT64 systemStartSample,
                           UINT32 outputRate, UINT64 outputFrames,
                           const wchar_t *wavPath,
                           RawMixProgressFn progress, void *progressContext)
{
    RawReader mic, sys;
    WavSink *wav = NULL;
    INT16 pcm[RAWMIX_CHUNK_FRAMES * 2u];
    UINT64 frame = 0;
    float limiterGain = 1.0f;
    ULONGLONG started = GetTickCount64();
    BOOL ok = FALSE;
    ZeroMemory(&mic, sizeof(mic));
    ZeroMemory(&sys, sizeof(sys));
    mic.file = sys.file = INVALID_HANDLE_VALUE;

    if (!outputRate || !outputFrames || !wavPath ||
        !ReaderOpen(&mic, microphonePath, microphoneFormat, microphoneFormatBytes))
        goto done;
    if (!ReaderOpen(&sys, systemPath, systemFormat, systemFormatBytes))
        goto done;
    wav = Wav_Create(wavPath, outputRate, 2u);
    if (!wav) goto done;

    while (frame < outputFrames) {
        UINT32 count = (UINT32)((outputFrames - frame) > RAWMIX_CHUNK_FRAMES
            ? RAWMIX_CHUNK_FRAMES : (outputFrames - frame));
        UINT32 i;
        for (i = 0; i < count; i++) {
            float ml, mr, sl, sr;
            if (!ResampledStereoAt(&mic, microphoneStartSample, frame + i,
                                   outputRate, &ml, &mr) ||
                !ResampledStereoAt(&sys, systemStartSample, frame + i,
                                   outputRate, &sl, &sr))
                goto done;
            /* Offline, timestamp-aligned mix. A linked-channel peak limiter is
               applied after summing so simultaneous system/microphone peaks do
               not hit the PCM full-scale ceiling. Fast attack protects peaks;
               slow release avoids the pumping of a block-by-block live limiter. */
            {
                /* Give the microphone a little extra headroom in the combined track.
                   This affects AAC mix only: individual FLAC remains an untouched
                   capture of the endpoint. The linked limiter handles coincident peaks. */
                float left = 0.40f * ml + 0.50f * sl;
                float right = 0.40f * mr + 0.50f * sr;
                float absL = left < 0.0f ? -left : left;
                float absR = right < 0.0f ? -right : right;
                float peak = absL > absR ? absL : absR;
                float targetGain = peak > RAWMIX_LIMITER_THRESHOLD
                    ? RAWMIX_LIMITER_THRESHOLD / peak : 1.0f;
                if (targetGain < limiterGain)
                    limiterGain = targetGain;
                else
                    limiterGain += (1.0f - limiterGain) * RAWMIX_LIMITER_RELEASE;
                pcm[i * 2u] = ToPcm16(left * limiterGain);
                pcm[i * 2u + 1u] = ToPcm16(right * limiterGain);
            }
        }
        if (!Wav_Write(wav, (const BYTE *)pcm, count)) goto done;
        frame += count;
        if (progress) progress(progressContext, frame, outputFrames);
    }
    ok = TRUE;
    Log_Printf(L"rawmix: built %llu frames at %lu Hz from timestamp-aligned system and microphone RAW streams in %llu ms",
               (unsigned long long)outputFrames, (unsigned long)outputRate,
               (unsigned long long)(GetTickCount64() - started));

done:
    if (wav && !Wav_Close(wav)) ok = FALSE;
    ReaderClose(&mic);
    ReaderClose(&sys);
    if (!ok && wavPath) DeleteFileW(wavPath);
    return ok;
}
