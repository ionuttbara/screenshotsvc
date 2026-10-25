/*
 * audio.c - WASAPI shared-mode capture of any playback or recording device.
 */

#define COBJMACROS

#include "nitshot.h"
#include "audio.h"
#include "settings.h"
#include "log.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <propsys.h>
#include <process.h>
#include <strsafe.h>

/*
 * The WASAPI headers only declare these; the definitions live in a generated
 * object that no import library on this SDK carries, and the class id is a C++
 * class declaration that C cannot see at all. Every value is taken from the
 * MIDL_INTERFACE attributes in mmdeviceapi.h / Audioclient.h, ksmedia.h and
 * functiondiscoverykeys_devpkey.h.
 */
static const CLSID CLSID_MMDeviceEnumerator_ =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID IID_IMMDeviceEnumerator_ =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID IID_IMMEndpoint_ =
    { 0x1BE09788, 0x6894, 0x4089, { 0x85, 0x86, 0x9A, 0x2A, 0x6C, 0x26, 0x5A, 0xC5 } };
static const IID IID_IAudioClient_ =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID IID_IAudioCaptureClient_ =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const IID IID_IAudioClient2_ =
    { 0x726778CD, 0xF60A, 0x4EDA, { 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA } };
static const GUID SUBTYPE_IEEE_FLOAT_ =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const PROPERTYKEY PKEY_Device_FriendlyName_ =
    { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };

#define BUFFER_DURATION_100NS  2000000   /* 200 ms of endpoint buffer */

struct AudioCapture {
    LONG                 refs;        /* the caller's and the thread's */
    IAudioClient        *client;
    IAudioCaptureClient *capture;
    WAVEFORMATEXTENSIBLE format;      /* what the stream delivers */
    HANDLE               thread;
    HANDLE               stopEvent;
    AudioSinkFn          sink;
    void                *ctx;
    UINT32               outChannels;
    wchar_t              name[AUDIO_NAME_CCH];
};

/* ---------------------------------------------------------------- devices */

static IMMDeviceEnumerator *OpenEnumerator(void)
{
    IMMDeviceEnumerator *e = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator_, (void **)&e)))
        return NULL;
    return e;
}

static void FriendlyName(IMMDevice *dev, wchar_t *name, size_t cch)
{
    IPropertyStore *store = NULL;
    PROPVARIANT     v;

    name[0] = 0;
    PropVariantInit(&v);
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &store))) {
        if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName_, &v)) &&
            v.vt == VT_LPWSTR && v.pwszVal)
            StringCchCopyW(name, cch, v.pwszVal);
        PropVariantClear(&v);
        IPropertyStore_Release(store);
    }
    if (!name[0])
        StringCchCopyW(name, cch, L"?");
}

static BOOL DataFlow(IMMDevice *dev, EDataFlow *flow)
{
    IMMEndpoint *ep = NULL;
    BOOL ok = FALSE;
    if (SUCCEEDED(IMMDevice_QueryInterface(dev, &IID_IMMEndpoint_, (void **)&ep))) {
        ok = SUCCEEDED(IMMEndpoint_GetDataFlow(ep, flow));
        IMMEndpoint_Release(ep);
    }
    return ok;
}

BOOL Audio_IsNone(const wchar_t *spec)
{
    return !spec || !spec[0] || lstrcmpiW(spec, AUDIO_DEV_NONE) == 0;
}

/* Resolves a spec to a device that is present and active, and says whether
   it is to be recorded as what it plays. */
static IMMDevice *OpenSpec(IMMDeviceEnumerator *e, const wchar_t *spec, BOOL *loopback)
{
    IMMDevice *dev = NULL;
    EDataFlow  flow;
    DWORD      state = 0;

    if (Audio_IsNone(spec))
        return NULL;
    if (lstrcmpiW(spec, AUDIO_DEV_DEFAULT_PLAYBACK) == 0) {
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, eConsole, &dev)))
            return NULL;
        *loopback = TRUE;
        return dev;
    }
    if (lstrcmpiW(spec, AUDIO_DEV_DEFAULT_INPUT) == 0) {
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eCapture, eConsole, &dev)))
            return NULL;
        *loopback = FALSE;
        return dev;
    }
    /* A device that was unplugged still opens by id; it is just not active. */
    if (FAILED(IMMDeviceEnumerator_GetDevice(e, spec, &dev)))
        return NULL;
    if (FAILED(IMMDevice_GetState(dev, &state)) || state != DEVICE_STATE_ACTIVE ||
        !DataFlow(dev, &flow)) {
        IMMDevice_Release(dev);
        return NULL;
    }
    *loopback = flow == eRender;
    return dev;
}

static UINT AddFlow(IMMDeviceEnumerator *e, EDataFlow flow, AudioDevice *out, UINT at, UINT max)
{
    IMMDeviceCollection *col = NULL;
    UINT n = 0, i;

    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(e, flow, DEVICE_STATE_ACTIVE, &col)))
        return at;
    IMMDeviceCollection_GetCount(col, &n);
    for (i = 0; i < n && at < max; ++i) {
        IMMDevice *dev = NULL;
        LPWSTR     id  = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &dev)))
            continue;
        if (SUCCEEDED(IMMDevice_GetId(dev, &id)) && id) {
            if (SUCCEEDED(StringCchCopyW(out[at].id, ARRAYSIZE(out[at].id), id))) {
                FriendlyName(dev, out[at].name, ARRAYSIZE(out[at].name));
                out[at].flow = flow == eRender ? AUDIO_FLOW_PLAYBACK : AUDIO_FLOW_INPUT;
                ++at;
            }
            CoTaskMemFree(id);
        }
        IMMDevice_Release(dev);
    }
    IMMDeviceCollection_Release(col);
    return at;
}

UINT Audio_ListDevices(AudioDevice **list)
{
    IMMDeviceEnumerator *e;
    IMMDeviceCollection *col;
    AudioDevice *out;
    UINT render = 0, capture = 0, n;

    *list = NULL;
    e = OpenEnumerator();
    if (!e)
        return 0;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(e, eRender, DEVICE_STATE_ACTIVE, &col))) {
        IMMDeviceCollection_GetCount(col, &render);
        IMMDeviceCollection_Release(col);
    }
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(e, eCapture, DEVICE_STATE_ACTIVE, &col))) {
        IMMDeviceCollection_GetCount(col, &capture);
        IMMDeviceCollection_Release(col);
    }
    n = render + capture;
    if (n == 0 || n > 256 ||
        !(out = (AudioDevice *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         n * sizeof(AudioDevice)))) {
        IMMDeviceEnumerator_Release(e);
        return 0;
    }
    /* A device plugged in between counting and listing is simply not listed. */
    n = AddFlow(e, eRender, out, 0, n);
    n = AddFlow(e, eCapture, out, n, render + capture);
    IMMDeviceEnumerator_Release(e);
    if (n == 0) {
        HeapFree(GetProcessHeap(), 0, out);
        return 0;
    }
    *list = out;
    return n;
}

void Audio_FreeDevices(AudioDevice *list)
{
    if (list)
        HeapFree(GetProcessHeap(), 0, list);
}

BOOL Audio_DeviceName(const wchar_t *spec, wchar_t *name, size_t cch)
{
    IMMDeviceEnumerator *e;
    IMMDevice *dev;
    BOOL loopback = FALSE;

    if (cch) name[0] = 0;
    e = OpenEnumerator();
    if (!e)
        return FALSE;
    dev = OpenSpec(e, spec, &loopback);
    if (dev) {
        FriendlyName(dev, name, cch);
        IMMDevice_Release(dev);
    }
    IMMDeviceEnumerator_Release(e);
    return dev != NULL;
}

/* ------------------------------------------------------------ conversion */

/*
 * The stream is practically always 32-bit float and the AAC encoder wants
 * 16-bit PCM. Where Windows converts to stereo for us this sees exactly two
 * channels; on the fallback path, channels beyond the first two are dropped
 * rather than downmixed, as a correct 5.1 fold-down needs the channel mask and
 * matrix coefficients.
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

static INT16 ClampToPcm(float v)
{
    float s = v * 32767.0f;
    if (s >  32767.0f) s =  32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    return (INT16)(s < 0.0f ? s - 0.5f : s + 0.5f);
}

static void Convert(const AudioCapture *cap, const BYTE *src, UINT32 frames, INT16 *dst)
{
    const WAVEFORMATEX *wf = &cap->format.Format;
    UINT32 i, c;
    UINT32 inCh  = wf->nChannels;
    UINT32 outCh = cap->outChannels;

    if (IsFloat(wf)) {
        const float *f = (const float *)src;
        for (i = 0; i < frames; i++)
            for (c = 0; c < outCh; c++)
                dst[i * outCh + c] = ClampToPcm(f[i * inCh + c]);
    } else if (wf->wBitsPerSample == 16) {
        const INT16 *s = (const INT16 *)src;
        for (i = 0; i < frames; i++)
            for (c = 0; c < outCh; c++)
                dst[i * outCh + c] = s[i * inCh + c];
    } else {
        ZeroMemory(dst, (SIZE_T)frames * outCh * sizeof(INT16));
    }
}

/* ---------------------------------------------------------------- object */

static void Release(AudioCapture *cap)
{
    if (InterlockedDecrement(&cap->refs) != 0)
        return;
    if (cap->capture) IAudioCaptureClient_Release(cap->capture);
    if (cap->client)  { IAudioClient_Stop(cap->client); IAudioClient_Release(cap->client); }
    if (cap->stopEvent) CloseHandle(cap->stopEvent);
    HeapFree(GetProcessHeap(), 0, cap);
}

/* ---------------------------------------------------------------- thread */

static unsigned __stdcall CaptureThread(void *arg)
{
    AudioCapture *cap = (AudioCapture *)arg;
    INT16 *scratch = NULL;
    UINT32 scratchFrames = 0;
    BOOL   lost = FALSE;
    /*
     * Poll far more often than the endpoint buffer is long. Half the buffer
     * looks like it ought to be enough and is not: the thread only has to be
     * descheduled once for the buffer to overrun, and WASAPI's answer to an
     * overrun is to throw the samples away. It also decides how lumpy the
     * delivery is, and lumpy delivery upsets the recorder's timeline.
     */
    DWORD  waitMs = 10;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    while (WaitForSingleObject(cap->stopEvent, waitMs) == WAIT_TIMEOUT) {
        UINT32  packet = 0;
        HRESULT hr;

        while (SUCCEEDED(hr = IAudioCaptureClient_GetNextPacketSize(cap->capture, &packet)) &&
               packet > 0) {
            BYTE   *data  = NULL;
            UINT32  frames = 0;
            DWORD   flags = 0;

            if (FAILED(IAudioCaptureClient_GetBuffer(cap->capture, &data, &frames,
                                                     &flags, NULL, NULL)))
                break;

            if (frames > scratchFrames) {
                SIZE_T bytes = (SIZE_T)frames * cap->outChannels * sizeof(INT16);
                INT16 *n = (INT16 *)(scratch
                    ? HeapReAlloc(GetProcessHeap(), 0, scratch, bytes)
                    : HeapAlloc(GetProcessHeap(), 0, bytes));
                if (!n) {
                    IAudioCaptureClient_ReleaseBuffer(cap->capture, frames);
                    break;
                }
                scratch = n;
                scratchFrames = frames;
            }

            /* A loopback stream reports silence rather than delivering zeros,
               and the encoder still needs those samples to keep its timeline. */
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                ZeroMemory(scratch, (SIZE_T)frames * cap->outChannels * sizeof(INT16));
            else
                Convert(cap, data, frames, scratch);

            cap->sink(cap->ctx, (const BYTE *)scratch, frames);
            IAudioCaptureClient_ReleaseBuffer(cap->capture, frames);
        }
        /* Unplugged mid-recording. The recorder pads the track with silence
           from here on; there is nothing to reopen until it is plugged back. */
        if (FAILED(hr) && !lost) {
            lost = TRUE;
            waitMs = 100;
            Log_Printf(L"audio: %s went away (0x%08lX)", cap->name, (unsigned long)hr);
        }
    }

    if (scratch)
        HeapFree(GetProcessHeap(), 0, scratch);
    Release(cap);
    CoUninitialize();
    return 0;
}

/* ----------------------------------------------------------------- setup */

/*
 * Ask for the raw stream, which bypasses the endpoint's effects.
 *
 * This matters more than it sounds. A loopback capture taps what the audio
 * engine is playing, and on a device with enhancements switched on that is the
 * *processed* signal - on one measured headset amplifier, a bass and treble
 * lift of up to 18 dB against the middle. Recording that and then playing the
 * recording back through the same enhancements applies the curve twice, which
 * is exactly the muddy, damped sound the whole thing was reported for.
 *
 * Not every driver honours it - plenty answer AUDCLNT_E_RAW_MODE_UNSUPPORTED
 * (0x88890027) and keep their effects in the chain - so a refusal is logged
 * and otherwise ignored. On such a device the only way to record the untouched
 * signal is to turn the endpoint's enhancements off in Sound settings.
 */
static void RequestRaw(IAudioClient *client)
{
    IAudioClient2 *client2 = NULL;
    AudioClientProperties props;
    HRESULT hr;

    if (!g_cfg.recordRawAudio) {
        Log_Printf(L"audio: raw mode disabled by settings");
        return;
    }
    if (FAILED(IAudioClient_QueryInterface(client, &IID_IAudioClient2_, (void **)&client2)))
        return;
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
                   L"effects stay in the captured signal", (unsigned long)hr);
    IAudioClient2_Release(client2);
}

/*
 * Opens the stream at AUDIO_RATE stereo float and lets the audio engine
 * resample and fold the channels down. That takes any device - a 96 kHz
 * interface, a 5.1 headset, a mono microphone - and gives every track the same
 * format, which is what lets two of them be mixed or muxed side by side. Only
 * if the engine refuses is the device's own mix format used, as before.
 */
static BOOL InitStream(AudioCapture *cap, BOOL loopback)
{
    WAVEFORMATEXTENSIBLE *want = &cap->format;
    WAVEFORMATEX *mix = NULL;
    DWORD   flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    HRESULT hr;

    ZeroMemory(want, sizeof(*want));
    want->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    want->Format.nChannels       = AUDIO_CHANNELS;
    want->Format.nSamplesPerSec  = AUDIO_RATE;
    want->Format.wBitsPerSample  = 32;
    want->Format.nBlockAlign     = AUDIO_CHANNELS * 4;
    want->Format.nAvgBytesPerSec = AUDIO_RATE * AUDIO_CHANNELS * 4;
    want->Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    want->Samples.wValidBitsPerSample = 32;
    want->dwChannelMask          = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    want->SubFormat              = SUBTYPE_IEEE_FLOAT_;

    hr = IAudioClient_Initialize(cap->client, AUDCLNT_SHAREMODE_SHARED,
                                 flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 BUFFER_DURATION_100NS, 0, &want->Format, NULL);
    if (SUCCEEDED(hr)) {
        cap->outChannels = AUDIO_CHANNELS;
        return TRUE;
    }
    Log_Printf(L"audio: %s refused conversion (0x%08lX), using its own format",
               cap->name, (unsigned long)hr);

    /* A refused Initialize leaves the client unusable; start over. */
    if (FAILED(IAudioClient_GetMixFormat(cap->client, &mix)))
        return FALSE;
    if (mix->nSamplesPerSec != 44100 && mix->nSamplesPerSec != 48000) {
        Log_Printf(L"audio: %s runs at %lu Hz, which AAC cannot take",
                   cap->name, (unsigned long)mix->nSamplesPerSec);
        CoTaskMemFree(mix);
        return FALSE;
    }
    ZeroMemory(want, sizeof(*want));
    CopyMemory(want, mix, min(sizeof(*want), sizeof(WAVEFORMATEX) + mix->cbSize));
    cap->outChannels = mix->nChannels >= 2 ? 2 : 1;
    hr = IAudioClient_Initialize(cap->client, AUDCLNT_SHAREMODE_SHARED, flags,
                                 BUFFER_DURATION_100NS, 0, mix, NULL);
    CoTaskMemFree(mix);
    return SUCCEEDED(hr);
}

static BOOL Activate(IMMDevice *dev, AudioCapture *cap)
{
    if (cap->client) {
        IAudioClient_Release(cap->client);
        cap->client = NULL;
    }
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient_, CLSCTX_ALL, NULL,
                                  (void **)&cap->client)))
        return FALSE;
    RequestRaw(cap->client);
    return TRUE;
}

AudioCapture *Audio_Start(const wchar_t *spec, AudioFormat *fmt,
                          AudioSinkFn sink, void *ctx)
{
    IMMDeviceEnumerator *e;
    IMMDevice    *dev = NULL;
    AudioCapture *cap;
    BOOL          loopback = FALSE, ok = FALSE;
    uintptr_t     th;

    if (Audio_IsNone(spec) || !fmt || !sink)
        return NULL;
    cap = (AudioCapture *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*cap));
    if (!cap)
        return NULL;
    cap->refs = 1;
    cap->sink = sink;
    cap->ctx  = ctx;

    e = OpenEnumerator();
    if (e) {
        dev = OpenSpec(e, spec, &loopback);
        IMMDeviceEnumerator_Release(e);
    }
    if (!dev) {
        Log_Printf(L"audio: device %s is not present", spec);
        goto done;
    }
    FriendlyName(dev, cap->name, ARRAYSIZE(cap->name));

    if (!Activate(dev, cap))
        goto done;
    if (!InitStream(cap, loopback)) {
        /* The conversion attempt may have consumed the client; one more try
           on a fresh one with the device's own format. */
        if (!Activate(dev, cap) || !InitStream(cap, loopback))
            goto done;
    }
    if (FAILED(IAudioClient_GetService(cap->client, &IID_IAudioCaptureClient_,
                                       (void **)&cap->capture)))
        goto done;

    cap->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!cap->stopEvent || FAILED(IAudioClient_Start(cap->client)))
        goto done;

    cap->refs = 2;                       /* one more for the thread */
    th = _beginthreadex(NULL, 0, CaptureThread, cap, 0, NULL);
    if (!th) {
        cap->refs = 1;
        goto done;
    }
    cap->thread = (HANDLE)th;

    fmt->sampleRate = cap->format.Format.nSamplesPerSec;
    fmt->channels   = cap->outChannels;
    ok = TRUE;
    Log_Printf(L"audio: %s %s at %lu Hz, %u channel(s)",
               loopback ? L"loopback of" : L"input from", cap->name,
               (unsigned long)fmt->sampleRate, fmt->channels);

done:
    if (dev)
        IMMDevice_Release(dev);
    if (!ok) {
        Release(cap);
        return NULL;
    }
    return cap;
}

const wchar_t *Audio_Name(const AudioCapture *cap)
{
    return cap ? cap->name : L"";
}

void Audio_Stop(AudioCapture *cap)
{
    if (!cap)
        return;
    SetEvent(cap->stopEvent);
    /*
     * The capture loop wakes every 10 ms and only ever waits on the stop
     * event, so it is gone well inside this. If it is not, the thread still
     * holds its own reference, so nothing is pulled out from under it: it
     * sees the stop event when it gets going again and frees the object then.
     */
    if (WaitForSingleObject(cap->thread, 2000) != WAIT_OBJECT_0)
        Log_Printf(L"audio: capture thread for %s did not stop in time", cap->name);
    CloseHandle(cap->thread);
    Release(cap);
}
