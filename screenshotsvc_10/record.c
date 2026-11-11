/*
 * record.c - the recording worker: duplication in, MP4 out.
 *
 * Two capture paths. The SDR one takes the tone-mapped 8-bit desktop from
 * plain DuplicateOutput and hands it to the encoder as RGB32, which is what it
 * has always done. The HDR one asks DuplicateOutput1 for scRGB half-float,
 * converts it to 10-bit BT.2020 PQ on the GPU (hdrvideo.c) and feeds P010 to
 * a 10-bit encoder. The codec/profile is resolved per recording from the
 * display state, the configured preference and the encoder probe results.
 */

#define COBJMACROS

#include "screenshotservice.h"
#include "record.h"
#include "recovery.h"
#include "settings.h"
#include "encoder.h"
#include "hwenc.h"
#include "nvenc.h"
#include "amfenc.h"
#include "qsvenc.h"
#include "crash.h"
#include "hdrvideo.h"
#include "mp4tag.h"
#include "wavsink.h"
#include "remux.h"
#include "rawpcm.h"
#include "rawmix.h"
#include "flacenc.h"
#include "mp4multi.h"
#include "gif.h"
#include "lang.h"
#include "hdr.h"
#include "log.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>      /* IDXGIOutput6, for the display's colour volume */
#include <d3d11_4.h>      /* ID3D11Multithread                             */
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmreg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <process.h>


/* How long the source must have been silent before the timeline is padded,
   and how much of the recent past is left alone while doing it. */
#define CRASH_FINALIZE_WAIT_MS (5u * 60u * 1000u)
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

typedef union {
    WAVEFORMATEXTENSIBLE extensible;
    BYTE bytes[256];
} RecordNativeFormat;

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
    HANDLE        resumeEvent;       /* signalled while the timeline may advance */
    HANDLE        frameTimer;        /* high-resolution timer for CFR pacing */
    volatile LONG paused;
    volatile LONG64 pausedTicks;     /* accumulated QPC ticks excluded from output */
    volatile LONG64 pauseStartQpc;
    volatile LONG active;        /* frames are being captured right now      */
    /*
     * True from Record_Start until the worker's very last instruction.
     * 'active' is false during setup and again during finalisation, and a
     * second Record_Start in either window would zero this struct under a
     * live thread - so starting and stopping go by this one instead.
     */
    volatile LONG busy;
    volatile LONG displayModeChanged;
    volatile LONG saveProgress;
    DWORD           workerThreadId;
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
    UINT                    mgrToken;
    /* Native hardware encoder; Media Foundation remains the fallback. */
    const HwEncOps         *hwOps;
    void                   *hw;
    ID3D11Texture2D        *hwTex;
    LONGLONG                avOffset;
    BOOL                    hwFailed;
    BOOL                    hwMarked;     /* persistent marker set for crash recovery */
    LONGLONG                hwPackets;
    ID3D11Multithread      *mt;
    ID3D11Texture2D        *frameTex;    /* SDR capture: the current frame   */
    HdrVideo               *hv;          /* HDR path or SDR10 source converter */
    HdrVideo               *sdrhv;       /* float HDR capture -> SDR P010 helper */
    POINT                   outOrigin;   /* current output top-left in screen coordinates */
    POINT                   sourceOrigin;/* output top-left when this recording began */
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
    BOOL                    sdr10;       /* Main10 SDR, Rec.709 in P010        */
    DXGI_FORMAT             captureFormat; /* selected Duplication source format */
    BOOL                    hdrLocked;   /* ...and the file already says so   */
    float                   sdrWhiteNits;
    /* What the file has to say about itself afterwards: the display it was
       made on, and how bright the frames actually turned out. */
    Mp4ColourInfo           colour;

    /* encoding */
    IMFSinkWriter *writer;
    wchar_t        finalPath[MAX_PATH]; /* published only after clean finalisation */
    UINT32         sourceModeWidth, sourceModeHeight; /* original canvas mapping */
    UINT32         activeModeWidth, activeModeHeight;
    DXGI_FORMAT    activeModeFormat;
    EncoderId      enc;
    DWORD          videoStream, audioStream;
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
    LONGLONG       audioFrames;          /* the audio timeline, in samples */
    ULONGLONG      lastAudioTick;        /* when real audio last arrived    */
    UINT32         audioRate, audioChannels;
    BOOL           haveAudio;
    WavSink       *wav;                  /* the lossless sidecar, or NULL   */
    BOOL           muxFromWav;           /* no live AAC; build it afterwards */

    /* Optional three-track mode. Source packets are written untouched to raw
       files while recording; FLAC encoding and MP4 muxing happen afterwards. */
    BOOL                 multiTrack;
    volatile LONG        rawCaptureFailed;
    RawPcmSink          *rawPcm[3];
    RecordNativeFormat   rawFormat[3];
    UINT32               rawFormatBytes[3];
    UINT32               rawPreferredFlacBits[3];
    UINT32               rawRate[3];
    UINT32               rawBlockAlign[3];
    UINT64               rawOrigin100ns;
    UINT64               rawRecordStart100ns;
    UINT64               rawRecordStop100ns;
    wchar_t              rawPath[3][MAX_PATH];

    RecordResult  *result;
} Recorder;

static Recorder g_rec;
static volatile LONG g_crashHandling;

typedef struct { int base; int span; } RecordProgressRange;

static void Record_PostProgress(int percent)
{
    LONG old;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    old = InterlockedExchange(&g_rec.saveProgress, percent);
    if (percent != old && g_rec.notify)
        PostMessageW(g_rec.notify, WM_BSNIP_RECORD_PROGRESS, (WPARAM)percent, 0);
}

static void Record_GifProgress(int percent, void *context)
{
    UNREFERENCED_PARAMETER(context);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    Record_PostProgress(25 + percent * 70 / 100);
}

static void Record_MappedProgress(void *context, UINT64 done, UINT64 total)
{
    RecordProgressRange *range = (RecordProgressRange *)context;
    UINT64 scaled;
    if (!range || !total) return;
    if (done > total) done = total;
    scaled = (done * (UINT64)range->span) / total;
    Record_PostProgress(range->base + (int)scaled);
}

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

void Record_RequestStop(void)
{
    if (g_rec.stopEvent)
        SetEvent(g_rec.stopEvent);
}

void Record_NotifyDisplayChange(void)
{
    /* Signal the worker; it alone releases/reopens DXGI duplication objects. */
    if (Record_IsBusy())
        InterlockedExchange(&g_rec.displayModeChanged, 1);
}

void Record_HandleCrash(void)
{
    HANDLE thread;
    DWORD workerId;
    if (InterlockedCompareExchange(&g_crashHandling, 1, 0) != 0)
        return;
    workerId = g_rec.workerThreadId;
    thread = g_rec.thread;
    /* An exception on the recording worker itself cannot safely ask that same
       thread to finalize; the .partial file and RAW sidecars are kept instead. */
    if (thread && workerId && GetCurrentThreadId() != workerId && Record_IsBusy()) {
        OutputDebugStringW(L"ScreenshotService: unhandled exception; requesting recording finalisation.\n");
        Record_RequestStop();
        (void)WaitForSingleObject(thread, CRASH_FINALIZE_WAIT_MS);
    }
}

BOOL Record_IsPaused(void)
{
    return InterlockedCompareExchange(&g_rec.paused, 0, 0) != 0;
}

/* QPC ticks spent paused, including the currently open pause interval. */
static LONGLONG PausedTicksNow(void)
{
    LONGLONG total = InterlockedCompareExchange64(&g_rec.pausedTicks, 0, 0);
    if (Record_IsPaused()) {
        LARGE_INTEGER now;
        LONGLONG start = InterlockedCompareExchange64(&g_rec.pauseStartQpc, 0, 0);
        QueryPerformanceCounter(&now);
        if (start > 0 && now.QuadPart > start)
            total += now.QuadPart - start;
    }
    return total;
}

static LONGLONG ActiveElapsedTicksNow(void)
{
    LARGE_INTEGER now;
    LONGLONG elapsed;
    if (g_rec.qpcFreq.QuadPart <= 0)
        return 0;
    QueryPerformanceCounter(&now);
    elapsed = now.QuadPart - g_rec.qpcStart.QuadPart - PausedTicksNow();
    return elapsed > 0 ? elapsed : 0;
}

DWORD Record_ElapsedMs(void)
{
    if (!Record_IsActive() || g_rec.qpcFreq.QuadPart == 0)
        return 0;
    return (DWORD)(ActiveElapsedTicksNow() * 1000 / g_rec.qpcFreq.QuadPart);
}

static LONGLONG ElapsedNs100(void)
{
    if (g_rec.qpcFreq.QuadPart <= 0)
        return 0;
    return ActiveElapsedTicksNow() * 10000000 / g_rec.qpcFreq.QuadPart;
}

static HANDLE CreateFrameTimer(void)
{
    typedef HANDLE (WINAPI *PFNCreateWaitableTimerExW)(LPSECURITY_ATTRIBUTES,
                                                       LPCWSTR, DWORD, DWORD);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    PFNCreateWaitableTimerExW createEx = kernel
        ? (PFNCreateWaitableTimerExW)(void *)GetProcAddress(kernel, "CreateWaitableTimerExW")
        : NULL;
    HANDLE timer = NULL;
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is 0x2 on supported Windows 10+.
       Resolve dynamically so older Windows builds still have the regular timer. */
    if (createEx)
        timer = createEx(NULL, NULL, 0x00000002u, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    return timer;
}

BOOL Record_TogglePause(void)
{
    LARGE_INTEGER now;
    if (!Record_IsActive() || !g_rec.resumeEvent)
        return Record_IsPaused();
    QueryPerformanceCounter(&now);
    if (!Record_IsPaused()) {
        ResetEvent(g_rec.resumeEvent);
        InterlockedExchange64(&g_rec.pauseStartQpc, now.QuadPart);
        InterlockedExchange(&g_rec.paused, 1);
        Log_Printf(L"record: paused at %lu ms", (unsigned long)Record_ElapsedMs());
    } else {
        LONGLONG start = InterlockedCompareExchange64(&g_rec.pauseStartQpc, 0, 0);
        LONGLONG total = InterlockedCompareExchange64(&g_rec.pausedTicks, 0, 0);
        if (start > 0 && now.QuadPart > start)
            InterlockedExchange64(&g_rec.pausedTicks, total + now.QuadPart - start);
        InterlockedExchange64(&g_rec.pauseStartQpc, 0);
        InterlockedExchange(&g_rec.paused, 0);
        SetEvent(g_rec.resumeEvent);
        Log_Printf(L"record: resumed");
    }
    return Record_IsPaused();
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
                if (!reuseDevice && g_rec.opt.syncToDisplay) {
                    DEVMODEW dm;
                    ZeroMemory(&dm, sizeof(dm));
                    dm.dmSize = sizeof(dm);
                    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) &&
                        dm.dmDisplayFrequency >= 5 && dm.dmDisplayFrequency <= 240) {
                        int refresh = (int)dm.dmDisplayFrequency;
                        /* Some drivers expose nominal 59 Hz for a 59.94-Hz mode. */
                        if (refresh == 59) refresh = 60;
                        g_rec.opt.fps = refresh;
                        Log_Printf(L"record: frame pacing matched to display refresh (%d fps)", refresh);
                    } else {
                        Log_Printf(L"record: display refresh query failed; using configured %d fps",
                                   g_rec.opt.fps);
                    }
                }

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
                } else if (g_rec.sdr10) {
                    IDXGIOutput5 *out5 = NULL;
                    if (SUCCEEDED(IDXGIOutput_QueryInterface(output,
                            &IID_IDXGIOutput5, (void **)&out5))) {
                        DXGI_FORMAT want[2] = {
                            DXGI_FORMAT_R10G10B10A2_UNORM,
                            DXGI_FORMAT_B8G8R8A8_UNORM
                        };
                        hrDup = IDXGIOutput5_DuplicateOutput1(out5,
                                    (IUnknown *)g_rec.dev, 0, ARRAYSIZE(want), want,
                                    &g_rec.dup);
                        if (FAILED(hrDup))
                            Log_Printf(L"record: DuplicateOutput1(R10/BGRA) failed "
                                       L"0x%08lX, trying legacy BGRA duplication",
                                       (unsigned long)hrDup);
                        IDXGIOutput5_Release(out5);
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
                    DXGI_OUTDUPL_DESC dupDesc;
                    ZeroMemory(&dupDesc, sizeof(dupDesc));
                    IDXGIOutputDuplication_GetDesc(g_rec.dup, &dupDesc);
                    if (!reuseDevice) {
                        /* These dimensions/coordinates define the video canvas
                           for the entire recording. Reopened duplications may
                           have different dimensions; frames will be rescaled. */
                        g_rec.sourceModeWidth = dupDesc.ModeDesc.Width;
                        g_rec.sourceModeHeight = dupDesc.ModeDesc.Height;
                        g_rec.activeModeWidth = dupDesc.ModeDesc.Width;
                        g_rec.activeModeHeight = dupDesc.ModeDesc.Height;
                        g_rec.activeModeFormat = dupDesc.ModeDesc.Format;
                        g_rec.captureFormat = dupDesc.ModeDesc.Format;
                        g_rec.sourceOrigin.x = od.DesktopCoordinates.left;
                        g_rec.sourceOrigin.y = od.DesktopCoordinates.top;
                    } else if ((g_rec.activeModeWidth &&
                                dupDesc.ModeDesc.Width != g_rec.activeModeWidth) ||
                               (g_rec.activeModeHeight &&
                                dupDesc.ModeDesc.Height != g_rec.activeModeHeight) ||
                               (g_rec.activeModeFormat != DXGI_FORMAT_UNKNOWN &&
                                dupDesc.ModeDesc.Format != g_rec.activeModeFormat)) {
                        Log_Printf(L"record: display mode changed %ux%u -> %ux%u (format %u -> %u); rescaling to fixed %ux%u canvas",
                                   g_rec.activeModeWidth, g_rec.activeModeHeight,
                                   dupDesc.ModeDesc.Width, dupDesc.ModeDesc.Height,
                                   (unsigned)g_rec.activeModeFormat,
                                   (unsigned)dupDesc.ModeDesc.Format,
                                   g_rec.width, g_rec.height);
                        g_rec.activeModeWidth = dupDesc.ModeDesc.Width;
                        g_rec.activeModeHeight = dupDesc.ModeDesc.Height;
                        g_rec.activeModeFormat = dupDesc.ModeDesc.Format;
                    }
                    if (g_rec.dup) {
                        g_rec.outOrigin.x = od.DesktopCoordinates.left;
                        g_rec.outOrigin.y = od.DesktopCoordinates.top;
                        if (!reuseDevice) {
                            g_rec.out = g_rec.opt.region;
                            IntersectRect(&g_rec.out, &g_rec.out, &od.DesktopCoordinates);
                        }
                        ok = TRUE;
                    }
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
    d.Format           = (g_rec.sdr10 &&
                          g_rec.captureFormat == DXGI_FORMAT_R10G10B10A2_UNORM)
                         ? DXGI_FORMAT_R10G10B10A2_UNORM
                         : DXGI_FORMAT_B8G8R8A8_UNORM;
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

    /* Select the encoder capability set for the requested GPU vendor before
       resolving Auto or an explicit codec profile. */
    Encoder_ProbeForVendor(g_rec.cfg.recordEncoderVendor, FALSE);

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
    if (g_rec.opt.makeGif) {
        /* GIF is SDR and silent; keep the intermediate MP4 decoder-friendly. */
        want = ENC_H264;
        g_rec.floatCapture = FALSE;
        displayHdr = FALSE;
    } else if (!g_rec.cfg.forceSdr && g_rec.outName[0] &&
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
    if (g_rec.enc >= ENC_COUNT) {
        Log_Printf(L"record: no usable Media Foundation hardware video encoder passed the actual MP4 probe");
        return FALSE;
    }
    g_rec.hdr = Encoder_IsHdr(g_rec.enc);
    g_rec.sdr10 = Encoder_Is10Bit(g_rec.enc) && !g_rec.hdr;

    Log_Printf(L"record: display is %s, encoder preference=%s, resolved=%s (%s)%s",
               displayHdr ? L"HDR" : L"SDR",
               want == ENC_AUTO ? L"Automatic" : Encoder_Name(want),
               Encoder_Name(g_rec.enc),
               Encoder_Caps()->mft[g_rec.enc][0]
                   ? Encoder_Caps()->mft[g_rec.enc] : L"unnamed",
               g_rec.opt.makeGif ? L" [GIF intermediate intentionally uses H.264]" : L"");
    return TRUE;
}

static BOOL AddAudioStream(UINT32 rate, UINT32 channels)
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
    g_rec.audioStream = index;

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
    if (FAILED(IMFSinkWriter_SetInputMediaType(g_rec.writer, g_rec.audioStream,
                                               inType, NULL)))
        goto done;
    ok = TRUE;

done:
    if (inType)  IMFMediaType_Release(inType);
    if (outType) IMFMediaType_Release(outType);
    return ok;
}

static BOOL RecordingSidecarsExist(const wchar_t *finalMp4)
{
    static const wchar_t *extensions[] = {
        L".partial.wav", L".wav",
        L".partial.mic.raw", L".partial.system.raw",
        L".partial.mic.flac", L".partial.system.flac"
    };
    wchar_t candidate[MAX_PATH];
    size_t i;

    for (i = 0; i < ARRAYSIZE(extensions); ++i) {
        if (FAILED(StringCchCopyW(candidate, ARRAYSIZE(candidate), finalMp4)) ||
            !PathRenameExtensionW(candidate, extensions[i]))
            return TRUE; /* fail closed: do not risk overwriting an artifact */
        if (PathFileExistsW(candidate))
            return TRUE;
    }
    return FALSE;
}

static BOOL PickPath(wchar_t *path, size_t cch)
{
    wchar_t    dir[MAX_PATH];
    SYSTEMTIME st;
    int        n;

    if (!Settings_ResolveFolderFrom(g_rec.cfg.videoFolder, TRUE, dir, ARRAYSIZE(dir))) {
        Log_Printf(L"record: PickPath failed: cannot resolve/create VideoFolder='%s' (empty setting defaults to C:\\Screenshots)",
                   g_rec.cfg.videoFolder[0] ? g_rec.cfg.videoFolder : L"(default C:\\Screenshots)");
        return FALSE;
    }
    Log_Printf(L"record: video output directory='%s' (configured VideoFolder='%s')",
               dir, g_rec.cfg.videoFolder[0] ? g_rec.cfg.videoFolder : L"(default)");

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
        if (FAILED(hr)) {
            Log_Printf(L"record: PickPath failed formatting output path (candidate=%d, HRESULT=0x%08lX, directory='%s')",
                       n, (unsigned long)hr, dir);
            return FALSE;
        }
        if (!PathFileExistsW(path) && !RecordingSidecarsExist(path)) {
            wchar_t partial[MAX_PATH];
            if (g_rec.opt.makeGif) {
                wchar_t gifPath[MAX_PATH], gifPartial[MAX_PATH];
                if (FAILED(StringCchCopyW(gifPath, ARRAYSIZE(gifPath), path)) ||
                    !PathRenameExtensionW(gifPath, L".gif"))
                    return FALSE;
                if (FAILED(StringCchCopyW(gifPartial, ARRAYSIZE(gifPartial), path)) ||
                    !PathRenameExtensionW(gifPartial, L".partial.gif"))
                    return FALSE;
                if (PathFileExistsW(gifPath) || PathFileExistsW(gifPartial))
                    continue;
            }
            if (FAILED(StringCchCopyW(partial, ARRAYSIZE(partial), path))) {
                Log_Printf(L"record: PickPath failed copying partial path from '%s' (path too long)", path);
                return FALSE;
            }
            if (!PathRenameExtensionW(partial, L".partial.mp4")) {
                Log_Printf(L"record: PickPath failed making partial MP4 path from '%s' (path may be too long)", path);
                continue;
            }
            if (!PathFileExistsW(partial)) {
                Log_Printf(L"record: selected output='%s'; partial='%s'", path, partial);
                return TRUE;
            }
        }
    }
    Log_Printf(L"record: PickPath failed: no available filename after 99 candidates in '%s'", dir);
    return FALSE;
}

static BOOL OpenWriter(void)
{
    IMFAttributes *attrs = NULL;
    BOOL ok = FALSE;
    HRESULT hr;

    if (!g_rec.finalPath[0]) {
        if (!PickPath(g_rec.finalPath, ARRAYSIZE(g_rec.finalPath))) {
            Log_Printf(L"record: OpenWriter stopped before Media Foundation because PickPath failed");
            return FALSE;
        }
        hr = StringCchCopyW(g_rec.result->path,
                            ARRAYSIZE(g_rec.result->path), g_rec.finalPath);
        if (FAILED(hr)) {
            Log_Printf(L"record: OpenWriter could not copy final path '%s' (HRESULT=0x%08lX)",
                       g_rec.finalPath, (unsigned long)hr);
            g_rec.result->path[0] = 0;
            g_rec.finalPath[0] = 0;
            return FALSE;
        }
        if (!PathRenameExtensionW(g_rec.result->path, L".partial.mp4")) {
            Log_Printf(L"record: OpenWriter could not form partial path from '%s' (path may be too long)",
                       g_rec.finalPath);
            g_rec.result->path[0] = 0;
            g_rec.finalPath[0] = 0;
            return FALSE;
        }
        Log_Printf(L"record: opening MP4 partial path='%s' final path='%s'",
                   g_rec.result->path, g_rec.finalPath);
    }

    hr = MFCreateAttributes(&attrs, 3);
    if (FAILED(hr)) {
        Log_Printf(L"record: MFCreateAttributes failed 0x%08lX for '%s'",
                   (unsigned long)hr, g_rec.result->path);
        return FALSE;
    }
    IMFAttributes_SetGUID(attrs, &MF_TRANSCODE_CONTAINERTYPE,
                          &MFTranscodeContainerType_MPEG4);
    IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    /* Without this the writer blocks the capture loop to pace the encoder,
       which for a live screen recording means dropped frames instead. */
    IMFAttributes_SetUINT32(attrs, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    /* With this the encoder - and on the 8-bit path the video processor in
       front of it, which converts RGB to NV12 - work on our textures on the
       GPU, instead of the frames making a round trip through system memory. */
    if (g_rec.gpu && g_rec.mgr)
        IMFAttributes_SetUnknown(attrs, &MF_SINK_WRITER_D3D_MANAGER, (IUnknown *)g_rec.mgr);

    hr = MFCreateSinkWriterFromURL(g_rec.result->path, NULL, attrs,
                                   &g_rec.writer);
    if (SUCCEEDED(hr)) {
        ok = TRUE;
        Log_Printf(L"record: Media Foundation Sink Writer created for '%s'",
                   g_rec.result->path);
    } else {
        Log_Printf(L"record: MFCreateSinkWriterFromURL failed 0x%08lX for '%s' (GetLastError=%lu)",
                   (unsigned long)hr, g_rec.result->path,
                   (unsigned long)GetLastError());
    }

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
    IMFSample      *sample;

    sample = PoolGet(size, &buf);
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

/* Main10 SDR: desktop RGB -> Rec.709 limited-range P010. For an SDR capture
   we render from BGRA8/R10G10B10A2 directly; on an HDR display we first use the
   existing tone-map shader and feed that BGRA result to the SDR converter. */
static void WriteSdrP010Sample(LONGLONG when)
{
    IMFMediaBuffer *buf = NULL;
    DWORD size = g_rec.width * g_rec.height * 3;
    BYTE *dst = NULL;
    IMFSample *sample = PoolGet(size, &buf);
    BOOL ok = FALSE;
    if (!sample) return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        HdrVideo *cv = g_rec.hv;
        ID3D11Texture2D *tone = NULL;
        if (g_rec.sdrhv) {
            tone = HdrVideo_RenderBgra(g_rec.hv);
            if (tone) {
                ID3D11DeviceContext_CopyResource(g_rec.ctx,
                    (ID3D11Resource *)HdrVideo_Input(g_rec.sdrhv),
                    (ID3D11Resource *)tone);
                cv = g_rec.sdrhv;
            }
        }
        if (cv && (!g_rec.sdrhv || tone))
            ok = HdrVideo_ConvertToP010(cv, dst);
        IMFMediaBuffer_Unlock(buf);
    }
    if (ok)
        SubmitVideo(sample, buf, size, when);
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
    if (!g_rec.devVideo || !g_rec.cfg.recordZeroCopy || g_rec.sdr10)
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
            if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d))) {
                vendor = d.VendorId;
                Log_Printf(L"record: capture adapter '%s' vendor=%04X device=%04X",
                           d.Description, (unsigned)d.VendorId, (unsigned)d.DeviceId);
            }
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
    if (g_rec.opt.makeGif ||
        (g_rec.enc != ENC_H264 && g_rec.enc != ENC_HEVC8 &&
         !(g_rec.enc == ENC_HEVC10 && g_rec.hdr))) {
        Log_Printf(L"record: native encoding is not used for this profile; keeping Media Foundation");
        return FALSE;
    }
    /* Offline WAV muxing and multitrack mode have their own audio timeline.
       Keep those features on the MFT path until their packet timestamps can
       be shifted by the native encoder's B-frame reorder delay too. */
    if ((g_rec.cfg.recordWavSidecar && g_rec.cfg.recordMuxFromWav) ||
        (g_rec.cfg.recordMultiTrack && g_rec.opt.audio == AUDIO_BOTH)) {
        Log_Printf(L"record: native encoding skipped for offline/multitrack audio");
        return FALSE;
    }
    g_rec.hwOps = PickHwEncoder();
    if (!g_rec.hwOps) return FALSE;
    if (!g_rec.hwOps->query(g_rec.dev, NULL, &info)) {
        Log_Printf(L"record: %s query failed; trying Media Foundation", g_rec.hwOps->name);
        g_rec.hwOps = NULL;
        return FALSE;
    }
    if (!info.available) {
        Log_Printf(L"record: %s unavailable: %s", g_rec.hwOps->name, info.why);
        g_rec.hwOps = NULL;
        return FALSE;
    }
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
    /* Persist the selected vendor before entering its runtime. If its first
       call corrupts the process or hangs, next startup can disable this path
       once and recover through MFT. The marker is cleared on ordinary cleanup. */
    Settings_MarkDirectRecording(g_rec.hwOps->name);
    g_rec.hwMarked = TRUE;
    Log_Printf(L"record: crash-recovery marker set before entering native encoder %s",
               g_rec.hwOps->name);
    Crash_Step(L"record: creating native encoder instance");
    Crash_Beat();
    g_rec.hw = g_rec.hwOps->create(g_rec.dev, &c, why, ARRAYSIZE(why));
    if (!g_rec.hw && c.lookahead > 0) {
        Log_Printf(L"record: %s refused preset/config with lookahead (%s); retrying without lookahead",
                   g_rec.hwOps->name, why[0] ? why : L"no details");
        c.lookahead = 0;
        g_rec.hw = g_rec.hwOps->create(g_rec.dev, &c, why, ARRAYSIZE(why));
    }
    if (!g_rec.hw && c.bFrames > 0) {
        Log_Printf(L"record: %s refused configuration with B-frames (%s); retrying without B-frames",
                   g_rec.hwOps->name, why[0] ? why : L"no details");
        c.bFrames = 0;
        why[0] = 0;
        g_rec.hw = g_rec.hwOps->create(g_rec.dev, &c, why, ARRAYSIZE(why));
    }
    /* AMF drivers vary more in which optional rate-control features they
       accept than the other native APIs. Keep the codec, bit depth and HDR
       transfer unchanged, but try without VBAQ, then step down one quality
       tier if the requested preset is still refused. Each create call builds
       a fresh AMF component/context encoder state. */
    if (!g_rec.hw && g_rec.hwOps == &g_amfOps && c.aq) {
        Log_Printf(L"record: AMF refused the requested configuration (%s); retrying with VBAQ disabled",
                   why[0] ? why : L"no details");
        c.aq = FALSE;
        why[0] = 0;
        g_rec.hw = g_rec.hwOps->create(g_rec.dev, &c, why, ARRAYSIZE(why));
    }
    if (!g_rec.hw && g_rec.hwOps == &g_amfOps && c.preset > 3) {
        int previousPreset = c.preset;
        c.preset = c.preset > 5 ? 5 : 3;
        Log_Printf(L"record: AMF refused preset %d (%s); retrying with simpler preset %d",
                   previousPreset, why[0] ? why : L"no details", c.preset);
        why[0] = 0;
        g_rec.hw = g_rec.hwOps->create(g_rec.dev, &c, why, ARRAYSIZE(why));
    }
    if (!g_rec.hw) {
        Log_Printf(L"record: %s initialization failed: %s; trying Media Foundation",
                   g_rec.hwOps->name, why[0] ? why : L"no details");
        TeardownHwEncoder();
        g_rec.hwOps = NULL;
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

/* A vendor-native encoder that cannot start or produce its first packet is
   retried once through the tested hardware MFT. The UI owns starting the
   replacement recording after this worker has completely detached. */
static BOOL ScheduleMftRetry(const wchar_t *reason)
{
    if (!g_rec.result || !g_rec.hw || !g_rec.hwOps || g_rec.hwPackets != 0)
        return FALSE;
    Log_Printf(L"record: %s failed before the first encoded packet; scheduling Media Foundation hardware MFT retry",
               g_rec.hwOps->name);
    if (reason && reason[0])
        Log_Printf(L"record: native retry reason: %s", reason);
    g_rec.result->retryWithoutDirect = TRUE;
    StringCchCopyW(g_rec.result->directName,
                   ARRAYSIZE(g_rec.result->directName), g_rec.hwOps->name);
    g_rec.result->retryOptions = g_rec.opt;
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
    if (!g_rec.writer || size == 0) {
        g_rec.hwFailed = TRUE;
        Log_Printf(L"record: native packet callback received an invalid writer or empty packet");
        return;
    }
    {
        HRESULT hr = MFCreateMemoryBuffer(size, &buf);
        if (FAILED(hr)) {
            g_rec.hwFailed = TRUE;
            Log_Printf(L"record: MFCreateMemoryBuffer(%lu) failed for native packet: 0x%08lX",
                       (unsigned long)size, (unsigned long)hr);
            return;
        }
    }
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
            Crash_Step(L"record: writing an encoded picture to the MP4");
            EnterCriticalSection(&g_rec.writeLock);
            {
                HRESULT hr = IMFSinkWriter_WriteSample(g_rec.writer, g_rec.videoStream, sample);
                LeaveCriticalSection(&g_rec.writeLock);
                if (FAILED(hr)) {
                    Log_Printf(L"record: writing native compressed packet failed 0x%08lX",
                               (unsigned long)hr);
                    g_rec.hwFailed = TRUE;
                } else {
                    g_rec.hwPackets++;
                }
            }
            IMFSample_Release(sample);
        } else {
            g_rec.hwFailed = TRUE;
            Log_Printf(L"record: MFCreateSample failed while muxing a native packet");
        }
    } else {
        g_rec.hwFailed = TRUE;
        Log_Printf(L"record: IMFMediaBuffer_Lock failed while muxing a native packet");
    }
    IMFMediaBuffer_Release(buf);
}

/* The frame in the format the encoder was set up for, then into it. */
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
    if (!src) {
        g_rec.hwFailed = TRUE;
        Log_Printf(L"record: could not prepare the frame for %s", g_rec.hwOps->name);
    } else if (!g_rec.hwOps->encode(g_rec.hw, src, when, OnHwPacket, NULL)) {
        g_rec.hwFailed = TRUE;
        Log_Printf(L"record: %s rejected a frame", g_rec.hwOps->name);
    }
    if (g_rec.videoFrames <= 3) {
        Log_Printf(L"record: frame %u through %s done", (unsigned)g_rec.videoFrames,
                   g_rec.hwOps->name);
        Crash_Reassert(L"by the card's encoder at work");
    }
    Crash_Step(L"record: between frames");
}


static UINT64 RecordQpcTo100ns(LONGLONG ticks, LONGLONG frequency)
{
    UINT64 whole, part;
    if (ticks < 0 || frequency <= 0)
        return 0;
    whole = (UINT64)ticks / (UINT64)frequency;
    part = (UINT64)ticks % (UINT64)frequency;
    return whole * 10000000u + part * 10000000u / (UINT64)frequency;
}

static UINT64 RecordPauseDuration100ns(void)
{
    return RecordQpcTo100ns(PausedTicksNow(), g_rec.qpcFreq.QuadPart);
}

static UINT64 RecordQpcToActive100ns(LONGLONG ticks, LONGLONG frequency)
{
    UINT64 absolute = RecordQpcTo100ns(ticks, frequency);
    UINT64 paused = RecordPauseDuration100ns();
    return absolute > paused ? absolute - paused : 0;
}

static int RawSlotForSource(AudioSource source)
{
    return source == AUDIO_SYSTEM ? 1 : source == AUDIO_MICROPHONE ? 2 : 0;
}

static void RecordRawNativeFormat(void *ctx, AudioSource source,
                                 const BYTE *formatBytes, UINT32 formatByteCount,
                                 UINT32 preferredPcmBits, UINT64 clockOrigin100ns)
{
    int slot = RawSlotForSource(source);
    WAVEFORMATEX *fmt;
    wchar_t extension[24];
    UNREFERENCED_PARAMETER(ctx);

    if (!g_rec.multiTrack || slot == 0 || !formatBytes ||
        formatByteCount < sizeof(WAVEFORMATEX) ||
        formatByteCount > sizeof(g_rec.rawFormat[slot].bytes)) {
        if (g_rec.multiTrack)
            InterlockedExchange(&g_rec.rawCaptureFailed, 1);
        return;
    }
    CopyMemory(g_rec.rawFormat[slot].bytes, formatBytes, formatByteCount);
    fmt = (WAVEFORMATEX *)g_rec.rawFormat[slot].bytes;
    if ((UINT32)sizeof(WAVEFORMATEX) + fmt->cbSize > formatByteCount ||
        !fmt->nSamplesPerSec || !fmt->nBlockAlign) {
        Log_Printf(L"record: invalid native audio format for source %d", (int)source);
        InterlockedExchange(&g_rec.rawCaptureFailed, 1);
        return;
    }

    g_rec.rawFormatBytes[slot] = formatByteCount;
    g_rec.rawPreferredFlacBits[slot] = preferredPcmBits;
    g_rec.rawRate[slot] = fmt->nSamplesPerSec;
    g_rec.rawBlockAlign[slot] = fmt->nBlockAlign;
    if (!g_rec.rawOrigin100ns)
        g_rec.rawOrigin100ns = clockOrigin100ns;

    StringCchCopyW(g_rec.rawPath[slot], ARRAYSIZE(g_rec.rawPath[slot]),
                   g_rec.result->path);
    StringCchCopyW(extension, ARRAYSIZE(extension),
                   source == AUDIO_SYSTEM ? L".system.raw" : L".mic.raw");
    if (!PathRenameExtensionW(g_rec.rawPath[slot], extension)) {
        Log_Printf(L"record: cannot make raw audio path for source %d", (int)source);
        g_rec.rawPath[slot][0] = 0;
        InterlockedExchange(&g_rec.rawCaptureFailed, 1);
        return;
    }
    g_rec.rawPcm[slot] = RawPcm_Open(g_rec.rawPath[slot], fmt->nSamplesPerSec,
                                      fmt->nBlockAlign, clockOrigin100ns);
    if (!g_rec.rawPcm[slot]) {
        InterlockedExchange(&g_rec.rawCaptureFailed, 1);
        return;
    }
    /* Persist the exact format before accepting packets. If the process dies,
       startup recovery can wrap the headerless PCM in a playable WAV. */
    if (!Recovery_WriteRawMetadata(g_rec.rawPath[slot], formatBytes,
                                   formatByteCount, clockOrigin100ns))
        Log_Printf(L"record: could not persist RAW audio recovery metadata for %s",
                   g_rec.rawPath[slot]);
    /* Unsigned PCM8 silence is 0x80; 0x00 would encode as full negative scale. */
    if (fmt->wBitsPerSample == 8 && fmt->wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
        RawPcm_SetSilenceByte(g_rec.rawPcm[slot], 0x80);
    Log_Printf(L"record: raw source %d at %lu Hz, %u ch, %u bits, format tag %u",
               (int)source, (unsigned long)fmt->nSamplesPerSec,
               fmt->nChannels, fmt->wBitsPerSample, fmt->wFormatTag);
}

static void RecordRawNativePacket(void *ctx, AudioSource source,
                                  const BYTE *data, UINT32 frames, BOOL silent,
                                  UINT64 packetQpc100ns)
{
    int slot = RawSlotForSource(source);
    UNREFERENCED_PARAMETER(ctx);
    if (!g_rec.multiTrack || slot == 0 || !g_rec.rawPcm[slot] || Record_IsPaused())
        return;
    /* Collapse paused intervals out of the RAW timeline; post-resume packet
       timestamps are shifted back so no pause-sized silence is inserted. */
    if (packetQpc100ns) {
        UINT64 paused = RecordPauseDuration100ns();
        packetQpc100ns = packetQpc100ns > paused ? packetQpc100ns - paused : 0;
    }
    if (!RawPcm_WriteTimed(g_rec.rawPcm[slot], data, frames, silent, packetQpc100ns))
        InterlockedExchange(&g_rec.rawCaptureFailed, 1);
}

static void CloseRawPcmSinks(BOOL padToRecordingEnd)
{
    int slot;
    for (slot = 1; slot <= 2; slot++) {
        RawPcmSink *raw = g_rec.rawPcm[slot];
        if (!raw)
            continue;
        if (padToRecordingEnd && g_rec.rawRecordStop100ns > g_rec.rawOrigin100ns &&
            g_rec.rawRate[slot]) {
            UINT64 duration = g_rec.rawRecordStop100ns - g_rec.rawOrigin100ns;
            UINT64 targetFrames = (duration / 10000000u) * g_rec.rawRate[slot] +
                ((duration % 10000000u) * g_rec.rawRate[slot]) / 10000000u;
            if (!RawPcm_PadTo(raw, targetFrames))
                InterlockedExchange(&g_rec.rawCaptureFailed, 1);
        }
        if (!RawPcm_Close(raw, NULL))
            InterlockedExchange(&g_rec.rawCaptureFailed, 1);
        g_rec.rawPcm[slot] = NULL;
    }
}

/* Write one PCM block at the current point on the audio timeline. */
static void WriteAudioBlock(const BYTE *pcm16, UINT32 frames)
{
    IMFMediaBuffer *buf = NULL;
    IMFSample      *sample = NULL;
    DWORD           size = frames * g_rec.audioChannels * 2;
    BYTE           *dst = NULL;

    if (size == 0)
        return;

    /* No AAC stream exists in this mode; the WAV is the only destination, and
       the padding still goes through here so the timeline stays right. */
    if (g_rec.muxFromWav) {
        if (g_rec.wav)
            Wav_Write(g_rec.wav, pcm16, frames);
        g_rec.audioFrames += frames;
        return;
    }

    if (FAILED(MFCreateMemoryBuffer(size, &buf)))
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
            IMFSample_SetSampleTime(sample,
                g_rec.audioFrames * 10000000LL / g_rec.audioRate + g_rec.avOffset);
            IMFSample_SetSampleDuration(sample,
                (LONGLONG)frames * 10000000LL / g_rec.audioRate);
            IMFSinkWriter_WriteSample(g_rec.writer, g_rec.audioStream, sample);
            IMFSample_Release(sample);
        }
        /* Fed from here rather than from OnAudio so the sidecar gets the
           padding too and stays aligned with the video timeline. */
        if (g_rec.wav)
            Wav_Write(g_rec.wav, pcm16, frames);
        g_rec.audioFrames += frames;
    }
    IMFMediaBuffer_Release(buf);
}

/*
 * A loopback capture delivers nothing at all while the machine is silent - not
 * zeroed packets, no packets. Left alone that produces a recording with an
 * empty audio track, or one whose sound jumps whenever playback paused. The
 * video loop therefore tops the audio timeline up with real silence whenever it
 * falls behind the elapsed time.
 */
static void PadAudioSilence(void)
{
    LONGLONG expected, behind;
    UINT32   chunk;

    if (g_rec.multiTrack || !g_rec.haveAudio || g_rec.audioRate == 0)
        return;

    /*
     * Only when the source has genuinely gone quiet. Comparing the audio
     * timeline against the clock is not enough on its own: capture always runs
     * a little behind the clock, so a threshold small enough to catch real
     * gaps also fires constantly during normal playback - and every spurious
     * pad inserts silence in the middle of continuous sound and pushes the
     * samples already in flight to a later timestamp. That was audible.
     */
    if (GetTickCount64() - g_rec.lastAudioTick < QUIET_BEFORE_PAD_MS)
        return;

    EnterCriticalSection(&g_rec.writeLock);
    /* Never fill the most recent stretch: samples for it may still be on
       their way from the audio engine. */
    expected = (ElapsedNs100() - PAD_LAG_MS * 10000LL) * g_rec.audioRate / 10000000LL;
    behind   = expected - g_rec.audioFrames;
    if (behind > 0) {
        while (behind > 0) {
            chunk = (UINT32)(behind > 4096 ? 4096 : behind);
            WriteAudioBlock(NULL, chunk);
            behind -= chunk;
        }
    }
    LeaveCriticalSection(&g_rec.writeLock);
}

/*
 * Audio is timestamped from its own sample count rather than the wall clock.
 * The endpoint's clock and QPC drift apart slightly, and a timeline built from
 * the samples actually captured is the one that plays back without gaps.
 */
static void OnAudio(void *ctx, const BYTE *pcm16, UINT32 frames)
{
    UNREFERENCED_PARAMETER(ctx);
    if (!g_rec.writer || !g_rec.haveAudio ||
        !Record_IsActive() || Record_IsPaused())
        return;

    EnterCriticalSection(&g_rec.writeLock);
    g_rec.lastAudioTick = GetTickCount64();
    WriteAudioBlock(pcm16, frames);
    LeaveCriticalSection(&g_rec.writeLock);
}

static UINT64 SamplesFor100ns(UINT64 duration100ns, UINT32 sampleRate)
{
    return (duration100ns / 10000000u) * sampleRate +
           ((duration100ns % 10000000u) * sampleRate) / 10000000u;
}

typedef struct {
    volatile LONG trackPercent[2];
    volatile LONG lastMappedPercent;
} MultiTrackFlacProgress;

typedef struct {
    MultiTrackFlacProgress *progress;
    LONG slot;
    const wchar_t *rawPath;
    const BYTE *formatBytes;
    UINT32 formatByteCount;
    UINT64 startSample;
    UINT64 sampleCount;
    UINT32 preferredBits;
    const wchar_t *flacPath;
    FlacEncodedTrack *outTrack;
    BOOL ok;
    ULONGLONG elapsedMs;
} MultiTrackFlacJob;

static void Record_MultiTrackFlacProgress(void *context, UINT64 done, UINT64 total)
{
    MultiTrackFlacJob *job = (MultiTrackFlacJob *)context;
    MultiTrackFlacProgress *state;
    LONG mic, sys, average, mapped, old;
    if (!job || !job->progress || !total)
        return;
    state = job->progress;
    if (done > total) done = total;
    InterlockedExchange(&state->trackPercent[job->slot],
                        (LONG)((done * 100u) / total));
    mic = InterlockedCompareExchange(&state->trackPercent[0], 0, 0);
    sys = InterlockedCompareExchange(&state->trackPercent[1], 0, 0);
    average = (mic + sys) / 2;
    mapped = 66 + (average * 24) / 100;

    /* Both encoders report concurrently. Publish only a monotonic aggregate
       so the UI progress bar never moves backwards while the tracks finish. */
    for (;;) {
        old = InterlockedCompareExchange(&state->lastMappedPercent, 0, 0);
        if (mapped <= old)
            break;
        if (InterlockedCompareExchange(&state->lastMappedPercent, mapped, old) == old) {
            Record_PostProgress(mapped);
            break;
        }
    }
}

static void MultiTrackFlacEncodeRun(MultiTrackFlacJob *job)
{
    ULONGLONG started = GetTickCount64();
    job->ok = Flac_EncodeRaw(job->rawPath, job->formatBytes,
        job->formatByteCount, job->startSample, job->sampleCount,
        job->preferredBits, job->flacPath, job->outTrack,
        Record_MultiTrackFlacProgress, job);
    job->elapsedMs = GetTickCount64() - started;
}

static unsigned __stdcall MultiTrackFlacEncodeThread(void *context)
{
    MultiTrackFlacJob *job = (MultiTrackFlacJob *)context;
    MultiTrackFlacEncodeRun(job);
    return job->ok ? 0u : 1u;
}

static BOOL FinishMultiTrackAudio(void)
{
    FlacEncodedTrack microphone, systemAudio;
    wchar_t micFlac[MAX_PATH], sysFlac[MAX_PATH];
    UINT64 duration100ns, micStart, sysStart, micCount, sysCount;
    MultiTrackFlacProgress progress;
    MultiTrackFlacJob micJob, sysJob;
    uintptr_t micThread = 0, sysThread = 0;
    BOOL micOk = FALSE, sysOk = FALSE, muxOk = FALSE;

    ZeroMemory(&microphone, sizeof(microphone));
    ZeroMemory(&systemAudio, sizeof(systemAudio));
    if (!g_rec.multiTrack)
        return TRUE;

    if (InterlockedCompareExchange(&g_rec.rawCaptureFailed, 0, 0) != 0 ||
        !g_rec.rawPath[AUDIO_MICROPHONE][0] || !g_rec.rawPath[AUDIO_SYSTEM][0] ||
        !g_rec.rawFormatBytes[AUDIO_MICROPHONE] || !g_rec.rawFormatBytes[AUDIO_SYSTEM] ||
        !g_rec.rawRate[AUDIO_MICROPHONE] || !g_rec.rawRate[AUDIO_SYSTEM] ||
        !g_rec.rawOrigin100ns || g_rec.rawRecordStop100ns <= g_rec.rawRecordStart100ns) {
        Fail(L"Multi-track audio capture was incomplete; the combined recording is retained and RAW audio files were preserved where available.");
        return FALSE;
    }

    duration100ns = g_rec.rawRecordStop100ns - g_rec.rawRecordStart100ns;
    micStart = g_rec.rawRecordStart100ns > g_rec.rawOrigin100ns
        ? SamplesFor100ns(g_rec.rawRecordStart100ns - g_rec.rawOrigin100ns,
                          g_rec.rawRate[AUDIO_MICROPHONE]) : 0;
    sysStart = g_rec.rawRecordStart100ns > g_rec.rawOrigin100ns
        ? SamplesFor100ns(g_rec.rawRecordStart100ns - g_rec.rawOrigin100ns,
                          g_rec.rawRate[AUDIO_SYSTEM]) : 0;
    micCount = SamplesFor100ns(duration100ns, g_rec.rawRate[AUDIO_MICROPHONE]);
    sysCount = SamplesFor100ns(duration100ns, g_rec.rawRate[AUDIO_SYSTEM]);
    if (!micCount || !sysCount) {
        Fail(L"Multi-track audio recording was too short to encode.");
        return FALSE;
    }

    StringCchCopyW(micFlac, ARRAYSIZE(micFlac), g_rec.result->path);
    StringCchCopyW(sysFlac, ARRAYSIZE(sysFlac), g_rec.result->path);
    if (!PathRenameExtensionW(micFlac, L".mic.flac") ||
        !PathRenameExtensionW(sysFlac, L".system.flac")) {
        Fail(L"Could not create temporary FLAC paths for multi-track audio.");
        return FALSE;
    }

    ZeroMemory(&progress, sizeof(progress));
    progress.lastMappedPercent = 66;
    ZeroMemory(&micJob, sizeof(micJob));
    ZeroMemory(&sysJob, sizeof(sysJob));

    micJob.progress = &progress;
    micJob.slot = 0;
    micJob.rawPath = g_rec.rawPath[AUDIO_MICROPHONE];
    micJob.formatBytes = g_rec.rawFormat[AUDIO_MICROPHONE].bytes;
    micJob.formatByteCount = g_rec.rawFormatBytes[AUDIO_MICROPHONE];
    micJob.startSample = micStart;
    micJob.sampleCount = micCount;
    micJob.preferredBits = g_rec.rawPreferredFlacBits[AUDIO_MICROPHONE];
    micJob.flacPath = micFlac;
    micJob.outTrack = &microphone;

    sysJob.progress = &progress;
    sysJob.slot = 1;
    sysJob.rawPath = g_rec.rawPath[AUDIO_SYSTEM];
    sysJob.formatBytes = g_rec.rawFormat[AUDIO_SYSTEM].bytes;
    sysJob.formatByteCount = g_rec.rawFormatBytes[AUDIO_SYSTEM];
    sysJob.startSample = sysStart;
    sysJob.sampleCount = sysCount;
    sysJob.preferredBits = g_rec.rawPreferredFlacBits[AUDIO_SYSTEM];
    sysJob.flacPath = sysFlac;
    sysJob.outTrack = &systemAudio;

    /* Each endpoint is independent and owns its input/output handles and FLAC
       frame index. Encode them concurrently to use available CPU and storage
       bandwidth rather than making Stop wait for two serial passes. If thread
       creation is unavailable, that track safely falls back to this thread. */
    micThread = _beginthreadex(NULL, 0, MultiTrackFlacEncodeThread, &micJob, 0, NULL);
    sysThread = _beginthreadex(NULL, 0, MultiTrackFlacEncodeThread, &sysJob, 0, NULL);
    if (!micThread) {
        Log_Printf(L"record: could not start microphone FLAC worker; encoding on the recording worker");
        MultiTrackFlacEncodeRun(&micJob);
    }
    if (!sysThread) {
        Log_Printf(L"record: could not start system FLAC worker; encoding on the recording worker");
        MultiTrackFlacEncodeRun(&sysJob);
    }
    if (micThread) {
        WaitForSingleObject((HANDLE)micThread, INFINITE);
        CloseHandle((HANDLE)micThread);
    }
    if (sysThread) {
        WaitForSingleObject((HANDLE)sysThread, INFINITE);
        CloseHandle((HANDLE)sysThread);
    }
    micOk = micJob.ok;
    sysOk = sysJob.ok;
    Log_Printf(L"record: parallel FLAC encoding finished in %llu ms (microphone %llu ms, system %llu ms)",
               (unsigned long long)(micJob.elapsedMs > sysJob.elapsedMs
                    ? micJob.elapsedMs : sysJob.elapsedMs),
               (unsigned long long)micJob.elapsedMs,
               (unsigned long long)sysJob.elapsedMs);
    if (!micOk || !sysOk) {
        Fail(L"Native FLAC encoding failed; temporary RAW audio was retained for recovery.");
        goto done;
    }

    Record_PostProgress(91);
    muxOk = Mp4Multi_AddFlacTracks(g_rec.result->path, &microphone, &systemAudio);
    if (muxOk) Record_PostProgress(98);
    if (!muxOk) {
        Fail(L"Could not embed microphone/system FLAC tracks into MP4; RAW and FLAC temporary files were retained.");
        goto done;
    }

    DeleteFileW(g_rec.rawPath[AUDIO_MICROPHONE]);
    DeleteFileW(g_rec.rawPath[AUDIO_SYSTEM]);
    Recovery_DeleteRawMetadata(g_rec.rawPath[AUDIO_MICROPHONE]);
    Recovery_DeleteRawMetadata(g_rec.rawPath[AUDIO_SYSTEM]);
    DeleteFileW(micFlac);
    DeleteFileW(sysFlac);
    Log_Printf(L"record: multi-track MP4 completed (combined AAC + mic FLAC + system FLAC)");

done:
    Flac_FreeTrack(&microphone);
    Flac_FreeTrack(&systemAudio);
    return muxOk;
}

/* ---------------------------------------------------------------- the loop */

/* Pull the newest frame, if any, into the staging texture. */
static BOOL Record_SourceRect(const D3D11_TEXTURE2D_DESC *srcDesc, RECT *box,
                              BOOL *requiresScale)
{
    LONG relLeft, relTop, relRight, relBottom;
    UINT32 baseW = g_rec.sourceModeWidth;
    UINT32 baseH = g_rec.sourceModeHeight;
    BOOL changed;

    if (!srcDesc || !box || !requiresScale || !srcDesc->Width || !srcDesc->Height ||
        !baseW || !baseH)
        return FALSE;

    changed = srcDesc->Width != baseW || srcDesc->Height != baseH ||
              srcDesc->Format != g_rec.captureFormat;
    *requiresScale = changed;
    relLeft = g_rec.out.left - g_rec.sourceOrigin.x;
    relTop = g_rec.out.top - g_rec.sourceOrigin.y;
    relRight = g_rec.out.right - g_rec.sourceOrigin.x;
    relBottom = g_rec.out.bottom - g_rec.sourceOrigin.y;

    if (!changed) {
        box->left = relLeft;
        box->top = relTop;
        box->right = box->left + (LONG)g_rec.width;
        box->bottom = box->top + (LONG)g_rec.height;
    } else {
        /* Map the original selected rectangle proportionally into the new
           desktop mode. The scaler then fills the unchanged output canvas. */
        box->left = (LONG)(((INT64)relLeft * srcDesc->Width) / baseW);
        box->top = (LONG)(((INT64)relTop * srcDesc->Height) / baseH);
        box->right = (LONG)((((INT64)relRight * srcDesc->Width) + baseW - 1) / baseW);
        box->bottom = (LONG)((((INT64)relBottom * srcDesc->Height) + baseH - 1) / baseH);
    }
    if (box->left < 0) box->left = 0;
    if (box->top < 0) box->top = 0;
    if (box->right > (LONG)srcDesc->Width) box->right = (LONG)srcDesc->Width;
    if (box->bottom > (LONG)srcDesc->Height) box->bottom = (LONG)srcDesc->Height;
    return box->right > box->left && box->bottom > box->top;
}

/* Pull the newest frame, if any, into the fixed-size capture canvas. */
static BOOL GrabFrame(void)
{
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource   *res = NULL;
    ID3D11Texture2D *tex = NULL;
    D3D11_TEXTURE2D_DESC srcDesc;
    RECT              sourceRect;
    HRESULT           hr;
    BOOL              got = FALSE, scale = FALSE;

    hr = IDXGIOutputDuplication_AcquireNextFrame(g_rec.dup, 0, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        g_rec.grabTimeouts++;
        return FALSE;
    }
    if (FAILED(hr)) {
        /* ACCESS_LOST is normal when a game changes display mode. A fresh
           duplication is created by the loop without changing the MP4 canvas. */
        g_rec.grabFailures++;
        IDXGIOutputDuplication_Release(g_rec.dup);
        g_rec.dup = NULL;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            Log_Printf(L"record: graphics device lost 0x%08lX, stopping",
                       (unsigned long)hr);
            g_rec.deviceLost = TRUE;
        } else {
            Log_Printf(L"record: duplication lost (0x%08lX); will reopen and rescale",
                       (unsigned long)hr);
        }
        return FALSE;
    }

    if (info.LastPresentTime.QuadPart == 0) {
        g_rec.grabPointerOnly++;
        IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(g_rec.dup);
        return FALSE;
    }

    if (SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D,
                                               (void **)&tex))) {
        ID3D11Texture2D *dest;
        ID3D11Texture2D_GetDesc(tex, &srcDesc);
        dest = (g_rec.floatCapture || g_rec.sdr10)
             ? HdrVideo_Input(g_rec.hv)
             : g_rec.gpu ? g_rec.frameTex : g_rec.staging;

        if (dest && Record_SourceRect(&srcDesc, &sourceRect, &scale)) {
            if (scale) {
                if (g_rec.floatCapture &&
                    srcDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    /* A colour-mode switch is different from a resolution
                       change: do not reinterpret SDR BGRA values as scRGB.
                       This also protects an HDR-desktop-to-SDR-output encode. */
                    Log_Printf(L"record: source colour format changed during floating-point capture; cannot preserve colour interpretation");
                    g_rec.deviceLost = TRUE;
                } else if (HdrVideo_ScaleInput(g_rec.hv, tex, &sourceRect)) {
                    /* Regular SDR's converter object doubles as a BGRA scaler;
                       copy the scaled canvas to the selected CPU/GPU sink. */
                    if (!g_rec.floatCapture && !g_rec.sdr10) {
                        ID3D11DeviceContext_CopyResource(g_rec.ctx,
                            (ID3D11Resource *)dest,
                            (ID3D11Resource *)HdrVideo_Input(g_rec.hv));
                    }
                    got = TRUE;
                    g_rec.grabCopies++;
                } else {
                    Log_Printf(L"record: GPU scaling failed for %ux%u -> %ux%u",
                               srcDesc.Width, srcDesc.Height, g_rec.width, g_rec.height);
                }
            } else {
                D3D11_BOX copyBox;
                copyBox.left = (UINT)sourceRect.left;
                copyBox.top = (UINT)sourceRect.top;
                copyBox.right = copyBox.left + g_rec.width;
                copyBox.bottom = copyBox.top + g_rec.height;
                copyBox.front = 0;
                copyBox.back = 1;
                ID3D11DeviceContext_CopySubresourceRegion(g_rec.ctx,
                    (ID3D11Resource *)dest, 0, 0, 0, 0,
                    (ID3D11Resource *)tex, 0, &copyBox);
                got = TRUE;
                g_rec.grabCopies++;
            }
        }
        ID3D11Texture2D_Release(tex);
    }
    IDXGIResource_Release(res);
    IDXGIOutputDuplication_ReleaseFrame(g_rec.dup);
    return got;
}

static unsigned __stdcall RecordThread(void *arg)
{
    AudioFormat afmt;
    LONGLONG    frameIndex = 0;
    DWORD       intervalMs;
    BOOL        haveFrame = FALSE;

    UNREFERENCED_PARAMETER(arg);
    Crash_Step(L"record: initializing COM and Media Foundation");
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    Crash_Step(L"record: starting Media Foundation");
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    PoolOpen();

    intervalMs = (DWORD)(1000 / g_rec.opt.fps);

    Crash_Step(L"record: choosing the video profile");
    if (!ChooseEncoder())         { Fail(Lang_Str(STR_ERR_NO_ENCODER)); goto done; }
    Crash_Step(L"record: opening Desktop Duplication for selected display");
    if (!OpenOutput(FALSE))       { Fail(Lang_Str(STR_ERR_NO_DISPLAY)); goto done; }
    intervalMs = (DWORD)(1000 / g_rec.opt.fps);
    if (!intervalMs) intervalMs = 1;

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

    Crash_Step(L"record: preparing colour conversion for selected profile");
    if (g_rec.hdr) {
        g_rec.hv = HdrVideo_Create(g_rec.dev, g_rec.ctx, g_rec.width, g_rec.height);
        if (!g_rec.hv) { Fail(Lang_Str(STR_ERR_HDR_SETUP)); goto done; }
    } else if (g_rec.sdr10 && !g_rec.floatCapture) {
        DXGI_FORMAT sourceFmt = g_rec.captureFormat;
        if (sourceFmt != DXGI_FORMAT_R10G10B10A2_UNORM &&
            sourceFmt != DXGI_FORMAT_B8G8R8A8_UNORM)
            sourceFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        g_rec.hv = HdrVideo_CreateSdr(g_rec.dev, g_rec.ctx, g_rec.width,
                                      g_rec.height, sourceFmt);
        if (!g_rec.hv) { Fail(Lang_Str(STR_ERR_HDR_SETUP)); goto done; }
        if (sourceFmt != DXGI_FORMAT_R10G10B10A2_UNORM)
            Log_Printf(L"record: 10-bit SDR encoding requested but DXGI supplied BGRA8; output is Rec.709 up-converted from 8-bit source");
    } else if (g_rec.floatCapture) {
        g_rec.hv = HdrVideo_Create(g_rec.dev, g_rec.ctx, g_rec.width, g_rec.height);
        if (!g_rec.hv) { Fail(Lang_Str(STR_ERR_HDR_SETUP)); goto done; }
        HdrVideo_SetSdrWhite(g_rec.hv, g_rec.sdrWhiteNits, g_rec.cfg.hdrSdrRollOff);
        if (g_rec.sdr10) {
            g_rec.sdrhv = HdrVideo_CreateSdr(g_rec.dev, g_rec.ctx, g_rec.width,
                                             g_rec.height, DXGI_FORMAT_B8G8R8A8_UNORM);
            if (!g_rec.sdrhv) { Fail(Lang_Str(STR_ERR_HDR_SETUP)); goto done; }
        } else if (!g_rec.hdr) {
            Log_Printf(L"record: float capture, tone-mapped to 8-bit at "
                       L"%.0f nits white%s", g_rec.sdrWhiteNits,
                       g_rec.cfg.hdrSdrRollOff ? L", highlights rolled off" : L"");
        }
    } else {
        /* Also create the BGRA input canvas on the regular SDR path. It is
           normally bypassed, but lets us resize a newly negotiated desktop
           mode on the GPU while retaining the original MP4 dimensions. */
        g_rec.hv = HdrVideo_CreateSdr(g_rec.dev, g_rec.ctx, g_rec.width,
                                      g_rec.height, DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!g_rec.hv) {
            Fail(Lang_Str(STR_ERR_HDR_SETUP));
            goto done;
        }
        if (!MakeStaging()) {
            Fail(Lang_Str(STR_ERR_CAPTURE_BUF));
            goto done;
        }
    }

    /* The CPU readback texture is dead weight - 33 MB at 4K - once frames
       stay on the GPU; the fallback below makes another if it is needed. */
    Crash_Step(L"record: preparing GPU transfer path");
    if (SetupGpu() && g_rec.staging) {
        ID3D11Texture2D_Release(g_rec.staging);
        g_rec.staging = NULL;
    }

    /* Keep a last-step marker and watchdog active through encoder startup,
       not just during the frame loop: vendor APIs can hang before frame one. */
    Crash_Step(L"record: initializing encoder and MP4 writer");
    Crash_WatchBegin(20000);
    Crash_Beat();

    /* Prefer a native vendor encoder when it can encode this exact profile.
       Every failure before the first encoded packet can fall back to the
       hardware Media Foundation MFT without losing the recording. */
    Crash_Step(L"record: querying native encoder capabilities");
    if (SetupHwEncoder()) {
        Crash_Step(L"record: opening output for native encoder");
        Crash_Beat();
        if (!OpenWriter()) {
            Fail(Lang_Str(STR_ERR_FILE_CREATE));
            goto done;
        }
        if (!AddHwStream()) {
            Log_Printf(L"record: MP4 writer refused %s's compressed stream; retrying with Media Foundation",
                       g_rec.hwOps->name);
            if (g_rec.writer) {
                IMFSinkWriter_Release(g_rec.writer);
                g_rec.writer = NULL;
            }
            if (g_rec.result->path[0]) DeleteFileW(g_rec.result->path);
            TeardownHwEncoder();
            g_rec.hwOps = NULL;
            if (!g_rec.floatCapture && !g_rec.staging && !MakeStaging()) {
                Fail(Lang_Str(STR_ERR_CAPTURE_BUF));
                goto done;
            }
        }
        if (g_rec.hw && g_rec.result)
            g_rec.result->usedDirect = TRUE;
    }
    if (g_rec.hw) {
        Log_Printf(L"record: native %s encoder active; audio offset=%lld ms",
                   g_rec.hwOps->name, (long long)(g_rec.avOffset / 10000));
    } else {
        Crash_Step(L"record: preparing Media Foundation hardware MFT");
        Crash_Beat();
        if (!Encoder_PrepareVendor(g_rec.enc, g_rec.cfg.recordEncoderVendor)) {
            Fail(Lang_Str(STR_ERR_NO_ENCODER));
            goto done;
        }
        {
            BOOL mftStreamReady = FALSE;
            Crash_Step(L"record: opening Media Foundation MP4 writer");
            Crash_Beat();
            if (!OpenWriter()) {
                Fail(Lang_Str(STR_ERR_FILE_CREATE));
                goto done;
            }
            Crash_Step(L"record: negotiating Media Foundation video media types");
            if (Encoder_AddVideoStream(g_rec.writer, g_rec.enc, g_rec.width,
                                       g_rec.height, g_rec.opt.fps,
                                       &g_rec.videoStream)) {
                mftStreamReady = TRUE;
            } else if (g_rec.gpu) {
                /* Some hardware MFTs reject GPU textures but accept the same
                   hardware encoder when fed system-memory frames. */
                Log_Printf(L"record: %s refused GPU textures; retrying CPU-fed input (hardware encoding remains mandatory)",
                           Encoder_Name(g_rec.enc));
                IMFSinkWriter_Release(g_rec.writer);
                g_rec.writer = NULL;
                Encoder_ClearVendor();
                DeleteFileW(g_rec.result->path);
                TeardownGpu();
                if (!g_rec.floatCapture && !g_rec.staging && !MakeStaging()) {
                    Fail(Lang_Str(STR_ERR_CAPTURE_BUF));
                    goto done;
                }
                if (Encoder_PrepareVendor(g_rec.enc, g_rec.cfg.recordEncoderVendor) &&
                    OpenWriter() &&
                    Encoder_AddVideoStream(g_rec.writer, g_rec.enc, g_rec.width,
                                           g_rec.height, g_rec.opt.fps,
                                           &g_rec.videoStream)) {
                    mftStreamReady = TRUE;
                }
            }
            if (!mftStreamReady) {
                Fail(Lang_Str(STR_ERR_ENC_FORMAT));
                goto done;
            }
        }
        Log_Printf(L"record: Media Foundation hardware MFT backend (%s input); software encoding is disabled",
                   g_rec.gpu ? L"zero-copy GPU" : L"CPU-fed");
    }

    Crash_Step(L"record: opening audio source and configuring audio stream");
    Crash_Beat();
    ZeroMemory(&afmt, sizeof(afmt));
    if (!g_rec.opt.makeGif && g_rec.opt.audio != AUDIO_NONE) {
        g_rec.multiTrack = g_rec.cfg.recordMultiTrack && g_rec.opt.audio == AUDIO_BOTH;
        if (g_rec.multiTrack)
            Log_Printf(L"record: three-track audio enabled; stop/save includes offline AAC mix, two FLAC encodes and MP4 muxing");
        /* Multitrack always records to a combined PCM sidecar first so AAC is
           encoded after capture, then it adds both original endpoint sources. */
        g_rec.muxFromWav = g_rec.multiTrack ||
            (g_rec.cfg.recordWavSidecar && g_rec.cfg.recordMuxFromWav);

        if (Audio_Start(g_rec.opt.audio, &afmt, OnAudio, NULL,
                        g_rec.multiTrack,
                        g_rec.multiTrack ? RecordRawNativeFormat : NULL,
                        g_rec.multiTrack ? RecordRawNativePacket : NULL,
                        NULL)) {
            /*
             * With the sidecar as the source of the MP4's sound there is no
             * AAC stream in the file while recording - the samples go to the
             * WAV only, and the encoding happens once, afterwards. Nothing
             * then has to keep up with the capture clock.
             */
            BOOL haveStream = g_rec.muxFromWav ||
                              AddAudioStream(afmt.sampleRate, afmt.channels);

            if (haveStream) {
                g_rec.audioRate     = afmt.sampleRate;
                g_rec.audioChannels = afmt.channels;
                g_rec.haveAudio     = TRUE;

                if (g_rec.cfg.recordWavSidecar || g_rec.multiTrack) {
                    StringCchCopyW(g_rec.result->wavPath,
                                   ARRAYSIZE(g_rec.result->wavPath),
                                   g_rec.result->path);
                    if (!PathRenameExtensionW(g_rec.result->wavPath, L".partial.wav"))
                        g_rec.result->wavPath[0] = 0;
                    /* In multitrack mode this path is a post-capture render
                       target. Do not write a live mix while capturing RAW. */
                    else if (!g_rec.multiTrack) {
                        g_rec.wav = Wav_Create(g_rec.result->wavPath,
                                               afmt.sampleRate, afmt.channels);
                        if (!g_rec.wav)
                            g_rec.result->wavPath[0] = 0;
                    }
                }
                /* Outside multitrack mode, a requested WAV sidecar is still
                   prepared live; otherwise AAC stays live unless explicitly
                   configured to use the WAV remux path. */
                if (g_rec.muxFromWav && !g_rec.multiTrack && !g_rec.wav) {
                    Log_Printf(L"record: cannot create PCM sidecar; encoding AAC live instead");
                    g_rec.muxFromWav = FALSE;
                    if (!AddAudioStream(afmt.sampleRate, afmt.channels)) {
                        Audio_Stop();
                        g_rec.haveAudio = FALSE;
                        Log_Printf(L"record: no AAC encoder, continuing silent");
                    }
                }
            } else {
                Audio_Stop();
                Log_Printf(L"record: no AAC encoder, continuing without sound");
            }
        } else {
            Log_Printf(L"record: audio could not be opened, continuing silent");
        }
    }

    {
        HRESULT hrBegin;
        Crash_Step(L"record: beginning MP4 writer");
        Crash_Beat();
        hrBegin = IMFSinkWriter_BeginWriting(g_rec.writer);
        if (FAILED(hrBegin)) {
            Log_Printf(L"record: IMFSinkWriter_BeginWriting failed 0x%08lX",
                       (unsigned long)hrBegin);
            if (!ScheduleMftRetry(L"IMFSinkWriter_BeginWriting failed"))
                Fail(Lang_Str(STR_ERR_ENC_START));
            goto done;
        }
    }
    if (!g_rec.hw) {
        if (!Encoder_WriterMatchesVendor(g_rec.writer, g_rec.videoStream,
                                         g_rec.cfg.recordEncoderVendor)) {
            Fail(Lang_Str(STR_ERR_NO_ENCODER));
            goto done;
        }
        if (!Encoder_ConfigureVBR(g_rec.writer, g_rec.videoStream, g_rec.enc,
                                  g_rec.width, g_rec.height, g_rec.opt.fps)) {
            /* Probing requires VBR to work too. If the active-size mode is refused,
               fail closed instead of silently recording CBR or software video. */
            Fail(Lang_Str(STR_ERR_NO_ENCODER));
            goto done;
        }
    }

    QueryPerformanceFrequency(&g_rec.qpcFreq);
    QueryPerformanceCounter(&g_rec.qpcStart);
    g_rec.frameTimer = CreateFrameTimer();
    g_rec.rawRecordStart100ns = RecordQpcTo100ns(g_rec.qpcStart.QuadPart,
                                                 g_rec.qpcFreq.QuadPart);
    g_rec.lastAudioTick = GetTickCount64();
    g_rec.hdrLocked     = g_rec.hdr;
    Crash_Step(L"record: entering capture loop");
    Crash_Reassert(L"before the recording loop");
    Crash_Beat();
    InterlockedExchange(&g_rec.active, 1);

    for (;;) {
        LONGLONG due;
        LONGLONG now;
        Crash_Beat();
        if (g_rec.opt.makeGif && Record_ElapsedMs() >= 15000u) {
            Log_Printf(L"gif: maximum duration of 15 seconds reached; finalising");
            SetEvent(g_rec.stopEvent);
            break;
        }
        if (InterlockedExchange(&g_rec.displayModeChanged, 0)) {
            /* The UI thread only signals; the recording thread owns DXGI objects.
               Drop the stale duplication here and reacquire it at the new mode. */
            if (g_rec.dup) {
                IDXGIOutputDuplication_Release(g_rec.dup);
                g_rec.dup = NULL;
            }
            Log_Printf(L"record: display-change notification; reacquiring desktop and keeping the original encoded dimensions");
        }
        if (Record_IsPaused()) {
            HANDLE waits[2] = { g_rec.stopEvent, g_rec.resumeEvent };
            DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 1000);
            Crash_Beat(); /* a deliberate pause is not a hung recording */
            if (wr == WAIT_OBJECT_0 || WaitForSingleObject(g_rec.stopEvent, 0) == WAIT_OBJECT_0)
                break;
            continue;
        }
        if (WaitForSingleObject(g_rec.stopEvent, 0) == WAIT_OBJECT_0)
            break;

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

        /* Do not advance the MP4 timeline before the first real desktop image
           arrives. Otherwise a static desktop can start with an empty PTS gap. */
        if (!haveFrame) {
            if (g_rec.deviceLost)
                break;
            if (!g_rec.dup) {
                if (!OpenOutput(TRUE))
                    Sleep(intervalMs);
                else
                    Log_Printf(L"record: duplication rebuilt before first frame");
            }
            if (WaitForSingleObject(g_rec.stopEvent, intervalMs) == WAIT_OBJECT_0)
                break;
            continue;
        }

        /* If capture/encoding overruns a frame deadline, skip frame slots rather
           than compressing elapsed real time into a shorter video. Timestamps
           remain tied to the QPC-based active timeline; the last image is still
           repeated on ordinary no-change ticks. */
        if (g_rec.videoFrames > 0) {
            LONGLONG elapsed = ElapsedNs100();
            LONGLONG slot = elapsed * g_rec.opt.fps / 10000000LL;
            if (slot > frameIndex)
                frameIndex = slot;
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
            } else if (g_rec.sdr10) {
                WriteSdrP010Sample(when);
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
        if (g_rec.hwFailed) {
            if (ScheduleMftRetry(L"encode or packet writing failed")) {
                /* The original attempt has no decodable packet. The common
                   cleanup removes its empty partial before the MFT retry. */
            } else {
                Log_Printf(L"record: native encoder failed after %lld packet(s); preserving diagnostic partial output",
                           (long long)g_rec.hwPackets);
                Fail(L"The native hardware encoder stopped during recording; the incomplete video was retained.");
            }
            SetEvent(g_rec.stopEvent);
            break;
        }
        frameIndex++;
        PadAudioSilence();

        /* Constant-frame-rate pacing is driven from the timeline, not from
           time spent processing the previous frame. A high-resolution waitable
           timer reduces 1-ms Sleep quantisation at 60/75/120/144 Hz. */
        due = frameIndex * 10000000LL / g_rec.opt.fps;
        now = ElapsedNs100();
        if (due > now) {
            LONGLONG remain100ns = due - now;
            if (g_rec.frameTimer) {
                LARGE_INTEGER dueTimer;
                HANDLE waits[2] = { g_rec.stopEvent, g_rec.frameTimer };
                dueTimer.QuadPart = -remain100ns;
                if (SetWaitableTimer(g_rec.frameTimer, &dueTimer, 0, NULL, NULL, FALSE)) {
                    DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                    if (wr == WAIT_OBJECT_0)
                        break;
                } else if (WaitForSingleObject(g_rec.stopEvent,
                                               (DWORD)(remain100ns / 10000)) == WAIT_OBJECT_0) {
                    break;
                }
            } else {
                DWORD waitMs = (DWORD)(remain100ns / 10000);
                if (waitMs > intervalMs * 4) waitMs = intervalMs * 4;
                if (WaitForSingleObject(g_rec.stopEvent, waitMs) == WAIT_OBJECT_0)
                    break;
            }
        } else if (WaitForSingleObject(g_rec.stopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        if (g_rec.deviceLost)
            break;

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

    if (g_rec.gpu && g_rec.hdr && g_rec.hv) {
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

    g_rec.result->seconds = Record_ElapsedMs() / 1000;
    {
        LARGE_INTEGER stopQpc;
        QueryPerformanceCounter(&stopQpc);
        g_rec.rawRecordStop100ns = RecordQpcToActive100ns(stopQpc.QuadPart,
                                                          g_rec.qpcFreq.QuadPart);
    }
    InterlockedExchange(&g_rec.active, 0);
    /* The 20-second watchdog protects the live capture loop only. Stop it
       before offline WAV/AAC/FLAC work, which can legitimately take longer
       than the capture heartbeat interval and must not be reported as a hang. */
    Crash_WatchEnd();
    Record_PostProgress(3);

    Audio_Stop();
    CloseRawPcmSinks(TRUE);
    Record_PostProgress(7);
    if (g_rec.videoFrames > 0 && g_rec.qpcFreq.QuadPart > 0)
        Log_Printf(L"record: %ux%u, %.2f ms per loop frame to build/write",
                   g_rec.width, g_rec.height,
                   (double)g_rec.buildTicks * 1000.0 /
                   (double)g_rec.qpcFreq.QuadPart / (double)g_rec.videoFrames);
    Log_Printf(L"record: %lld capture frame(s); grabs: %lld copied, %lld timed out, %lld pointer-only, %lld failed",
               (long long)g_rec.videoFrames, (long long)g_rec.grabCopies,
               (long long)g_rec.grabTimeouts,
               (long long)g_rec.grabPointerOnly,
               (long long)g_rec.grabFailures);
    Record_PostProgress(10);

    /* Drain delayed B-frames/lookahead before closing the MP4 writer. */
    if (g_rec.hw && !g_rec.hwFailed) {
        Crash_Step(L"record: draining native hardware encoder");
        if (!g_rec.hwOps->flush(g_rec.hw, OnHwPacket, NULL)) {
            Log_Printf(L"record: %s flush failed", g_rec.hwOps->name);
            g_rec.hwFailed = TRUE;
            if (!ScheduleMftRetry(L"native encoder flush failed"))
                Fail(L"The native hardware encoder could not finish the video stream.");
        } else if (g_rec.hwFailed && g_rec.hwPackets == 0) {
            (void)ScheduleMftRetry(L"writing a packet during flush failed");
        }
    }

    {
        HRESULT hrFin;
        ULONGLONG finalizeStarted = GetTickCount64();
        EnterCriticalSection(&g_rec.writeLock);
        /* MF_E_SINK_NO_SAMPLES_PROCESSED means the duplication never supplied
           a frame; report the distinction from an encoder failure in the log. */
        hrFin = IMFSinkWriter_Finalize(g_rec.writer);
        g_rec.result->ok = SUCCEEDED(hrFin) && !g_rec.hwFailed;
        if (g_rec.result->ok) Record_PostProgress(25);
        if (FAILED(hrFin))
            Log_Printf(L"record: MFT finalise failed 0x%08lX after %lld frame(s)",
                       (unsigned long)hrFin, (long long)g_rec.videoFrames);
        else
            Log_Printf(L"record: base MP4 video/audio finalisation completed in %llu ms",
                       (unsigned long long)(GetTickCount64() - finalizeStarted));
        LeaveCriticalSection(&g_rec.writeLock);
    }

    if (g_rec.wav) {
        if (!Wav_Close(g_rec.wav))
            g_rec.result->wavPath[0] = 0;
        g_rec.wav = NULL;
    }
    /* The finalised writer/process must be closed before any native MP4 remux. */
    if (g_rec.writer) {
        IMFSinkWriter_Release(g_rec.writer);
        g_rec.writer = NULL;
    }
    if (!g_rec.result->ok) {
        Fail(Lang_Str(STR_ERR_FINALISE));
    } else {
        /*
         * The sound is built from the sidecar here, before the colour box is
         * stamped: remuxing writes a brand new file, so anything added to the
         * old one would be thrown away.
         */
        {
            BOOL combinedAudioReady = !g_rec.muxFromWav;
            if (g_rec.multiTrack) {
                UINT64 duration100ns = g_rec.rawRecordStop100ns > g_rec.rawRecordStart100ns
                    ? g_rec.rawRecordStop100ns - g_rec.rawRecordStart100ns : 0;
                UINT64 micStart = g_rec.rawRecordStart100ns > g_rec.rawOrigin100ns
                    ? SamplesFor100ns(g_rec.rawRecordStart100ns - g_rec.rawOrigin100ns,
                                      g_rec.rawRate[AUDIO_MICROPHONE]) : 0;
                UINT64 sysStart = g_rec.rawRecordStart100ns > g_rec.rawOrigin100ns
                    ? SamplesFor100ns(g_rec.rawRecordStart100ns - g_rec.rawOrigin100ns,
                                      g_rec.rawRate[AUDIO_SYSTEM]) : 0;
                UINT64 mixFrames = SamplesFor100ns(duration100ns, 48000u);
                RecordProgressRange mixProgress = { 25, 15 };
                combinedAudioReady = g_rec.result->wavPath[0] && mixFrames &&
                    RawMix_BuildStereoWav(
                        g_rec.rawPath[AUDIO_MICROPHONE],
                        g_rec.rawFormat[AUDIO_MICROPHONE].bytes,
                        g_rec.rawFormatBytes[AUDIO_MICROPHONE], micStart,
                        g_rec.rawPath[AUDIO_SYSTEM],
                        g_rec.rawFormat[AUDIO_SYSTEM].bytes,
                        g_rec.rawFormatBytes[AUDIO_SYSTEM], sysStart,
                        48000u, mixFrames, g_rec.result->wavPath,
                        Record_MappedProgress, &mixProgress);
                if (combinedAudioReady)
                    Log_Printf(L"record: timestamp-aligned combined mix rendered from RAW sources after stop");
                else {
                    Fail(L"Could not render the combined AAC source from the captured RAW audio. RAW files were preserved where available.");
                    Log_Printf(L"record: offline combined mix failed");
                }
                g_rec.muxFromWav = TRUE;
            }
            if (g_rec.muxFromWav && g_rec.result->wavPath[0] && combinedAudioReady) {
                Record_PostProgress(g_rec.multiTrack ? 42 : 30);
                {
                    RecordProgressRange remuxProgress = { g_rec.multiTrack ? 42 : 30,
                                                          g_rec.multiTrack ? 22 : 60 };
                    if (Remux_AddWavAudio(g_rec.result->path, g_rec.result->wavPath,
                            Record_MappedProgress, &remuxProgress,
                            g_rec.rawRecordStop100ns > g_rec.rawRecordStart100ns
                                ? g_rec.rawRecordStop100ns - g_rec.rawRecordStart100ns
                                : (UINT64)g_rec.result->seconds * 10000000u)) {
                    combinedAudioReady = TRUE;
                    /* Do not remove the PCM source until all three tracks have
                       been embedded successfully in multitrack mode. */
                    if (!g_rec.multiTrack && !g_rec.cfg.recordWavSidecar) {
                        if (DeleteFileW(g_rec.result->wavPath))
                            g_rec.result->wavPath[0] = 0;
                        else
                            Log_Printf(L"record: the sidecar could not be removed (%lu)",
                                       GetLastError());
                    }
                } else {
                    combinedAudioReady = FALSE;
                    Fail(L"The audio could not be added to the MP4. The video and WAV recovery audio were retained separately.");
                    Log_Printf(L"record: the sound could not be muxed in; the video "
                               L"is silent and the sound is in the .wav");
                    }
                }
            }

            if (g_rec.multiTrack) {
                Record_PostProgress(66);
                if (combinedAudioReady && FinishMultiTrackAudio()) {
                    /* The WAV is useful as a user-requested sidecar. Otherwise
                       it was only an intermediate source for AAC encoding. */
                    BOOL keepWav = g_rec.cfg.recordWavSidecar &&
                                   !g_rec.cfg.recordMuxFromWav;
                    if (!keepWav && g_rec.result->wavPath[0]) {
                        if (DeleteFileW(g_rec.result->wavPath))
                            g_rec.result->wavPath[0] = 0;
                        else
                            Log_Printf(L"record: could not remove intermediate WAV (%lu)",
                                       GetLastError());
                    }
                } else if (!combinedAudioReady) {
                    Fail(L"Multi-track mux skipped because combined AAC could not be prepared; recovery audio files were kept.");
                }
            }
        }

        if (g_rec.hdr) {
            /* 9 = BT.2020 primaries, 16 = SMPTE ST 2084, 9 = BT.2020 NCL, and
               the range is limited because that is what the encoder was given. */
            g_rec.colour.primaries = 9;
            g_rec.colour.transfer  = 16;
            g_rec.colour.matrix    = 9;
            g_rec.colour.fullRange = FALSE;
            Mp4Tag_WriteColour(g_rec.result->path, &g_rec.colour);
        } else if (Encoder_Is10Bit(g_rec.enc) && !g_rec.hdr) {
            /* Rec.709 SDR metadata, independent of the monitor's HDR state. */
            g_rec.colour.primaries = 1;
            g_rec.colour.transfer  = 1;
            g_rec.colour.matrix    = 1;
            g_rec.colour.fullRange = FALSE;
            Mp4Tag_WriteColour(g_rec.result->path, &g_rec.colour);
        }
        if (!g_rec.opt.makeGif)
            Record_PostProgress(99);
    }

    /* For GIF capture the MP4 is an intermediate. Decode it only after the
       sink writer has finalised and closed, then publish the GIF atomically. */
    if (g_rec.result && g_rec.result->ok && g_rec.opt.makeGif &&
        g_rec.finalPath[0] && g_rec.result->path[0]) {
        wchar_t gifFinal[MAX_PATH], gifPartial[MAX_PATH];
        BOOL converted = FALSE;
        Record_PostProgress(25);
        if (SUCCEEDED(StringCchCopyW(gifFinal, ARRAYSIZE(gifFinal), g_rec.finalPath)) &&
            PathRenameExtensionW(gifFinal, L".gif") &&
            SUCCEEDED(StringCchCopyW(gifPartial, ARRAYSIZE(gifPartial), gifFinal)) &&
            PathRenameExtensionW(gifPartial, L".partial.gif")) {
            DeleteFileW(gifPartial);
            converted = Gif_ConvertMp4ToGif(g_rec.result->path, gifPartial, 15000,
                                             Record_GifProgress, NULL);
            if (converted && MoveFileExW(gifPartial, gifFinal, MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(g_rec.result->path); /* intermediate MP4 no longer needed */
                StringCchCopyW(g_rec.result->path, ARRAYSIZE(g_rec.result->path), gifFinal);
                Record_PostProgress(99);
                Log_Printf(L"gif: published %s", gifFinal);
            } else {
                DeleteFileW(gifPartial);
                converted = FALSE;
            }
        }
        if (!converted) {
            Log_Printf(L"gif: conversion failed; retaining the encoded MP4 as fallback");
            StringCchCopyW(g_rec.result->error, ARRAYSIZE(g_rec.result->error),
                           L"GIF export failed; the MP4 recording was saved instead.");
            Record_PostProgress(99);
        }
    }

    /* The writer always targets .partial.mp4. Publish a normal .mp4 only after
       all muxing is done. A process crash leaves a visibly incomplete artifact
       instead of a misleadingly named final recording. */
    if (g_rec.result && g_rec.result->ok && g_rec.finalPath[0] &&
        g_rec.result->path[0] &&
        (lstrlenW(g_rec.result->path) < 4 ||
         lstrcmpiW(g_rec.result->path + lstrlenW(g_rec.result->path) - 4, L".gif"))) {
        wchar_t finalWav[MAX_PATH];
        if (MoveFileExW(g_rec.result->path, g_rec.finalPath, MOVEFILE_WRITE_THROUGH)) {
            StringCchCopyW(g_rec.result->path, ARRAYSIZE(g_rec.result->path),
                           g_rec.finalPath);
            if (g_rec.result->wavPath[0] && PathFileExistsW(g_rec.result->wavPath)) {
                StringCchCopyW(finalWav, ARRAYSIZE(finalWav), g_rec.finalPath);
                if (PathRenameExtensionW(finalWav, L".wav") &&
                    MoveFileExW(g_rec.result->wavPath, finalWav, MOVEFILE_WRITE_THROUGH)) {
                    StringCchCopyW(g_rec.result->wavPath, ARRAYSIZE(g_rec.result->wavPath), finalWav);
                } else {
                    Log_Printf(L"record: completed MP4, but the WAV sidecar remains at %s",
                               g_rec.result->wavPath);
                }
            }
            Record_PostProgress(100);
        } else {
            g_rec.result->ok = FALSE;
            Fail(L"Recording was encoded but could not be published; the .partial.mp4 was retained.");
        }
    }

done:
    InterlockedExchange(&g_rec.active, 0);
    Audio_Stop();
    CloseRawPcmSinks(FALSE);
    /* Only reached with a sink still open when setup failed part-way. */
    if (g_rec.wav) {
        Wav_Close(g_rec.wav);
        g_rec.wav = NULL;
    }

    if (g_rec.writer)  { IMFSinkWriter_Release(g_rec.writer);          g_rec.writer  = NULL; }
    Encoder_ClearVendor();
    PoolClose();       /* after the writer: releasing it is what returns the samples */
    if (g_rec.hw || g_rec.hwOps) {
        TeardownHwEncoder();
    }
    if (g_rec.hwMarked) {
        Settings_MarkDirectRecording(NULL);
        g_rec.hwMarked = FALSE;
    }
    Crash_WatchEnd();
    if (g_rec.hv)      { HdrVideo_Destroy(g_rec.hv);                   g_rec.hv      = NULL; }
    if (g_rec.sdrhv)   { HdrVideo_Destroy(g_rec.sdrhv);                g_rec.sdrhv   = NULL; }
    if (g_rec.staging) { ID3D11Texture2D_Release(g_rec.staging);       g_rec.staging = NULL; }
    TeardownGpu();     /* the manager holds the device; it goes before it */
    if (g_rec.dup)     { IDXGIOutputDuplication_Release(g_rec.dup);    g_rec.dup     = NULL; }
    if (g_rec.ctx)     { ID3D11DeviceContext_Release(g_rec.ctx);       g_rec.ctx     = NULL; }
    if (g_rec.dev)     { ID3D11Device_Release(g_rec.dev);              g_rec.dev     = NULL; }
    if (g_rec.frameTimer) { CloseHandle(g_rec.frameTimer); g_rec.frameTimer = NULL; }

    MFShutdown();
    CoUninitialize();

    {
        RecordResult *res = g_rec.result;
        HWND notify = g_rec.notify;
        UINT doneMsg = g_rec.doneMsg;
        g_rec.result = NULL;
        if (res && res->retryWithoutDirect) {
            /* A pre-first-packet native failure is retried as a fresh MFT
               recording. Remove only that empty attempt's artifacts; keep
               any RAW crash diagnostics from a later-stage failure. */
            if (res->path[0]) DeleteFileW(res->path);
            if (res->wavPath[0]) DeleteFileW(res->wavPath);
            res->path[0] = 0;
            res->wavPath[0] = 0;
            g_rec.finalPath[0] = 0;
            Log_Printf(L"record: removed empty native partial; ready for MFT retry");
        } else if (res && !res->ok) {
            /* Never destroy a partial container or WAV after an encoder failure.
               A partial MP4 may need specialist recovery, but the bytes and any
               RAW sidecars are more useful than deleting the only evidence. */
            Log_Printf(L"record: incomplete artifacts preserved at %s (audio %s)",
                       res->path, res->wavPath);
        }
        if (res && !PostMessageW(notify, doneMsg, 0, (LPARAM)res))
            Record_FreeResult(res);
        /* Keep busy set until this worker has posted its result and is about
           to return. Record_Start waits briefly for this exact completion race
           (the UI can receive PostMessage before this worker clears busy). */
        InterlockedExchange(&g_rec.busy, 0);
    }
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
    {
        BOOL hadEvents = g_rec.stopEvent != NULL || g_rec.resumeEvent != NULL;
        if (g_rec.stopEvent) {
            CloseHandle(g_rec.stopEvent);
            g_rec.stopEvent = NULL;
        }
        if (g_rec.resumeEvent) {
            CloseHandle(g_rec.resumeEvent);
            g_rec.resumeEvent = NULL;
        }
        if (hadEvents)
            DeleteCriticalSection(&g_rec.writeLock);
    }
}

BOOL Record_Start(HWND notify, UINT doneMsg, const RecordOptions *opt)
{
    uintptr_t th;
    unsigned int threadId = 0;

    if (!opt)
        return FALSE;
    if (Record_IsBusy()) {
        HANDLE previous = g_rec.thread;
        /* The recorder posts its completion immediately before clearing busy.
           A fallback started by the UI can arrive in that tiny interval. If
           frames are no longer being captured, wait for that worker to return;
           an active recording is never waited on and still rejects a second start. */
        if (!Record_IsActive() && previous &&
            GetCurrentThreadId() != g_rec.workerThreadId)
            (void)WaitForSingleObject(previous, 5000);
        if (Record_IsBusy())
            return FALSE;
    }

    /* A previous stop that timed out left these behind on purpose. The
       worker has since finished - busy says so - so they can go now. */
    ReleaseThreadObjects();

    ZeroMemory(&g_rec, sizeof(g_rec));
    g_rec.cfg     = g_cfg;
    g_rec.opt     = *opt;
    g_rec.notify  = notify;
    g_rec.doneMsg = doneMsg;
    if (g_rec.opt.fps < 5)  g_rec.opt.fps = 5;
    if (g_rec.opt.fps > 240) g_rec.opt.fps = 240;

    g_rec.result = (RecordResult *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                             sizeof(RecordResult));
    if (!g_rec.result)
        return FALSE;

    InitializeCriticalSection(&g_rec.writeLock);
    g_rec.stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_rec.resumeEvent = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!g_rec.stopEvent || !g_rec.resumeEvent) {
        if (g_rec.stopEvent) CloseHandle(g_rec.stopEvent);
        if (g_rec.resumeEvent) CloseHandle(g_rec.resumeEvent);
        g_rec.stopEvent = g_rec.resumeEvent = NULL;
        DeleteCriticalSection(&g_rec.writeLock);
        Record_FreeResult(g_rec.result);
        g_rec.result = NULL;
        return FALSE;
    }

    /* Before the thread exists, because it is what keeps a second start out
       while this one sets itself up. The worker clears it, and nothing else. */
    InterlockedExchange(&g_rec.busy, 1);

    th = _beginthreadex(NULL, 0, RecordThread, NULL, 0, &threadId);
    if (!th) {
        InterlockedExchange(&g_rec.busy, 0);
        CloseHandle(g_rec.stopEvent);
        CloseHandle(g_rec.resumeEvent);
        g_rec.stopEvent = g_rec.resumeEvent = NULL;
        DeleteCriticalSection(&g_rec.writeLock);
        Record_FreeResult(g_rec.result);
        g_rec.result = NULL;
        return FALSE;
    }
    g_rec.thread = (HANDLE)th;
    g_rec.workerThreadId = (DWORD)threadId;
    return TRUE;
}

void Record_Stop(void)
{
    Record_RequestStop();

    if (g_rec.thread) {
        /* This synchronous path is reserved for final application teardown.
           The normal Stop button requests a stop asynchronously so the saving
           percentage remains visible. Do not let process exit kill the worker
           midway through AAC/FLAC post-processing. */
        (void)WaitForSingleObject(g_rec.thread, INFINITE);
    }
    ReleaseThreadObjects();
}
