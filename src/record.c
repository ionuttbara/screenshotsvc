/*
 * record.c - the recording worker: duplication in, MP4 out.
 *
 * Two capture paths. The SDR one takes the tone-mapped 8-bit desktop from
 * plain DuplicateOutput and hands it to the encoder as RGB32, which is what it
 * has always done. The HDR one asks DuplicateOutput1 for scRGB half-float,
 * converts it to 10-bit BT.2020 PQ on the GPU (hdrvideo.c) and feeds P010 to
 * HEVC Main10. Which one runs is decided per recording, from the display's
 * state and what the encoder probe found.
 */

#define COBJMACROS

#include "nitshot.h"
#include "record.h"
#include "settings.h"
#include "encoder.h"
#include "hdrvideo.h"
#include "mp4tag.h"
#include "wavsink.h"
#include "remux.h"
#include "nvenc.h"
#include "amfenc.h"
#include "qsvenc.h"
#include "lang.h"
#include "hdr.h"
#include "log.h"
#include "crash.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>      /* IDXGIOutput6, for the display's colour volume */
#include <d3d11_4.h>      /* ID3D11Multithread                             */
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <process.h>


/* How long the source must have been silent before the timeline is padded,
   and how much of the recent past is left alone while doing it. */
/* How long a stop waits for the worker before giving up on it; finalisation
   plus an optional WAV remux of a long recording is the slow case. The test
   harness shortens it, which is the only way to reach the timeout path. */
#ifndef STOP_WAIT_MS
#define STOP_WAIT_MS         15000
#endif

#define QUIET_BEFORE_PAD_MS  400
#define PAD_LAG_MS           250

/*
 * MFSetAttributeSize and MFSetAttributeRatio are inline helpers that the SDK
 * only defines for C++ - they call pAttributes->SetUINT64 directly. The packing
 * they do is no secret, so C does it here.
 */
static void SetPacked(IMFMediaType *t, const GUID *key, UINT32 high, UINT32 low)
{
    IMFMediaType_SetUINT64(t, key, ((UINT64)high << 32) | (UINT64)low);
}

/*
 * One sound source. Each runs on its device's own clock, so each keeps its own
 * timeline, counted in the samples it actually delivered (plus padding), and
 * its own idea of when it last delivered anything.
 */
#define MIX_RING   (AUDIO_RATE * 4)   /* how far one source may run ahead */
#define MIX_CHUNK  4096

typedef struct {
    AudioCapture *cap;
    UINT32        rate, channels;
    LONGLONG      frames;              /* its timeline, in samples        */
    ULONGLONG     lastTick;            /* when real audio last arrived    */
    WavSink      *wav;                 /* its own .wav, or NULL           */
    int           wavSlot;             /* its path in result->wavPath     */
    wchar_t       name[AUDIO_NAME_CCH];
} RecSource;

typedef struct {
    /* configuration */
    RecordOptions opt;
    /* A copy of the settings taken when the recording starts. The worker
       reads only this, so the settings dialog can change - or be in the
       middle of rewriting - the live ones without this thread ever seeing a
       half-copied folder path or a choice that changed mid-recording. */
    Settings      cfg;
    RECT          out;          /* region clipped to one output, screen coords */
    HWND          notify;
    UINT          doneMsg;

    /* threading */
    HANDLE        thread;
    HANDLE        stopEvent;
    volatile LONG active;        /* frames are being captured right now      */
    /*
     * True from Record_Start until the worker's very last instruction.
     * 'active' is false during setup and again during finalisation, and a
     * second Record_Start in either window would zero this struct under a
     * live thread - so starting and stopping go by this one instead.
     */
    volatile LONG busy;
    CRITICAL_SECTION writeLock; /* the sink writer is not thread safe */
    LARGE_INTEGER qpcStart, qpcFreq;

    /* capture */
    ID3D11Device           *dev;
    ID3D11DeviceContext    *ctx;
    IDXGIOutputDuplication *dup;
    BOOL                    deviceLost;  /* the D3D device itself went away  */
    ID3D11Texture2D        *staging;     /* SDR path only: BGRA readback      */
    /*
     * The zero-copy path. Frames never come back to the CPU: they are copied
     * or rendered into textures the encoder reads directly, through a DXGI
     * device manager. Media Foundation then uses this device from its own
     * threads, so every sequence of our calls on it holds 'mt'.
     */
    BOOL                    gpu;
    BOOL                    devVideo;    /* the device was made video-capable */
    IMFDXGIDeviceManager   *mgr;
    /* The direct path: the graphics card's own encoder driven through its
       vendor's API (hwenc.h), the MP4 writer only muxing what it produces.
       NULL on the Media Foundation paths. */
    const HwEncOps         *hwOps;
    void                   *hw;
    ID3D11Texture2D        *hwTex;       /* HDR: the P010 frame it reads    */
    LONGLONG                avOffset;    /* how much later the picture shows
                                            than it was captured - the sound
                                            is moved by the same            */
    BOOL                    hwFailed;
    BOOL                    direct;      /* the card's encoder or nothing   */
    LONGLONG                hwPackets;   /* pictures the card's encoder gave */
    UINT                    mgrToken;
    ID3D11Multithread      *mt;
    ID3D11Texture2D        *frameTex;    /* SDR capture: the current frame   */
    HdrVideo               *hv;          /* HDR path only: scRGB -> P010      */
    POINT                   outOrigin;   /* the output's top-left, screen coords */
    wchar_t                 outName[32]; /* GDI device name, for the HDR query */
    /*
     * These are two different questions. 'floatCapture' is how the desktop is
     * taken; 'hdr' is what is written. On an HDR display the capture is always
     * floating point, because plain DuplicateOutput does not hand back a
     * usable SDR image there - it skips the divide by SDR white, so everything
     * arrives three times too bright and clips. An 8-bit recording of an HDR
     * desktop is therefore float in, tone-mapped here, 8-bit out.
     */
    BOOL                    floatCapture;
    BOOL                    hdr;         /* ...and encoding 10-bit PQ         */
    BOOL                    hdrLocked;   /* ...and the file already says so   */
    float                   sdrWhiteNits;
    /* What the file has to say about itself afterwards: the display it was
       made on, and how bright the frames actually turned out. */
    Mp4ColourInfo           colour;

    /* encoding */
    IMFSinkWriter *writer;
    EncoderId      enc;
    DWORD          videoStream;
    UINT32         width, height;
    LONGLONG       videoFrames;          /* written to the sink so far      */
    /* Why frames did not arrive, summarised in the log when the file closes:
       a recording of a still desktop and a broken capture look identical
       from the outside otherwise. */
    LONGLONG       grabTimeouts, grabPointerOnly, grabFailures, grabCopies;
    /* QPC ticks spent turning captured frames into samples for the writer,
       so the log can say what one frame costs - the figure the buffer pool
       exists to bring down. */
    LONGLONG       buildTicks;
    /* sound */
    RecSource      src[RECORD_SOURCES];
    int            srcCount;             /* sources actually running         */
    BOOL           haveAudio;            /* there is somewhere to write them */
    BOOL           mix;                  /* both sources go into one track   */
    DWORD          aac[RECORD_SOURCES];  /* the MP4's live AAC streams       */
    int            aacCount;             /* 0 when built from the WAVs later */
    BOOL           muxFromWav;           /* no live AAC; build it afterwards */
    INT32         *mixRing;              /* MIX_RING frames being summed     */
    LONGLONG       mixOut;               /* the mix's timeline: emitted      */
    WavSink       *mixWav;               /* .track0.wav                      */
    int            mixWavSlot;
    INT16          mixScratch[MIX_CHUNK * AUDIO_CHANNELS];

    RecordResult  *result;
} Recorder;

static Recorder g_rec;

void Record_FreeResult(RecordResult *res)
{
    if (res)
        HeapFree(GetProcessHeap(), 0, res);
}

BOOL Record_IsActive(void)
{
    return InterlockedCompareExchange(&g_rec.active, 0, 0) != 0;
}

BOOL Record_IsBusy(void)
{
    return InterlockedCompareExchange(&g_rec.busy, 0, 0) != 0;
}

DWORD Record_ElapsedMs(void)
{
    LARGE_INTEGER now;

    if (!Record_IsActive() || g_rec.qpcFreq.QuadPart == 0)
        return 0;
    QueryPerformanceCounter(&now);
    return (DWORD)((now.QuadPart - g_rec.qpcStart.QuadPart) * 1000 /
                   g_rec.qpcFreq.QuadPart);
}

static LONGLONG ElapsedNs100(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - g_rec.qpcStart.QuadPart) * 10000000 /
           g_rec.qpcFreq.QuadPart;
}

static void Fail(const wchar_t *why)
{
    if (g_rec.result && !g_rec.result->error[0])
        StringCchCopyW(g_rec.result->error, ARRAYSIZE(g_rec.result->error), why);
    Log_Printf(L"record: %s", why);
}

/* --------------------------------------------------------- the duplication */

/* Find the output containing the region's centre, and bring up duplication for
   it. A region spanning two monitors is clipped to one; recording a composite
   of several outputs would need one duplication each and a shared clock. */
/*
 * Finds the output the region sits on and starts duplicating it.
 *
 * 'reuseDevice' is what a rebuild after a lost duplication asks for. The
 * staging texture and the HDR conversion resources belong to the device they
 * were created on, so creating a second device here would leave the loop
 * copying into textures the new context knows nothing about - silently, with
 * the recording continuing on a frozen frame. A rebuild therefore keeps the
 * device and re-duplicates the same monitor; only that monitor will do.
 */
static BOOL OpenOutput(BOOL reuseDevice)
{
    IDXGIFactory1 *factory = NULL;
    POINT          centre;
    BOOL           ok = FALSE;
    UINT           ai;

    if (reuseDevice && !g_rec.dev)
        return FALSE;

    centre.x = (g_rec.opt.region.left + g_rec.opt.region.right) / 2;
    centre.y = (g_rec.opt.region.top + g_rec.opt.region.bottom) / 2;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return FALSE;

    for (ai = 0; !ok; ai++) {
        IDXGIAdapter1 *adapter = NULL;
        UINT oi;

        if (IDXGIFactory1_EnumAdapters1(factory, ai, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        for (oi = 0; !ok; oi++) {
            IDXGIOutput      *output = NULL;
            IDXGIOutput1     *out1   = NULL;
            DXGI_OUTPUT_DESC  od;
            D3D_FEATURE_LEVEL got;

            if (IDXGIAdapter1_EnumOutputs(adapter, oi, &output) == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(IDXGIOutput_GetDesc(output, &od)) || !od.AttachedToDesktop) {
                IDXGIOutput_Release(output);
                continue;
            }
            /* A rebuild has to land on the monitor the recording started on;
               a first open takes whichever one the region is centred over. */
            if (reuseDevice ? wcscmp(od.DeviceName, g_rec.outName) != 0
                            : !PtInRect(&od.DesktopCoordinates, centre)) {
                IDXGIOutput_Release(output);
                continue;
            }

            if (!reuseDevice) {
                /* Video support is what lets the encoder take this device's
                   textures. Asked for first, and only when wanted; a driver
                   that refuses it still gets a device for the CPU path. */
                HRESULT hrDev = E_FAIL;
                g_rec.devVideo = FALSE;
                if (g_rec.cfg.recordZeroCopy) {
                    hrDev = D3D11CreateDevice((IDXGIAdapter *)adapter,
                                              D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                              NULL, 0, D3D11_SDK_VERSION,
                                              &g_rec.dev, &got, &g_rec.ctx);
                    g_rec.devVideo = SUCCEEDED(hrDev);
                }
                if (FAILED(hrDev))
                    hrDev = D3D11CreateDevice((IDXGIAdapter *)adapter,
                                              D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                              NULL, 0, D3D11_SDK_VERSION,
                                              &g_rec.dev, &got, &g_rec.ctx);
                if (FAILED(hrDev)) {
                    IDXGIOutput_Release(output);
                    continue;
                }
            }

            if (SUCCEEDED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1,
                                                     (void **)&out1))) {
                HRESULT hrDup = E_FAIL;

                StringCchCopyW(g_rec.outName, ARRAYSIZE(g_rec.outName),
                               od.DeviceName);

                /*
                 * DuplicateOutput1 with a float format is the only way to be
                 * handed the desktop before the compositor tone-maps it to
                 * 8-bit. Plain DuplicateOutput on an HDR display gives the
                 * already-flattened version - which is what every recording
                 * before this looked like.
                 */
                if (g_rec.floatCapture) {
                    IDXGIOutput5 *out5 = NULL;
                    if (SUCCEEDED(IDXGIOutput_QueryInterface(output,
                            &IID_IDXGIOutput5, (void **)&out5))) {
                        DXGI_FORMAT want = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        hrDup = IDXGIOutput5_DuplicateOutput1(out5,
                                    (IUnknown *)g_rec.dev, 0, 1, &want,
                                    &g_rec.dup);
                        if (FAILED(hrDup))
                            Log_Printf(L"record: DuplicateOutput1(FLOAT) failed "
                                       L"0x%08lX, dropping to 8-bit",
                                       (unsigned long)hrDup);
                        IDXGIOutput5_Release(out5);
                    }
                    /*
                     * Before writing starts, dropping to 8-bit is a fair
                     * trade. Once the file's media type says 10-bit BT.2020
                     * it is not: an 8-bit frame cannot be fed to it and the
                     * codec cannot change mid-stream, so a rebuild that
                     * cannot get float back reports failure and the loop
                     * retries, repeating the last frame meanwhile.
                     */
                    if (FAILED(hrDup)) {
                        if (g_rec.hdrLocked) {
                            IDXGIOutput1_Release(out1);
                            IDXGIOutput_Release(output);
                            IDXGIAdapter1_Release(adapter);
                            IDXGIFactory1_Release(factory);
                            return FALSE;
                        }
                        g_rec.floatCapture = FALSE;
                        g_rec.hdr          = FALSE;
                    }
                }

                if (FAILED(hrDup)) {
                    hrDup = IDXGIOutput1_DuplicateOutput(out1,
                                (IUnknown *)g_rec.dev, &g_rec.dup);
                    if (FAILED(hrDup))
                        Log_Printf(L"record: DuplicateOutput failed 0x%08lX",
                                   (unsigned long)hrDup);
                }

                /*
                 * The display's own colour volume is the honest answer to
                 * "what was this graded on", and without it a player has to
                 * guess - defensively, and at the cost of brightness.
                 */
                if (SUCCEEDED(hrDup) && g_rec.hdr) {
                    IDXGIOutput6 *out6 = NULL;
                    if (SUCCEEDED(IDXGIOutput_QueryInterface(output,
                            &IID_IDXGIOutput6, (void **)&out6))) {
                        DXGI_OUTPUT_DESC1 d1;
                        if (SUCCEEDED(IDXGIOutput6_GetDesc1(out6, &d1))) {
                            g_rec.colour.redX   = d1.RedPrimary[0];
                            g_rec.colour.redY   = d1.RedPrimary[1];
                            g_rec.colour.greenX = d1.GreenPrimary[0];
                            g_rec.colour.greenY = d1.GreenPrimary[1];
                            g_rec.colour.blueX  = d1.BluePrimary[0];
                            g_rec.colour.blueY  = d1.BluePrimary[1];
                            g_rec.colour.whiteX = d1.WhitePoint[0];
                            g_rec.colour.whiteY = d1.WhitePoint[1];
                            g_rec.colour.maxMasteringNits = d1.MaxLuminance;
                            g_rec.colour.minMasteringNits = d1.MinLuminance;
                            Log_Printf(L"record: mastering display %.0f..%.4f "
                                       L"nits, white %.4f,%.4f",
                                       d1.MaxLuminance, d1.MinLuminance,
                                       d1.WhitePoint[0], d1.WhitePoint[1]);
                        }
                        IDXGIOutput6_Release(out6);
                    }
                }

                if (SUCCEEDED(hrDup)) {
                    g_rec.outOrigin.x = od.DesktopCoordinates.left;
                    g_rec.outOrigin.y = od.DesktopCoordinates.top;
                    g_rec.out = g_rec.opt.region;
                    IntersectRect(&g_rec.out, &g_rec.out, &od.DesktopCoordinates);
                    ok = TRUE;
                }
                IDXGIOutput1_Release(out1);
            } else if (!reuseDevice) {
                /* No IDXGIOutput1 here: hand the device back rather than let
                   the next candidate overwrite the pointers and strand it. */
                ID3D11DeviceContext_Release(g_rec.ctx); g_rec.ctx = NULL;
                ID3D11Device_Release(g_rec.dev);        g_rec.dev = NULL;
            }
            IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }

    IDXGIFactory1_Release(factory);
    return ok;
}

/* SDR only: duplication delivers BGRA and the encoder takes RGB32, so the
   frame just needs to reach the CPU. The HDR path renders instead. */
static BOOL MakeStaging(void)
{
    D3D11_TEXTURE2D_DESC d;

    ZeroMemory(&d, sizeof(d));
    d.Width            = g_rec.width;
    d.Height           = g_rec.height;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_STAGING;
    d.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    return SUCCEEDED(ID3D11Device_CreateTexture2D(g_rec.dev, &d, NULL, &g_rec.staging));
}

/* ------------------------------------------------------------ the encoder */

/*
 * Which encoder, and therefore which capture path. The probe in encoder.c has
 * already established what this system can actually finalise a file with, so
 * all that is left here is to honour the setting and record what was chosen.
 */
static BOOL ChooseEncoder(void)
{
    EncoderId    want = Encoder_FromToken(g_rec.cfg.recordEncoder);
    HdrInfo      info;
    BOOL         displayHdr = FALSE;
    MONITORINFOEXW mi;
    POINT        centre;
    HMONITOR     mon;

    /* The monitor is looked up here rather than taken from OpenOutput, because
       the answer decides which duplication format OpenOutput has to ask for. */
    centre.x = (g_rec.opt.region.left + g_rec.opt.region.right) / 2;
    centre.y = (g_rec.opt.region.top + g_rec.opt.region.bottom) / 2;
    mon = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, (MONITORINFO *)&mi))
        StringCchCopyW(g_rec.outName, ARRAYSIZE(g_rec.outName), mi.szDevice);

    g_rec.sdrWhiteNits = 80.0f;
    if (!g_rec.cfg.forceSdr && g_rec.outName[0] &&
        Hdr_QueryMonitor(g_rec.outName, &info) && info.active) {
        displayHdr = TRUE;
        g_rec.sdrWhiteNits = info.sdrWhiteNits;
        /* Float capture regardless of what gets written; the 8-bit desktop
           duplication is simply wrong on such a display. */
        g_rec.floatCapture = TRUE;
    }
    if (!g_rec.cfg.recordHdr)
        displayHdr = FALSE;      /* capture float, but write 8-bit */

    g_rec.enc = Encoder_Resolve(want, displayHdr);
    g_rec.direct = FALSE;
    if (g_rec.enc >= ENC_COUNT && g_rec.cfg.recordDirect) {
        /* Media Foundation has nothing, but the card's own encoder needs
           nothing from it except the MP4 writer: H.264 through that, or no
           recording. Decided for good in SetupHwEncoder. */
        Log_Printf(L"record: no Media Foundation encoder; trying the card's own for H.264");
        g_rec.enc = ENC_H264;
        g_rec.direct = TRUE;
    } else if (g_rec.enc >= ENC_COUNT) {
        Log_Printf(L"record: no usable video encoder at all");
        return FALSE;
    }
    g_rec.hdr = Encoder_IsHdr(g_rec.enc);

    Log_Printf(L"record: display is %s, using %s (%s)",
               displayHdr ? L"HDR" : L"SDR", Encoder_Name(g_rec.enc),
               Encoder_Caps()->mft[g_rec.enc][0]
                   ? Encoder_Caps()->mft[g_rec.enc] : L"unnamed");
    return TRUE;
}

static BOOL AddAudioStream(UINT32 rate, UINT32 channels, DWORD *stream)
{
    IMFMediaType *outType = NULL, *inType = NULL;
    DWORD         index = 0;
    BOOL          ok = FALSE;

    if (FAILED(MFCreateMediaType(&outType)))
        goto done;
    IMFMediaType_SetGUID(outType, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(outType, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    IMFMediaType_SetUINT32(outType, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(outType, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    IMFMediaType_SetUINT32(outType, &MF_MT_AUDIO_NUM_CHANNELS, channels);
    IMFMediaType_SetUINT32(outType, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    if (FAILED(IMFSinkWriter_AddStream(g_rec.writer, outType, &index)))
        goto done;
    *stream = index;

    if (FAILED(MFCreateMediaType(&inType)))
        goto done;
    IMFMediaType_SetGUID(inType, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(inType, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_NUM_CHANNELS, channels);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           rate * channels * 2);
    if (FAILED(IMFSinkWriter_SetInputMediaType(g_rec.writer, index,
                                               inType, NULL)))
        goto done;
    ok = TRUE;

done:
    if (inType)  IMFMediaType_Release(inType);
    if (outType) IMFMediaType_Release(outType);
    return ok;
}

static BOOL PickPath(wchar_t *path, size_t cch)
{
    wchar_t    dir[MAX_PATH];
    SYSTEMTIME st;
    int        n;

    if (!Settings_ResolveFolderFrom(g_rec.cfg.videoFolder, TRUE, dir, ARRAYSIZE(dir)))
        return FALSE;

    GetLocalTime(&st);
    for (n = 1; n <= 99; n++) {
        HRESULT hr;
        if (n == 1)
            hr = StringCchPrintfW(path, cch,
                    L"%s\\Recording %04u-%02u-%02u %02u%02u%02u.mp4",
                    dir, st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
        else
            hr = StringCchPrintfW(path, cch,
                    L"%s\\Recording %04u-%02u-%02u %02u%02u%02u (%d).mp4",
                    dir, st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, n);
        if (FAILED(hr))
            return FALSE;
        if (!PathFileExistsW(path))
            return TRUE;
    }
    return FALSE;
}

static BOOL OpenWriter(void)
{
    IMFAttributes *attrs = NULL;
    BOOL ok = FALSE;

    if (!PickPath(g_rec.result->path, ARRAYSIZE(g_rec.result->path)))
        return FALSE;

    if (FAILED(MFCreateAttributes(&attrs, 3)))
        return FALSE;
    IMFAttributes_SetGUID(attrs, &MF_TRANSCODE_CONTAINERTYPE,
                          &MFTranscodeContainerType_MPEG4);
    IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                            !Encoder_Caps()->softwareOnly[g_rec.enc]);
    /* Without this the writer blocks the capture loop to pace the encoder,
       which for a live screen recording means dropped frames instead. */
    IMFAttributes_SetUINT32(attrs, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    /* With this the encoder - and on the 8-bit path the video processor in
       front of it, which converts RGB to NV12 - work on our textures on the
       GPU, instead of the frames making a round trip through system memory. */
    if (g_rec.gpu && g_rec.mgr)
        IMFAttributes_SetUnknown(attrs, &MF_SINK_WRITER_D3D_MANAGER, (IUnknown *)g_rec.mgr);

    if (SUCCEEDED(MFCreateSinkWriterFromURL(g_rec.result->path, NULL, attrs,
                                            &g_rec.writer)))
        ok = TRUE;

    IMFAttributes_Release(attrs);
    return ok;
}

/* ------------------------------------------------------------ sample pool */

/*
 * Video samples are recycled instead of allocated per frame.
 *
 * A 4K frame is a 25-33 MB buffer. Allocating one fresh for every frame meant
 * the system handing over and zero-filling every page of it, every frame:
 * measured at 4K30, six to eight thousand page faults per frame and over 20 ms
 * of the 33 ms frame budget spent building the sample. A pooled buffer has its
 * pages resident already, so the copy into it is all that is left.
 *
 * The writer queues samples (throttling is off, so a slow encoder never stalls
 * the capture), which means a buffer can only be reused once the writer has
 * finished with it. Each pooled sample is therefore a tracked sample: when the
 * writer drops its last reference, Media Foundation calls PoolReturn instead of
 * destroying it, and it goes back on the free list. The pool is capped; when
 * every pooled buffer is still in flight, a frame gets a one-off allocation as
 * before rather than waiting for one.
 */
#define POOL_MAX 6
#define POOL_TEX 0x80000000u

/* A buffer for one frame: system memory of 'key' bytes, or - with POOL_TEX -
   a GPU texture of the recording's size in that format, wrapped so the
   encoder reads it in place. */
static HRESULT MakeBuffer(DWORD key, IMFMediaBuffer **out)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D     *tex = NULL;
    HRESULT              hr;

    *out = NULL;
    if (!(key & POOL_TEX))
        return MFCreateMemoryBuffer(key, out);

    ZeroMemory(&d, sizeof(d));
    d.Width            = g_rec.width;
    d.Height           = g_rec.height;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = (DXGI_FORMAT)(key & ~POOL_TEX);
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_DEFAULT;
    /* Rendered into on the HDR path, read by the encoder and the video
       processor on both. */
    d.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(g_rec.dev, &d, NULL, &tex);
    if (FAILED(hr))
        return hr;
    hr = MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown *)tex, 0, FALSE, out);
    ID3D11Texture2D_Release(tex);     /* the buffer holds it now */
    return hr;
}

/* Stamped on every pooled sample, so one that comes back after its recording
   has ended is released instead of joining the next recording's pool. */
static const GUID POOL_GENERATION =
    { 0x7a3c5e21, 0x4b8f, 0x4d2a, { 0x9e, 0x61, 0x0c, 0x55, 0x3b, 0xa8, 0x17, 0xd4 } };

static struct {
    SRWLOCK       lock;
    IMFSample    *free[POOL_MAX];
    int           freeCount;
    int           total;          /* pooled samples in existence */
    DWORD         key;            /* what every one of them holds: a byte
                                     size, or POOL_TEX | a DXGI format     */
    UINT32        generation;
    BOOL          open;
    volatile LONG outstanding;    /* handed to the writer, not back yet */
    LONGLONG      reused, misses; /* for the log */
} g_pool;

static HRESULT STDMETHODCALLTYPE PoolCbQueryInterface(IMFAsyncCallback *self,
                                                      REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFAsyncCallback)) {
        *ppv = self;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

/* A static object: nothing to count and nothing to free. */
static ULONG STDMETHODCALLTYPE PoolCbAddRef(IMFAsyncCallback *self)
{
    UNREFERENCED_PARAMETER(self);
    return 2;
}

static ULONG STDMETHODCALLTYPE PoolCbRelease(IMFAsyncCallback *self)
{
    UNREFERENCED_PARAMETER(self);
    return 1;
}

static HRESULT STDMETHODCALLTYPE PoolCbGetParameters(IMFAsyncCallback *self,
                                                     DWORD *flags, DWORD *queue)
{
    UNREFERENCED_PARAMETER(self);
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(queue);
    return E_NOTIMPL;
}

/* Runs on a Media Foundation work-queue thread once the writer is done with a
   sample. The result's object is the sample, with a reference that is now
   ours. */
static HRESULT STDMETHODCALLTYPE PoolReturn(IMFAsyncCallback *self, IMFAsyncResult *res)
{
    IUnknown  *obj = NULL;
    IMFSample *sample = NULL;
    UINT32     gen = 0;

    UNREFERENCED_PARAMETER(self);
    if (res && SUCCEEDED(IMFAsyncResult_GetObject(res, &obj)) && obj) {
        IUnknown_QueryInterface(obj, &IID_IMFSample, (void **)&sample);
        IUnknown_Release(obj);
    }
    if (!sample)
        return S_OK;
    IMFSample_GetUINT32(sample, &POOL_GENERATION, &gen);

    AcquireSRWLockExclusive(&g_pool.lock);
    if (g_pool.open && gen == g_pool.generation && g_pool.freeCount < POOL_MAX) {
        g_pool.free[g_pool.freeCount++] = sample;
        sample = NULL;
    } else if (gen == g_pool.generation && g_pool.total > 0) {
        g_pool.total--;
    }
    ReleaseSRWLockExclusive(&g_pool.lock);
    if (gen == g_pool.generation)
        InterlockedDecrement(&g_pool.outstanding);

    if (sample)
        IMFSample_Release(sample);   /* closed or stale: let it go for good */
    return S_OK;
}

static IMFAsyncCallbackVtbl g_poolCbVtbl = {
    PoolCbQueryInterface, PoolCbAddRef, PoolCbRelease, PoolCbGetParameters, PoolReturn
};
static IMFAsyncCallback g_poolCb = { &g_poolCbVtbl };

static void PoolOpen(void)
{
    static UINT32 nextGeneration;

    InitializeSRWLock(&g_pool.lock);
    AcquireSRWLockExclusive(&g_pool.lock);
    g_pool.freeCount   = 0;
    g_pool.total       = 0;
    g_pool.key         = 0;
    g_pool.generation  = ++nextGeneration;
    g_pool.open        = TRUE;
    g_pool.reused      = 0;
    g_pool.misses      = 0;
    InterlockedExchange(&g_pool.outstanding, 0);
    ReleaseSRWLockExclusive(&g_pool.lock);
}

/* Called once the writer has been released, which is what returns the
   samples it was still holding. */
static void PoolClose(void)
{
    IMFSample *drop[POOL_MAX];
    int        n, i, waited;

    AcquireSRWLockExclusive(&g_pool.lock);
    g_pool.open = FALSE;
    n = g_pool.freeCount;
    for (i = 0; i < n; i++)
        drop[i] = g_pool.free[i];
    g_pool.freeCount = 0;
    ReleaseSRWLockExclusive(&g_pool.lock);

    for (i = 0; i < n; i++)
        IMFSample_Release(drop[i]);

    /* The last returns come in on Media Foundation's threads; they must land
       before MFShutdown takes those threads away. */
    for (waited = 0; waited < 2000 &&
                     InterlockedCompareExchange(&g_pool.outstanding, 0, 0) > 0; waited += 10)
        Sleep(10);
    if (InterlockedCompareExchange(&g_pool.outstanding, 0, 0) > 0)
        Log_Printf(L"record: %ld pooled sample(s) never came back",
                   InterlockedCompareExchange(&g_pool.outstanding, 0, 0));
    if (g_pool.reused || g_pool.misses)
        Log_Printf(L"record: sample pool reused %lld time(s), %lld one-off allocation(s)",
                   g_pool.reused, g_pool.misses);
}

/*
 * A video sample with one 'size'-byte buffer, from the pool when a buffer is
 * free. '*buf' receives that buffer with a reference the caller releases; the
 * sample also carries a reference the caller releases once it is written.
 */
static IMFSample *PoolTake(DWORD key, IMFMediaBuffer **buf)
{
    IMFSample        *sample = NULL;
    IMFTrackedSample *tracked = NULL;
    IMFSample        *drop[POOL_MAX];
    int               nDrop = 0, i;
    BOOL              fresh = FALSE;

    *buf = NULL;

    AcquireSRWLockExclusive(&g_pool.lock);
    if (g_pool.key != key) {
        /* The frame size never changes within a recording, but should it,
           buffers of the old size are of no use. */
        for (i = 0; i < g_pool.freeCount; i++)
            drop[nDrop++] = g_pool.free[i];
        g_pool.total    -= g_pool.freeCount;
        g_pool.freeCount = 0;
        g_pool.key       = key;
    }
    if (g_pool.freeCount > 0) {
        sample = g_pool.free[--g_pool.freeCount];
        g_pool.reused++;
    } else if (g_pool.open && g_pool.total < POOL_MAX) {
        g_pool.total++;
        fresh = TRUE;
    }
    ReleaseSRWLockExclusive(&g_pool.lock);
    for (i = 0; i < nDrop; i++)
        IMFSample_Release(drop[i]);

    if (fresh) {
        IMFMediaBuffer *mb = NULL;
        if (SUCCEEDED(MFCreateTrackedSample(&tracked)) &&
            SUCCEEDED(IMFTrackedSample_QueryInterface(tracked, &IID_IMFSample,
                                                      (void **)&sample)) &&
            SUCCEEDED(MakeBuffer(key, &mb)) &&
            SUCCEEDED(IMFSample_AddBuffer(sample, mb)) &&
            SUCCEEDED(IMFSample_SetUINT32(sample, &POOL_GENERATION, g_pool.generation))) {
            /* kept: the sample now holds the buffer */
        } else if (sample) {
            IMFSample_Release(sample);
            sample = NULL;
        }
        if (mb)
            IMFMediaBuffer_Release(mb);
        if (tracked) {
            IMFTrackedSample_Release(tracked);
            tracked = NULL;
        }
        if (!sample) {
            AcquireSRWLockExclusive(&g_pool.lock);
            g_pool.total--;
            ReleaseSRWLockExclusive(&g_pool.lock);
        }
    }

    if (sample) {
        /* Arm the return for this trip; it is one-shot. */
        if (SUCCEEDED(IMFSample_QueryInterface(sample, &IID_IMFTrackedSample,
                                               (void **)&tracked))) {
            if (SUCCEEDED(IMFTrackedSample_SetAllocator(tracked, &g_poolCb, NULL)))
                InterlockedIncrement(&g_pool.outstanding);
            IMFTrackedSample_Release(tracked);
        }
        if (FAILED(IMFSample_GetBufferByIndex(sample, 0, buf))) {
            IMFSample_Release(sample);
            return NULL;
        }
        return sample;
    }

    /* Every pooled buffer is still with the writer: a one-off, as before. */
    g_pool.misses++;
    if (FAILED(MFCreateSample(&sample)))
        return NULL;
    if (FAILED(MakeBuffer(key, buf)) ||
        FAILED(IMFSample_AddBuffer(sample, *buf))) {
        if (*buf) {
            IMFMediaBuffer_Release(*buf);
            *buf = NULL;
        }
        IMFSample_Release(sample);
        return NULL;
    }
    return sample;
}

static IMFSample *PoolGet(DWORD size, IMFMediaBuffer **buf)
{
    return PoolTake(size, buf);
}

/* A pooled texture sample; '*tex' is the texture inside it, borrowed. */
static IMFSample *PoolGetTex(DXGI_FORMAT fmt, IMFMediaBuffer **buf,
                             ID3D11Texture2D **tex, DWORD *length)
{
    IMFSample   *sample = PoolTake(POOL_TEX | (DWORD)fmt, buf);
    IMFDXGIBuffer *dx = NULL;
    IMF2DBuffer   *b2 = NULL;

    *tex = NULL;
    *length = 0;
    if (!sample)
        return NULL;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(*buf, &IID_IMFDXGIBuffer, (void **)&dx))) {
        IMFDXGIBuffer_GetResource(dx, &IID_ID3D11Texture2D, (void **)tex);
        IMFDXGIBuffer_Release(dx);
    }
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(*buf, &IID_IMF2DBuffer, (void **)&b2))) {
        IMF2DBuffer_GetContiguousLength(b2, length);
        IMF2DBuffer_Release(b2);
    }
    if (!*tex) {
        IMFMediaBuffer_Release(*buf);
        *buf = NULL;
        IMFSample_Release(sample);
        return NULL;
    }
    /* GetResource added a reference; the buffer keeps the texture alive for
       as long as the sample is ours, so the caller borrows it. */
    ID3D11Texture2D_Release(*tex);
    return sample;
}

/* ------------------------------------------------------------- the samples */

/* Times a filled sample and hands it to the writer. */
static void SubmitVideo(IMFSample *sample, IMFMediaBuffer *buf, DWORD size, LONGLONG when)
{
    IMFMediaBuffer_SetCurrentLength(buf, size);
    IMFSample_SetSampleTime(sample, when);
    IMFSample_SetSampleDuration(sample, 10000000LL / g_rec.opt.fps);

    EnterCriticalSection(&g_rec.writeLock);
    IMFSinkWriter_WriteSample(g_rec.writer, g_rec.videoStream, sample);
    LeaveCriticalSection(&g_rec.writeLock);
}

/* SDR: the staging texture's rows, stripped of their padding. */
static void WriteRgb32Sample(const BYTE *bgra, UINT32 stride, LONGLONG when)
{
    IMFMediaBuffer *buf = NULL;
    DWORD           size = g_rec.width * 4 * g_rec.height;
    BYTE           *dst = NULL;
    IMFSample      *sample = PoolGet(size, &buf);

    if (!sample)
        return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        UINT32 y;
        for (y = 0; y < g_rec.height; y++)
            memcpy(dst + (SIZE_T)y * g_rec.width * 4,
                   bgra + (SIZE_T)y * stride, (SIZE_T)g_rec.width * 4);
        IMFMediaBuffer_Unlock(buf);
        SubmitVideo(sample, buf, size, when);
    }
    IMFMediaBuffer_Release(buf);
    IMFSample_Release(sample);
}

/* An HDR desktop recorded as 8-bit: float in, tone-mapped on the GPU, RGB32
   out, so the recording matches what the screen actually looked like. */
static void WriteToneMappedSample(LONGLONG when)
{
    IMFMediaBuffer *buf = NULL;
    DWORD           size = g_rec.width * 4 * g_rec.height;
    BYTE           *dst = NULL;
    IMFSample      *sample = PoolGet(size, &buf);

    if (!sample)
        return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        BOOL ok = HdrVideo_ConvertToBgra(g_rec.hv, dst, g_rec.width * 4);
        IMFMediaBuffer_Unlock(buf);
        if (ok)
            SubmitVideo(sample, buf, size, when);
    }
    IMFMediaBuffer_Release(buf);
    IMFSample_Release(sample);
}

/* HDR: the conversion writes P010 straight into the encoder's buffer. */
static void WriteP010Sample(LONGLONG when)
{
    IMFMediaBuffer *buf = NULL;
    DWORD           size = g_rec.width * g_rec.height * 3;
    BYTE           *dst = NULL;
    IMFSample      *sample = PoolGet(size, &buf);

    if (!sample)
        return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        BOOL ok = HdrVideo_ConvertToP010(g_rec.hv, dst);
        IMFMediaBuffer_Unlock(buf);
        if (ok)
            SubmitVideo(sample, buf, size, when);
    }
    IMFMediaBuffer_Release(buf);
    IMFSample_Release(sample);
}

/*
 * The zero-copy path: the frame goes into a pooled GPU texture and from there
 * straight to the encoder. On the 8-bit paths that texture is BGRA and Media
 * Foundation's video processor converts it to NV12 on the GPU; on the HDR path
 * our own shaders render P010 into it. Nothing crosses back to system memory.
 */
static void WriteGpuSample(LONGLONG when)
{
    IMFMediaBuffer  *buf = NULL;
    ID3D11Texture2D *tex = NULL;
    DWORD            length = 0;
    BOOL             ok = FALSE;
    IMFSample       *sample = PoolGetTex(g_rec.hdr ? DXGI_FORMAT_P010
                                                   : DXGI_FORMAT_B8G8R8A8_UNORM,
                                         &buf, &tex, &length);
    if (!sample)
        return;

    ID3D11Multithread_Enter(g_rec.mt);
    if (g_rec.hdr) {
        ok = HdrVideo_RenderP010Into(g_rec.hv, tex);
    } else if (g_rec.floatCapture) {
        ID3D11Texture2D *src = HdrVideo_RenderBgra(g_rec.hv);
        if (src) {
            ID3D11DeviceContext_CopyResource(g_rec.ctx, (ID3D11Resource *)tex,
                                             (ID3D11Resource *)src);
            ok = TRUE;
        }
    } else if (g_rec.frameTex) {
        ID3D11DeviceContext_CopyResource(g_rec.ctx, (ID3D11Resource *)tex,
                                         (ID3D11Resource *)g_rec.frameTex);
        ok = TRUE;
    }
    ID3D11Multithread_Leave(g_rec.mt);

    if (ok)
        SubmitVideo(sample, buf, length, when);
    IMFMediaBuffer_Release(buf);
    IMFSample_Release(sample);
}

static void TeardownGpu(void)
{
    if (g_rec.frameTex) { ID3D11Texture2D_Release(g_rec.frameTex);    g_rec.frameTex = NULL; }
    if (g_rec.mgr)      { IMFDXGIDeviceManager_Release(g_rec.mgr);    g_rec.mgr      = NULL; }
    if (g_rec.mt)       { ID3D11Multithread_Release(g_rec.mt);        g_rec.mt       = NULL; }
    g_rec.gpu = FALSE;
}

/* Everything the zero-copy path needs beyond the device itself. FALSE leaves
   the recorder exactly as the CPU path expects it. */
static BOOL SetupGpu(void)
{
    if (!g_rec.devVideo || !g_rec.cfg.recordZeroCopy)
        return FALSE;
    /* Windows' software encoder takes frames in system memory. */
    if (Encoder_Caps()->softwareOnly[g_rec.enc])
        return FALSE;

    if (FAILED(ID3D11DeviceContext_QueryInterface(g_rec.ctx, &IID_ID3D11Multithread,
                                                  (void **)&g_rec.mt)))
        goto fail;
    /* Media Foundation calls into this device from its own threads. */
    ID3D11Multithread_SetMultithreadProtected(g_rec.mt, TRUE);

    if (FAILED(MFCreateDXGIDeviceManager(&g_rec.mgrToken, &g_rec.mgr)) ||
        FAILED(IMFDXGIDeviceManager_ResetDevice(g_rec.mgr, (IUnknown *)g_rec.dev,
                                                g_rec.mgrToken)))
        goto fail;

    if (!g_rec.floatCapture) {
        D3D11_TEXTURE2D_DESC d;
        ZeroMemory(&d, sizeof(d));
        d.Width            = g_rec.width;
        d.Height           = g_rec.height;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        if (FAILED(ID3D11Device_CreateTexture2D(g_rec.dev, &d, NULL, &g_rec.frameTex)))
            goto fail;
    }
    g_rec.gpu = TRUE;
    return TRUE;

fail:
    TeardownGpu();
    return FALSE;
}

/* ------------------------------------------------- the card's own encoder */

/* The vendor of the adapter the capture device is on - which is the one
   whose encoder can read the capture's textures. */
static const HwEncOps *PickHwEncoder(void)
{
    IDXGIDevice  *dxgi = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC d;
    UINT          vendor = 0;

    if (SUCCEEDED(ID3D11Device_QueryInterface(g_rec.dev, &IID_IDXGIDevice, (void **)&dxgi))) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &ad))) {
            if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d)))
                vendor = d.VendorId;
            IDXGIAdapter_Release(ad);
        }
        IDXGIDevice_Release(dxgi);
    }
    switch (vendor) {
    case 0x10DE: return &g_nvencOps;            /* NVIDIA */
    case 0x1002: return &g_amfOps;              /* AMD    */
    case 0x8086: return &g_qsvOps;              /* Intel  */
    default:
        Log_Printf(L"record: no direct encoder for this graphics card (vendor %04X)", vendor);
        return NULL;
    }
}

static DWORD WINAPI DestroyHwThread(void *hw)
{
    g_rec.hwOps->destroy(hw);
    return 0;
}

static void TeardownHwEncoder(void)
{
    if (g_rec.hw && g_rec.hwFailed) {
        /* A runtime that reported its device failed has been seen not to
           return from closing (oneVPL on an Arc 140V). Closing it is given
           a few seconds; past that it is left behind - a leak, not a
           recorder that never finishes. */
        HANDLE th;
        Crash_Step(L"record: closing the failed encoder");
        th = CreateThread(NULL, 0, DestroyHwThread, g_rec.hw, 0, NULL);
        if (th) {
            if (WaitForSingleObject(th, 3000) != WAIT_OBJECT_0)
                Log_Printf(L"record: %s did not close within 3 s, left behind",
                           g_rec.hwOps->name);
            CloseHandle(th);
        }
        g_rec.hw = NULL;
    }
    if (g_rec.hw)    { g_rec.hwOps->destroy(g_rec.hw);         g_rec.hw    = NULL; }
    if (g_rec.hwTex) { ID3D11Texture2D_Release(g_rec.hwTex); g_rec.hwTex = NULL; }
    g_rec.avOffset = 0;
}

/*
 * The direct path, where it can be had: a card whose vendor's encoder API is
 * supported, a recent enough driver, the setting on, and the codec already
 * chosen offered there too. It takes the frames where the zero-copy path has
 * them - the BGRA capture (converted as BT.709, which is what the stream
 * says), the tone-mapped BGRA of an HDR desktop, or P010 rendered into a
 * texture of its own - so nothing new crosses to system memory. FALSE leaves
 * the recorder as the Media Foundation paths expect it.
 */
static BOOL SetupHwEncoder(void)
{
    HwEncInfo   info;
    HwEncConfig c;
    wchar_t     why[160];
    BOOL        hevc = g_rec.enc != ENC_H264;
    int         k = hevc ? 1 : 0;

    if (!g_rec.cfg.recordDirect) {
        Log_Printf(L"record: the card's own encoder is switched off in the settings");
        return FALSE;
    }
    g_rec.hwOps = PickHwEncoder();
    if (!g_rec.hwOps || !g_rec.hwOps->query(g_rec.dev, NULL, &info))
        return FALSE;                       /* the reason is in the log */
    if ((g_rec.enc == ENC_H264 && !info.caps.h264) ||
        (g_rec.enc == ENC_HEVC8 && !info.caps.hevc) ||
        (g_rec.enc == ENC_HEVC10 && (!info.caps.hevc10 || !g_rec.hdr))) {
        Log_Printf(L"record: %s does not offer %s here", g_rec.hwOps->name,
                   Encoder_Name(g_rec.enc));
        return FALSE;
    }

    /* The device is used from here and, for the sound, from capture threads
       that never touch it - but the lock is what the copies take. */
    if (!g_rec.mt) {
        if (FAILED(ID3D11DeviceContext_QueryInterface(g_rec.ctx, &IID_ID3D11Multithread,
                                                      (void **)&g_rec.mt)))
            return FALSE;
        ID3D11Multithread_SetMultithreadProtected(g_rec.mt, TRUE);
    }
    if (!g_rec.floatCapture && !g_rec.frameTex) {
        D3D11_TEXTURE2D_DESC d;
        ZeroMemory(&d, sizeof(d));
        d.Width = g_rec.width; d.Height = g_rec.height;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        /* A render target, as the video processor's input views are made
           on (the Intel path converts it to NV12 that way). */
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(g_rec.dev, &d, NULL, &g_rec.frameTex)))
            return FALSE;
    }
    if (g_rec.hdr) {
        D3D11_TEXTURE2D_DESC d;
        ZeroMemory(&d, sizeof(d));
        d.Width = g_rec.width; d.Height = g_rec.height;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_P010;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        /* Rendered into plane by plane, then copied by the encoder. */
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_rec.dev, &d, NULL, &g_rec.hwTex)))
            return FALSE;
    }

    ZeroMemory(&c, sizeof(c));
    c.hevc      = hevc;
    c.tenBit    = g_rec.enc == ENC_HEVC10;
    c.hdr       = c.tenBit;
    c.input     = c.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_B8G8R8A8_UNORM;
    c.width     = g_rec.width;
    c.height    = g_rec.height;
    c.fps       = g_rec.opt.fps;
    c.bitrate   = Encoder_Bitrate(g_rec.enc, g_rec.width, g_rec.height, g_rec.opt.fps);
    c.preset    = g_rec.cfg.recordHwPreset;
    c.bFrames   = g_rec.cfg.recordHwBFrames < 0 ? (hevc ? 3 : 2)
                                                  : g_rec.cfg.recordHwBFrames;
    if (c.bFrames > info.caps.maxB[k])
        c.bFrames = info.caps.maxB[k];
    c.lookahead = info.caps.lookahead[k] ? g_rec.cfg.recordHwLookahead : 0;
    c.aq        = g_rec.cfg.recordHwAq;
    c.videoMemory = g_rec.cfg.recordIntelVideoMemory;
    if (c.hdr) {
        c.redX   = g_rec.colour.redX;   c.redY   = g_rec.colour.redY;
        c.greenX = g_rec.colour.greenX; c.greenY = g_rec.colour.greenY;
        c.blueX  = g_rec.colour.blueX;  c.blueY  = g_rec.colour.blueY;
        c.whiteX = g_rec.colour.whiteX; c.whiteY = g_rec.colour.whiteY;
        c.maxNits = g_rec.colour.maxMasteringNits;
        c.minNits = g_rec.colour.minMasteringNits;
    }
    g_rec.hw = g_rec.hwOps->create(g_rec.dev, &c, why, ARRAYSIZE(why));
    if (!g_rec.hw) {
        TeardownHwEncoder();
        return FALSE;
    }
    g_rec.avOffset = g_rec.hwOps->offset(g_rec.hw);

    /* Media Foundation now only muxes; its device manager and the CPU
       readback texture have nothing left to do. */
    if (g_rec.mgr) {
        IMFDXGIDeviceManager_Release(g_rec.mgr);
        g_rec.mgr = NULL;
    }
    g_rec.gpu = FALSE;
    if (g_rec.staging) {
        ID3D11Texture2D_Release(g_rec.staging);
        g_rec.staging = NULL;
    }
    return TRUE;
}

/* The video stream when the card's encoder encodes: compressed in,
   compressed out, so the writer only muxes, with the encoder's parameter
   sets for the sample entry. */
static BOOL AddHwStream(void)
{
    IMFMediaType *t = NULL;
    const BYTE   *seq;
    UINT32        seqLen;
    BOOL          ok = FALSE;

    if (!g_rec.hwOps->header(g_rec.hw, &seq, &seqLen) || FAILED(MFCreateMediaType(&t)))
        return FALSE;
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, g_rec.enc == ENC_H264 ? &MFVideoFormat_H264
                                                                 : &MFVideoFormat_HEVC);
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT32(t, &MF_MT_AVG_BITRATE,
                           Encoder_Bitrate(g_rec.enc, g_rec.width, g_rec.height, g_rec.opt.fps));
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, ((UINT64)g_rec.width << 32) | g_rec.height);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, ((UINT64)(UINT32)g_rec.opt.fps << 32) | 1);
    IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    IMFMediaType_SetBlob(t, &MF_MT_MPEG_SEQUENCE_HEADER, seq, seqLen);
    if (SUCCEEDED(IMFSinkWriter_AddStream(g_rec.writer, t, &g_rec.videoStream)) &&
        SUCCEEDED(IMFSinkWriter_SetInputMediaType(g_rec.writer, g_rec.videoStream, t, NULL)))
        ok = TRUE;
    IMFMediaType_Release(t);
    return ok;
}

/* One encoded picture, in decode order, to the writer. */
static void OnHwPacket(void *ctx, const BYTE *data, UINT32 size, LONGLONG pts,
                          LONGLONG dts, BOOL keyFrame)
{
    IMFMediaBuffer *buf = NULL;
    IMFSample      *sample = NULL;
    BYTE           *dst = NULL;

    UNREFERENCED_PARAMETER(ctx);
    if (!g_rec.writer || size == 0 || FAILED(MFCreateMemoryBuffer(size, &buf)))
        return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        memcpy(dst, data, size);
        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_SetCurrentLength(buf, size);
        if (SUCCEEDED(MFCreateSample(&sample))) {
            IMFSample_AddBuffer(sample, buf);
            IMFSample_SetSampleTime(sample, pts);
            IMFSample_SetSampleDuration(sample, 10000000LL / g_rec.opt.fps);
            /* Without it the sink takes decode order for display order, and
               every B-frame lands in the wrong place. */
            IMFSample_SetUINT64(sample, &MFSampleExtension_DecodeTimestamp, (UINT64)dts);
            if (keyFrame)
                IMFSample_SetUINT32(sample, &MFSampleExtension_CleanPoint, TRUE);
            g_rec.hwPackets++;
            Crash_Step(L"record: writing an encoded picture to the MP4");
            EnterCriticalSection(&g_rec.writeLock);
            IMFSinkWriter_WriteSample(g_rec.writer, g_rec.videoStream, sample);
            LeaveCriticalSection(&g_rec.writeLock);
            IMFSample_Release(sample);
        }
    }
    IMFMediaBuffer_Release(buf);
}

/* The frame in the format the encoder was set up for, then into it. */
volatile LONG g_recordFailDirectAfter = -1;

static void WriteHwFrame(LONGLONG when)
{
    ID3D11Texture2D *src = NULL;

    Crash_Step(L"record: preparing the frame for the card's encoder");
    ID3D11Multithread_Enter(g_rec.mt);
    if (g_rec.hdr)
        src = HdrVideo_RenderP010Into(g_rec.hv, g_rec.hwTex) ? g_rec.hwTex : NULL;
    else if (g_rec.floatCapture)
        src = HdrVideo_RenderBgra(g_rec.hv);
    else
        src = g_rec.frameTex;
    ID3D11Multithread_Leave(g_rec.mt);
    Crash_Step(L"record: handing the frame to the card's encoder");
    if (src && !g_rec.hwOps->encode(g_rec.hw, src, when, OnHwPacket, NULL))
        g_rec.hwFailed = TRUE;
    if (g_recordFailDirectAfter >= 0 && g_rec.videoFrames > g_recordFailDirectAfter)
        g_rec.hwFailed = TRUE;
    if (g_rec.videoFrames <= 3) {
        Log_Printf(L"record: frame %u through %s done", (unsigned)g_rec.videoFrames,
                   g_rec.hwOps->name);
        Crash_Reassert(L"by the card's encoder at work");
    }
    Crash_Step(L"record: between frames");
}

/* One PCM block into an AAC stream, at a point on that stream's timeline. */
static void WriteAac(DWORD stream, const BYTE *pcm16, UINT32 frames,
                     UINT32 channels, UINT32 rate, LONGLONG at)
{
    IMFMediaBuffer *buf = NULL;
    IMFSample      *sample = NULL;
    DWORD           size = frames * channels * 2;
    BYTE           *dst = NULL;

    if (size == 0 || FAILED(MFCreateMemoryBuffer(size, &buf)))
        return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        if (pcm16)
            memcpy(dst, pcm16, size);
        else
            ZeroMemory(dst, size);
        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_SetCurrentLength(buf, size);

        if (SUCCEEDED(MFCreateSample(&sample))) {
            IMFSample_AddBuffer(sample, buf);
            IMFSample_SetSampleTime(sample, at * 10000000LL / rate + g_rec.avOffset);
            IMFSample_SetSampleDuration(sample, (LONGLONG)frames * 10000000LL / rate);
            IMFSinkWriter_WriteSample(g_rec.writer, stream, sample);
            IMFSample_Release(sample);
        }
    }
    IMFMediaBuffer_Release(buf);
}

/*
 * Mixing two sources.
 *
 * Each source adds its samples into a ring at its own timeline position, and
 * the mix is handed on only up to where *both* have arrived - the sources
 * deliver in packets of their own size and at their own moments, and summing
 * them as they come would put one source's sound against the wrong instant of
 * the other's. A source that goes quiet is padded with silence like any other
 * (PadAudioSilence), so the mix never waits on it for long.
 *
 * Plain summation, clamped. Attenuating each source by half would keep the sum
 * from ever clipping, but a voice over quiet game sound would then come out at
 * half its level for nothing; real clipping needs both loud at once.
 */
static void MixEmit(LONGLONG upTo)
{
    UINT32 ch   = g_rec.src[0].channels;
    UINT32 rate = g_rec.src[0].rate;

    while (g_rec.mixOut < upTo) {
        LONGLONG at  = g_rec.mixOut;
        UINT32   idx = (UINT32)(at % MIX_RING);
        UINT32   n   = (UINT32)(upTo - at > MIX_CHUNK ? MIX_CHUNK : upTo - at);
        UINT32   i;

        if (n > MIX_RING - idx)
            n = MIX_RING - idx;              /* up to the end of the ring */
        for (i = 0; i < n * ch; ++i) {
            INT32 v = g_rec.mixRing[idx * ch + i];
            g_rec.mixScratch[i] = (INT16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            g_rec.mixRing[idx * ch + i] = 0;
        }
        if (g_rec.aacCount > 0)
            WriteAac(g_rec.aac[0], (const BYTE *)g_rec.mixScratch, n, ch, rate, at);
        if (g_rec.mixWav)
            Wav_Write(g_rec.mixWav, (const BYTE *)g_rec.mixScratch, n);
        g_rec.mixOut += n;
    }
}

static void MixAdd(const RecSource *s, const BYTE *pcm16, UINT32 frames)
{
    const INT16 *pcm = (const INT16 *)pcm16;
    LONGLONG     at  = s->frames;
    UINT32       ch  = s->channels, f, c;

    /* The other source is a whole ring behind without having been padded -
       stalled rather than quiet. What has to go goes, its share silent. */
    if (at + frames - g_rec.mixOut > MIX_RING)
        MixEmit(at + frames - MIX_RING);
    if (!pcm)
        return;                              /* silence adds nothing */
    for (f = 0; f < frames; ++f) {
        INT32 *slot;
        if (at + f < g_rec.mixOut)
            continue;                        /* emitted already: too late */
        slot = &g_rec.mixRing[(UINT32)((at + f) % MIX_RING) * ch];
        for (c = 0; c < ch; ++c)
            slot[c] += pcm[f * ch + c];
    }
}

/*
 * Write one block of source 'k' at the current point on its timeline: to its
 * own WAV, and to its AAC track or into the mix. Called with writeLock held.
 */
static void WriteSource(int k, const BYTE *pcm16, UINT32 frames)
{
    RecSource *s = &g_rec.src[k];

    if (frames == 0)
        return;
    /* Fed from here rather than from OnAudio so the sidecar gets the padding
       too and stays aligned with the video timeline. */
    if (s->wav)
        Wav_Write(s->wav, pcm16, frames);
    if (g_rec.mix)
        MixAdd(s, pcm16, frames);
    else if (k < g_rec.aacCount)
        WriteAac(g_rec.aac[k], pcm16, frames, s->channels, s->rate, s->frames);
    s->frames += frames;
    if (g_rec.mix)
        MixEmit(g_rec.src[0].frames < g_rec.src[1].frames ? g_rec.src[0].frames
                                                          : g_rec.src[1].frames);
}

/*
 * A loopback capture delivers nothing at all while the machine is silent - not
 * zeroed packets, no packets. Left alone that produces a recording with an
 * empty audio track, or one whose sound jumps whenever playback paused. The
 * video loop therefore tops each source's timeline up with real silence
 * whenever it falls behind the elapsed time.
 */
static void PadAudioSilence(void)
{
    ULONGLONG now = GetTickCount64();
    int       k;

    if (!g_rec.haveAudio)
        return;

    EnterCriticalSection(&g_rec.writeLock);
    for (k = 0; k < g_rec.srcCount; ++k) {
        RecSource *s = &g_rec.src[k];
        LONGLONG   expected, behind;

        /*
         * Only when the source has genuinely gone quiet. Comparing the audio
         * timeline against the clock is not enough on its own: capture always
         * runs a little behind the clock, so a threshold small enough to catch
         * real gaps also fires constantly during normal playback - and every
         * spurious pad inserts silence in the middle of continuous sound and
         * pushes the samples already in flight to a later timestamp. That was
         * audible.
         */
        if (s->rate == 0 || now - s->lastTick < QUIET_BEFORE_PAD_MS)
            continue;
        /* Never fill the most recent stretch: samples for it may still be on
           their way from the audio engine. */
        expected = (ElapsedNs100() - PAD_LAG_MS * 10000LL) * s->rate / 10000000LL;
        behind   = expected - s->frames;
        while (behind > 0) {
            UINT32 chunk = (UINT32)(behind > MIX_CHUNK ? MIX_CHUNK : behind);
            WriteSource(k, NULL, chunk);
            behind -= chunk;
        }
    }
    LeaveCriticalSection(&g_rec.writeLock);
}

/*
 * Audio is timestamped from its own sample count rather than the wall clock.
 * The endpoint's clock and QPC drift apart slightly, and a timeline built from
 * the samples actually captured is the one that plays back without gaps.
 * 'ctx' is the source's index.
 */
static void OnAudio(void *ctx, const BYTE *pcm16, UINT32 frames)
{
    int k = (int)(INT_PTR)ctx;

    if (!g_rec.haveAudio || !Record_IsActive() || k < 0 || k >= g_rec.srcCount)
        return;

    EnterCriticalSection(&g_rec.writeLock);
    g_rec.src[k].lastTick = GetTickCount64();
    WriteSource(k, pcm16, frames);
    LeaveCriticalSection(&g_rec.writeLock);
}

/* ---------------------------------------------------------------- the loop */

/* Pull the newest frame, if any, into the staging texture. */
static BOOL GrabFrame(void)
{
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource   *res = NULL;
    ID3D11Texture2D *tex = NULL;
    D3D11_BOX        box;
    HRESULT          hr;
    BOOL             got = FALSE;

    hr = IDXGIOutputDuplication_AcquireNextFrame(g_rec.dup, 0, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        g_rec.grabTimeouts++;
        return FALSE;
    }
    if (FAILED(hr)) {
        /* The mode changed or another process took exclusive control; a fresh
           duplication object is the documented recovery. */
        g_rec.grabFailures++;
        IDXGIOutputDuplication_Release(g_rec.dup);
        g_rec.dup = NULL;
        /*
         * A lost *device* is a different matter: every texture and shader the
         * loop writes through died with it, and rebuilding all of that mid-
         * recording would change the encoder's input behind its back. Stop
         * instead, so what has been captured so far is finalised and playable.
         */
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            Log_Printf(L"record: graphics device lost 0x%08lX, stopping",
                       (unsigned long)hr);
            g_rec.deviceLost = TRUE;
        }
        return FALSE;
    }

    /*
     * A frame that only reports the pointer moving carries no new image, and
     * on a duplication that has not presented yet its surface is empty -
     * copying it would start the recording on black frames. The copy of the
     * last real image is still in the capture texture, so there is nothing to
     * do but let this one go.
     */
    if (info.LastPresentTime.QuadPart == 0) {
        g_rec.grabPointerOnly++;
        IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(g_rec.dup);
        return FALSE;
    }

    if (SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D,
                                               (void **)&tex))) {
        ID3D11Texture2D *dest = g_rec.floatCapture       ? HdrVideo_Input(g_rec.hv)
                              : g_rec.gpu || g_rec.hw    ? g_rec.frameTex
                                                         : g_rec.staging;
        if (dest) {
            box.left   = (UINT)(g_rec.out.left   - g_rec.outOrigin.x);
            box.top    = (UINT)(g_rec.out.top    - g_rec.outOrigin.y);
            box.right  = box.left + g_rec.width;
            box.bottom = box.top  + g_rec.height;
            box.front  = 0;
            box.back   = 1;
            ID3D11DeviceContext_CopySubresourceRegion(g_rec.ctx,
                (ID3D11Resource *)dest, 0, 0, 0, 0,
                (ID3D11Resource *)tex, 0, &box);
            got = TRUE;
            g_rec.grabCopies++;
        }
        ID3D11Texture2D_Release(tex);
    }
    IDXGIResource_Release(res);
    IDXGIOutputDuplication_ReleaseFrame(g_rec.dup);
    return got;
}

/* Stops every capture. Must not be called with writeLock held: a capture
   thread may be waiting for it, and stopping waits for that thread. */
static void StopAudio(void)
{
    int k;
    for (k = 0; k < RECORD_SOURCES; ++k) {
        Audio_Stop(g_rec.src[k].cap);
        g_rec.src[k].cap = NULL;
    }
}

static void CloseWav(WavSink **wav, int slot, BOOL keep)
{
    if (!*wav)
        return;
    if (!Wav_Close(*wav) && slot >= 0)
        keep = FALSE;
    *wav = NULL;
    if (!keep && slot >= 0) {
        DeleteFileW(g_rec.result->wavPath[slot]);
        g_rec.result->wavPath[slot][0] = 0;
    }
}

static void CloseWavs(BOOL keep)
{
    int k;
    for (k = 0; k < RECORD_SOURCES; ++k)
        CloseWav(&g_rec.src[k].wav, g_rec.src[k].wavSlot, keep);
    CloseWav(&g_rec.mixWav, g_rec.mixWavSlot, keep);
}

/* "<video><suffix>" as the next sidecar; the slot it took, or -1. */
static int OpenWav(const wchar_t *suffix, UINT32 rate, UINT32 channels, WavSink **out)
{
    int      slot = g_rec.result->wavCount;
    wchar_t *path;

    *out = NULL;
    if (slot >= RECORD_MAX_WAVS)
        return -1;
    path = g_rec.result->wavPath[slot];
    if (FAILED(StringCchCopyW(path, MAX_PATH, g_rec.result->path)))
        return -1;
    PathRemoveExtensionW(path);
    if (FAILED(StringCchCatW(path, MAX_PATH, suffix)) ||
        !(*out = Wav_Create(path, rate, channels))) {
        path[0] = 0;
        return -1;
    }
    g_rec.result->wavCount++;
    return slot;
}

static void SoundWarning(const wchar_t *name)
{
    wchar_t line[160];
    wchar_t *w = g_rec.result->warning;

    Lang_Format(line, ARRAYSIZE(line), STR_WARN_AUDIO_MISSING, name);
    if (w[0])
        StringCchCatW(w, ARRAYSIZE(g_rec.result->warning), L"\n");
    StringCchCatW(w, ARRAYSIZE(g_rec.result->warning), line);
}

/*
 * Opens the sound sources, their sidecars and the MP4's audio tracks. Nothing
 * here fails the recording: whatever cannot be had is logged, and a missing
 * device is named in the notification, but the video is recorded regardless.
 */
static void StartSound(void)
{
    BOOL needWavs;
    int  k, tracks;

    g_rec.mixWavSlot = -1;
    for (k = 0; k < RECORD_SOURCES; ++k) {
        const wchar_t *spec = g_rec.opt.audioDev[k];
        RecSource     *s    = &g_rec.src[g_rec.srcCount];
        AudioFormat    fmt;

        if (Audio_IsNone(spec))
            continue;
        /* The same device twice would only record the same sound twice. */
        if (k > 0 && lstrcmpiW(spec, g_rec.opt.audioDev[0]) == 0) {
            Log_Printf(L"record: second source is the first one again, ignored");
            continue;
        }
        ZeroMemory(&fmt, sizeof(fmt));
        s->wavSlot = -1;
        s->cap = Audio_Start(spec, &fmt, OnAudio, (void *)(INT_PTR)g_rec.srcCount);
        if (!s->cap) {
            Log_Printf(L"record: sound source %d could not be opened", k + 1);
            SoundWarning(g_rec.opt.audioName[k][0] ? g_rec.opt.audioName[k] : spec);
            continue;
        }
        s->rate     = fmt.sampleRate;
        s->channels = fmt.channels;
        StringCchCopyW(s->name, ARRAYSIZE(s->name), Audio_Name(s->cap));
        g_rec.srcCount++;
    }
    if (g_rec.srcCount == 0)
        return;

    /* Every device normally arrives as 48 kHz stereo; only one that refused
       the conversion can differ, and two different formats cannot be summed. */
    g_rec.mix = g_rec.opt.mixAudio && g_rec.srcCount == 2;
    if (g_rec.mix && (g_rec.src[0].rate != g_rec.src[1].rate ||
                      g_rec.src[0].channels != g_rec.src[1].channels)) {
        Log_Printf(L"record: the sources' formats differ, recording two tracks "
                   L"instead of mixing");
        g_rec.mix = FALSE;
    }
    if (g_rec.mix) {
        g_rec.mixRing = (INT32 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            (SIZE_T)MIX_RING * g_rec.src[0].channels * sizeof(INT32));
        if (!g_rec.mixRing)
            g_rec.mix = FALSE;
    }

    if (g_rec.cfg.recordWavSidecar) {
        if (g_rec.srcCount == 1) {
            g_rec.src[0].wavSlot = OpenWav(L".wav", g_rec.src[0].rate,
                                           g_rec.src[0].channels, &g_rec.src[0].wav);
        } else {
            g_rec.src[0].wavSlot = OpenWav(L".track1.wav", g_rec.src[0].rate,
                                           g_rec.src[0].channels, &g_rec.src[0].wav);
            g_rec.src[1].wavSlot = OpenWav(L".track2.wav", g_rec.src[1].rate,
                                           g_rec.src[1].channels, &g_rec.src[1].wav);
            if (g_rec.mix)
                g_rec.mixWavSlot = OpenWav(L".track0.wav", g_rec.src[0].rate,
                                           g_rec.src[0].channels, &g_rec.mixWav);
        }
    }

    /*
     * With the sidecars as the source of the MP4's sound there is no AAC
     * stream in the file while recording - the samples go to the WAVs only,
     * and the encoding happens once, afterwards. Nothing then has to keep up
     * with the capture clock. That needs every WAV a track is built from.
     */
    needWavs = g_rec.mix ? g_rec.mixWav != NULL
                         : g_rec.src[0].wav && (g_rec.srcCount < 2 || g_rec.src[1].wav);
    g_rec.muxFromWav = g_rec.cfg.recordWavSidecar && g_rec.cfg.recordMuxFromWav;
    if (g_rec.muxFromWav && !needWavs) {
        Log_Printf(L"record: no sidecar, encoding AAC live instead");
        g_rec.muxFromWav = FALSE;
    }
    if (!g_rec.muxFromWav) {
        tracks = g_rec.mix ? 1 : g_rec.srcCount;
        for (k = 0; k < tracks; ++k) {
            if (!AddAudioStream(g_rec.src[k].rate, g_rec.src[k].channels, &g_rec.aac[k])) {
                Log_Printf(L"record: no AAC encoder for track %d", k + 1);
                break;
            }
            g_rec.aacCount++;
        }
    }

    g_rec.haveAudio = g_rec.muxFromWav || g_rec.aacCount > 0 ||
                      g_rec.src[0].wav || g_rec.src[1].wav;
    if (!g_rec.haveAudio) {
        Log_Printf(L"record: nowhere to put the sound, continuing silent");
        StopAudio();
    }
    Log_Printf(L"record: %d sound source(s)%s, %d live track(s)%s",
               g_rec.srcCount, g_rec.mix ? L" mixed" : L"", g_rec.aacCount,
               g_rec.muxFromWav ? L", built from the WAVs afterwards" : L"");
}

/*
 * After the file is final: build its sound from the WAVs if that was asked
 * for, and name its audio tracks after their devices. Both before the colour
 * box, since the remux writes a whole new file.
 */
static void FinishSound(void)
{
    const wchar_t *names[RECORD_SOURCES];
    wchar_t        mixName[2 * AUDIO_NAME_CCH + 4];
    int            tracks = g_rec.aacCount, k;

    if (g_rec.muxFromWav) {
        const wchar_t *wavs[RECORD_SOURCES];
        int            n = 0;
        BOOL           all = TRUE;

        if (g_rec.mix) {
            all = g_rec.mixWavSlot >= 0 && g_rec.result->wavPath[g_rec.mixWavSlot][0];
            if (all)
                wavs[n++] = g_rec.result->wavPath[g_rec.mixWavSlot];
        } else {
            for (k = 0; k < g_rec.srcCount; ++k) {
                int slot = g_rec.src[k].wavSlot;
                if (slot < 0 || !g_rec.result->wavPath[slot][0])
                    all = FALSE;
                else
                    wavs[n++] = g_rec.result->wavPath[slot];
            }
        }
        if (all && n > 0 && Remux_AddWavAudio(g_rec.result->path, wavs, n, g_rec.avOffset)) {
            /*
             * In this mode the WAVs were only ever the way the sound reached
             * the encoder. Once it is in the MP4, keeping them as well just
             * leaves a second copy of the same audio in the folder. Mixed,
             * the sources' own WAVs hold something the MP4 does not - each
             * source on its own - and may be asked to stay.
             */
            BOOL keepSources = g_rec.mix && g_rec.cfg.recordKeepSourceWavs;
            for (k = 0; k < RECORD_MAX_WAVS; ++k) {
                wchar_t *p = g_rec.result->wavPath[k];
                if (!p[0] || (keepSources && k != g_rec.mixWavSlot))
                    continue;
                if (DeleteFileW(p))
                    p[0] = 0;
                else
                    Log_Printf(L"record: a sidecar could not be removed (%lu)",
                               GetLastError());
            }
            tracks = n;
        } else {
            /* Keeping them is the whole reason the recording still has its
               sound at all. */
            Log_Printf(L"record: the sound could not be muxed in; the video "
                       L"is silent and the sound is in the .wav");
            tracks = 0;
        }
    }

    if (tracks <= 0)
        return;
    if (g_rec.mix) {
        StringCchPrintfW(mixName, ARRAYSIZE(mixName), L"%s + %s",
                         g_rec.src[0].name, g_rec.src[1].name);
        names[0] = mixName;
    } else {
        for (k = 0; k < tracks; ++k)
            names[k] = g_rec.src[k].name;
    }
    Mp4Tag_NameAudioTracks(g_rec.result->path, names, tracks);
}


static unsigned __stdcall RecordThread(void *arg)
{
    int         k;
    LONGLONG    frameIndex = 0;
    DWORD       intervalMs;
    BOOL        haveFrame = FALSE;

    UNREFERENCED_PARAMETER(arg);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    PoolOpen();

    intervalMs = (DWORD)(1000 / g_rec.opt.fps);

    if (!ChooseEncoder())         { Fail(Lang_Str(STR_ERR_NO_ENCODER)); goto done; }
    if (!OpenOutput(FALSE))       { Fail(Lang_Str(STR_ERR_NO_DISPLAY)); goto done; }

    /* Every codec here wants even dimensions, and 4:2:0 chroma has no meaning
       without them. */
    g_rec.width  = (UINT32)(g_rec.out.right - g_rec.out.left) & ~1u;
    g_rec.height = (UINT32)(g_rec.out.bottom - g_rec.out.top) & ~1u;
    if (g_rec.width < 16 || g_rec.height < 16) { Fail(Lang_Str(STR_ERR_AREA_SMALL)); goto done; }

    /*
     * OpenOutput may have had to fall back to 8-bit duplication, in which case
     * the 10-bit encoder chosen a moment ago no longer matches what is being
     * captured and has to be reconsidered.
     */
    if (!g_rec.hdr && Encoder_IsHdr(g_rec.enc)) {
        EncoderId next = Encoder_Resolve(Encoder_FromToken(g_rec.cfg.recordEncoder), FALSE);
        if (next >= ENC_COUNT || Encoder_IsHdr(next))
            next = Encoder_Fallback(g_rec.enc);
        if (next >= ENC_COUNT)    { Fail(Lang_Str(STR_ERR_NO_ENCODER)); goto done; }
        Log_Printf(L"record: no float duplication, falling back to %s",
                   Encoder_Name(next));
        g_rec.enc = next;
    }

    if (g_rec.floatCapture) {
        g_rec.hv = HdrVideo_Create(g_rec.dev, g_rec.ctx, g_rec.width, g_rec.height);
        if (!g_rec.hv) { Fail(Lang_Str(STR_ERR_HDR_SETUP)); goto done; }
        /* Only the 8-bit path uses these, but setting them is free. */
        HdrVideo_SetSdrWhite(g_rec.hv, g_rec.sdrWhiteNits, g_rec.cfg.hdrSdrRollOff);
        if (!g_rec.hdr)
            Log_Printf(L"record: float capture, tone-mapped to 8-bit at "
                       L"%.0f nits white%s", g_rec.sdrWhiteNits,
                       g_rec.cfg.hdrSdrRollOff ? L", highlights rolled off" : L"");
    } else if (!MakeStaging()) {
        Fail(Lang_Str(STR_ERR_CAPTURE_BUF));
        goto done;
    }

    /* The CPU readback texture is dead weight - 33 MB at 4K - once frames
       stay on the GPU; the fallback below makes another if it is needed. */
    if (SetupGpu() && g_rec.staging) {
        ID3D11Texture2D_Release(g_rec.staging);
        g_rec.staging = NULL;
    }

    if (SetupHwEncoder()) {
        if (!OpenWriter())        { Fail(Lang_Str(STR_ERR_FILE_CREATE)); goto done; }
        if (!AddHwStream()) {
            /* Encoded fine, but the writer will not take the stream: record
               through Media Foundation instead, on the CPU path. */
            Log_Printf(L"record: the MP4 writer refused %s's stream, using %s",
                       g_rec.hwOps->name, Encoder_Name(g_rec.enc));
            IMFSinkWriter_Release(g_rec.writer);
            g_rec.writer = NULL;
            DeleteFileW(g_rec.result->path);
            TeardownHwEncoder();
            if (!g_rec.floatCapture && !g_rec.staging && !MakeStaging()) {
                Fail(Lang_Str(STR_ERR_CAPTURE_BUF));
                goto done;
            }
        }
    }
    if (!g_rec.hw && g_rec.direct) {
        Log_Printf(L"record: the card's own encoder is not available either");
        Fail(Lang_Str(STR_ERR_NO_ENCODER));
        goto done;
    }
    if (g_rec.hw) {
        Log_Printf(L"record: %s path, sound %lld ms later to match the picture",
                   g_rec.hwOps->name, g_rec.avOffset / 10000);
    } else {
    if (!OpenWriter())            { Fail(Lang_Str(STR_ERR_FILE_CREATE)); goto done; }
    if (!Encoder_AddVideoStream(g_rec.writer, g_rec.enc, g_rec.width,
                                g_rec.height, g_rec.opt.fps,
                                &g_rec.videoStream)) {
        if (!g_rec.gpu) {
            Fail(Lang_Str(STR_ERR_ENC_FORMAT));
            goto done;
        }
        /* An encoder that will not take GPU textures is not a reason to fail
           the recording: the CPU path has always worked with it. */
        Log_Printf(L"record: %s refused GPU frames, using the CPU path",
                   Encoder_Name(g_rec.enc));
        IMFSinkWriter_Release(g_rec.writer);
        g_rec.writer = NULL;
        DeleteFileW(g_rec.result->path);
        TeardownGpu();
        if (!g_rec.floatCapture && !g_rec.staging && !MakeStaging()) {
            Fail(Lang_Str(STR_ERR_CAPTURE_BUF));
            goto done;
        }
        if (!OpenWriter())        { Fail(Lang_Str(STR_ERR_FILE_CREATE)); goto done; }
        if (!Encoder_AddVideoStream(g_rec.writer, g_rec.enc, g_rec.width,
                                    g_rec.height, g_rec.opt.fps,
                                    &g_rec.videoStream)) {
            Fail(Lang_Str(STR_ERR_ENC_FORMAT));
            goto done;
        }
    }
    Log_Printf(L"record: %s path", g_rec.gpu ? L"zero-copy GPU" : L"CPU");
    }

    StartSound();

    if (FAILED(IMFSinkWriter_BeginWriting(g_rec.writer))) {
        Fail(Lang_Str(STR_ERR_ENC_START));
        goto done;
    }

    QueryPerformanceFrequency(&g_rec.qpcFreq);
    QueryPerformanceCounter(&g_rec.qpcStart);
    for (k = 0; k < g_rec.srcCount; ++k)
        g_rec.src[k].lastTick = GetTickCount64();
    if (g_rec.hw) {
        Settings_MarkDirectRecording(g_rec.hwOps->name);
        Crash_Reassert(L"while the card's encoder was set up");
        Crash_WatchBegin(10000);
    }
    g_rec.hdrLocked     = g_rec.hdr;
    InterlockedExchange(&g_rec.active, 1);

    for (;;) {
        LONGLONG due;
        LONGLONG now;

        Crash_Beat();

        if (g_rec.dup) {
            /* The encoder works this device from its own threads; the copy
               out of the duplication must not interleave with its calls. */
            if (g_rec.mt)
                ID3D11Multithread_Enter(g_rec.mt);
            if (GrabFrame())
                haveFrame = TRUE;
            if (g_rec.mt)
                ID3D11Multithread_Leave(g_rec.mt);
        }

        /*
         * Every tick writes a frame, repeating the last one when nothing
         * changed. A still desktop produces no duplication frames at all, and
         * a video whose timeline simply stops for those seconds plays back
         * wrongly in most players.
         */
        if (haveFrame) {
            LONGLONG      when = frameIndex * 10000000LL / g_rec.opt.fps;
            LARGE_INTEGER b0, b1;
            QueryPerformanceCounter(&b0);
            g_rec.videoFrames++;
            if (g_rec.hw) {
                WriteHwFrame(when);
            } else if (g_rec.gpu) {
                WriteGpuSample(when);
            } else if (g_rec.hdr) {
                WriteP010Sample(when);
            } else if (g_rec.floatCapture) {
                WriteToneMappedSample(when);
            } else {
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(ID3D11DeviceContext_Map(g_rec.ctx,
                        (ID3D11Resource *)g_rec.staging, 0, D3D11_MAP_READ, 0,
                        &map))) {
                    WriteRgb32Sample((const BYTE *)map.pData, map.RowPitch, when);
                    ID3D11DeviceContext_Unmap(g_rec.ctx,
                                              (ID3D11Resource *)g_rec.staging, 0);
                }
            }
            if (g_rec.hdr) {
                double peak = 0.0, mean = 0.0;
                /* MaxCLL is the brightest pixel anywhere in the recording,
                   MaxFALL the brightest frame average - both maxima, so they
                   only ever climb. */
                HdrVideo_LastFrameLight(g_rec.hv, &peak, &mean);
                if (peak > g_rec.colour.maxCLL)
                    g_rec.colour.maxCLL = (float)peak;
                if (mean > g_rec.colour.maxFALL)
                    g_rec.colour.maxFALL = (float)mean;
            }
            QueryPerformanceCounter(&b1);
            g_rec.buildTicks += b1.QuadPart - b0.QuadPart;
        }
        frameIndex++;
        PadAudioSilence();

        /* Pace against the start, so a slow tick does not push every later
           frame late and stretch the recording. */
        due = frameIndex * 1000LL / g_rec.opt.fps;
        now = ElapsedNs100() / 10000;
        {
            DWORD waitMs = due > now ? (DWORD)(due - now) : 0;
            if (waitMs > intervalMs * 4)
                waitMs = intervalMs * 4;
            if (WaitForSingleObject(g_rec.stopEvent, waitMs) == WAIT_OBJECT_0)
                break;
        }

        if (g_rec.deviceLost)
            break;
        /* The encoder cannot be swapped for another mid-file; what was
           encoded until now is finalised and kept. */
        if (g_rec.hwFailed && g_rec.hwPackets == 0) {
            Log_Printf(L"record: %s failed before its first picture; "
                       L"recording again through Windows' encoder", g_rec.hwOps->name);
            g_rec.result->retryWithoutDirect = TRUE;
            StringCchCopyW(g_rec.result->directName, ARRAYSIZE(g_rec.result->directName),
                           g_rec.hwOps->name);
            break;
        }
        if (g_rec.hwFailed) {
            Log_Printf(L"record: %s stopped encoding, ending the recording",
                       g_rec.hwOps->name);
            {
                wchar_t line[160], *w = g_rec.result->warning;
                Lang_Format(line, ARRAYSIZE(line), STR_WARN_HW_STOPPED);
                if (w[0])
                    StringCchCatW(w, ARRAYSIZE(g_rec.result->warning), L"\n");
                StringCchCatW(w, ARRAYSIZE(g_rec.result->warning), line);
            }
            break;
        }

        if (!g_rec.dup) {
            /* Rebuild after a lost duplication, on the same device so the
               staging texture and the HDR resources stay valid; the file
               keeps running meanwhile on the last frame. */
            if (!OpenOutput(TRUE))
                Sleep(intervalMs);
            else
                Log_Printf(L"record: duplication rebuilt");
        }
    }

    if ((g_rec.gpu || g_rec.hw) && g_rec.hdr && g_rec.hv) {
        double peak = 0.0, mean = 0.0;
        ID3D11Multithread_Enter(g_rec.mt);
        HdrVideo_DrainLight(g_rec.hv);
        HdrVideo_LastFrameLight(g_rec.hv, &peak, &mean);
        ID3D11Multithread_Leave(g_rec.mt);
        if (peak > g_rec.colour.maxCLL)
            g_rec.colour.maxCLL = (float)peak;
        if (mean > g_rec.colour.maxFALL)
            g_rec.colour.maxFALL = (float)mean;
    }

    /* What the encoder still holds for its lookahead and reordering. */
    if (g_rec.hw && !g_rec.hwFailed && !g_rec.hwOps->flush(g_rec.hw, OnHwPacket, NULL))
        Log_Printf(L"record: %s could not be drained", g_rec.hwOps->name);

    g_rec.result->seconds = Record_ElapsedMs() / 1000;
    InterlockedExchange(&g_rec.active, 0);

    StopAudio();
    EnterCriticalSection(&g_rec.writeLock);
    /* The rest of the longer source, against silence from the other. */
    if (g_rec.mix)
        MixEmit(g_rec.src[0].frames > g_rec.src[1].frames ? g_rec.src[0].frames
                                                          : g_rec.src[1].frames);
    {
        /* Worth naming: MF_E_SINK_NO_SAMPLES_PROCESSED (0xC00D4A44) here means
           the duplication never handed over a frame, which looks nothing like
           an encoder problem from the outside. */
        HRESULT hrFin;

        if (g_rec.videoFrames > 0 && g_rec.qpcFreq.QuadPart > 0)
            Log_Printf(L"record: %ux%u, %.2f ms per frame to build the sample",
                       g_rec.width, g_rec.height,
                       (double)g_rec.buildTicks * 1000.0 /
                       (double)g_rec.qpcFreq.QuadPart / (double)g_rec.videoFrames);
        Log_Printf(L"record: %lld frame(s) written; grabs: %lld copied, "
                   L"%lld timed out, %lld pointer-only, %lld failed",
                   (long long)g_rec.videoFrames, (long long)g_rec.grabCopies,
                   (long long)g_rec.grabTimeouts,
                   (long long)g_rec.grabPointerOnly,
                   (long long)g_rec.grabFailures);
        hrFin = IMFSinkWriter_Finalize(g_rec.writer);
        g_rec.result->ok = SUCCEEDED(hrFin);
        if (FAILED(hrFin))
            Log_Printf(L"record: finalise failed 0x%08lX after %lld frame(s)",
                       (unsigned long)hrFin, (long long)g_rec.videoFrames);
    }
    CloseWavs(TRUE);
    LeaveCriticalSection(&g_rec.writeLock);
    if (!g_rec.result->ok) {
        Fail(Lang_Str(STR_ERR_FINALISE));
    } else {
        FinishSound();

        if (g_rec.hdr) {
            /* 9 = BT.2020 primaries, 16 = SMPTE ST 2084, 9 = BT.2020 NCL, and
               the range is limited because that is what the encoder was given. */
            g_rec.colour.primaries = 9;
            g_rec.colour.transfer  = 16;
            g_rec.colour.matrix    = 9;
            g_rec.colour.fullRange = FALSE;
            Mp4Tag_WriteColour(g_rec.result->path, &g_rec.colour);
        }
    }

done:
    InterlockedExchange(&g_rec.active, 0);
    StopAudio();
    /* Only reached with sinks still open when setup failed part-way. */
    CloseWavs(TRUE);
    if (g_rec.mixRing) {
        HeapFree(GetProcessHeap(), 0, g_rec.mixRing);
        g_rec.mixRing = NULL;
    }

    if (g_rec.writer)  { IMFSinkWriter_Release(g_rec.writer);          g_rec.writer  = NULL; }
    PoolClose();       /* after the writer: releasing it is what returns the samples */
    if (g_rec.hv)      { HdrVideo_Destroy(g_rec.hv);                   g_rec.hv      = NULL; }
    if (g_rec.staging) { ID3D11Texture2D_Release(g_rec.staging);       g_rec.staging = NULL; }
    {
        /* Closing the encoder is still the vendor's code: the mark and the
           watch stay until it has returned. */
        BOOL direct = g_rec.hw != NULL;
        TeardownHwEncoder();  /* holds the device and its textures too */
        if (direct) {
            Crash_WatchEnd();
            Settings_MarkDirectRecording(NULL);  /* ended, not died */
        }
    }
    TeardownGpu();     /* the manager holds the device; it goes before it */
    if (g_rec.dup)     { IDXGIOutputDuplication_Release(g_rec.dup);    g_rec.dup     = NULL; }
    if (g_rec.ctx)     { ID3D11DeviceContext_Release(g_rec.ctx);       g_rec.ctx     = NULL; }
    if (g_rec.dev)     { ID3D11Device_Release(g_rec.dev);              g_rec.dev     = NULL; }

    MFShutdown();
    CoUninitialize();

    {
        RecordResult *res = g_rec.result;
        g_rec.result = NULL;
        if (res && !res->ok) {               /* a broken stub helps nobody */
            if (res->path[0])
                DeleteFileW(res->path);
            for (k = 0; k < RECORD_MAX_WAVS; ++k)
                if (res->wavPath[k][0])
                    DeleteFileW(res->wavPath[k]);
        }
        if (res) {
            /* Close up the list, so the first wavCount entries are the files
               that exist. */
            int n = 0;
            for (k = 0; k < RECORD_MAX_WAVS; ++k)
                if (res->wavPath[k][0] && res->ok) {
                    if (k != n)
                        StringCchCopyW(res->wavPath[n], MAX_PATH, res->wavPath[k]);
                    ++n;
                }
            for (k = n; k < RECORD_MAX_WAVS; ++k)
                res->wavPath[k][0] = 0;
            res->wavCount = n;
        }
        if (res && !PostMessageW(g_rec.notify, g_rec.doneMsg, 0, (LPARAM)res))
            Record_FreeResult(res);
    }

    /* Last of all: from here on the UI may start a new recording, which
       zeroes everything above. Nothing may touch g_rec after this line. */
    InterlockedExchange(&g_rec.busy, 0);
    return 0;
}

/* --------------------------------------------------------------- the entry */

/* Hands back what Record_Start created once the worker is known to be gone. */
static void ReleaseThreadObjects(void)
{
    if (g_rec.thread) {
        CloseHandle(g_rec.thread);
        g_rec.thread = NULL;
    }
    if (g_rec.stopEvent) {
        CloseHandle(g_rec.stopEvent);
        g_rec.stopEvent = NULL;
        DeleteCriticalSection(&g_rec.writeLock);
    }
}

BOOL Record_Start(HWND notify, UINT doneMsg, const RecordOptions *opt)
{
    uintptr_t th;

    if (!opt || Record_IsBusy())
        return FALSE;

    /* A previous stop that timed out left these behind on purpose. The
       worker has since finished - busy says so - so they can go now. */
    ReleaseThreadObjects();

    ZeroMemory(&g_rec, sizeof(g_rec));
    g_rec.cfg     = g_cfg;
    g_rec.opt     = *opt;
    g_rec.notify  = notify;
    g_rec.doneMsg = doneMsg;
    if (g_rec.opt.fps < 5)  g_rec.opt.fps = 5;
    if (g_rec.opt.fps > 60) g_rec.opt.fps = 60;

    g_rec.result = (RecordResult *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                             sizeof(RecordResult));
    if (!g_rec.result)
        return FALSE;

    InitializeCriticalSection(&g_rec.writeLock);
    g_rec.stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_rec.stopEvent) {
        DeleteCriticalSection(&g_rec.writeLock);
        Record_FreeResult(g_rec.result);
        g_rec.result = NULL;
        return FALSE;
    }

    /* Before the thread exists, because it is what keeps a second start out
       while this one sets itself up. The worker clears it, and nothing else. */
    InterlockedExchange(&g_rec.busy, 1);

    th = _beginthreadex(NULL, 0, RecordThread, NULL, 0, NULL);
    if (!th) {
        InterlockedExchange(&g_rec.busy, 0);
        CloseHandle(g_rec.stopEvent);
        g_rec.stopEvent = NULL;
        DeleteCriticalSection(&g_rec.writeLock);
        Record_FreeResult(g_rec.result);
        g_rec.result = NULL;
        return FALSE;
    }
    g_rec.thread = (HANDLE)th;
    return TRUE;
}

void Record_Stop(void)
{
    if (g_rec.stopEvent)
        SetEvent(g_rec.stopEvent);

    if (g_rec.thread) {
        /* Waiting keeps the "it is saved" message honest: MP4 finalisation
           writes the index, without which the file will not play. */
        if (WaitForSingleObject(g_rec.thread, STOP_WAIT_MS) != WAIT_OBJECT_0) {
            /*
             * Still working. Freeing the stop event and the lock now would
             * pull them out from under a thread that is about to use both,
             * so leave everything standing: the worker finishes the file on
             * its own and clears 'busy', and the next Record_Start cleans up
             * after it. Recording simply cannot be restarted until then.
             */
            Log_Printf(L"record: worker still finalising after %u ms, "
                       L"leaving it to finish", (unsigned)STOP_WAIT_MS);
            return;
        }
    }
    ReleaseThreadObjects();
}
