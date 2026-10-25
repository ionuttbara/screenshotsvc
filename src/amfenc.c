/*
 * amfenc.c - AMD's encoder through AMF.
 */

#define COBJMACROS

#include "nitshot.h"
#include "amfenc.h"
#include "vpconv.h"
#include "log.h"
#include "crash.h"

#include <d3d11_4.h>     /* ID3D11Multithread */
#include <strsafe.h>

/* AMD's SDK headers, unmodified, used through their C interface. */
#include "core/Factory.h"
#include "core/Trace.h"
#include "components/VideoEncoderVCE.h"
#include "components/VideoEncoderHEVC.h"
#include "components/ColorSpace.h"
#include "components/PreAnalysis.h"

/* ------------------------------------------------------------- runtime */

/*
 * The runtime is loaded once and kept for the life of the process, as AMD's
 * samples do: AMF keeps internal threads and the trace writer registered
 * below, and unloading it under either is asking for trouble. Loading costs
 * nothing on a machine without AMD hardware - the DLL is not there.
 */
typedef struct {
    HMODULE     dll;
    AMFFactory *factory;
    AMFTrace   *trace;
    amf_uint64  version;        /* the runtime's own */
    wchar_t     why[128];       /* why it is not there, once decided */
    BOOL        ok;
} AmfRuntime;

static AmfRuntime g_amf;
static SRWLOCK    g_amfLock = SRWLOCK_INIT;

/* AMF's trace, into Nitshot's log: its warnings and errors carry what no
   result code says - why a property was refused, what the driver lacks. */
static void AMF_CDECL_CALL TraceWrite(AMFTraceWriter *self, const wchar_t *scope,
                                      const wchar_t *message)
{
    wchar_t line[512];
    size_t  n;
    (void)self;
    if (!message)
        return;
    StringCchCopyW(line, ARRAYSIZE(line), message);
    n = wcslen(line);
    while (n > 0 && (line[n - 1] == L'\n' || line[n - 1] == L'\r'))
        line[--n] = 0;
    Log_Printf(L"amf: [%s] %s", scope ? scope : L"", line);
}

static void AMF_CDECL_CALL TraceFlush(AMFTraceWriter *self)
{
    (void)self;
}

static const AMFTraceWriterVtbl g_traceVtbl = { TraceWrite, TraceFlush };
static AMFTraceWriter           g_traceWriter = { &g_traceVtbl };

static const wchar_t *ResultText(AMF_RESULT r)
{
    const wchar_t *t = NULL;
    if (g_amf.trace)
        t = g_amf.trace->pVtbl->GetResultText(g_amf.trace, r);
    return t ? t : L"?";
}

/*
 * Loads amfrt64.dll from System32 only - it is part of the driver, and a copy
 * planted next to Nitshot.exe must not be the one that runs - and checks the
 * runtime is at least the version these headers describe.
 */
static BOOL LoadRuntime(const wchar_t *name, AmfRuntime *rt)
{
    AMFQueryVersion_Fn queryVersion;
    AMFInit_Fn         init;
    AMF_RESULT         r;

    ZeroMemory(rt, sizeof(*rt));
    rt->dll = LoadLibraryExW(name ? name : AMF_DLL_NAME, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!rt->dll) {
        StringCchPrintfW(rt->why, ARRAYSIZE(rt->why), L"no AMD encoder runtime (%lu)",
                         GetLastError());
        return FALSE;
    }
    queryVersion = (AMFQueryVersion_Fn)(void *)GetProcAddress(rt->dll, AMF_QUERY_VERSION_FUNCTION_NAME);
    init = (AMFInit_Fn)(void *)GetProcAddress(rt->dll, AMF_INIT_FUNCTION_NAME);
    if (!queryVersion || !init) {
        StringCchCopyW(rt->why, ARRAYSIZE(rt->why), L"the AMD runtime lacks its entry points");
        goto fail;
    }
    r = queryVersion(&rt->version);
    if (r != AMF_OK) {
        StringCchPrintfW(rt->why, ARRAYSIZE(rt->why),
                         L"the AMD runtime did not report its version (%d)", (int)r);
        goto fail;
    }
    if (rt->version < AMF_FULL_VERSION) {
        StringCchPrintfW(rt->why, ARRAYSIZE(rt->why),
                         L"the AMD runtime is %u.%u.%u, %u.%u.%u is needed - update the AMD driver",
                         (unsigned)(rt->version >> 48), (unsigned)((rt->version >> 32) & 0xFFFF),
                         (unsigned)((rt->version >> 16) & 0xFFFF),
                         AMF_VERSION_MAJOR, AMF_VERSION_MINOR, AMF_VERSION_RELEASE);
        goto fail;
    }
    r = init(AMF_FULL_VERSION, &rt->factory);
    if (r != AMF_OK || !rt->factory) {
        StringCchPrintfW(rt->why, ARRAYSIZE(rt->why), L"the AMD runtime would not start (%d)", (int)r);
        goto fail;
    }
    if (rt->factory->pVtbl->GetTrace(rt->factory, &rt->trace) != AMF_OK)
        rt->trace = NULL;
    if (rt->trace) {
        /* Its own outputs off - a console would pop up in a GUI process -
           and ours on, at warnings and above. */
        rt->trace->pVtbl->EnableWriter(rt->trace, AMF_TRACE_WRITER_CONSOLE, 0);
        rt->trace->pVtbl->EnableWriter(rt->trace, AMF_TRACE_WRITER_DEBUG_OUTPUT, 0);
        rt->trace->pVtbl->RegisterWriter(rt->trace, L"Nitshot", &g_traceWriter, 1);
        rt->trace->pVtbl->SetWriterLevel(rt->trace, L"Nitshot", AMF_TRACE_WARNING);
    }
    rt->ok = TRUE;
    Log_Printf(L"amf: runtime %u.%u.%u.%u (built for %u.%u.%u)",
               (unsigned)(rt->version >> 48), (unsigned)((rt->version >> 32) & 0xFFFF),
               (unsigned)((rt->version >> 16) & 0xFFFF), (unsigned)(rt->version & 0xFFFF),
               AMF_VERSION_MAJOR, AMF_VERSION_MINOR, AMF_VERSION_RELEASE);
    return TRUE;

fail:
    FreeLibrary(rt->dll);
    rt->dll = NULL;
    return FALSE;
}

/* The shared runtime, loaded on first use. A test may name another DLL,
   which is loaded on its own and dropped again. */
static AmfRuntime *Runtime(const wchar_t *dll, AmfRuntime *scratch)
{
    AmfRuntime *rt;

    if (dll) {
        LoadRuntime(dll, scratch);
        return scratch;
    }
    AcquireSRWLockExclusive(&g_amfLock);
    if (!g_amf.ok && !g_amf.why[0])
        LoadRuntime(NULL, &g_amf);
    rt = &g_amf;
    ReleaseSRWLockExclusive(&g_amfLock);
    return rt;
}

static void DropScratch(AmfRuntime *rt, AmfRuntime *scratch)
{
    if (rt == scratch && scratch->dll) {
        if (scratch->trace)
            scratch->trace->pVtbl->UnregisterWriter(scratch->trace, L"Nitshot");
        FreeLibrary(scratch->dll);
    }
}

/* ------------------------------------------------------------- helpers */

static AMF_RESULT SetInt(AMFComponent *c, const wchar_t *name, amf_int64 value)
{
    AMFVariantStruct v;
    AMF_RESULT       r;
    AMFVariantInit(&v);
    AMFVariantAssignInt64(&v, value);
    r = c->pVtbl->SetProperty(c, name, v);
    Log_Printf(L"amf:   %s = %lld%s%s", name, (long long)value,
               r == AMF_OK ? L"" : L"  -> ", r == AMF_OK ? L"" : ResultText(r));
    return r;
}

static AMF_RESULT SetBool(AMFComponent *c, const wchar_t *name, BOOL value)
{
    AMFVariantStruct v;
    AMF_RESULT       r;
    AMFVariantInit(&v);
    AMFVariantAssignBool(&v, value ? 1 : 0);
    r = c->pVtbl->SetProperty(c, name, v);
    Log_Printf(L"amf:   %s = %s%s%s", name, value ? L"true" : L"false",
               r == AMF_OK ? L"" : L"  -> ", r == AMF_OK ? L"" : ResultText(r));
    return r;
}

static AMF_RESULT SetSize(AMFComponent *c, const wchar_t *name, amf_int32 w, amf_int32 h)
{
    AMFVariantStruct v;
    AMFSize          s = AMFConstructSize(w, h);
    AMF_RESULT       r;
    AMFVariantInit(&v);
    AMFVariantAssignSize(&v, &s);
    r = c->pVtbl->SetProperty(c, name, v);
    Log_Printf(L"amf:   %s = %dx%d%s%s", name, w, h,
               r == AMF_OK ? L"" : L"  -> ", r == AMF_OK ? L"" : ResultText(r));
    return r;
}

static AMF_RESULT SetRate(AMFComponent *c, const wchar_t *name, amf_uint32 num, amf_uint32 den)
{
    AMFVariantStruct v;
    AMFRate          rate = AMFConstructRate(num, den);
    AMF_RESULT       r;
    AMFVariantInit(&v);
    AMFVariantAssignRate(&v, &rate);
    r = c->pVtbl->SetProperty(c, name, v);
    Log_Printf(L"amf:   %s = %u/%u%s%s", name, num, den,
               r == AMF_OK ? L"" : L"  -> ", r == AMF_OK ? L"" : ResultText(r));
    return r;
}

static amf_int64 CapInt(AMFCaps *caps, const wchar_t *name, amf_int64 def)
{
    AMFVariantStruct v;
    AMFVariantInit(&v);
    if (caps->pVtbl->GetProperty(caps, name, &v) != AMF_OK)
        return def;
    if (v.type == AMF_VARIANT_INT64)
        return v.int64Value;
    if (v.type == AMF_VARIANT_BOOL)
        return v.boolValue ? 1 : 0;
    return def;
}

/* ---------------------------------------------------------------- query */

/* One codec's component on 'ctx', asked for its capabilities. */
static BOOL CodecCaps(AmfRuntime *rt, AMFContext *ctx, BOOL hevc, HwEncCaps *c)
{
    AMFComponent *comp = NULL;
    AMFCaps      *caps = NULL;
    AMF_RESULT    r;
    int           k = hevc ? 1 : 0;
    BOOL          ok = FALSE;

    r = rt->factory->pVtbl->CreateComponent(rt->factory, ctx,
            hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC, &comp);
    if (r != AMF_OK || !comp) {
        Log_Printf(L"amf: no %s encoder (%s)", hevc ? L"HEVC" : L"H.264", ResultText(r));
        return FALSE;
    }
    if (comp->pVtbl->GetCaps(comp, &caps) == AMF_OK && caps) {
        AMF_ACCELERATION_TYPE accel = caps->pVtbl->GetAccelerationType(caps);
        if (accel != AMF_ACCEL_HARDWARE) {
            Log_Printf(L"amf: the %s encoder is not hardware-accelerated (%d)",
                       hevc ? L"HEVC" : L"H.264", (int)accel);
        } else if (hevc) {
            c->hevc         = TRUE;
            c->hevc10       = CapInt(caps, AMF_VIDEO_ENCODER_HEVC_CAP_MAX_PROFILE, 0) >=
                              AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10;
            c->maxB[k]      = 0;     /* SDK 1.4.35 has no HEVC B-frames */
            c->lookahead[k] = CapInt(caps, AMF_VIDEO_ENCODER_HEVC_CAP_PRE_ANALYSIS, 0) != 0;
            c->engines      = (int)CapInt(caps, AMF_VIDEO_ENCODER_HEVC_CAP_NUM_OF_HW_INSTANCES, 1);
            ok = TRUE;
        } else {
            c->h264         = TRUE;
            /* The B-picture pattern takes up to three. */
            c->maxB[k]      = CapInt(caps, AMF_VIDEO_ENCODER_CAP_BFRAMES, 0) ? 3 : 0;
            c->lookahead[k] = CapInt(caps, AMF_VIDEO_ENCODER_CAP_PRE_ANALYSIS, 0) != 0;
            ok = TRUE;
        }
        caps->pVtbl->Release(caps);
    }
    comp->pVtbl->Terminate(comp);
    comp->pVtbl->Release(comp);
    return ok;
}

static AMFContext *OpenContext(AmfRuntime *rt, ID3D11Device *dev, wchar_t *why, size_t cch)
{
    AMFContext *ctx = NULL;
    AMF_RESULT  r = rt->factory->pVtbl->CreateContext(rt->factory, &ctx);

    if (r != AMF_OK || !ctx) {
        StringCchPrintfW(why, cch, L"no AMF context (%s)", ResultText(r));
        return NULL;
    }
    /*
     * The context works on the recorder's own device, so the encoder reads
     * the capture's textures where they are. On a machine whose desktop runs
     * on another vendor's GPU this is where AMF says no.
     */
    r = ctx->pVtbl->InitDX11(ctx, dev, AMF_DX11_1);
    if (r != AMF_OK) {
        StringCchPrintfW(why, cch, L"AMF would not take the graphics device (%s)", ResultText(r));
        ctx->pVtbl->Terminate(ctx);
        ctx->pVtbl->Release(ctx);
        return NULL;
    }
    return ctx;
}

BOOL Amf_Query(ID3D11Device *dev, const wchar_t *dll, HwEncInfo *info)
{
    AmfRuntime  scratch;
    AmfRuntime *rt;
    AMFContext *ctx = NULL;

    if (!info)
        return FALSE;
    ZeroMemory(info, sizeof(*info));
    if (!dev) {
        StringCchCopyW(info->why, ARRAYSIZE(info->why), L"no graphics device");
        goto done;
    }
    rt = Runtime(dll, &scratch);
    if (!rt->ok) {
        StringCchCopyW(info->why, ARRAYSIZE(info->why), rt->why);
        DropScratch(rt, &scratch);
        goto done;
    }
    info->driverMajor = (UINT32)(rt->version >> 48);
    info->driverMinor = (UINT32)((rt->version >> 32) & 0xFFFF);
    ctx = OpenContext(rt, dev, info->why, ARRAYSIZE(info->why));
    if (ctx) {
        CodecCaps(rt, ctx, FALSE, &info->caps);
        CodecCaps(rt, ctx, TRUE, &info->caps);
        ctx->pVtbl->Terminate(ctx);
        ctx->pVtbl->Release(ctx);
        if (info->caps.h264 || info->caps.hevc)
            info->available = TRUE;
        else
            StringCchCopyW(info->why, ARRAYSIZE(info->why),
                           L"the AMD encoder offers neither H.264 nor HEVC in hardware");
    }
    DropScratch(rt, &scratch);

done:
    if (info->available)
        Log_Printf(L"amf: H.264 %s, B-frames %d%s; HEVC %s%s%s; %d engine(s)",
                   info->caps.h264 ? L"yes" : L"no", info->caps.maxB[0],
                   info->caps.lookahead[0] ? L", pre-analysis" : L"",
                   info->caps.hevc ? L"yes" : L"no", info->caps.hevc10 ? L" + Main10" : L"",
                   info->caps.lookahead[1] ? L", pre-analysis" : L"", info->caps.engines);
    else
        Log_Printf(L"amf: not used - %s", info->why);
    return info->available;
}

/* ------------------------------------------------------------- encoding */

#define PTS_RING 256      /* inputs still waiting for their output, at most */

struct AmfEncoder {
    AMFContext          *ctx;
    AMFComponent        *enc;
    ID3D11Device        *dev;
    ID3D11DeviceContext *dctx;
    ID3D11Multithread   *mt;
    BOOL                 hevc;
    AMF_SURFACE_FORMAT   fmt;
    VpConv              *vp;               /* BGRA input: to NV12 first */
    ID3D11Texture2D     *nv12;             /* ...into this */
    UINT32               width, height;
    LONGLONG             duration;
    int                  delay;            /* B-frames: the reorder depth */
    UINT32               gop;              /* frames from key frame to key frame */
    LONGLONG             inPts[PTS_RING];  /* arrival order, for the DTS */
    UINT64               sent, got;
    BYTE                 seq[1024];
    UINT32               seqLen;
    BOOL                 failed;
};

/* AMD's samples tag a texture that is one slice of an array with its index
   as private data; a surface from the context's pool may be one. */
static const GUID AmfTextureArrayIndexGUID =
    { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };

/*
 * The parameter sets for the container. AMF hands them over as an extradata
 * buffer; Annex-B is what the MP4 sink wants, so a configuration record
 * (avcC/hvcC, first byte 1) is turned into start-code-prefixed NAL units.
 */
BOOL Amf_ToAnnexB(const BYTE *in, UINT32 len, BOOL hevc, BYTE *out, UINT32 cap, UINT32 *outLen)
{
    UINT32 n = 0, p;
    *outLen = 0;
    if (len >= 4 && in[0] == 0 && in[1] == 0 && (in[2] == 1 || (in[2] == 0 && in[3] == 1))) {
        if (len > cap)
            return FALSE;
        memcpy(out, in, len);               /* already Annex-B */
        *outLen = len;
        return TRUE;
    }
    if (len < 7 || in[0] != 1)
        return FALSE;
#define PUT_NAL(src, sz) do { \
        if (n + 4 + (sz) > cap) return FALSE; \
        out[n] = 0; out[n + 1] = 0; out[n + 2] = 0; out[n + 3] = 1; \
        memcpy(out + n + 4, (src), (sz)); n += 4 + (sz); } while (0)
    if (!hevc) {
        /* avcC: 5 bytes, then SPS count & 0x1F, each 16-bit length + data,
           then PPS count, each likewise. */
        UINT32 count, i;
        p = 5;
        count = in[p++] & 0x1F;
        for (i = 0; i < count; ++i) {
            UINT32 sz;
            if (p + 2 > len) return FALSE;
            sz = ((UINT32)in[p] << 8) | in[p + 1];
            p += 2;
            if (p + sz > len) return FALSE;
            PUT_NAL(in + p, sz);
            p += sz;
        }
        if (p >= len) return FALSE;
        count = in[p++];
        for (i = 0; i < count; ++i) {
            UINT32 sz;
            if (p + 2 > len) return FALSE;
            sz = ((UINT32)in[p] << 8) | in[p + 1];
            p += 2;
            if (p + sz > len) return FALSE;
            PUT_NAL(in + p, sz);
            p += sz;
        }
    } else {
        /* hvcC: 22 bytes, then arrays: type, 16-bit count, each 16-bit
           length + data. */
        UINT32 arrays, a;
        if (len < 23) return FALSE;
        p = 22;
        arrays = in[p++];
        for (a = 0; a < arrays; ++a) {
            UINT32 count, i;
            if (p + 3 > len) return FALSE;
            p += 1;
            count = ((UINT32)in[p] << 8) | in[p + 1];
            p += 2;
            for (i = 0; i < count; ++i) {
                UINT32 sz;
                if (p + 2 > len) return FALSE;
                sz = ((UINT32)in[p] << 8) | in[p + 1];
                p += 2;
                if (p + sz > len) return FALSE;
                PUT_NAL(in + p, sz);
                p += sz;
            }
        }
    }
#undef PUT_NAL
    *outLen = n;
    return n > 0;
}

static BOOL ReadExtradata(AmfEncoder *e)
{
    AMFVariantStruct v;
    AMFBuffer       *buf = NULL;
    AMFGuid          iid = IID_AMFBuffer();
    BOOL             ok = FALSE;

    AMFVariantInit(&v);
    if (e->enc->pVtbl->GetProperty(e->enc, e->hevc ? AMF_VIDEO_ENCODER_HEVC_EXTRADATA
                                                   : AMF_VIDEO_ENCODER_EXTRADATA, &v) != AMF_OK ||
        v.type != AMF_VARIANT_INTERFACE || !v.pInterface) {
        Log_Printf(L"amf: no extradata");
        return FALSE;
    }
    if (v.pInterface->pVtbl->QueryInterface(v.pInterface, &iid, (void **)&buf) == AMF_OK && buf) {
        const BYTE *data = (const BYTE *)buf->pVtbl->GetNative(buf);
        UINT32      size = (UINT32)buf->pVtbl->GetSize(buf);
        ok = data && Amf_ToAnnexB(data, size, e->hevc, e->seq, sizeof(e->seq), &e->seqLen);
        Log_Printf(L"amf: extradata %u bytes (%s), %u as Annex-B", size,
                   size && data && data[0] == 1 ? L"configuration record" : L"Annex-B",
                   e->seqLen);
        buf->pVtbl->Release(buf);
    }
    AMFVariantClear(&v);
    return ok;
}

/* The steps AMF has in place of NVENC's P1..P7. */
static amf_int64 Preset(int preset, BOOL hevc)
{
    if (preset <= 3)
        return hevc ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED : AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED;
    if (preset <= 5)
        return hevc ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED : AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED;
    return hevc ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY : AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY;
}

/* HDR10's mastering display, for the encoder's SEI. Same units as the
   container's 'mdcv' box. */
static void SetHdrMetadata(AmfEncoder *e, const HwEncConfig *c)
{
    AMFBuffer       *buf = NULL;
    AMFHDRMetadata  *m;
    AMFVariantStruct v;
    AMF_RESULT       r;

    if (!(c->whiteX > 0.0f && c->greenX > 0.0f))
        return;
    r = e->ctx->pVtbl->AllocBuffer(e->ctx, AMF_MEMORY_HOST, sizeof(AMFHDRMetadata), &buf);
    if (r != AMF_OK || !buf) {
        Log_Printf(L"amf: no buffer for the HDR metadata (%s)", ResultText(r));
        return;
    }
    m = (AMFHDRMetadata *)buf->pVtbl->GetNative(buf);
    ZeroMemory(m, sizeof(*m));
#define XY(f) ((amf_uint16)((f) * 50000.0f + 0.5f))
    m->redPrimary[0]   = XY(c->redX);   m->redPrimary[1]   = XY(c->redY);
    m->greenPrimary[0] = XY(c->greenX); m->greenPrimary[1] = XY(c->greenY);
    m->bluePrimary[0]  = XY(c->blueX);  m->bluePrimary[1]  = XY(c->blueY);
    m->whitePoint[0]   = XY(c->whiteX); m->whitePoint[1]   = XY(c->whiteY);
#undef XY
    m->maxMasteringLuminance = (amf_uint32)(c->maxNits * 10000.0f + 0.5f);
    m->minMasteringLuminance = (amf_uint32)(c->minNits * 10000.0f + 0.5f);
    /* Light levels are only known at the end; zero means "unknown" here and
       the container carries the measured ones. */
    AMFVariantInit(&v);
    AMFVariantAssignInterface(&v, (AMFInterface *)buf);
    r = e->enc->pVtbl->SetProperty(e->enc, AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA, v);
    Log_Printf(L"amf:   %s = mastering %.0f..%.4f nits%s%s",
               AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA, c->maxNits, c->minNits,
               r == AMF_OK ? L"" : L"  -> ", r == AMF_OK ? L"" : ResultText(r));
    AMFVariantClear(&v);
    buf->pVtbl->Release(buf);
}

/*
 * Configures the encoder the way AMD documents it: the usage first - setting
 * it resets every other property to that usage's defaults - then the static
 * properties, Init, and the rate control after.
 */
static BOOL Configure(AmfEncoder *e, const HwEncConfig *c, int bFrames, int lookahead)
{
    AMFComponent *x = e->enc;
    UINT32        gop = (UINT32)c->fps * 2;
    AMF_RESULT    r;

    if (!e->hevc) {
        if (SetInt(x, AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_TRANSCODING) != AMF_OK)
            return FALSE;
        SetInt(x, AMF_VIDEO_ENCODER_QUALITY_PRESET, Preset(c->preset, FALSE));
        if (SetInt(x, AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH) != AMF_OK)
            return FALSE;
        SetSize(x, AMF_VIDEO_ENCODER_FRAMESIZE, (amf_int32)c->width, (amf_int32)c->height);
        SetRate(x, AMF_VIDEO_ENCODER_FRAMERATE, (amf_uint32)c->fps, 1);
        /* B-frames are a static property: before Init, and only asked for
           where the encoder reported them. */
        SetInt(x, AMF_VIDEO_ENCODER_B_PIC_PATTERN, bFrames);
        if (bFrames > 0)
            SetBool(x, AMF_VIDEO_ENCODER_B_REFERENCE_ENABLE, TRUE);
        SetInt(x, AMF_VIDEO_ENCODER_IDR_PERIOD, gop);
        SetInt(x, AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, gop);
        SetBool(x, AMF_VIDEO_ENCODER_ENABLE_VBAQ, c->aq);
        SetBool(x, AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, FALSE);
        SetInt(x, AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
        SetInt(x, AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
        SetInt(x, AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC, AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709);
        SetInt(x, AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT709);
        if (lookahead > 0 && SetBool(x, AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, TRUE) == AMF_OK) {
            SetInt(x, AMF_PA_LOOKAHEAD_BUFFER_DEPTH, lookahead);
            SetInt(x, AMF_PA_PAQ_MODE, AMF_PA_PAQ_MODE_NONE);   /* as VBAQ: not for text */
            SetInt(x, AMF_PA_TAQ_MODE, AMF_PA_TAQ_MODE_NONE);
        }
        SetInt(x, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
               AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR);
    } else {
        if (SetInt(x, AMF_VIDEO_ENCODER_HEVC_USAGE, AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING) != AMF_OK)
            return FALSE;
        SetInt(x, AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, Preset(c->preset, TRUE));
        if (SetInt(x, AMF_VIDEO_ENCODER_HEVC_PROFILE, c->tenBit ? AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10
                                                                : AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN) != AMF_OK)
            return FALSE;
        SetInt(x, AMF_VIDEO_ENCODER_HEVC_TIER, AMF_VIDEO_ENCODER_HEVC_TIER_MAIN);
        SetSize(x, AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, (amf_int32)c->width, (amf_int32)c->height);
        SetRate(x, AMF_VIDEO_ENCODER_HEVC_FRAMERATE, (amf_uint32)c->fps, 1);
        SetInt(x, AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, gop);
        SetInt(x, AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, 1);
        SetInt(x, AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
               AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED);
        SetBool(x, AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, c->aq);
        SetInt(x, AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE, AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_STUDIO);
        if (c->tenBit && SetInt(x, AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_10) != AMF_OK)
            return FALSE;
        if (c->hdr) {
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020);
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_INPUT_TRANSFER_CHARACTERISTIC, AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084);
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT2020);
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020);
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC, AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084);
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT2020);
            SetHdrMetadata(e, c);
        } else {
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC, AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709);
            SetInt(x, AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT709);
        }
        if (lookahead > 0 && SetBool(x, AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE, TRUE) == AMF_OK) {
            SetInt(x, AMF_PA_LOOKAHEAD_BUFFER_DEPTH, lookahead);
            SetInt(x, AMF_PA_PAQ_MODE, AMF_PA_PAQ_MODE_NONE);
            SetInt(x, AMF_PA_TAQ_MODE, AMF_PA_TAQ_MODE_NONE);
        }
        SetInt(x, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
               AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR);
    }

    r = x->pVtbl->Init(x, e->fmt, (amf_int32)c->width, (amf_int32)c->height);
    Log_Printf(L"amf: Init(%s %ux%u) %s", e->fmt == AMF_SURFACE_P010 ? L"P010" :
               e->fmt == AMF_SURFACE_NV12 ? L"NV12" : L"BGRA", c->width, c->height, ResultText(r));
    if (r != AMF_OK)
        return FALSE;

    /* Dynamic properties, after Init. */
    SetInt(x, e->hevc ? AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE : AMF_VIDEO_ENCODER_TARGET_BITRATE,
           c->bitrate);
    SetInt(x, e->hevc ? AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE : AMF_VIDEO_ENCODER_PEAK_BITRATE,
           (amf_int64)c->bitrate * 2);
    /* QueryOutput waits up to this long instead of returning at once, which
       spares a busy loop; drivers that do not know it ignore it. */
    SetInt(x, e->hevc ? AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT : AMF_VIDEO_ENCODER_QUERY_TIMEOUT, 50);
    return TRUE;
}

AmfEncoder *Amf_Create(ID3D11Device *dev, const HwEncConfig *cfg, wchar_t *why, size_t cch)
{
    AmfRuntime *rt;
    AmfEncoder *e;
    HwEncInfo   info;
    AMF_RESULT  r;
    int         bFrames, lookahead;
    wchar_t     scratch[128];

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
    rt = Runtime(NULL, NULL);
    if (!rt->ok) {
        StringCchCopyW(why, cch, rt->why);
        return NULL;
    }
    /* What this chip offers decides what is asked for. */
    if (!Amf_Query(dev, NULL, &info)) {
        StringCchCopyW(why, cch, info.why);
        return NULL;
    }
    bFrames   = cfg->hevc ? 0 : (cfg->bFrames < info.caps.maxB[0] ? cfg->bFrames : info.caps.maxB[0]);
    lookahead = info.caps.lookahead[cfg->hevc ? 1 : 0] ? cfg->lookahead : 0;

    e = (AmfEncoder *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*e));
    if (!e)
        return NULL;
    e->dev = dev;
    ID3D11Device_AddRef(dev);
    ID3D11Device_GetImmediateContext(dev, &e->dctx);
    if (FAILED(ID3D11DeviceContext_QueryInterface(e->dctx, &IID_ID3D11Multithread, (void **)&e->mt)))
        e->mt = NULL;
    e->hevc     = cfg->hevc;
    e->width    = cfg->width;
    e->height   = cfg->height;
    e->duration = 10000000LL / cfg->fps;
    e->delay    = bFrames;
    e->gop      = (UINT32)cfg->fps * 2;
    switch (cfg->input) {
    case DXGI_FORMAT_NV12:           e->fmt = AMF_SURFACE_NV12; break;
    case DXGI_FORMAT_P010:           e->fmt = AMF_SURFACE_P010; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: e->fmt = AMF_SURFACE_NV12; break;
    default:
        StringCchPrintfW(why, cch, L"unsupported input format %d", (int)cfg->input);
        goto fail;
    }
    if (cfg->input == DXGI_FORMAT_B8G8R8A8_UNORM) {
        /* AMF's pre-analysis takes NV12 only - BGRA failed Init with
           "format (BGRA) != AMF_SURFACE_NV12" on a Radeon - so BGRA is
           converted first, by the video processor the Intel path uses. */
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
    }
    if (e->fmt == AMF_SURFACE_P010 && lookahead > 0) {
        /* ...and P010 cannot be converted to it without losing the HDR. */
        Log_Printf(L"amf: pre-analysis takes NV12 only, off for P010");
        lookahead = 0;
    }

    e->ctx = OpenContext(rt, dev, why, cch);
    if (!e->ctx)
        goto fail;
    r = rt->factory->pVtbl->CreateComponent(rt->factory, e->ctx,
            e->hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC, &e->enc);
    if (r != AMF_OK || !e->enc) {
        StringCchPrintfW(why, cch, L"no %s encoder (%s)", e->hevc ? L"HEVC" : L"H.264", ResultText(r));
        e->enc = NULL;
        goto fail;
    }
    Log_Printf(L"amf: configuring %s%s %ux%u at %d fps, preset %d, %d B-frame(s), "
               L"pre-analysis %d, %u kbit/s", e->hevc ? L"HEVC" : L"H.264 High",
               cfg->tenBit ? L" Main10" : L"", cfg->width, cfg->height, cfg->fps,
               cfg->preset, bFrames, lookahead, cfg->bitrate / 1000);
    if (!Configure(e, cfg, bFrames, lookahead)) {
        if (lookahead == 0) {
            StringCchCopyW(why, cch, L"the AMD encoder refused the configuration - see the log");
            goto fail;
        }
        /* Pre-analysis is the part drivers disagree about: once more
           without it, on a fresh component (a failed Init leaves the old
           one in no defined state). */
        Log_Printf(L"amf: trying again without pre-analysis");
        e->enc->pVtbl->Terminate(e->enc);
        e->enc->pVtbl->Release(e->enc);
        e->enc = NULL;
        lookahead = 0;
        r = rt->factory->pVtbl->CreateComponent(rt->factory, e->ctx,
                e->hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC, &e->enc);
        if (r != AMF_OK || !e->enc) {
            StringCchPrintfW(why, cch, L"no %s encoder (%s)", e->hevc ? L"HEVC" : L"H.264", ResultText(r));
            e->enc = NULL;
            goto fail;
        }
        if (!Configure(e, cfg, bFrames, lookahead)) {
            StringCchCopyW(why, cch, L"the AMD encoder refused the configuration - see the log");
            goto fail;
        }
    }
    if (!ReadExtradata(e)) {
        StringCchCopyW(why, cch, L"the AMD encoder gave no parameter sets");
        goto fail;
    }
    return e;

fail:
    Log_Printf(L"amf: encoder not created - %s", why);
    Amf_Destroy(e);
    return NULL;
}

LONGLONG Amf_StartOffset(const AmfEncoder *e)
{
    return e ? (LONGLONG)e->delay * e->duration : 0;
}

BOOL Amf_SequenceHeader(AmfEncoder *e, const BYTE **data, UINT32 *size)
{
    if (!e || !e->seqLen)
        return FALSE;
    *data = e->seq;
    *size = e->seqLen;
    return TRUE;
}

/*
 * Takes out whatever is finished. With 'wait' it keeps asking - AMF waits up
 * to the query timeout each time - until the encoder says it has nothing
 * more (AMF_REPEAT) or, after Drain, that the stream has ended (AMF_EOF).
 * Timestamps as on the NVENC path: decode time the k-th input's time,
 * presentation time moved later by the reorder depth (see nvenc.c).
 */
static BOOL TakeOutput(AmfEncoder *e, HwPacketFn out, void *ctx, BOOL untilEof)
{
    /* A driver that never says end-of-stream must not hang the recording's
       end: after this long, what came out is what there is. */
    ULONGLONG deadline = GetTickCount64() + 5000;

    for (;;) {
        AMFData   *data = NULL;
        AMF_RESULT r;
        Crash_Step(L"amf: QueryOutput");
        r = e->enc->pVtbl->QueryOutput(e->enc, &data);

        if (r == AMF_EOF)
            return TRUE;
        if (r == AMF_REPEAT || (r == AMF_OK && !data)) {
            if (!untilEof)
                return TRUE;
            if (GetTickCount64() > deadline) {
                Log_Printf(L"amf: no end of stream after draining, %llu of %llu frames out",
                           e->got, e->sent);
                return e->got == e->sent;
            }
            Sleep(1);
            continue;
        }
        if (r != AMF_OK) {
            Log_Printf(L"amf: QueryOutput %s", ResultText(r));
            e->failed = TRUE;
            return FALSE;
        }
        {
            AMFBuffer       *buf = NULL;
            AMFGuid          iid = IID_AMFBuffer();
            AMFVariantStruct v;
            BOOL             key = FALSE;
            if (data->pVtbl->QueryInterface(data, &iid, (void **)&buf) == AMF_OK && buf) {
                LONGLONG pts = (LONGLONG)buf->pVtbl->GetPts(buf) + (LONGLONG)e->delay * e->duration;
                LONGLONG dts = e->inPts[e->got % PTS_RING];
                AMFVariantInit(&v);
                if (buf->pVtbl->GetProperty(buf, e->hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE
                                                         : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &v) == AMF_OK &&
                    v.type == AMF_VARIANT_INT64)
                    key = v.int64Value == (e->hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR
                                                   : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR);
                if (dts > pts)
                    dts = pts;
                out(ctx, (const BYTE *)buf->pVtbl->GetNative(buf), (UINT32)buf->pVtbl->GetSize(buf),
                    pts, dts, key);
                e->got++;
                buf->pVtbl->Release(buf);
            }
            data->pVtbl->Release(data);
        }
    }
}

BOOL Amf_Encode(AmfEncoder *e, ID3D11Texture2D *tex, LONGLONG pts, HwPacketFn out, void *ctx)
{
    AMFSurface      *surf = NULL;
    AMFPlane        *plane;
    ID3D11Texture2D *dst;
    AMF_RESULT       r;
    UINT             index = 0, size = sizeof(index);
    int              tries;

    if (!e || !tex || !out || e->failed)
        return FALSE;
    if (e->sent - e->got >= PTS_RING) {
        Log_Printf(L"amf: %u frames in flight, the encoder is not returning any", PTS_RING);
        e->failed = TRUE;
        return FALSE;
    }
    /* A surface from the context's own pool, which AMF recycles once the
       encoder has let go of it - so the caller's texture is free at once. */
    Crash_Step(L"amf: AllocSurface");
    r = e->ctx->pVtbl->AllocSurface(e->ctx, AMF_MEMORY_DX11, e->fmt, (amf_int32)e->width,
                                    (amf_int32)e->height, &surf);
    if (r != AMF_OK || !surf) {
        Log_Printf(L"amf: AllocSurface %s", ResultText(r));
        e->failed = TRUE;
        return FALSE;
    }
    plane = surf->pVtbl->GetPlaneAt(surf, 0);
    dst = plane ? (ID3D11Texture2D *)plane->pVtbl->GetNative(plane) : NULL;
    if (!dst) {
        surf->pVtbl->Release(surf);
        e->failed = TRUE;
        return FALSE;
    }
    if (FAILED(ID3D11Texture2D_GetPrivateData(dst, &AmfTextureArrayIndexGUID, &size, &index)))
        index = 0;
    {
        /* Subresource of mip 0 in array slice 'index'. */
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(dst, &d);
        index *= d.MipLevels ? d.MipLevels : 1;
    }
    if (e->mt) ID3D11Multithread_Enter(e->mt);
    if (e->vp) {
        Crash_Step(L"amf: converting BGRA to NV12");
        if (!VpConv_Run(e->vp, tex, e->nv12)) {
            if (e->mt) ID3D11Multithread_Leave(e->mt);
            surf->pVtbl->Release(surf);
            Log_Printf(L"amf: BGRA to NV12 failed");
            e->failed = TRUE;
            return FALSE;
        }
        tex = e->nv12;
    }
    ID3D11DeviceContext_CopySubresourceRegion(e->dctx, (ID3D11Resource *)dst,
        index, 0, 0, 0, (ID3D11Resource *)tex, 0, NULL);
    if (e->mt) ID3D11Multithread_Leave(e->mt);
    surf->pVtbl->SetPts(surf, pts);
    surf->pVtbl->SetDuration(surf, e->duration);
    if (e->sent % e->gop == 0) {
        /* Key frames at fixed places, as on the NVENC path. */
        AMFVariantStruct v;
        AMFVariantInit(&v);
        AMFVariantAssignInt64(&v, e->hevc ? AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR
                                          : AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
        surf->pVtbl->SetProperty(surf, e->hevc ? AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE
                                               : AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, v);
    }
    e->inPts[e->sent % PTS_RING] = pts;

    /* A full input queue is not an error: take what is ready and try again. */
    for (tries = 0; tries < 200; ++tries) {
        Crash_Step(L"amf: SubmitInput");
        r = e->enc->pVtbl->SubmitInput(e->enc, (AMFData *)surf);
        if (r != AMF_INPUT_FULL)
            break;
        if (!TakeOutput(e, out, ctx, FALSE))
            break;
        Sleep(1);
    }
    surf->pVtbl->Release(surf);
    if (r != AMF_OK && r != AMF_NEED_MORE_INPUT) {
        Log_Printf(L"amf: SubmitInput %s", ResultText(r));
        e->failed = TRUE;
        return FALSE;
    }
    e->sent++;
    return TakeOutput(e, out, ctx, FALSE);
}

BOOL Amf_Flush(AmfEncoder *e, HwPacketFn out, void *ctx)
{
    AMF_RESULT r;
    int        tries;

    if (!e || !out)
        return FALSE;
    for (tries = 0; tries < 200; ++tries) {
        r = e->enc->pVtbl->Drain(e->enc);
        if (r != AMF_INPUT_FULL)
            break;
        TakeOutput(e, out, ctx, FALSE);
        Sleep(1);
    }
    if (r != AMF_OK) {
        Log_Printf(L"amf: Drain %s", ResultText(r));
        return FALSE;
    }
    return TakeOutput(e, out, ctx, TRUE);
}

void Amf_Destroy(AmfEncoder *e)
{
    if (!e)
        return;
    if (e->enc) {
        e->enc->pVtbl->Terminate(e->enc);
        e->enc->pVtbl->Release(e->enc);
    }
    if (e->ctx) {
        e->ctx->pVtbl->Terminate(e->ctx);
        e->ctx->pVtbl->Release(e->ctx);
    }
    if (e->nv12) ID3D11Texture2D_Release(e->nv12);
    VpConv_Destroy(e->vp);
    if (e->mt)   ID3D11Multithread_Release(e->mt);
    if (e->dctx) ID3D11DeviceContext_Release(e->dctx);
    if (e->dev)  ID3D11Device_Release(e->dev);
    HeapFree(GetProcessHeap(), 0, e);
}

/* ---------------------------------------------------- as hwenc.h has it */

static void *OpsCreate(ID3D11Device *dev, const HwEncConfig *cfg, wchar_t *why, size_t cch)
{
    return Amf_Create(dev, cfg, why, cch);
}

static BOOL OpsHeader(void *e, const BYTE **data, UINT32 *size)
{
    return Amf_SequenceHeader((AmfEncoder *)e, data, size);
}

static LONGLONG OpsOffset(const void *e)
{
    return Amf_StartOffset((const AmfEncoder *)e);
}

static BOOL OpsEncode(void *e, ID3D11Texture2D *tex, LONGLONG pts, HwPacketFn out, void *ctx)
{
    return Amf_Encode((AmfEncoder *)e, tex, pts, out, ctx);
}

static BOOL OpsFlush(void *e, HwPacketFn out, void *ctx)
{
    return Amf_Flush((AmfEncoder *)e, out, ctx);
}

static void OpsDestroy(void *e)
{
    Amf_Destroy((AmfEncoder *)e);
}

const HwEncOps g_amfOps = {
    L"AMF", Amf_Query, OpsCreate, OpsHeader, OpsOffset, OpsEncode, OpsFlush, OpsDestroy
};
