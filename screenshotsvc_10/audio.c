/*
 * audio.c - WASAPI shared-mode capture, loopback, microphone, and dual-source capture.
 */

#define COBJMACROS

#include "screenshotservice.h"
#include "audio.h"
#include "settings.h"
#include "log.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <propsys.h>
#include <process.h>

/*
 * The WASAPI headers only declare these; the definitions live in a generated
 * object that no import library on this SDK carries, and the class id is a C++
 * class declaration that C cannot see at all. Every value is taken from the
 * MIDL_INTERFACE attributes in mmdeviceapi.h / Audioclient.h and ksmedia.h.
 */
static const CLSID CLSID_MMDeviceEnumerator_ =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID IID_IMMDeviceEnumerator_ =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID IID_IAudioClient_ =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID IID_IAudioCaptureClient_ =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const IID IID_IAudioClient2_ =
    { 0x726778CD, 0xF60A, 0x4EDA, { 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA } };
static const GUID SUBTYPE_IEEE_FLOAT_ =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID SUBTYPE_PCM_ =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const PROPERTYKEY KEY_AUDIOENGINE_DEVICEFORMAT_ = {
    { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0
};

#define BUFFER_DURATION_100NS  2000000   /* 200 ms of endpoint buffer */

static IAudioClient        *g_client;
static IAudioCaptureClient *g_capture;
static WAVEFORMATEX        *g_mix;
static HANDLE               g_thread;
static HANDLE               g_stopEvent;
static AudioSinkFn          g_sink;
static void                *g_ctx;
static UINT32               g_outChannels;
static AudioSource          g_singleSource = AUDIO_NONE;
static volatile LONG        g_microphoneMuted;

void Audio_SetMicrophoneMuted(BOOL muted)
{
    InterlockedExchange(&g_microphoneMuted, muted ? 1 : 0);
}

BOOL Audio_IsMicrophoneMuted(void)
{
    return InterlockedCompareExchange(&g_microphoneMuted, 0, 0) != 0;
}

/* Combined system + microphone capture. In live-mix mode the independent
   endpoint clocks are reconciled by rings. Multitrack recording instead saves
   both native streams and lets the recorder render the mix after stopping. */
#define BOTH_OUTPUT_RATE       48000u
#define BOTH_OUTPUT_CHANNELS   2u
#define BOTH_BLOCK_FRAMES      480u       /* 10 ms at 48 kHz */
#define BOTH_RING_SECONDS      2u
#define BOTH_LIMITER_THRESHOLD  0.90f
#define BOTH_LIMITER_RELEASE    0.00042f /* roughly 50 ms at 48 kHz */

typedef struct BothCapture BothCapture;
typedef struct {
    BothCapture             *owner;
    AudioSource              source;
    IAudioClient            *client;
    IAudioCaptureClient     *capture;
    WAVEFORMATEX            *format;
    HANDLE                   thread;
    UINT32                   nativeRate;
    UINT32                   preferredPcmBits;
    UINT32                   resampleRemainder;
    INT16                   *ring;
    ULONGLONG                readFrame;
    ULONGLONG                writeFrame;
    UINT32                   ringCapacity;
} BothEndpoint;

struct BothCapture {
    HANDLE                   stopEvent;
    HANDLE                   dataEvent;
    HANDLE                   mixerThread;
    CRITICAL_SECTION         lock;
    BOOL                     lockInit;
    BOOL                     active;
    BOOL                     deferCombinedMix; /* RAW-only capture, mix after stop */
    float                    limiterGain;
    AudioSinkFn              sink;
    void                    *ctx;
    AudioNativeFormatFn      nativeFormat;
    AudioNativeSinkFn        nativeSink;
    void                    *nativeCtx;
    UINT64                   qpcOrigin100ns;
    BothEndpoint             system;
    BothEndpoint             microphone;
};

static BothCapture g_both;

static BOOL Audio_StopBoth(void);

/* ------------------------------------------------------------ conversion */

/*
 * The mixer hands over whatever the endpoint runs at - practically always
 * 32-bit float - and the AAC encoder wants 16-bit PCM. Channels beyond the
 * first two are dropped rather than properly downmixed: a correct 5.1 fold-down
 * needs the channel mask and matrix coefficients, and for a screen recording
 * the front pair is what matters.
 */
static BOOL IsFloat(const WAVEFORMATEX *f)
{
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return TRUE;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)f;
        return IsEqualGUID(&e->SubFormat, &SUBTYPE_IEEE_FLOAT_);
    }
    return FALSE;
}

static UINT32 FormatValidBits(const WAVEFORMATEX *f, UINT32 formatBytes)
{
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
        formatBytes >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)f;
        if (e->Samples.wValidBitsPerSample)
            return e->Samples.wValidBitsPerSample;
    }
    return f->wBitsPerSample;
}

/* GetMixFormat is often FLOAT32 even when the configured endpoint format is
   16/24-bit PCM. The latter is the correct target precision for a lossless
   integer FLAC track; it does not change the actual packet format we capture. */
static UINT32 QueryPreferredPcmBits(IMMDevice *device, const WAVEFORMATEX *mixFormat)
{
    IPropertyStore *store = NULL;
    PROPVARIANT value;
    /* If the device property is unavailable, never invent a larger depth for
       a PCM mix format that already has a concrete integer precision. */
    UINT32 bits = mixFormat && !IsFloat(mixFormat)
        ? FormatValidBits(mixFormat, (UINT32)sizeof(WAVEFORMATEX) + mixFormat->cbSize)
        : 24u; /* sensible fallback for the common 32-bit-float shared mix */
    ZeroMemory(&value, sizeof(value));
    if (device && SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store))) {
        if (SUCCEEDED(IPropertyStore_GetValue(store, &KEY_AUDIOENGINE_DEVICEFORMAT_, &value)) &&
            value.vt == VT_BLOB && value.blob.pBlobData &&
            value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
            const WAVEFORMATEX *f = (const WAVEFORMATEX *)value.blob.pBlobData;
            UINT32 bytes = value.blob.cbSize;
            BOOL isPcm = f->wFormatTag == WAVE_FORMAT_PCM;
            if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
                bytes >= sizeof(WAVEFORMATEXTENSIBLE)) {
                const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)f;
                isPcm = IsEqualGUID(&e->SubFormat, &SUBTYPE_PCM_);
            }
            if (isPcm) {
                UINT32 valid = FormatValidBits(f, bytes);
                if (valid >= 4u && valid <= 32u)
                    bits = valid;
            }
        }
        PropVariantClear(&value);
        IPropertyStore_Release(store);
    }
    /* A 32-bit IEEE-float shared-mode stream has 24 bits of significand
       precision. If Windows exposes a 32-bit integer endpoint default format
       while the captured stream is float, 24-bit FLAC preserves the useful
       signal precision without labelling float storage width as hardware depth. */
    if (mixFormat && IsFloat(mixFormat) && bits > 24u)
        bits = 24u;
    return bits;
}

static INT16 ClampToPcm(float v)
{
    float s = v * 32767.0f;
    if (s >  32767.0f) s =  32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    return (INT16)(s < 0.0f ? s - 0.5f : s + 0.5f);
}

static void Convert(const BYTE *src, UINT32 frames, INT16 *dst)
{
    UINT32 i, c;
    UINT32 inCh  = g_mix->nChannels;
    UINT32 outCh = g_outChannels;

    if (IsFloat(g_mix)) {
        const float *f = (const float *)src;
        for (i = 0; i < frames; i++) {
            for (c = 0; c < outCh; c++)
                dst[i * outCh + c] = ClampToPcm(f[i * inCh + c]);
        }
    } else if (g_mix->wBitsPerSample == 16) {
        const INT16 *s = (const INT16 *)src;
        for (i = 0; i < frames; i++) {
            for (c = 0; c < outCh; c++)
                dst[i * outCh + c] = s[i * inCh + c];
        }
    } else {
        ZeroMemory(dst, (SIZE_T)frames * outCh * sizeof(INT16));
    }
}

static UINT64 Audio_QpcNow100ns(void)
{
    LARGE_INTEGER qpc, freq;
    UINT64 whole, part;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 ||
        !QueryPerformanceCounter(&qpc) || qpc.QuadPart < 0)
        return 0;
    whole = (UINT64)qpc.QuadPart / (UINT64)freq.QuadPart;
    part = (UINT64)qpc.QuadPart % (UINT64)freq.QuadPart;
    return whole * 10000000u + part * 10000000u / (UINT64)freq.QuadPart;
}

/* ---------------------------------------------------------- dual-source mix */

static INT16 ReadBothSample(const WAVEFORMATEX *f, const BYTE *data,
                            UINT32 frame, UINT32 channel)
{
    UINT32 channels = f->nChannels;
    UINT32 bits = f->wBitsPerSample;
    SIZE_T index = ((SIZE_T)frame * channels + channel) * (bits / 8u);
    const BYTE *p = data + index;

    if (IsFloat(f)) {
        if (bits == 64) {
            double v;
            CopyMemory(&v, p, sizeof(v));
            return ClampToPcm((float)v);
        } else {
            float v;
            CopyMemory(&v, p, sizeof(v));
            return ClampToPcm(v);
        }
    }
    if (bits == 8)
        return (INT16)(((INT32)p[0] - 128) << 8);
    if (bits == 16)
        return *(const INT16 *)p;
    if (bits == 24) {
        INT32 v = (INT32)((UINT32)p[0] | ((UINT32)p[1] << 8) |
                          ((UINT32)p[2] << 16));
        if (v & 0x00800000)
            v |= (INT32)0xFF000000;
        return (INT16)(v >> 8);
    }
    if (bits == 32) {
        INT32 v = *(const INT32 *)p;
        return (INT16)(v >> 16);
    }
    return 0;
}

/* Convert each endpoint to stereo PCM16. A mono microphone is duplicated to
   both channels so it mixes naturally with a stereo loopback stream. */
static void ConvertBothToStereo(const WAVEFORMATEX *f, const BYTE *src,
                                UINT32 frames, INT16 *dst)
{
    UINT32 i;
    for (i = 0; i < frames; i++) {
        dst[i * 2] = ReadBothSample(f, src, i, 0);
        dst[i * 2 + 1] = ReadBothSample(f, src, i, f->nChannels > 1 ? 1 : 0);
    }
}

static void AppendBothFrames(BothEndpoint *ep, const INT16 *src, UINT32 frames)
{
    BothCapture *owner = ep->owner;
    UINT32 i;
    ULONGLONG queued;

    if (!frames || !ep->ring || !owner->lockInit)
        return;

    EnterCriticalSection(&owner->lock);
    if (frames > ep->ringCapacity) {
        UINT32 skip = frames - ep->ringCapacity;
        src += (SIZE_T)skip * BOTH_OUTPUT_CHANNELS;
        frames = ep->ringCapacity;
    }

    queued = ep->writeFrame - ep->readFrame;
    if (queued + frames > ep->ringCapacity)
        ep->readFrame += queued + frames - ep->ringCapacity;

    for (i = 0; i < frames; i++) {
        UINT32 at = (UINT32)((ep->writeFrame + i) % ep->ringCapacity);
        ep->ring[at * BOTH_OUTPUT_CHANNELS] = src[i * BOTH_OUTPUT_CHANNELS];
        ep->ring[at * BOTH_OUTPUT_CHANNELS + 1] = src[i * BOTH_OUTPUT_CHANNELS + 1];
    }
    ep->writeFrame += frames;
    LeaveCriticalSection(&owner->lock);

    if (owner->dataEvent)
        SetEvent(owner->dataEvent);
}

static unsigned __stdcall BothCaptureThread(void *arg)
{
    BothEndpoint *ep = (BothEndpoint *)arg;
    INT16 *native = NULL, *resampled = NULL;
    UINT32 nativeCapacity = 0, resampledCapacity = 0;
    HRESULT comHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    while (WaitForSingleObject(ep->owner->stopEvent, 10) == WAIT_TIMEOUT) {
        UINT32 packet = 0;
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(ep->capture, &packet)) &&
               packet > 0) {
            BYTE *data = NULL;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0, qpcPosition = 0;
            UINT32 outputFrames, needNative, needOutput, i;
            INT16 *p;
            UINT64 total;

            if (FAILED(IAudioCaptureClient_GetBuffer(ep->capture, &data, &frames,
                                                     &flags, &devicePosition,
                                                     &qpcPosition)))
                break;
            if (!frames) {
                IAudioCaptureClient_ReleaseBuffer(ep->capture, frames);
                continue;
            }

            /* Preserve the endpoint's original packet before any resampling,
               channel conversion or mixing. WASAPI reports QPC in 100-ns units;
               packets whose timestamp is marked unreliable fall back to append. */
            if (ep->owner->nativeSink) {
                UINT64 packetStamp = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
                    ? 0 : qpcPosition;
                BOOL silence = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ||
                    (ep->source == AUDIO_MICROPHONE && Audio_IsMicrophoneMuted());
                ep->owner->nativeSink(ep->owner->nativeCtx, ep->source, data,
                    frames, silence, packetStamp);
            }

            /* Multitrack mode records endpoint packets only. No live resampling,
               ring-buffer contention or 10-ms software mix runs on capture
               threads; the aligned combined track is rendered after stop. */
            if (ep->owner->deferCombinedMix) {
                IAudioCaptureClient_ReleaseBuffer(ep->capture, frames);
                continue;
            }

            needNative = frames;
            if (needNative > nativeCapacity) {
                p = (INT16 *)(native
                    ? HeapReAlloc(GetProcessHeap(), 0, native,
                                  (SIZE_T)needNative * BOTH_OUTPUT_CHANNELS * sizeof(INT16))
                    : HeapAlloc(GetProcessHeap(), 0,
                                (SIZE_T)needNative * BOTH_OUTPUT_CHANNELS * sizeof(INT16)));
                if (!p) {
                    IAudioCaptureClient_ReleaseBuffer(ep->capture, frames);
                    break;
                }
                native = p;
                nativeCapacity = needNative;
            }

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) ||
                (ep->source == AUDIO_MICROPHONE && Audio_IsMicrophoneMuted()))
                ZeroMemory(native, (SIZE_T)frames * BOTH_OUTPUT_CHANNELS * sizeof(INT16));
            else
                ConvertBothToStereo(ep->format, data, frames, native);

            total = (UINT64)frames * BOTH_OUTPUT_RATE + ep->resampleRemainder;
            outputFrames = (UINT32)(total / ep->nativeRate);
            ep->resampleRemainder = (UINT32)(total % ep->nativeRate);
            needOutput = (UINT32)(((UINT64)frames * BOTH_OUTPUT_RATE) /
                                  ep->nativeRate) + 4;
            if (needOutput < outputFrames)
                needOutput = outputFrames;
            if (needOutput > resampledCapacity) {
                p = (INT16 *)(resampled
                    ? HeapReAlloc(GetProcessHeap(), 0, resampled,
                                  (SIZE_T)needOutput * BOTH_OUTPUT_CHANNELS * sizeof(INT16))
                    : HeapAlloc(GetProcessHeap(), 0,
                                (SIZE_T)needOutput * BOTH_OUTPUT_CHANNELS * sizeof(INT16)));
                if (!p) {
                    IAudioCaptureClient_ReleaseBuffer(ep->capture, frames);
                    break;
                }
                resampled = p;
                resampledCapacity = needOutput;
            }

            if (ep->nativeRate == BOTH_OUTPUT_RATE) {
                CopyMemory(resampled, native,
                           (SIZE_T)frames * BOTH_OUTPUT_CHANNELS * sizeof(INT16));
                outputFrames = frames;
            } else {
                for (i = 0; i < outputFrames; i++) {
                    UINT64 pos = (UINT64)i * ep->nativeRate;
                    UINT32 index = (UINT32)(pos / BOTH_OUTPUT_RATE);
                    UINT32 frac = (UINT32)(pos % BOTH_OUTPUT_RATE);
                    UINT32 next;
                    UINT32 ch;
                    if (index >= frames - 1) {
                        index = frames - 1;
                        next = index;
                        frac = 0;
                    } else {
                        next = index + 1;
                    }
                    for (ch = 0; ch < BOTH_OUTPUT_CHANNELS; ch++) {
                        INT32 a = native[index * BOTH_OUTPUT_CHANNELS + ch];
                        INT32 b = native[next * BOTH_OUTPUT_CHANNELS + ch];
                        resampled[i * BOTH_OUTPUT_CHANNELS + ch] =
                            (INT16)(a + (INT32)(((INT64)(b - a) * frac) /
                                                BOTH_OUTPUT_RATE));
                    }
                }
            }

            AppendBothFrames(ep, resampled, outputFrames);
            IAudioCaptureClient_ReleaseBuffer(ep->capture, frames);
        }
    }

    if (native)    HeapFree(GetProcessHeap(), 0, native);
    if (resampled) HeapFree(GetProcessHeap(), 0, resampled);
    if (SUCCEEDED(comHr))
        CoUninitialize();
    return 0;
}

static BOOL OpenBothEndpoint(BothEndpoint *ep, AudioSource src)
{
    IMMDeviceEnumerator *devEnum = NULL;
    IMMDevice *device = NULL;
    DWORD streamFlags = 0;
    BOOL ok = FALSE;
    HRESULT hr;

    ep->owner = &g_both;
    ep->source = src;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator_, (void **)&devEnum)))
        goto done;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devEnum,
            src == AUDIO_SYSTEM ? eRender : eCapture, eConsole, &device)))
        goto done;
    if (FAILED(IMMDevice_Activate(device, &IID_IAudioClient_, CLSCTX_ALL, NULL,
                                  (void **)&ep->client)))
        goto done;

    if (g_cfg.recordRawAudio) {
        IAudioClient2 *client2 = NULL;
        if (SUCCEEDED(IAudioClient_QueryInterface(ep->client, &IID_IAudioClient2_,
                                                  (void **)&client2))) {
            AudioClientProperties props;
            ZeroMemory(&props, sizeof(props));
            props.cbSize = sizeof(props);
            props.bIsOffload = FALSE;
            props.eCategory = AudioCategory_Other;
            props.Options = AUDCLNT_STREAMOPTIONS_RAW;
            hr = IAudioClient2_SetClientProperties(client2, &props);
            if (FAILED(hr))
                Log_Printf(L"audio: raw mode refused for %s (0x%08lX)",
                           src == AUDIO_SYSTEM ? L"system sound" : L"microphone",
                           (unsigned long)hr);
            IAudioClient2_Release(client2);
        }
    }

    if (FAILED(IAudioClient_GetMixFormat(ep->client, &ep->format)))
        goto done;
    ep->nativeRate = ep->format->nSamplesPerSec;
    ep->preferredPcmBits = QueryPreferredPcmBits(device, ep->format);
    /* The combined mix is resampled to 48 kHz, but the RAW/FLAC side tracks
       retain this negotiated endpoint format. Accept high-rate endpoints too. */
    if (ep->nativeRate < 8000u || ep->nativeRate > 384000u) {
        Log_Printf(L"audio: %s endpoint runs at unsupported rate %lu Hz",
                   src == AUDIO_SYSTEM ? L"system" : L"microphone",
                   (unsigned long)ep->nativeRate);
        goto done;
    }
    if (!ep->format->nChannels || ep->format->nChannels > 8u ||
        !ep->format->wBitsPerSample || !ep->format->nBlockAlign ||
        (ep->format->wBitsPerSample != 8 && ep->format->wBitsPerSample != 16 &&
         ep->format->wBitsPerSample != 24 && ep->format->wBitsPerSample != 32 &&
         ep->format->wBitsPerSample != 64) ||
        (ep->format->wBitsPerSample == 64 && !IsFloat(ep->format)))
        goto done;

    if (src == AUDIO_SYSTEM)
        streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;
    if (FAILED(IAudioClient_Initialize(ep->client, AUDCLNT_SHAREMODE_SHARED,
                                       streamFlags, BUFFER_DURATION_100NS, 0,
                                       ep->format, NULL)))
        goto done;
    if (FAILED(IAudioClient_GetService(ep->client, &IID_IAudioCaptureClient_,
                                       (void **)&ep->capture)))
        goto done;

    if (!ep->owner->deferCombinedMix) {
        ep->ringCapacity = BOTH_OUTPUT_RATE * BOTH_RING_SECONDS;
        ep->ring = (INT16 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            (SIZE_T)ep->ringCapacity * BOTH_OUTPUT_CHANNELS * sizeof(INT16));
        if (!ep->ring)
            goto done;
    }

    ok = TRUE;
done:
    if (device)  IMMDevice_Release(device);
    if (devEnum) IMMDeviceEnumerator_Release(devEnum);
    if (!ok)
        Log_Printf(L"audio: could not prepare %s capture endpoint",
                   src == AUDIO_SYSTEM ? L"system" : L"microphone");
    return ok;
}

static void ReleaseBothEndpoint(BothEndpoint *ep)
{
    if (ep->capture) {
        IAudioCaptureClient_Release(ep->capture);
        ep->capture = NULL;
    }
    if (ep->client) {
        IAudioClient_Stop(ep->client);
        IAudioClient_Release(ep->client);
        ep->client = NULL;
    }
    if (ep->format) {
        CoTaskMemFree(ep->format);
        ep->format = NULL;
    }
    if (ep->ring) {
        HeapFree(GetProcessHeap(), 0, ep->ring);
        ep->ring = NULL;
    }
}

static unsigned __stdcall BothMixerThread(void *arg)
{
    BothCapture *mix = (BothCapture *)arg;
    INT16 systemBlock[BOTH_BLOCK_FRAMES * BOTH_OUTPUT_CHANNELS];
    INT16 microphoneBlock[BOTH_BLOCK_FRAMES * BOTH_OUTPUT_CHANNELS];
    INT16 output[BOTH_BLOCK_FRAMES * BOTH_OUTPUT_CHANNELS];
    HANDLE waits[2];

    waits[0] = mix->stopEvent;
    waits[1] = mix->dataEvent;
    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 20);
        if (wait == WAIT_OBJECT_0 || WaitForSingleObject(mix->stopEvent, 0) == WAIT_OBJECT_0)
            break;

        for (;;) {
            UINT32 i;
            BOOL haveSystem, haveMicrophone;
            ULONGLONG sysAvailable, micAvailable;
            EnterCriticalSection(&mix->lock);
            sysAvailable = mix->system.writeFrame - mix->system.readFrame;
            micAvailable = mix->microphone.writeFrame - mix->microphone.readFrame;
            /* WASAPI loopback may deliver no packets while the desktop is
               silent. Keep the microphone alive, but never consume a partial
               block: squeezing (say) 3 ms of one endpoint into the start of a
               10 ms block shifts that endpoint against the other one. */
            haveSystem = sysAvailable >= BOTH_BLOCK_FRAMES;
            haveMicrophone = micAvailable >= BOTH_BLOCK_FRAMES;
            if (!haveSystem && !haveMicrophone) {
                LeaveCriticalSection(&mix->lock);
                break;
            }
            for (i = 0; i < BOTH_BLOCK_FRAMES; i++) {
                if (haveSystem) {
                    UINT32 si = (UINT32)((mix->system.readFrame + i) % mix->system.ringCapacity);
                    systemBlock[i * 2] = mix->system.ring[si * 2];
                    systemBlock[i * 2 + 1] = mix->system.ring[si * 2 + 1];
                } else {
                    systemBlock[i * 2] = 0;
                    systemBlock[i * 2 + 1] = 0;
                }
                if (haveMicrophone) {
                    UINT32 mi = (UINT32)((mix->microphone.readFrame + i) % mix->microphone.ringCapacity);
                    microphoneBlock[i * 2] = mix->microphone.ring[mi * 2];
                    microphoneBlock[i * 2 + 1] = mix->microphone.ring[mi * 2 + 1];
                } else {
                    microphoneBlock[i * 2] = 0;
                    microphoneBlock[i * 2 + 1] = 0;
                }
            }
            if (haveSystem)
                mix->system.readFrame += BOTH_BLOCK_FRAMES;
            if (haveMicrophone)
                mix->microphone.readFrame += BOTH_BLOCK_FRAMES;
            LeaveCriticalSection(&mix->lock);

            /* Mix in a wider domain and use a linked-channel soft limiter.
               Hard clipping created harsh distortion when both inputs peaked
               together. The fast attack catches the peak; the slow release
               avoids gain modulation on adjacent samples. */
            for (i = 0; i < BOTH_BLOCK_FRAMES; i++) {
                /* Fixed headroom for voice peaks in the combined AAC mix.
                   Separate RAW/FLAC microphone data remains unmodified. */
                float left = (float)systemBlock[i * 2] * (0.50f / 32768.0f) +
                             (float)microphoneBlock[i * 2] * (0.40f / 32768.0f);
                float right = (float)systemBlock[i * 2 + 1] * (0.50f / 32768.0f) +
                              (float)microphoneBlock[i * 2 + 1] * (0.40f / 32768.0f);
                float absL = left < 0.0f ? -left : left;
                float absR = right < 0.0f ? -right : right;
                float peak = absL > absR ? absL : absR;
                float targetGain = peak > BOTH_LIMITER_THRESHOLD
                    ? BOTH_LIMITER_THRESHOLD / peak : 1.0f;
                float scaled;

                if (targetGain < mix->limiterGain)
                    mix->limiterGain = targetGain;
                else
                    mix->limiterGain += (1.0f - mix->limiterGain) * BOTH_LIMITER_RELEASE;

                left *= mix->limiterGain;
                right *= mix->limiterGain;
                scaled = left * 32768.0f;
                if (scaled > 32767.0f) scaled = 32767.0f;
                if (scaled < -32768.0f) scaled = -32768.0f;
                output[i * 2] = (INT16)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
                scaled = right * 32768.0f;
                if (scaled > 32767.0f) scaled = 32767.0f;
                if (scaled < -32768.0f) scaled = -32768.0f;
                output[i * 2 + 1] = (INT16)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
            }
            if (mix->sink)
                mix->sink(mix->ctx, (const BYTE *)output, BOTH_BLOCK_FRAMES);
            if (WaitForSingleObject(mix->stopEvent, 0) == WAIT_OBJECT_0)
                break;
        }
    }
    return 0;
}

static BOOL Audio_StartBoth(AudioFormat *fmt, AudioSinkFn sink, void *ctx,
                            BOOL deferCombinedMix,
                            AudioNativeFormatFn nativeFormat,
                            AudioNativeSinkFn nativeSink, void *nativeCtx)
{
    uintptr_t thread;

    if (!fmt || !sink || g_both.active || g_thread)
        return FALSE;
    ZeroMemory(&g_both, sizeof(g_both));
    InitializeCriticalSection(&g_both.lock);
    g_both.lockInit = TRUE;
    g_both.active = TRUE;
    g_both.deferCombinedMix = deferCombinedMix;
    g_both.limiterGain = 1.0f;
    g_both.sink = sink;
    g_both.ctx = ctx;
    g_both.nativeFormat = nativeFormat;
    g_both.nativeSink = nativeSink;
    g_both.nativeCtx = nativeCtx;
    g_both.stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_both.dataEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_both.stopEvent || !g_both.dataEvent)
        goto fail;
    if (!OpenBothEndpoint(&g_both.system, AUDIO_SYSTEM) ||
        !OpenBothEndpoint(&g_both.microphone, AUDIO_MICROPHONE))
        goto fail;
    if (FAILED(IAudioClient_Start(g_both.system.client)))
        goto fail;
    if (FAILED(IAudioClient_Start(g_both.microphone.client)))
        goto fail;

    g_both.qpcOrigin100ns = Audio_QpcNow100ns();
    if (g_both.nativeFormat) {
        UINT32 sysFormatBytes = (UINT32)sizeof(WAVEFORMATEX) + g_both.system.format->cbSize;
        UINT32 micFormatBytes = (UINT32)sizeof(WAVEFORMATEX) + g_both.microphone.format->cbSize;
        if (sysFormatBytes <= 256u)
            g_both.nativeFormat(g_both.nativeCtx, AUDIO_SYSTEM,
                (const BYTE *)g_both.system.format, sysFormatBytes,
                g_both.system.preferredPcmBits, g_both.qpcOrigin100ns);
        else
            Log_Printf(L"audio: system mix format is too large for raw capture metadata");
        if (micFormatBytes <= 256u)
            g_both.nativeFormat(g_both.nativeCtx, AUDIO_MICROPHONE,
                (const BYTE *)g_both.microphone.format, micFormatBytes,
                g_both.microphone.preferredPcmBits, g_both.qpcOrigin100ns);
        else
            Log_Printf(L"audio: microphone mix format is too large for raw capture metadata");
    }

    thread = _beginthreadex(NULL, 0, BothCaptureThread, &g_both.system, 0, NULL);
    if (!thread)
        goto fail;
    g_both.system.thread = (HANDLE)thread;
    thread = _beginthreadex(NULL, 0, BothCaptureThread, &g_both.microphone, 0, NULL);
    if (!thread)
        goto fail;
    g_both.microphone.thread = (HANDLE)thread;
    if (!g_both.deferCombinedMix) {
        thread = _beginthreadex(NULL, 0, BothMixerThread, &g_both, 0, NULL);
        if (!thread)
            goto fail;
        g_both.mixerThread = (HANDLE)thread;
    }

    fmt->sampleRate = BOTH_OUTPUT_RATE;
    fmt->channels = BOTH_OUTPUT_CHANNELS;
    if (g_both.deferCombinedMix)
        Log_Printf(L"audio: RAW-only system/microphone capture; combined 48 kHz stereo mix deferred until stop");
    else
        Log_Printf(L"audio: system sound + microphone mixed at %u Hz stereo",
                   BOTH_OUTPUT_RATE);
    return TRUE;

fail:
    Audio_StopBoth();
    return FALSE;
}

static BOOL Audio_StopBoth(void)
{
    HANDLE threads[3];
    UINT32 i;

    if (!g_both.active)
        return TRUE;
    if (g_both.stopEvent)
        SetEvent(g_both.stopEvent);
    if (g_both.dataEvent)
        SetEvent(g_both.dataEvent);

    threads[0] = g_both.system.thread;
    threads[1] = g_both.microphone.thread;
    threads[2] = g_both.mixerThread;
    for (i = 0; i < ARRAYSIZE(threads); i++) {
        if (threads[i] && WaitForSingleObject(threads[i], 2000) != WAIT_OBJECT_0) {
            Log_Printf(L"audio: combined capture thread did not stop in time");
            return FALSE;
        }
    }
    for (i = 0; i < ARRAYSIZE(threads); i++) {
        if (threads[i])
            CloseHandle(threads[i]);
    }
    g_both.system.thread = NULL;
    g_both.microphone.thread = NULL;
    g_both.mixerThread = NULL;

    ReleaseBothEndpoint(&g_both.system);
    ReleaseBothEndpoint(&g_both.microphone);
    if (g_both.stopEvent) CloseHandle(g_both.stopEvent);
    if (g_both.dataEvent) CloseHandle(g_both.dataEvent);
    if (g_both.lockInit) DeleteCriticalSection(&g_both.lock);
    ZeroMemory(&g_both, sizeof(g_both));
    return TRUE;
}

/* ---------------------------------------------------------------- thread */

static unsigned __stdcall CaptureThread(void *arg)
{
    INT16 *scratch = NULL;
    UINT32 scratchFrames = 0;
    /*
     * Poll far more often than the endpoint buffer is long. Half the buffer
     * looks like it ought to be enough and is not: the thread only has to be
     * descheduled once for the buffer to overrun, and WASAPI's answer to an
     * overrun is to throw the samples away. It also decides how lumpy the
     * delivery is, and lumpy delivery upsets the recorder's timeline.
     */
    DWORD  waitMs = 10;

    UNREFERENCED_PARAMETER(arg);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    while (WaitForSingleObject(g_stopEvent, waitMs) == WAIT_TIMEOUT) {
        UINT32 packet = 0;

        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(g_capture, &packet)) &&
               packet > 0) {
            BYTE   *data  = NULL;
            UINT32  frames = 0;
            DWORD   flags = 0;

            if (FAILED(IAudioCaptureClient_GetBuffer(g_capture, &data, &frames,
                                                     &flags, NULL, NULL)))
                break;

            if (frames > scratchFrames) {
                INT16 *n = (INT16 *)(scratch
                    ? HeapReAlloc(GetProcessHeap(), 0, scratch,
                                  (SIZE_T)frames * g_outChannels * sizeof(INT16))
                    : HeapAlloc(GetProcessHeap(), 0,
                                (SIZE_T)frames * g_outChannels * sizeof(INT16)));
                if (!n) {
                    IAudioCaptureClient_ReleaseBuffer(g_capture, frames);
                    break;
                }
                scratch = n;
                scratchFrames = frames;
            }

            /* A loopback stream reports silence rather than delivering zeros,
               and the encoder still needs those samples to keep its timeline. */
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) ||
                (g_singleSource == AUDIO_MICROPHONE && Audio_IsMicrophoneMuted()))
                ZeroMemory(scratch, (SIZE_T)frames * g_outChannels * sizeof(INT16));
            else
                Convert(data, frames, scratch);

            if (g_sink)
                g_sink(g_ctx, (const BYTE *)scratch, frames);

            IAudioCaptureClient_ReleaseBuffer(g_capture, frames);
        }
    }

    if (scratch)
        HeapFree(GetProcessHeap(), 0, scratch);
    CoUninitialize();
    return 0;
}

/* ----------------------------------------------------------------- setup */

static void Cleanup(void)
{
    if (g_capture) { IAudioCaptureClient_Release(g_capture); g_capture = NULL; }
    if (g_client)  { IAudioClient_Stop(g_client);
                     IAudioClient_Release(g_client);         g_client  = NULL; }
    if (g_mix)     { CoTaskMemFree(g_mix);                   g_mix     = NULL; }
    if (g_stopEvent) { CloseHandle(g_stopEvent);             g_stopEvent = NULL; }
    if (g_thread)  { CloseHandle(g_thread);                  g_thread  = NULL; }
    g_sink = NULL;
    g_ctx  = NULL;
}

BOOL Audio_Start(AudioSource src, AudioFormat *fmt, AudioSinkFn sink, void *ctx,
                 BOOL deferCombinedMix,
                 AudioNativeFormatFn nativeFormat, AudioNativeSinkFn nativeSink,
                 void *nativeCtx)
{
    IMMDeviceEnumerator *devEnum = NULL;
    IMMDevice           *device  = NULL;
    uintptr_t            th;
    DWORD                streamFlags = 0;
    BOOL                 ok = FALSE;

    if (src == AUDIO_NONE || !fmt || !sink)
        return FALSE;
    Audio_Stop();
    if (g_thread || g_both.active)
        return FALSE;      /* a previous capture session is still winding down */
    Audio_SetMicrophoneMuted(FALSE);
    g_singleSource = src;
    if (src == AUDIO_BOTH)
        return Audio_StartBoth(fmt, sink, ctx, deferCombinedMix,
                              nativeFormat, nativeSink, nativeCtx);

    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator_, (void **)&devEnum)))
        return FALSE;

    /* Loopback captures the render endpoint, which is why system audio asks
       for eRender rather than eCapture. */
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devEnum,
            src == AUDIO_SYSTEM ? eRender : eCapture, eConsole, &device)))
        goto done;

    if (FAILED(IMMDevice_Activate(device, &IID_IAudioClient_, CLSCTX_ALL, NULL,
                                  (void **)&g_client)))
        goto done;
    /*
     * Ask for the raw stream, which bypasses the endpoint's effects.
     *
     * This matters more than it sounds. A loopback capture taps what the audio
     * engine is playing, and on a device with enhancements switched on that is
     * the *processed* signal - on one measured headset amplifier, a bass and
     * treble lift of up to 18 dB against the middle. Recording that and then playing the recording
     * back through the same enhancements applies the curve twice, which is
     * exactly the muddy, damped sound the whole thing was reported for.
     *
     * Not every driver honours it - plenty answer
     * AUDCLNT_E_RAW_MODE_UNSUPPORTED (0x88890027) and keep their effects in
     * the chain - so a refusal is logged and otherwise ignored. On such a device
     * the only way to record the untouched signal is to turn the endpoint's
     * enhancements off in Sound settings.
     */
    if (!g_cfg.recordRawAudio) {
        Log_Printf(L"audio: raw mode disabled by settings");
    } else {
        IAudioClient2 *client2 = NULL;
        if (SUCCEEDED(IAudioClient_QueryInterface(g_client, &IID_IAudioClient2_,
                                                  (void **)&client2))) {
            AudioClientProperties props;
            HRESULT hr;
            ZeroMemory(&props, sizeof(props));
            props.cbSize     = sizeof(props);
            props.bIsOffload = FALSE;
            props.eCategory  = AudioCategory_Other;
            props.Options    = AUDCLNT_STREAMOPTIONS_RAW;
            hr = IAudioClient2_SetClientProperties(client2, &props);
            if (SUCCEEDED(hr))
                Log_Printf(L"audio: raw mode on, endpoint effects bypassed");
            else
                Log_Printf(L"audio: raw mode refused (0x%08lX); the endpoint's "
                           L"effects stay in the captured signal",
                           (unsigned long)hr);
            IAudioClient2_Release(client2);
        }
    }

    if (FAILED(IAudioClient_GetMixFormat(g_client, &g_mix)))
        goto done;

    /* The AAC encoder takes 44.1 or 48 kHz only. Resampling here would be a
       module of its own, so an exotic endpoint is reported rather than faked. */
    if (g_mix->nSamplesPerSec != 44100 && g_mix->nSamplesPerSec != 48000) {
        Log_Printf(L"audio: endpoint runs at %lu Hz, which AAC cannot take",
                   (unsigned long)g_mix->nSamplesPerSec);
        goto done;
    }

    g_outChannels = g_mix->nChannels >= 2 ? 2 : 1;

    if (src == AUDIO_SYSTEM)
        streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;

    if (FAILED(IAudioClient_Initialize(g_client, AUDCLNT_SHAREMODE_SHARED,
                                       streamFlags, BUFFER_DURATION_100NS, 0,
                                       g_mix, NULL)))
        goto done;
    if (FAILED(IAudioClient_GetService(g_client, &IID_IAudioCaptureClient_,
                                       (void **)&g_capture)))
        goto done;

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent)
        goto done;

    g_sink = sink;
    g_ctx  = ctx;

    if (FAILED(IAudioClient_Start(g_client)))
        goto done;

    th = _beginthreadex(NULL, 0, CaptureThread, NULL, 0, NULL);
    if (!th)
        goto done;
    g_thread = (HANDLE)th;

    fmt->sampleRate = g_mix->nSamplesPerSec;
    fmt->channels   = g_outChannels;
    ok = TRUE;
    Log_Printf(L"audio: %s at %lu Hz, %u channel(s)",
               src == AUDIO_SYSTEM ? L"system loopback" : L"microphone",
               (unsigned long)fmt->sampleRate, fmt->channels);

done:
    if (device)  IMMDevice_Release(device);
    if (devEnum) IMMDeviceEnumerator_Release(devEnum);
    if (!ok)
        Cleanup();
    return ok;
}

void Audio_Stop(void)
{
    Audio_StopBoth();
    if (g_stopEvent)
        SetEvent(g_stopEvent);
    if (g_thread) {
        /*
         * The capture loop wakes every 10 ms and only ever waits on the stop
         * event, so it is gone well inside this. If it is not, releasing the
         * client and the capture service would pull them out from under it -
         * a crash rather than a slow stop. The thread reads these globals on
         * every pass, so they are left exactly as they are: it will see the
         * stop event and leave, and the next stop finishes the cleanup.
         * Audio_Start refuses to run while that is still pending.
         */
        if (WaitForSingleObject(g_thread, 2000) != WAIT_OBJECT_0) {
            Log_Printf(L"audio: capture thread did not stop in time");
            return;
        }
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    Cleanup();
    g_singleSource = AUDIO_NONE;
    Audio_SetMicrophoneMuted(FALSE);
}
