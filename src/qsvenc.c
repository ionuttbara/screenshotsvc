/*
 * qsvenc.c - Intel's encoder through oneVPL.
 */

#define COBJMACROS

#include "nitshot.h"
#include "qsvenc.h"
#include "vpconv.h"
#include "log.h"
#include "crash.h"

#include <d3d11_4.h>     /* ID3D11Multithread */
#include <dxgi.h>
#include <setupapi.h>
#include <devguid.h>
#include <strsafe.h>

/* Intel's headers, unmodified; the functions are not linked but looked up
   in the runtime, so only the types are used from them. */
#include "vpl/mfxvideo.h"
#include "vpl/mfximplcaps.h"

/* ------------------------------------------------------------- runtime */

typedef mfxStatus (MFX_CDECL *PFN_Initialize)(mfxInitializationParam, mfxSession *);
typedef mfxStatus (MFX_CDECL *PFN_Close)(mfxSession);
typedef mfxStatus (MFX_CDECL *PFN_QueryVersion)(mfxSession, mfxVersion *);
typedef mfxHDL *  (MFX_CDECL *PFN_QueryImpls)(mfxImplCapsDeliveryFormat, mfxU32 *);
typedef mfxStatus (MFX_CDECL *PFN_ReleaseImpl)(mfxHDL);
typedef mfxStatus (MFX_CDECL *PFN_SetHandle)(mfxSession, mfxHandleType, mfxHDL);
typedef mfxStatus (MFX_CDECL *PFN_SetAllocator)(mfxSession, mfxFrameAllocator *);
typedef mfxStatus (MFX_CDECL *PFN_Sync)(mfxSession, mfxSyncPoint, mfxU32);
typedef mfxStatus (MFX_CDECL *PFN_EncQuery)(mfxSession, mfxVideoParam *, mfxVideoParam *);
typedef mfxStatus (MFX_CDECL *PFN_EncQueryIOSurf)(mfxSession, mfxVideoParam *, mfxFrameAllocRequest *);
typedef mfxStatus (MFX_CDECL *PFN_EncInit)(mfxSession, mfxVideoParam *);
typedef mfxStatus (MFX_CDECL *PFN_EncClose)(mfxSession);
typedef mfxStatus (MFX_CDECL *PFN_EncGetParam)(mfxSession, mfxVideoParam *);
typedef mfxStatus (MFX_CDECL *PFN_EncFrame)(mfxSession, mfxEncodeCtrl *, mfxFrameSurface1 *,
                                            mfxBitstream *, mfxSyncPoint *);

typedef struct {
    HMODULE            dll;
    PFN_Initialize     Initialize;
    PFN_Close          Close;
    PFN_QueryVersion   QueryVersion;
    PFN_QueryImpls     QueryImpls;
    PFN_ReleaseImpl    ReleaseImpl;
    PFN_SetHandle      SetHandle;
    PFN_SetAllocator   SetAllocator;
    PFN_Sync           Sync;
    PFN_EncQuery       EncQuery;
    PFN_EncQueryIOSurf EncQueryIOSurf;
    PFN_EncInit        EncInit;
    PFN_EncClose       EncClose;
    PFN_EncGetParam    EncGetParam;
    PFN_EncFrame       EncFrame;
    wchar_t            path[MAX_PATH];
    wchar_t            why[160];
    BOOL               ok;
} VplRuntime;

static VplRuntime g_vpl;
static SRWLOCK    g_vplLock = SRWLOCK_INIT;

static const wchar_t *StatusText(mfxStatus st)
{
    switch (st) {
    case MFX_ERR_NONE:                     return L"ok";
    case MFX_ERR_UNKNOWN:                  return L"unknown error";
    case MFX_ERR_NULL_PTR:                 return L"null pointer";
    case MFX_ERR_UNSUPPORTED:              return L"unsupported";
    case MFX_ERR_MEMORY_ALLOC:             return L"memory allocation failed";
    case MFX_ERR_NOT_ENOUGH_BUFFER:        return L"buffer too small";
    case MFX_ERR_LOCK_MEMORY:              return L"lock failed";
    case MFX_ERR_NOT_INITIALIZED:          return L"not initialised";
    case MFX_ERR_MORE_DATA:                return L"more data needed";
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return L"incompatible video parameters";
    case MFX_ERR_INVALID_VIDEO_PARAM:      return L"invalid video parameters";
    case MFX_ERR_INVALID_HANDLE:           return L"invalid handle";
    case MFX_ERR_NOT_FOUND:                return L"not found";
    case MFX_ERR_MORE_SURFACE:             return L"more surfaces needed";
    case MFX_ERR_ABORTED:                  return L"aborted";
    case MFX_ERR_DEVICE_LOST:              return L"device lost";
    case MFX_ERR_UNDEFINED_BEHAVIOR:       return L"undefined behaviour";
    case MFX_ERR_DEVICE_FAILED:            return L"device failed";
    case MFX_WRN_DEVICE_BUSY:              return L"device busy";
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM: return L"parameters corrected";
    default:                               return L"see status code";
    }
}

/* Whether the path is a folder under %SystemRoot%\System32\DriverStore. */
BOOL Qsv_IsDriverStorePath(const wchar_t *dir)
{
    wchar_t root[MAX_PATH], full[MAX_PATH];
    size_t  n;

    if (!dir || !dir[0])
        return FALSE;
    if (!GetSystemDirectoryW(root, MAX_PATH) ||
        FAILED(StringCchCatW(root, MAX_PATH, L"\\DriverStore\\")))
        return FALSE;
    if (!GetFullPathNameW(dir, MAX_PATH, full, NULL))
        return FALSE;
    /* A path that climbs out again ("..") resolves outside and fails here. */
    n = wcslen(root);
    return CompareStringOrdinal(full, (int)min(wcslen(full), n), root, (int)n, TRUE) == CSTR_EQUAL &&
           wcslen(full) > n;
}

/*
 * The runtime's folder, from the driver key of the Intel display adapter
 * whose PCI device id is 'deviceId' - the value Intel's dispatcher reads.
 */
static BOOL FindDriverStoreRuntime(UINT deviceId, wchar_t *path, size_t cch)
{
    HDEVINFO        set;
    SP_DEVINFO_DATA info;
    DWORD           i;
    BOOL            found = FALSE;
    wchar_t         want[32];

    StringCchPrintfW(want, ARRAYSIZE(want), L"DEV_%04X", deviceId);
    set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return FALSE;
    info.cbSize = sizeof(info);
    for (i = 0; !found && SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        wchar_t ids[1024], dir[MAX_PATH];
        DWORD   type = 0, size = sizeof(dir);
        HKEY    key;

        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, NULL,
                                               (BYTE *)ids, sizeof(ids) - sizeof(wchar_t), NULL))
            continue;
        ids[ARRAYSIZE(ids) - 1] = 0;
        if (!wcsstr(ids, L"VEN_8086") || (deviceId && !wcsstr(ids, want)))
            continue;
        key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        if (RegQueryValueExW(key, L"DriverStorePathForVPL", NULL, &type, (BYTE *)dir, &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ) && size >= sizeof(wchar_t)) {
            dir[min(size / sizeof(wchar_t), ARRAYSIZE(dir) - 1)] = 0;
            Log_Printf(L"qsv: driver key of %s says the runtime is in %s", ids, dir);
            if (!Qsv_IsDriverStorePath(dir))
                Log_Printf(L"qsv: that is not under System32\\DriverStore, not loaded");
            else if (SUCCEEDED(StringCchPrintfW(path, cch, L"%s\\libmfx64-gen.dll", dir)))
                found = TRUE;
        }
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

#define GET(field, type, name) \
    (rt->field = (type)(void *)GetProcAddress(rt->dll, name)) != NULL

/*
 * 'name' NULL: the runtime of the Intel adapter with this device id, from
 * the DriverStore, or else a libmfx64-gen.dll in System32. Otherwise that
 * DLL from System32 (for the tests' missing- and wrong-runtime paths).
 */
static BOOL LoadRuntime(const wchar_t *name, UINT deviceId, VplRuntime *rt)
{
    ZeroMemory(rt, sizeof(*rt));
    if (!name && FindDriverStoreRuntime(deviceId, rt->path, ARRAYSIZE(rt->path)))
        rt->dll = LoadLibraryExW(rt->path, NULL,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!rt->dll) {
        StringCchCopyW(rt->path, ARRAYSIZE(rt->path), name ? name : L"libmfx64-gen.dll");
        rt->dll = LoadLibraryExW(rt->path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!rt->dll) {
        StringCchPrintfW(rt->why, ARRAYSIZE(rt->why),
                         L"no Intel oneVPL runtime (%lu) - Intel Arc or Xe graphics needed", GetLastError());
        return FALSE;
    }
    if (!(GET(Initialize, PFN_Initialize, "MFXInitialize") && GET(Close, PFN_Close, "MFXClose") &&
          GET(QueryVersion, PFN_QueryVersion, "MFXQueryVersion") && GET(QueryImpls, PFN_QueryImpls, "MFXQueryImplsDescription") &&
          GET(ReleaseImpl, PFN_ReleaseImpl, "MFXReleaseImplDescription") && GET(SetHandle, PFN_SetHandle, "MFXVideoCORE_SetHandle") &&
          GET(SetAllocator, PFN_SetAllocator, "MFXVideoCORE_SetFrameAllocator") && GET(Sync, PFN_Sync, "MFXVideoCORE_SyncOperation") &&
          GET(EncQuery, PFN_EncQuery, "MFXVideoENCODE_Query") && GET(EncQueryIOSurf, PFN_EncQueryIOSurf, "MFXVideoENCODE_QueryIOSurf") &&
          GET(EncInit, PFN_EncInit, "MFXVideoENCODE_Init") && GET(EncClose, PFN_EncClose, "MFXVideoENCODE_Close") &&
          GET(EncGetParam, PFN_EncGetParam, "MFXVideoENCODE_GetVideoParam") &&
          GET(EncFrame, PFN_EncFrame, "MFXVideoENCODE_EncodeFrameAsync"))) {
        StringCchCopyW(rt->why, ARRAYSIZE(rt->why),
                       L"not a oneVPL 2.x runtime (an older Intel GPU's runtime, or another DLL)");
        FreeLibrary(rt->dll);
        rt->dll = NULL;
        return FALSE;
    }
    rt->ok = TRUE;
    Log_Printf(L"qsv: runtime %s", rt->path);
    return TRUE;
}
#undef GET

static UINT AdapterDeviceId(ID3D11Device *dev)
{
    IDXGIDevice  *dxgi = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC d;
    UINT id = 0;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi))) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &ad))) {
            if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d)))
                id = d.DeviceId;
            IDXGIAdapter_Release(ad);
        }
        IDXGIDevice_Release(dxgi);
    }
    return id;
}

/* The shared runtime, loaded once and kept, as with AMF. A test's other
   DLL is loaded on its own and dropped again. */
static VplRuntime *Runtime(const wchar_t *dll, UINT deviceId, VplRuntime *scratch)
{
    if (dll) {
        LoadRuntime(dll, deviceId, scratch);
        return scratch;
    }
    AcquireSRWLockExclusive(&g_vplLock);
    if (!g_vpl.ok && !g_vpl.why[0])
        LoadRuntime(NULL, deviceId, &g_vpl);
    ReleaseSRWLockExclusive(&g_vplLock);
    return &g_vpl;
}

static void DropScratch(VplRuntime *rt, VplRuntime *scratch)
{
    if (rt == scratch && scratch->dll)
        FreeLibrary(scratch->dll);
}

/*
 * A session on the hardware implementation for this adapter, with the
 * recorder's D3D11 device handed over so the encoder works where the
 * capture's textures are. The implementations the runtime offers are all
 * logged; the one whose device id matches is taken.
 */
static mfxSession OpenSession(VplRuntime *rt, ID3D11Device *dev, BOOL handOver, mfxVersion *ver,
                              wchar_t *why, size_t cch)
{
    mfxInitializationParam ip;
    mfxSession  s = NULL;
    mfxStatus   st;
    mfxU32      n = 0, i;
    mfxHDL     *impls;
    UINT        deviceId = AdapterDeviceId(dev);
    char        want[16];

    ZeroMemory(&ip, sizeof(ip));
    ip.AccelerationMode = MFX_ACCEL_MODE_VIA_D3D11;
    sprintf_s(want, sizeof(want), "%x", deviceId);
    impls = rt->QueryImpls(MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &n);
    for (i = 0; impls && i < n; ++i) {
        const mfxImplDescription *d = (const mfxImplDescription *)impls[i];
        if (!d || d->Version.Major != 1)
            continue;
        Log_Printf(L"qsv: implementation %u: %hs, API %u.%u, %s, vendor %04X, impl id %u, device %hs",
                   i, d->ImplName, d->ApiVersion.Major, d->ApiVersion.Minor,
                   d->Impl == MFX_IMPL_TYPE_HARDWARE ? L"hardware" : L"software",
                   d->VendorID, d->VendorImplID, d->Dev.DeviceID);
        if (d->Impl == MFX_IMPL_TYPE_HARDWARE && _strnicmp(d->Dev.DeviceID, want, strlen(want)) == 0)
            ip.VendorImplID = d->VendorImplID;
    }
    if (impls)
        rt->ReleaseImpl(impls);

    st = rt->Initialize(ip, &s);
    if (st != MFX_ERR_NONE || !s) {
        StringCchPrintfW(why, cch, L"the Intel runtime would not start a session (%d, %s)", (int)st, StatusText(st));
        return NULL;
    }
    ZeroMemory(ver, sizeof(*ver));
    rt->QueryVersion(s, ver);
    if (!handOver)
        return s;                       /* the runtime makes its own device */
    st = rt->SetHandle(s, MFX_HANDLE_D3D11_DEVICE, dev);
    if (st != MFX_ERR_NONE) {
        StringCchPrintfW(why, cch, L"the Intel runtime would not take the graphics device (%d, %s)",
                         (int)st, StatusText(st));
        rt->Close(s);
        return NULL;
    }
    return s;
}

/* ------------------------------------------------------- parameters */

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

typedef struct {
    mfxVideoParam                    par;
    mfxExtCodingOption2              co2;
    mfxExtVideoSignalInfo            vsi;
    mfxExtMasteringDisplayColourVolume mdcv;
    mfxExtBuffer                    *ext[3];
} VplParams;

static void InitExt(mfxExtBuffer *h, mfxU32 id, mfxU32 size)
{
    ZeroMemory(h, size);
    h->BufferId = id;
    h->BufferSz = size;
}

static mfxU16 XY(float v)
{
    double u = v * 50000.0 + 0.5;
    return (mfxU16)(u < 0 ? 0 : u > 50000 ? 50000 : u);
}

/* The configuration, before the runtime has had its say. */
static void BuildParams(VplParams *p, const HwEncConfig *c, int bFrames, int lookahead)
{
    mfxFrameInfo *fi = &p->par.mfx.FrameInfo;
    UINT32        maxKbps = c->bitrate * 2 / 1000, mult = 1;

    ZeroMemory(p, sizeof(*p));
    p->par.IOPattern  = c->videoMemory ? MFX_IOPATTERN_IN_VIDEO_MEMORY : MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    p->par.AsyncDepth = 1;              /* one frame at a time, synced at once */
    p->par.mfx.CodecId = c->hevc ? MFX_CODEC_HEVC : MFX_CODEC_AVC;
    p->par.mfx.CodecProfile = (mfxU16)(c->hevc ? (c->tenBit ? MFX_PROFILE_HEVC_MAIN10 : MFX_PROFILE_HEVC_MAIN)
                                               : MFX_PROFILE_AVC_HIGH);
    /* TargetUsage 1 is best quality, 7 fastest: the reverse of 'preset'. */
    p->par.mfx.TargetUsage = (mfxU16)(8 - (c->preset < 1 ? 1 : c->preset > 7 ? 7 : c->preset));
    p->par.mfx.GopPicSize  = (mfxU16)(c->fps * 2);
    p->par.mfx.GopRefDist  = (mfxU16)(bFrames + 1);
    p->par.mfx.GopOptFlag  = MFX_GOP_CLOSED;
    p->par.mfx.IdrInterval = 0;         /* every I-frame an IDR */
    p->par.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
    /* Rates are 16-bit kbit/s, scaled by a multiplier when they do not fit. */
    while (maxKbps / mult > 65535)
        ++mult;
    p->par.mfx.BRCParamMultiplier = (mfxU16)mult;
    p->par.mfx.TargetKbps = (mfxU16)(c->bitrate / 1000 / mult);
    p->par.mfx.MaxKbps    = (mfxU16)(maxKbps / mult);

    fi->FourCC        = c->tenBit ? MFX_FOURCC_P010 : MFX_FOURCC_NV12;
    fi->ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
    fi->PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
    fi->FrameRateExtN = (mfxU32)c->fps;
    fi->FrameRateExtD = 1;
    /* HEVC wants its frame size in 32s, H.264 in 16s; the crop is the
       picture actually shown. */
    fi->Width  = (mfxU16)ALIGN(c->width,  c->hevc ? 32 : 16);
    fi->Height = (mfxU16)ALIGN(c->height, c->hevc ? 32 : 16);
    fi->CropW  = (mfxU16)c->width;
    fi->CropH  = (mfxU16)c->height;
    if (c->tenBit) {
        fi->BitDepthLuma = fi->BitDepthChroma = 10;
        fi->Shift = 1;                  /* P010 keeps its bits at the top */
    }

    InitExt(&p->co2.Header, MFX_EXTBUFF_CODING_OPTION2, sizeof(p->co2));
    p->co2.LookAheadDepth = (mfxU16)lookahead;
    if (bFrames > 1)
        p->co2.BRefType = MFX_B_REF_PYRAMID;
    /* Macroblock-level rate control is Intel's adaptive quantisation: off
       for screens, as on the other paths, unless asked for. */
    p->co2.MBBRC = (mfxU16)(c->aq ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF);
    p->co2.RepeatPPS = MFX_CODINGOPTION_OFF;

    InitExt(&p->vsi.Header, MFX_EXTBUFF_VIDEO_SIGNAL_INFO, sizeof(p->vsi));
    p->vsi.VideoFormat              = 5;          /* unspecified */
    p->vsi.VideoFullRange           = 0;
    p->vsi.ColourDescriptionPresent = 1;
    p->vsi.ColourPrimaries          = (mfxU16)(c->hdr ? 9 : 1);
    p->vsi.TransferCharacteristics  = (mfxU16)(c->hdr ? 16 : 1);
    p->vsi.MatrixCoefficients       = (mfxU16)(c->hdr ? 9 : 1);

    p->ext[0] = &p->co2.Header;
    p->ext[1] = &p->vsi.Header;
    p->par.NumExtParam = 2;
    if (c->hdr && c->whiteX > 0.0f && c->greenX > 0.0f) {
        /* Encoding uses H.265 Annex D's order and units: green, blue, red in
           0.00002, luminance in 0.0001 cd/m2 (the header's RGB order is for
           video processing). */
        InitExt(&p->mdcv.Header, MFX_EXTBUFF_MASTERING_DISPLAY_COLOUR_VOLUME, sizeof(p->mdcv));
        p->mdcv.InsertPayloadToggle  = MFX_PAYLOAD_IDR;
        p->mdcv.DisplayPrimariesX[0] = XY(c->greenX); p->mdcv.DisplayPrimariesY[0] = XY(c->greenY);
        p->mdcv.DisplayPrimariesX[1] = XY(c->blueX);  p->mdcv.DisplayPrimariesY[1] = XY(c->blueY);
        p->mdcv.DisplayPrimariesX[2] = XY(c->redX);   p->mdcv.DisplayPrimariesY[2] = XY(c->redY);
        p->mdcv.WhitePointX = XY(c->whiteX);
        p->mdcv.WhitePointY = XY(c->whiteY);
        p->mdcv.MaxDisplayMasteringLuminance = (mfxU32)(c->maxNits * 10000.0f + 0.5f);
        p->mdcv.MinDisplayMasteringLuminance = (mfxU32)(c->minNits * 10000.0f + 0.5f);
        p->ext[2] = &p->mdcv.Header;
        p->par.NumExtParam = 3;
    }
    p->par.ExtParam = p->ext;
}

/* What the runtime made of the configuration, field by field that matters. */
static void LogChanges(const VplParams *want, const VplParams *got)
{
#define CHG(f, name) if (want->f != got->f) \
        Log_Printf(L"qsv:   %s %u -> %u", name, (unsigned)want->f, (unsigned)got->f)
    CHG(par.mfx.CodecProfile, L"profile");
    CHG(par.mfx.TargetUsage, L"target usage");
    CHG(par.mfx.GopPicSize, L"GOP size");
    CHG(par.mfx.GopRefDist, L"GOP reference distance (B-frames + 1)");
    CHG(par.mfx.RateControlMethod, L"rate control");
    CHG(par.mfx.TargetKbps, L"target kbit/s");
    CHG(par.mfx.MaxKbps, L"maximum kbit/s");
    CHG(par.mfx.FrameInfo.FourCC, L"FourCC");
    CHG(co2.LookAheadDepth, L"lookahead");
    CHG(co2.BRefType, L"B-pyramid");
    CHG(co2.MBBRC, L"MBBRC");
#undef CHG
}

/* Asks the runtime what it would make of a configuration. */
static mfxStatus Probe(VplRuntime *rt, mfxSession s, const HwEncConfig *c, int b, int la, VplParams *out)
{
    VplParams in;
    mfxStatus st;
    BuildParams(&in, c, b, la);
    BuildParams(out, c, b, la);
    st = rt->EncQuery(s, &in.par, &out->par);
    if (st != MFX_ERR_NONE)
        Log_Printf(L"qsv: query of %s%s with %d B-frame(s), lookahead %d: %d (%s)",
                   c->hevc ? L"HEVC" : L"H.264", c->tenBit ? L" Main10" : L"", b, la,
                   (int)st, StatusText(st));
    if (st >= MFX_ERR_NONE)
        LogChanges(&in, out);
    return st;
}

/* ---------------------------------------------------------------- query */

/* What the installed runtime said for an adapter, kept for the process:
   the probe opens a session and asks three configurations - about 0.4 s
   on an Arc 140V, and every recording asked twice. The driver does not
   change under a running process in any way that matters here. */
static SRWLOCK   g_capsLock = SRWLOCK_INIT;
static UINT      g_capsDevice;
static HwEncInfo g_caps;

/* Lookahead the runtime refused to start with, per codec (H.264, HEVC):
   asked for once, not before every recording again. */
static volatile LONG g_laRefused[2];

static BOOL QueryUncached(ID3D11Device *dev, const wchar_t *dll, HwEncInfo *info);

BOOL Qsv_Query(ID3D11Device *dev, const wchar_t *dll, HwEncInfo *info)
{
    UINT id;
    BOOL ok;

    if (!info)
        return FALSE;
    if (dll || !dev)
        return QueryUncached(dev, dll, info);
    id = AdapterDeviceId(dev);
    AcquireSRWLockShared(&g_capsLock);
    ok = g_capsDevice && g_capsDevice == id;
    if (ok)
        *info = g_caps;
    ReleaseSRWLockShared(&g_capsLock);
    if (ok)
        return TRUE;
    if (!QueryUncached(dev, NULL, info))
        return FALSE;
    AcquireSRWLockExclusive(&g_capsLock);
    g_caps = *info;
    g_capsDevice = id;
    ReleaseSRWLockExclusive(&g_capsLock);
    return TRUE;
}

static BOOL QueryUncached(ID3D11Device *dev, const wchar_t *dll, HwEncInfo *info)
{
    VplRuntime  scratch;
    VplRuntime *rt;
    mfxSession  s;
    mfxVersion  ver;
    HwEncConfig c;
    VplParams   p;
    int         k;

    if (!info)
        return FALSE;
    ZeroMemory(info, sizeof(*info));
    if (!dev) {
        StringCchCopyW(info->why, ARRAYSIZE(info->why), L"no graphics device");
        goto done;
    }
    rt = Runtime(dll, AdapterDeviceId(dev), &scratch);
    if (!rt->ok) {
        StringCchCopyW(info->why, ARRAYSIZE(info->why), rt->why);
        DropScratch(rt, &scratch);
        goto done;
    }
    s = OpenSession(rt, dev, TRUE, &ver, info->why, ARRAYSIZE(info->why));
    if (!s) {
        DropScratch(rt, &scratch);
        goto done;
    }
    info->driverMajor = ver.Major;
    info->driverMinor = ver.Minor;

    /* Each codec asked with what the recorder would want: 1080p30, B-frames
       and lookahead; what comes back says what the chip can do. */
    for (k = 0; k < 2; ++k) {
        ZeroMemory(&c, sizeof(c));
        c.hevc = k == 1;
        c.width = 1920; c.height = 1080; c.fps = 30; c.bitrate = 8000000; c.preset = 5;
        if (Probe(rt, s, &c, 3, 16, &p) >= MFX_ERR_NONE) {
            if (k == 0) info->caps.h264 = TRUE; else info->caps.hevc = TRUE;
            info->caps.maxB[k] = p.par.mfx.GopRefDist > 1 ? p.par.mfx.GopRefDist - 1 : 0;
            info->caps.lookahead[k] = p.co2.LookAheadDepth > 0;
            info->caps.bRef[k] = p.co2.BRefType == MFX_B_REF_PYRAMID;
        }
    }
    if (info->caps.hevc) {
        ZeroMemory(&c, sizeof(c));
        c.hevc = c.tenBit = TRUE;
        c.width = 1920; c.height = 1080; c.fps = 30; c.bitrate = 8000000; c.preset = 5;
        info->caps.hevc10 = Probe(rt, s, &c, 0, 0, &p) >= MFX_ERR_NONE &&
                            p.par.mfx.CodecProfile == MFX_PROFILE_HEVC_MAIN10;
    }
    rt->Close(s);
    DropScratch(rt, &scratch);
    if (info->caps.h264 || info->caps.hevc)
        info->available = TRUE;
    else
        StringCchCopyW(info->why, ARRAYSIZE(info->why), L"the Intel encoder takes neither H.264 nor HEVC here");

done:
    if (info->available)
        Log_Printf(L"qsv: API %u.%u; H.264 %s, B-frames %d%s; HEVC %s%s, B-frames %d%s",
                   info->driverMajor, info->driverMinor,
                   info->caps.h264 ? L"yes" : L"no", info->caps.maxB[0],
                   info->caps.lookahead[0] ? L", lookahead" : L"",
                   info->caps.hevc ? L"yes" : L"no", info->caps.hevc10 ? L" + Main10" : L"",
                   info->caps.maxB[1], info->caps.lookahead[1] ? L", lookahead" : L"");
    else
        Log_Printf(L"qsv: not used - %s", info->why);
    return info->available;
}

/* ------------------------------------------------------ frame allocator */

/*
 * What the encoder asks for in video memory: its input frames (which
 * Nitshot also allocates through here), its reconstructed frames, and on
 * some drivers bitstream buffers as P8 "frames". Textures are made as
 * Intel's own sample allocator makes them - an array for decoder-target
 * requests, single textures for processor targets, a staging buffer for P8
 * - and each memory id carries the (texture, slice) pair GetHDL hands back.
 */
typedef struct {
    mfxHDLPair       pair;          /* first: ID3D11Resource*, second: slice */
    ID3D11Resource  *res;           /* the texture or buffer, one ref each id */
    BOOL             buffer;        /* P8: a staging buffer */
    UINT             size;
} VplMid;

#define MAX_ALLOCS 8

typedef struct {
    mfxFrameAllocator     base;         /* must be first */
    ID3D11Device         *dev;
    ID3D11DeviceContext  *ctx;
    struct {
        mfxMemId *mids;
        VplMid   *store;
        mfxU16    count;
    } alloc[MAX_ALLOCS];
} VplAllocator;

static mfxStatus MFX_CDECL AllocFrames(mfxHDL pthis, mfxFrameAllocRequest *req, mfxFrameAllocResponse *resp)
{
    VplAllocator *a = (VplAllocator *)pthis;
    mfxU16        n = req->NumFrameSuggested, i;
    int           slot;
    DXGI_FORMAT   fmt;
    BOOL          p8 = req->Info.FourCC == MFX_FOURCC_P8;
    BOOL          array = !p8 && (req->Type & MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET) &&
                          !(req->Type & MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET);
    HRESULT       hr = S_OK;
    VplMid       *store;
    mfxMemId     *mids;

    Log_Printf(L"qsv: allocate %u x %ux%u FourCC %.4hs, type %04X%s", n, req->Info.Width,
               req->Info.Height, (const char *)&req->Info.FourCC, req->Type, array ? L" (array)" : L"");
    if (!(req->Type & (MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET | MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET |
                       MFX_MEMTYPE_VIDEO_MEMORY_ENCODER_TARGET)))
        return MFX_ERR_UNSUPPORTED;          /* system memory is the runtime's own */
    if (p8)                 fmt = DXGI_FORMAT_UNKNOWN;
    else if (req->Info.FourCC == MFX_FOURCC_NV12) fmt = DXGI_FORMAT_NV12;
    else if (req->Info.FourCC == MFX_FOURCC_P010) fmt = DXGI_FORMAT_P010;
    else return MFX_ERR_UNSUPPORTED;
    for (slot = 0; slot < MAX_ALLOCS && a->alloc[slot].mids; ++slot)
        ;
    if (slot == MAX_ALLOCS || n == 0)
        return MFX_ERR_MEMORY_ALLOC;
    store = (VplMid *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VplMid) * n);
    mids = (mfxMemId *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(mfxMemId) * n);
    if (!store || !mids) {
        if (store) HeapFree(GetProcessHeap(), 0, store);
        if (mids) HeapFree(GetProcessHeap(), 0, mids);
        return MFX_ERR_MEMORY_ALLOC;
    }

    if (p8) {
        for (i = 0; i < n && SUCCEEDED(hr); ++i) {
            D3D11_BUFFER_DESC bd;
            ID3D11Buffer *b = NULL;
            ZeroMemory(&bd, sizeof(bd));
            bd.ByteWidth = (UINT)req->Info.Width * req->Info.Height;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = ID3D11Device_CreateBuffer(a->dev, &bd, NULL, &b);
            store[i].res = (ID3D11Resource *)b;
            store[i].buffer = TRUE;
            store[i].size = bd.ByteWidth;
        }
    } else {
        D3D11_TEXTURE2D_DESC d;
        ZeroMemory(&d, sizeof(d));
        d.Width = req->Info.Width;
        d.Height = req->Info.Height;
        d.MipLevels = 1;
        d.ArraySize = array ? n : 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = (req->Type & MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET) ? D3D11_BIND_RENDER_TARGET
                                                                              : D3D11_BIND_DECODER;
        if (req->Type & MFX_MEMTYPE_SHARED_RESOURCE)
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if ((req->Type & MFX_MEMTYPE_VIDEO_MEMORY_ENCODER_TARGET) &&
            !(req->Type & (MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET | MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET))) {
            /* An encoder target: what the video encoder binding allows, else
               a plain render target - both take the GPU copy of a frame. */
            ID3D11Texture2D *probe = NULL;
            d.BindFlags = D3D11_BIND_VIDEO_ENCODER;
            if (FAILED(ID3D11Device_CreateTexture2D(a->dev, &d, NULL, &probe)))
                d.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (probe)
                ID3D11Texture2D_Release(probe);
            Log_Printf(L"qsv:   encoder target, bound as %s",
                       d.BindFlags == D3D11_BIND_VIDEO_ENCODER ? L"video encoder" : L"render target");
        }
        if (array) {
            ID3D11Texture2D *t = NULL;
            hr = ID3D11Device_CreateTexture2D(a->dev, &d, NULL, &t);
            for (i = 0; i < n && SUCCEEDED(hr); ++i) {
                store[i].res = (ID3D11Resource *)t;
                if (i) ID3D11Texture2D_AddRef(t);
                store[i].pair.second = (mfxHDL)(size_t)i;
            }
        } else {
            for (i = 0; i < n && SUCCEEDED(hr); ++i) {
                ID3D11Texture2D *t = NULL;
                hr = ID3D11Device_CreateTexture2D(a->dev, &d, NULL, &t);
                store[i].res = (ID3D11Resource *)t;
            }
        }
    }
    if (FAILED(hr)) {
        Log_Printf(L"qsv: allocation failed (0x%08lX)", (unsigned long)hr);
        for (i = 0; i < n; ++i)
            if (store[i].res) ID3D11Resource_Release(store[i].res);
        HeapFree(GetProcessHeap(), 0, store);
        HeapFree(GetProcessHeap(), 0, mids);
        return MFX_ERR_MEMORY_ALLOC;
    }
    for (i = 0; i < n; ++i) {
        store[i].pair.first = store[i].res;
        mids[i] = &store[i];
    }
    a->alloc[slot].mids = mids;
    a->alloc[slot].store = store;
    a->alloc[slot].count = n;
    resp->mids = mids;
    resp->NumFrameActual = n;
    return MFX_ERR_NONE;
}

/* Only the P8 bitstream buffers are ever read by the CPU. */
static mfxStatus MFX_CDECL LockFrame(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
    VplAllocator *a = (VplAllocator *)pthis;
    VplMid       *m = (VplMid *)mid;
    D3D11_MAPPED_SUBRESOURCE map;
    if (!m || !ptr || !m->buffer) {
        /* Not expected for video memory; worth seeing if a runtime does. */
        Log_Printf(L"qsv: the runtime asked to lock a video frame (%p), refused", mid);
        return MFX_ERR_UNSUPPORTED;
    }
    if (FAILED(ID3D11DeviceContext_Map(a->ctx, m->res, 0, D3D11_MAP_READ, 0, &map)))
        return MFX_ERR_LOCK_MEMORY;
    ptr->Y = (mfxU8 *)map.pData;
    ptr->Pitch = (mfxU16)min(map.RowPitch, 0xFFFF);
    return MFX_ERR_NONE;
}

static mfxStatus MFX_CDECL UnlockFrame(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
    VplAllocator *a = (VplAllocator *)pthis;
    VplMid       *m = (VplMid *)mid;
    if (!m || !m->buffer)
        return MFX_ERR_UNSUPPORTED;
    ID3D11DeviceContext_Unmap(a->ctx, m->res, 0);
    if (ptr)
        ptr->Y = NULL;
    return MFX_ERR_NONE;
}

static mfxStatus MFX_CDECL GetFrameHDL(mfxHDL pthis, mfxMemId mid, mfxHDL *handle)
{
    VplMid *m = (VplMid *)mid;
    (void)pthis;
    if (!m || !handle)
        return MFX_ERR_INVALID_HANDLE;
    *handle = (mfxHDL)&m->pair;
    return MFX_ERR_NONE;
}

static mfxStatus MFX_CDECL FreeFrames(mfxHDL pthis, mfxFrameAllocResponse *resp)
{
    VplAllocator *a = (VplAllocator *)pthis;
    int slot;
    if (!resp)
        return MFX_ERR_NULL_PTR;
    for (slot = 0; slot < MAX_ALLOCS; ++slot) {
        if (a->alloc[slot].mids && a->alloc[slot].mids == resp->mids) {
            mfxU16 i;
            for (i = 0; i < a->alloc[slot].count; ++i)
                if (a->alloc[slot].store[i].res)
                    ID3D11Resource_Release(a->alloc[slot].store[i].res);
            HeapFree(GetProcessHeap(), 0, a->alloc[slot].store);
            HeapFree(GetProcessHeap(), 0, a->alloc[slot].mids);
            ZeroMemory(&a->alloc[slot], sizeof(a->alloc[slot]));
            resp->mids = NULL;
            resp->NumFrameActual = 0;
            return MFX_ERR_NONE;
        }
    }
    return MFX_ERR_NONE;
}

/* ------------------------------------------------------------- encoding */

#define TS_RING 256

struct QsvEncoder {
    VplRuntime          *rt;
    mfxSession           s;
    VplAllocator         alloc;
    mfxFrameAllocResponse inResp;       /* the input frames */
    mfxU16               inType;        /* ...and the memory type asked for */
    mfxFrameSurface1    *surf;
    mfxU16               surfCount;
    BOOL                 encInit;
    ID3D11Device        *dev;
    ID3D11DeviceContext *dctx;
    ID3D11Multithread   *mt;
    VpConv              *vp;            /* BGRA input: to NV12 first */
    ID3D11Texture2D     *nv12;          /* ...into this */
    BOOL                 sysMem;        /* frames handed over in system memory: */
    ID3D11Texture2D     *stage;         /* ...read back through this */
    BYTE                *sysBuf;        /* ...into these, one block per surface */
    UINT32               pitch, bpp;    /* ...rows of the blocks; bytes a sample */
    BOOL                 hevc;
    DXGI_FORMAT          input;
    UINT32               width, height;
    LONGLONG             duration;
    int                  delay;
    mfxBitstream         bs;
    LONGLONG             inPts[TS_RING];   /* arrival order, 100 ns */
    mfxU64               inTs[TS_RING];    /* the same, as the 90 kHz stamps */
    UINT64               sent, got;
    BYTE                 seq[1024];
    UINT32               seqLen;
    BOOL                 failed;
};

BOOL Qsv_AppendNal(const BYTE *nal, UINT32 len, BYTE *out, UINT32 cap, UINT32 *used)
{
    BOOL coded = len >= 4 && nal[0] == 0 && nal[1] == 0 &&
                 (nal[2] == 1 || (nal[2] == 0 && nal[3] == 1));
    UINT32 need = len + (coded ? 0 : 4);
    if (!nal || len == 0 || *used + need > cap)
        return FALSE;
    if (!coded) {
        out[*used] = 0; out[*used + 1] = 0; out[*used + 2] = 0; out[*used + 3] = 1;
        *used += 4;
    }
    memcpy(out + *used, nal, len);
    *used += len;
    return TRUE;
}

static BOOL ReadHeaders(QsvEncoder *e)
{
    mfxVideoParam            par;
    mfxExtCodingOptionSPSPPS sp;
    mfxExtCodingOptionVPS    vp;
    mfxExtBuffer            *ext[2];
    BYTE                     sps[512], pps[256], vps[256];
    mfxStatus                st;

    ZeroMemory(&par, sizeof(par));
    ZeroMemory(&vp, sizeof(vp));
    InitExt(&sp.Header, MFX_EXTBUFF_CODING_OPTION_SPSPPS, sizeof(sp));
    sp.SPSBuffer = sps; sp.SPSBufSize = sizeof(sps);
    sp.PPSBuffer = pps; sp.PPSBufSize = sizeof(pps);
    ext[0] = &sp.Header;
    par.NumExtParam = 1;
    if (e->hevc) {
        InitExt(&vp.Header, MFX_EXTBUFF_CODING_OPTION_VPS, sizeof(vp));
        vp.VPSBuffer = vps; vp.VPSBufSize = sizeof(vps);
        ext[1] = &vp.Header;
        par.NumExtParam = 2;
    }
    par.ExtParam = ext;
    st = e->rt->EncGetParam(e->s, &par);
    if (st != MFX_ERR_NONE) {
        Log_Printf(L"qsv: no parameter sets (%d, %s)", (int)st, StatusText(st));
        return FALSE;
    }
    e->seqLen = 0;
    if ((e->hevc && !Qsv_AppendNal(vps, vp.VPSBufSize, e->seq, sizeof(e->seq), &e->seqLen)) ||
        !Qsv_AppendNal(sps, sp.SPSBufSize, e->seq, sizeof(e->seq), &e->seqLen) ||
        !Qsv_AppendNal(pps, sp.PPSBufSize, e->seq, sizeof(e->seq), &e->seqLen))
        return FALSE;
    Log_Printf(L"qsv: parameter sets %u bytes%s", e->seqLen, e->hevc ? L" (VPS, SPS, PPS)" : L" (SPS, PPS)");
    /* The bitstream buffer is sized from what the encoder says it needs. */
    {
        mfxVideoParam q;
        mfxU32        bytes;
        ZeroMemory(&q, sizeof(q));
        if (e->rt->EncGetParam(e->s, &q) == MFX_ERR_NONE) {
            bytes = (mfxU32)q.mfx.BufferSizeInKB * 1000 * (q.mfx.BRCParamMultiplier ? q.mfx.BRCParamMultiplier : 1);
            if (bytes < e->width * e->height * 2)
                bytes = e->width * e->height * 2;
            e->bs.MaxLength = bytes;
            e->bs.Data = (mfxU8 *)HeapAlloc(GetProcessHeap(), 0, bytes);
            e->delay = q.mfx.GopRefDist > 1 ? q.mfx.GopRefDist - 1 : 0;
            Log_Printf(L"qsv: encoding with profile %u, target usage %u, GOP %u/%u, %u kbit/s, "
                       L"bitstream buffer %u KB", q.mfx.CodecProfile, q.mfx.TargetUsage,
                       q.mfx.GopPicSize, q.mfx.GopRefDist, q.mfx.TargetKbps, bytes / 1000);
        }
    }
    return e->bs.Data != NULL;
}

/* One attempt at an encoder with these B-frames and this lookahead.
   '*initFailed' says the runtime took everything but refused Init. */
static QsvEncoder *CreateWith(ID3D11Device *dev, const HwEncConfig *cfg, int bFrames, int lookahead,
                              wchar_t *why, size_t cch, BOOL *initFailed)
{
    QsvEncoder          *e;
    VplParams            p, q;
    mfxVersion           ver;
    mfxFrameAllocRequest req;
    mfxStatus            st;
    mfxU16               i;

    *initFailed = FALSE;
    e = (QsvEncoder *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*e));
    if (!e)
        return NULL;
    e->rt = &g_vpl;
    e->dev = dev;
    ID3D11Device_AddRef(dev);
    ID3D11Device_GetImmediateContext(dev, &e->dctx);
    if (FAILED(ID3D11DeviceContext_QueryInterface(e->dctx, &IID_ID3D11Multithread, (void **)&e->mt)))
        e->mt = NULL;
    e->hevc = cfg->hevc;
    e->input = cfg->input;
    e->width = cfg->width;
    e->height = cfg->height;
    e->duration = 10000000LL / cfg->fps;

    if (cfg->input == DXGI_FORMAT_B8G8R8A8_UNORM) {
        D3D11_TEXTURE2D_DESC d;
        e->vp = VpConv_Create(dev, cfg->width, cfg->height, why, cch);
        if (!e->vp)
            goto fail;
        ZeroMemory(&d, sizeof(d));
        d.Width = cfg->width; d.Height = cfg->height; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &e->nv12))) {
            StringCchCopyW(why, cch, L"no NV12 texture");
            goto fail;
        }
    } else if (cfg->input != (cfg->tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12)) {
        StringCchPrintfW(why, cch, L"unsupported input format %d", (int)cfg->input);
        goto fail;
    }

    e->sysMem = !cfg->videoMemory;
    e->s = OpenSession(e->rt, dev, !e->sysMem, &ver, why, cch);
    if (!e->s)
        goto fail;
    if (e->sysMem) {
        /* The runtime's own device and memory: Nitshot reads each frame
           back and the runtime uploads it, the way Intel's samples feed
           an encoder. */
        D3D11_TEXTURE2D_DESC d;
        ZeroMemory(&d, sizeof(d));
        d.Width = cfg->width; d.Height = cfg->height; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = cfg->tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &e->stage))) {
            StringCchCopyW(why, cch, L"no readback texture");
            goto fail;
        }
        goto configure;
    }
    e->alloc.base.pthis  = &e->alloc;
    e->alloc.base.Alloc  = AllocFrames;
    e->alloc.base.Lock   = LockFrame;
    e->alloc.base.Unlock = UnlockFrame;
    e->alloc.base.GetHDL = GetFrameHDL;
    e->alloc.base.Free   = FreeFrames;
    e->alloc.dev = dev;
    e->alloc.ctx = e->dctx;
    st = e->rt->SetAllocator(e->s, &e->alloc.base);
    if (st != MFX_ERR_NONE) {
        StringCchPrintfW(why, cch, L"the Intel runtime refused the frame allocator (%d, %s)", (int)st, StatusText(st));
        goto fail;
    }

configure:
    Log_Printf(L"qsv: configuring %s%s %ux%u at %d fps, preset %d, %d B-frame(s), lookahead %d, %u kbit/s, "
               L"frames in %s memory",
               e->hevc ? L"HEVC" : L"H.264 High", cfg->tenBit ? L" Main10" : L"", cfg->width,
               cfg->height, cfg->fps, cfg->preset, bFrames, lookahead, cfg->bitrate / 1000,
               e->sysMem ? L"system" : L"video");
    BuildParams(&p, cfg, bFrames, lookahead);
    BuildParams(&q, cfg, bFrames, lookahead);
    st = e->rt->EncQuery(e->s, &p.par, &q.par);
    if (st < MFX_ERR_NONE) {
        StringCchPrintfW(why, cch, L"the Intel encoder refused the configuration (%d, %s)", (int)st, StatusText(st));
        goto fail;
    }
    LogChanges(&p, &q);

    /* The input frames, sized and counted as the encoder asks, through
       the same allocator - Nitshot owns them and knows each texture. */
    ZeroMemory(&req, sizeof(req));
    BuildParams(&p, cfg, bFrames, lookahead);
    st = e->rt->EncQueryIOSurf(e->s, &p.par, &req);
    if (st < MFX_ERR_NONE) {
        StringCchPrintfW(why, cch, L"no surface requirements (%d, %s)", (int)st, StatusText(st));
        goto fail;
    }
    req.Type |= MFX_MEMTYPE_FROM_ENCODE | MFX_MEMTYPE_EXTERNAL_FRAME;
    e->inType = req.Type;
    req.NumFrameSuggested = (mfxU16)(req.NumFrameSuggested + 2);   /* one being filled, one spare */
    if (e->sysMem) {
        const mfxFrameInfo *fi = &p.par.mfx.FrameInfo;
        SIZE_T block;
        e->bpp   = cfg->tenBit ? 2 : 1;
        e->pitch = ALIGN((UINT32)fi->Width * e->bpp, 64);
        block    = (SIZE_T)e->pitch * fi->Height * 3 / 2;
        e->surfCount = req.NumFrameSuggested;
        e->sysBuf = (BYTE *)VirtualAlloc(NULL, block * e->surfCount, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
        e->surf = (mfxFrameSurface1 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                sizeof(mfxFrameSurface1) * e->surfCount);
        if (!e->sysBuf || !e->surf) {
            StringCchCopyW(why, cch, L"no input frames");
            goto fail;
        }
        Log_Printf(L"qsv: %u input frames in system memory, %ux%u, pitch %u, type %04X",
                   e->surfCount, fi->Width, fi->Height, e->pitch, e->inType);
        for (i = 0; i < e->surfCount; ++i) {
            BYTE *y = e->sysBuf + block * i;
            /* Black, so the padding below the picture codes cheaply. */
            memset(y, 0, (SIZE_T)e->pitch * fi->Height);
            if (cfg->tenBit) {
                UINT16 *uv = (UINT16 *)(y + (SIZE_T)e->pitch * fi->Height);
                SIZE_T  j, n = (SIZE_T)e->pitch * fi->Height / 2 / 2;
                for (j = 0; j < n; ++j) uv[j] = 0x8000;
            } else {
                memset(y + (SIZE_T)e->pitch * fi->Height, 0x80, (SIZE_T)e->pitch * fi->Height / 2);
            }
            e->surf[i].Info = *fi;
            e->surf[i].Data.Y  = y;
            e->surf[i].Data.UV = y + (SIZE_T)e->pitch * fi->Height;
            e->surf[i].Data.PitchLow  = (mfxU16)(e->pitch & 0xFFFF);
            e->surf[i].Data.PitchHigh = (mfxU16)(e->pitch >> 16);
            e->surf[i].Data.MemType = e->inType;
        }
        goto init;
    }
    if (AllocFrames(&e->alloc, &req, &e->inResp) != MFX_ERR_NONE) {
        StringCchCopyW(why, cch, L"no input frames");
        goto fail;
    }
    e->surfCount = e->inResp.NumFrameActual;
    e->surf = (mfxFrameSurface1 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                            sizeof(mfxFrameSurface1) * e->surfCount);
    if (!e->surf)
        goto fail;
    for (i = 0; i < e->surfCount; ++i) {
        e->surf[i].Info = p.par.mfx.FrameInfo;
        e->surf[i].Data.MemId = e->inResp.mids[i];
        /* API 2.x surfaces say what memory they are; an application's own
           ones, from its own allocator, carry the type it was asked for. */
        e->surf[i].Data.MemType = e->inType;
    }

init:
    BuildParams(&p, cfg, bFrames, lookahead);
    Crash_Step(L"qsv: Init");
    st = e->rt->EncInit(e->s, &p.par);
    Log_Printf(L"qsv: Init %d (%s)", (int)st, StatusText(st));
    if (st < MFX_ERR_NONE) {
        StringCchPrintfW(why, cch, L"the Intel encoder would not start (%d, %s)", (int)st, StatusText(st));
        *initFailed = TRUE;
        goto fail;
    }
    e->encInit = TRUE;
    if (!ReadHeaders(e)) {
        StringCchCopyW(why, cch, L"the Intel encoder gave no parameter sets");
        goto fail;
    }
    return e;

fail:
    Log_Printf(L"qsv: encoder not created - %s", why);
    Qsv_Destroy(e);
    return NULL;
}

/*
 * The encoder as configured, or - when the runtime refuses to start it -
 * without lookahead, then without B-frames too. On an Arc 140V (API 2.17)
 * Init gave MFX_ERR_UNDEFINED_BEHAVIOR with lookahead from system memory,
 * and lookahead from textures had brought the process down; what the
 * runtime will not do is dropped rather than the encoder.
 */
QsvEncoder *Qsv_Create(ID3D11Device *dev, const HwEncConfig *cfg, wchar_t *why, size_t cch)
{
    QsvEncoder *e;
    HwEncInfo   info;
    int         bFrames, lookahead, k = cfg && cfg->hevc ? 1 : 0;
    wchar_t     scratch[160];
    BOOL        initFailed;

    if (!why || !cch) { why = scratch; cch = ARRAYSIZE(scratch); }
    why[0] = 0;
    if (!dev || !cfg || (cfg->width & 1) || (cfg->height & 1) || cfg->fps <= 0) {
        StringCchCopyW(why, cch, L"bad parameters");
        return NULL;
    }
    if (cfg->hdr && !(cfg->hevc && cfg->tenBit && cfg->input == DXGI_FORMAT_P010)) {
        StringCchCopyW(why, cch, L"HDR needs HEVC Main10 from P010");
        return NULL;
    }
    if (!Qsv_Query(dev, NULL, &info)) {
        StringCchCopyW(why, cch, info.why);
        return NULL;
    }
    bFrames   = cfg->bFrames < info.caps.maxB[k] ? cfg->bFrames : info.caps.maxB[k];
    lookahead = info.caps.lookahead[k] ? cfg->lookahead : 0;
    if (lookahead > 0 && g_laRefused[k]) {
        Log_Printf(L"qsv: lookahead off - the runtime refused it earlier");
        lookahead = 0;
    }

    e = CreateWith(dev, cfg, bFrames, lookahead, why, cch, &initFailed);
    if (!e && initFailed && lookahead > 0) {
        Log_Printf(L"qsv: trying again without lookahead");
        lookahead = 0;
        e = CreateWith(dev, cfg, bFrames, lookahead, why, cch, &initFailed);
        if (e)
            InterlockedExchange(&g_laRefused[k], 1);
    }
    if (!e && initFailed && bFrames > 0) {
        Log_Printf(L"qsv: trying again without B-frames");
        e = CreateWith(dev, cfg, 0, 0, why, cch, &initFailed);
    }
    return e;
}

LONGLONG Qsv_StartOffset(const QsvEncoder *e)
{
    return e ? (LONGLONG)e->delay * e->duration : 0;
}

BOOL Qsv_SequenceHeader(QsvEncoder *e, const BYTE **data, UINT32 *size)
{
    if (!e || !e->seqLen)
        return FALSE;
    *data = e->seq;
    *size = e->seqLen;
    return TRUE;
}

/* The presentation time an output stamp stands for: looked up among the
   inputs rather than converted back, so no rounding creeps in. */
static LONGLONG PtsOf(const QsvEncoder *e, mfxU64 ts)
{
    UINT64 k;
    UINT64 first = e->sent > TS_RING ? e->sent - TS_RING : 0;
    for (k = first; k < e->sent; ++k)
        if (e->inTs[k % TS_RING] == ts)
            return e->inPts[k % TS_RING];
    return (LONGLONG)(ts * 1000 / 9);
}

/* Encodes one surface (NULL: drain) and hands out what comes back. */
static mfxStatus Submit(QsvEncoder *e, mfxFrameSurface1 *surf, HwPacketFn out, void *ctx)
{
    mfxSyncPoint sync = NULL;
    mfxStatus    st;
    int          tries;

    for (tries = 0; tries < 1000; ++tries) {
        Crash_Step(surf ? L"qsv: EncodeFrameAsync" : L"qsv: EncodeFrameAsync (draining)");
        st = e->rt->EncFrame(e->s, NULL, surf, &e->bs, &sync);
        if (st != MFX_WRN_DEVICE_BUSY)
            break;
        Sleep(1);
    }
    if (st == MFX_ERR_MORE_DATA)
        return st;
    if (st < MFX_ERR_NONE) {
        Log_Printf(L"qsv: EncodeFrameAsync %d (%s)", (int)st, StatusText(st));
        return st;
    }
    if (st > MFX_ERR_NONE)
        Log_Printf(L"qsv: EncodeFrameAsync warning %d (%s)", (int)st, StatusText(st));
    if (sync) {
        Crash_Step(L"qsv: SyncOperation");
        st = e->rt->Sync(e->s, sync, 60000);
        if (st < MFX_ERR_NONE) {
            Log_Printf(L"qsv: SyncOperation %d (%s)", (int)st, StatusText(st));
            return st;
        }
        {
            LONGLONG pts = PtsOf(e, e->bs.TimeStamp) + (LONGLONG)e->delay * e->duration;
            LONGLONG dts = e->inPts[e->got % TS_RING];
            if (dts > pts)
                dts = pts;
            out(ctx, e->bs.Data + e->bs.DataOffset, e->bs.DataLength, pts, dts,
                (e->bs.FrameType & MFX_FRAMETYPE_IDR) != 0);
            e->got++;
            e->bs.DataOffset = 0;
            e->bs.DataLength = 0;
        }
    }
    return MFX_ERR_NONE;
}

BOOL Qsv_Encode(QsvEncoder *e, ID3D11Texture2D *tex, LONGLONG pts, HwPacketFn out, void *ctx)
{
    mfxFrameSurface1 *surf = NULL;
    VplMid           *m;
    mfxU16            i;
    mfxStatus         st;
    ID3D11Texture2D  *src = tex;
    D3D11_BOX         box;

    if (!e || !tex || !out || e->failed)
        return FALSE;
    for (i = 0; i < e->surfCount && !surf; ++i)
        if (e->surf[i].Data.Locked == 0)
            surf = &e->surf[i];
    if (!surf) {
        Log_Printf(L"qsv: all %u input frames still in use", e->surfCount);
        e->failed = TRUE;
        return FALSE;
    }
    m = (VplMid *)surf->Data.MemId;
    if (e->sent < 3) {
        if (e->sysMem)
            Log_Printf(L"qsv: frame %llu: surface %u in system memory, from %s", e->sent,
                       (unsigned)(surf - e->surf),
                       e->vp ? L"BGRA via the video processor" : L"the caller's texture");
        else
            Log_Printf(L"qsv: frame %llu: surface %u, texture %p slice %u, from %s", e->sent,
                       (unsigned)(surf - e->surf), (void *)m->res, (unsigned)(size_t)m->pair.second,
                       e->vp ? L"BGRA via the video processor" : L"the caller's texture");
    }

    if (e->mt) ID3D11Multithread_Enter(e->mt);
    if (e->vp) {
        Crash_Step(L"qsv: converting BGRA to NV12");
        if (!VpConv_Run(e->vp, tex, e->nv12)) {
            if (e->mt) ID3D11Multithread_Leave(e->mt);
            Log_Printf(L"qsv: BGRA to NV12 failed");
            e->failed = TRUE;
            return FALSE;
        }
        src = e->nv12;
    }
    box.left = 0; box.top = 0; box.front = 0;
    box.right = e->width; box.bottom = e->height; box.back = 1;
    if (e->sysMem) {
        D3D11_MAPPED_SUBRESOURCE map;
        Crash_Step(L"qsv: reading the frame back");
        ID3D11DeviceContext_CopySubresourceRegion(e->dctx, (ID3D11Resource *)e->stage, 0,
                                                  0, 0, 0, (ID3D11Resource *)src, 0, &box);
        if (FAILED(ID3D11DeviceContext_Map(e->dctx, (ID3D11Resource *)e->stage, 0,
                                           D3D11_MAP_READ, 0, &map))) {
            if (e->mt) ID3D11Multithread_Leave(e->mt);
            Log_Printf(L"qsv: the frame could not be read back");
            e->failed = TRUE;
            return FALSE;
        }
        Crash_Step(L"qsv: copying the frame into the encoder's surface");
        {
            /* A planar staging texture maps with its chroma right after
               'Height' rows of luma. */
            const BYTE *sy = (const BYTE *)map.pData;
            const BYTE *suv = sy + (SIZE_T)map.RowPitch * e->height;
            UINT32 row, bytes = e->width * e->bpp;
            for (row = 0; row < e->height; ++row)
                memcpy(surf->Data.Y + (SIZE_T)e->pitch * row, sy + (SIZE_T)map.RowPitch * row, bytes);
            for (row = 0; row < e->height / 2; ++row)
                memcpy(surf->Data.UV + (SIZE_T)e->pitch * row, suv + (SIZE_T)map.RowPitch * row, bytes);
        }
        ID3D11DeviceContext_Unmap(e->dctx, (ID3D11Resource *)e->stage, 0);
    } else {
        Crash_Step(L"qsv: copying the frame into the encoder's surface");
        ID3D11DeviceContext_CopySubresourceRegion(e->dctx, m->res, (UINT)(size_t)m->pair.second,
                                                  0, 0, 0, (ID3D11Resource *)src, 0, &box);
    }
    if (e->mt) ID3D11Multithread_Leave(e->mt);

    e->inPts[e->sent % TS_RING] = pts;
    e->inTs[e->sent % TS_RING]  = (mfxU64)(pts * 9 / 1000);   /* 90 kHz */
    surf->Data.TimeStamp = e->inTs[e->sent % TS_RING];
    e->sent++;
    st = Submit(e, surf, out, ctx);
    if (e->sent <= 3)
        Log_Printf(L"qsv: frame %llu submitted: %d (%s), %llu out so far", e->sent - 1,
                   (int)st, StatusText(st), e->got);
    if (st < MFX_ERR_NONE && st != MFX_ERR_MORE_DATA) {
        e->failed = TRUE;
        return FALSE;
    }
    return TRUE;
}

BOOL Qsv_Flush(QsvEncoder *e, HwPacketFn out, void *ctx)
{
    int guard;
    if (!e || !out)
        return FALSE;
    for (guard = 0; guard < 1000; ++guard) {
        mfxStatus st = Submit(e, NULL, out, ctx);
        if (st == MFX_ERR_MORE_DATA)
            return e->got == e->sent;
        if (st < MFX_ERR_NONE)
            return FALSE;
    }
    return FALSE;
}

void Qsv_Destroy(QsvEncoder *e)
{
    if (!e)
        return;
    if (e->s) {
        if (e->encInit)
            e->rt->EncClose(e->s);
        e->rt->Close(e->s);
    }
    if (e->inResp.mids)
        FreeFrames(&e->alloc, &e->inResp);
    /* Whatever the runtime allocated and did not free before closing. */
    {
        int slot;
        for (slot = 0; slot < MAX_ALLOCS; ++slot)
            if (e->alloc.alloc[slot].mids) {
                mfxFrameAllocResponse r;
                ZeroMemory(&r, sizeof(r));
                r.mids = e->alloc.alloc[slot].mids;
                FreeFrames(&e->alloc, &r);
            }
    }
    if (e->surf)    HeapFree(GetProcessHeap(), 0, e->surf);
    if (e->sysBuf)  VirtualFree(e->sysBuf, 0, MEM_RELEASE);
    if (e->stage)   ID3D11Texture2D_Release(e->stage);
    if (e->bs.Data) HeapFree(GetProcessHeap(), 0, e->bs.Data);
    if (e->nv12)    ID3D11Texture2D_Release(e->nv12);
    VpConv_Destroy(e->vp);
    if (e->mt)   ID3D11Multithread_Release(e->mt);
    if (e->dctx) ID3D11DeviceContext_Release(e->dctx);
    if (e->dev)  ID3D11Device_Release(e->dev);
    HeapFree(GetProcessHeap(), 0, e);
}

/* ---------------------------------------------------- as hwenc.h has it */

static void *OpsCreate(ID3D11Device *dev, const HwEncConfig *cfg, wchar_t *why, size_t cch)
{
    return Qsv_Create(dev, cfg, why, cch);
}

static BOOL OpsHeader(void *e, const BYTE **data, UINT32 *size)
{
    return Qsv_SequenceHeader((QsvEncoder *)e, data, size);
}

static LONGLONG OpsOffset(const void *e)
{
    return Qsv_StartOffset((const QsvEncoder *)e);
}

static BOOL OpsEncode(void *e, ID3D11Texture2D *tex, LONGLONG pts, HwPacketFn out, void *ctx)
{
    return Qsv_Encode((QsvEncoder *)e, tex, pts, out, ctx);
}

static BOOL OpsFlush(void *e, HwPacketFn out, void *ctx)
{
    return Qsv_Flush((QsvEncoder *)e, out, ctx);
}

static void OpsDestroy(void *e)
{
    Qsv_Destroy((QsvEncoder *)e);
}

const HwEncOps g_qsvOps = {
    L"oneVPL", Qsv_Query, OpsCreate, OpsHeader, OpsOffset, OpsEncode, OpsFlush, OpsDestroy
};
