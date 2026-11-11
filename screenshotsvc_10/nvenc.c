/*
 * nvenc.c - loading NVENC and asking what it can do.
 */

#define COBJMACROS

#include "screenshotservice.h"
#include "nvenc.h"
#include "log.h"
#include "crash.h"

#include <d3d11_4.h>     /* ID3D11Multithread */
#include <strsafe.h>

/* NVIDIA's header, unmodified; it uses nameless unions throughout. */
#pragma warning(push)
#pragma warning(disable: 4201)
#include "third_party/nvenc/nvEncodeAPI.h"
#pragma warning(pop)

typedef NVENCSTATUS (NVENCAPI *PFN_CREATE)(NV_ENCODE_API_FUNCTION_LIST *);
typedef NVENCSTATUS (NVENCAPI *PFN_MAXVER)(uint32_t *);

/* A loaded driver and its function table. */
typedef struct {
    HMODULE                     dll;
    NV_ENCODE_API_FUNCTION_LIST fn;
} NvApi;

static void Why(HwEncInfo *info, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(info->why, ARRAYSIZE(info->why), fmt, ap);
    va_end(ap);
}

/*
 * Loads the DLL from System32 only - it is part of the driver, and a copy
 * planted next to ScreenshotService.exe must not be the one that runs - and checks that
 * the driver speaks at least the API version this was built against.
 */
static BOOL LoadApi(const wchar_t *name, NvApi *api, HwEncInfo *info)
{
    PFN_CREATE create;
    PFN_MAXVER maxVer;
    uint32_t   ver = 0;
    NVENCSTATUS st;

    ZeroMemory(api, sizeof(*api));
    api->dll = LoadLibraryExW(name ? name : L"nvEncodeAPI64.dll", NULL,
                              LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!api->dll) {
        Why(info, L"no NVIDIA encoder driver (%lu)", GetLastError());
        return FALSE;
    }
    maxVer = (PFN_MAXVER)(void *)GetProcAddress(api->dll, "NvEncodeAPIGetMaxSupportedVersion");
    create = (PFN_CREATE)(void *)GetProcAddress(api->dll, "NvEncodeAPICreateInstance");
    if (!maxVer || !create) {
        Why(info, L"the encoder driver lacks its entry points");
        return FALSE;
    }
    st = maxVer(&ver);
    if (st != NV_ENC_SUCCESS) {
        Why(info, L"the driver did not report its API version (%d)", (int)st);
        return FALSE;
    }
    /* The driver answers major << 4 | minor. */
    info->driverMajor = ver >> 4;
    info->driverMinor = ver & 0xF;
    if (info->driverMajor < NVENCAPI_MAJOR_VERSION ||
        (info->driverMajor == NVENCAPI_MAJOR_VERSION &&
         info->driverMinor < NVENCAPI_MINOR_VERSION)) {
        Why(info, L"the driver offers API %u.%u, %u.%u is needed - update the "
                  L"NVIDIA driver", info->driverMajor, info->driverMinor,
            NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        return FALSE;
    }
    api->fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    st = create(&api->fn);
    if (st != NV_ENC_SUCCESS || !api->fn.nvEncOpenEncodeSessionEx) {
        Why(info, L"the encoder driver would not hand out its functions (%d)", (int)st);
        return FALSE;
    }
    return TRUE;
}

static void UnloadApi(NvApi *api)
{
    if (api->dll)
        FreeLibrary(api->dll);
    ZeroMemory(api, sizeof(*api));
}

static int Cap(const NvApi *api, void *enc, GUID codec, NV_ENC_CAPS which)
{
    NV_ENC_CAPS_PARAM p;
    int v = 0;

    ZeroMemory(&p, sizeof(p));
    p.version = NV_ENC_CAPS_PARAM_VER;
    p.capsToQuery = which;
    if (api->fn.nvEncGetEncodeCaps(enc, codec, &p, &v) != NV_ENC_SUCCESS)
        return 0;
    return v;
}

/*
 * Both list calls follow the API's count-then-fetch protocol, and have to: the
 * driver answers the fetch's count with the array size it was handed, not the
 * number of entries, and asked for more than it has, the first answer of a
 * session came back as all-zero GUIDs. Asking for exactly the count gives the
 * real list.
 */
static BOOL HasCodec(const NvApi *api, void *enc, const GUID *codec)
{
    GUID     list[16];
    uint32_t count = 0, got = 0, i;

    if (api->fn.nvEncGetEncodeGUIDCount(enc, &count) != NV_ENC_SUCCESS ||
        count == 0 || count > ARRAYSIZE(list))
        return FALSE;
    if (api->fn.nvEncGetEncodeGUIDs(enc, list, count, &got) != NV_ENC_SUCCESS)
        return FALSE;
    if (got > count)
        got = count;
    for (i = 0; i < got; ++i)
        if (IsEqualGUID(&list[i], codec))
            return TRUE;
    return FALSE;
}

static BOOL TakesFormat(const NvApi *api, void *enc, GUID codec, NV_ENC_BUFFER_FORMAT fmt)
{
    NV_ENC_BUFFER_FORMAT list[32];
    uint32_t count = 0, got = 0, i;

    if (api->fn.nvEncGetInputFormatCount(enc, codec, &count) != NV_ENC_SUCCESS ||
        count == 0 || count > ARRAYSIZE(list))
        return FALSE;
    if (api->fn.nvEncGetInputFormats(enc, codec, list, count, &got) != NV_ENC_SUCCESS)
        return FALSE;
    if (got > count)
        got = count;
    for (i = 0; i < got; ++i)
        if (list[i] == fmt)
            return TRUE;
    return FALSE;
}

static void CodecCaps(const NvApi *api, void *enc, GUID codec, int k, HwEncCaps *c)
{
    c->maxB[k]       = Cap(api, enc, codec, NV_ENC_CAPS_NUM_MAX_BFRAMES);
    c->lookahead[k]  = Cap(api, enc, codec, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;
    c->bRef[k]       = Cap(api, enc, codec, NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE) != 0;
    c->temporalAq[k] = Cap(api, enc, codec, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) != 0;
}

BOOL Nvenc_Query(ID3D11Device *dev, const wchar_t *dll, HwEncInfo *info)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open;
    NvApi       api;
    void       *enc = NULL;
    NVENCSTATUS st;

    if (!info)
        return FALSE;
    ZeroMemory(info, sizeof(*info));
    if (!dev) {
        Why(info, L"no graphics device");
        return FALSE;
    }
    if (!LoadApi(dll, &api, info))
        goto done;

    /*
     * A session belongs to a device, and NVENC only accepts one on an NVIDIA
     * adapter: on a laptop whose desktop runs on the integrated GPU this is
     * where it says no, and Media Foundation carries on as before.
     */
    ZeroMemory(&open, sizeof(open));
    open.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device     = dev;
    open.apiVersion = NVENCAPI_VERSION;
    st = api.fn.nvEncOpenEncodeSessionEx(&open, &enc);
    if (st != NV_ENC_SUCCESS || !enc) {
        Why(info, L"the graphics device has no NVIDIA encoder (%d)", (int)st);
        enc = NULL;
        goto done;
    }

    info->caps.h264 = HasCodec(&api, enc, &NV_ENC_CODEC_H264_GUID) &&
                      TakesFormat(&api, enc, NV_ENC_CODEC_H264_GUID, NV_ENC_BUFFER_FORMAT_NV12);
    info->caps.hevc = HasCodec(&api, enc, &NV_ENC_CODEC_HEVC_GUID) &&
                      TakesFormat(&api, enc, NV_ENC_CODEC_HEVC_GUID, NV_ENC_BUFFER_FORMAT_NV12);
    if (info->caps.h264)
        CodecCaps(&api, enc, NV_ENC_CODEC_H264_GUID, 0, &info->caps);
    if (info->caps.hevc) {
        CodecCaps(&api, enc, NV_ENC_CODEC_HEVC_GUID, 1, &info->caps);
        info->caps.hevc10 = Cap(&api, enc, NV_ENC_CODEC_HEVC_GUID,
                                NV_ENC_CAPS_SUPPORT_10BIT_ENCODE) != 0 &&
                            TakesFormat(&api, enc, NV_ENC_CODEC_HEVC_GUID,
                                        NV_ENC_BUFFER_FORMAT_YUV420_10BIT);
        info->caps.maxWidth  = Cap(&api, enc, NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_WIDTH_MAX);
        info->caps.maxHeight = Cap(&api, enc, NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_HEIGHT_MAX);
        info->caps.engines   = Cap(&api, enc, NV_ENC_CODEC_HEVC_GUID,
                                   NV_ENC_CAPS_NUM_ENCODER_ENGINES);
    }
    if (!info->caps.h264 && !info->caps.hevc) {
        Why(info, L"the NVIDIA encoder offers neither H.264 nor HEVC from NV12");
        goto done;
    }
    info->available = TRUE;

done:
    if (enc)
        api.fn.nvEncDestroyEncoder(enc);
    UnloadApi(&api);
    if (info->available)
        Log_Printf(L"nvenc: API %u.%u (built for %u.%u); H.264 %s, B-frames %d%s; "
                   L"HEVC %s%s, B-frames %d%s%s; up to %dx%d, %d engine(s)",
                   info->driverMajor, info->driverMinor,
                   NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION,
                   info->caps.h264 ? L"yes" : L"no", info->caps.maxB[0],
                   info->caps.lookahead[0] ? L", lookahead" : L"",
                   info->caps.hevc ? L"yes" : L"no",
                   info->caps.hevc10 ? L" + Main10" : L"", info->caps.maxB[1],
                   info->caps.bRef[1] ? L" (as references)" : L"",
                   info->caps.lookahead[1] ? L", lookahead" : L"",
                   info->caps.maxWidth, info->caps.maxHeight, info->caps.engines);
    else
        Log_Printf(L"nvenc: not used - %s", info->why);
    return info->available;
}

/* ------------------------------------------------------------- encoding */

/*
 * Enough inputs and outputs in flight for the deepest reordering and
 * lookahead NVENC is asked for, plus a margin: a frame's input texture and
 * bitstream buffer are only free again once its output has been taken.
 */
#define RING_MAX 64

struct NvencEncoder {
    NvApi                 api;
    void                 *enc;
    ID3D11Device         *dev;
    ID3D11DeviceContext  *ctx;
    ID3D11Multithread    *mt;
    NV_ENC_BUFFER_FORMAT  fmt;
    DXGI_FORMAT           dxgi;
    UINT32                width, height;
    LONGLONG              duration;        /* one frame, 100 ns units */
    int                   ring;
    int                   delay;           /* reorder depth: the B-frames */
    int                   hold;            /* frames not yet to be taken out */
    ID3D11Texture2D      *tex[RING_MAX];
    NV_ENC_REGISTERED_PTR reg[RING_MAX];
    NV_ENC_INPUT_PTR      mapped[RING_MAX];
    NV_ENC_OUTPUT_PTR     bits[RING_MAX];
    /* Input timestamps in arrival order; the decode timestamps are built
       from them, because pictures leave in decode order but their own
       timestamps are display times. */
    LONGLONG              inPts[RING_MAX];
    UINT64                sent, got;       /* frames handed in / taken out */
    BYTE                  seq[512];
    UINT32                seqLen;
    BOOL                  failed;
    UINT32                gop;
    BOOL                  haveSei;
    BYTE                  seiBytes[24];
    NV_ENC_SEI_PAYLOAD    sei;
};

static const GUID *PresetGuid(int preset)
{
    switch (preset) {
    case 1:  return &NV_ENC_PRESET_P1_GUID;
    case 2:  return &NV_ENC_PRESET_P2_GUID;
    case 3:  return &NV_ENC_PRESET_P3_GUID;
    case 4:  return &NV_ENC_PRESET_P4_GUID;
    case 6:  return &NV_ENC_PRESET_P6_GUID;
    case 7:  return &NV_ENC_PRESET_P7_GUID;
    default: return &NV_ENC_PRESET_P5_GUID;
    }
}

static NV_ENC_BUFFER_FORMAT BufferFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_NV12:           return NV_ENC_BUFFER_FORMAT_NV12;
    case DXGI_FORMAT_P010:           return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return NV_ENC_BUFFER_FORMAT_ARGB;
    default:                         return NV_ENC_BUFFER_FORMAT_UNDEFINED;
    }
}

/*
 * What the stream says about its colour, limited range either way - the same
 * as the Media Foundation path writes: BT.709 for SDR; BT.2020 primaries, the
 * PQ curve and the BT.2020 non-constant-luminance matrix for HDR10.
 */
static void VuiColour(NV_ENC_CONFIG_H264_VUI_PARAMETERS *v, BOOL hdr)
{
    v->videoSignalTypePresentFlag   = 1;
    v->videoFormat                  = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    v->videoFullRangeFlag           = 0;
    v->colourDescriptionPresentFlag = 1;
    v->colourPrimaries         = hdr ? NV_ENC_VUI_COLOR_PRIMARIES_BT2020
                                     : NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    v->transferCharacteristics = hdr ? NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084
                                     : NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    v->colourMatrix            = hdr ? NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL
                                     : NV_ENC_VUI_MATRIX_COEFFS_BT709;
}

static void PutU16(BYTE *p, double v, double scale)
{
    double u = v * scale + 0.5;
    UINT32 x = u < 0 ? 0 : u > 65535.0 ? 65535u : (UINT32)u;
    p[0] = (BYTE)(x >> 8);
    p[1] = (BYTE)x;
}

static void PutU32(BYTE *p, double v, double scale)
{
    double u = v * scale + 0.5;
    UINT32 x = u < 0 ? 0 : u > 4294967295.0 ? 0xFFFFFFFFu : (UINT32)u;
    p[0] = (BYTE)(x >> 24); p[1] = (BYTE)(x >> 16);
    p[2] = (BYTE)(x >> 8);  p[3] = (BYTE)x;
}

/*
 * mastering_display_colour_volume (H.265 D.2.28, payload type 137): the
 * primaries in green, blue, red order and the white point, in units of
 * 0.00002, then the peak and minimum luminance in units of 0.0001 cd/m2.
 * The same order and units as the container's 'mdcv' box.
 */
static void BuildMasteringSei(const HwEncConfig *c, BYTE out[24])
{
    PutU16(out + 0,  c->greenX, 50000.0); PutU16(out + 2,  c->greenY, 50000.0);
    PutU16(out + 4,  c->blueX,  50000.0); PutU16(out + 6,  c->blueY,  50000.0);
    PutU16(out + 8,  c->redX,   50000.0); PutU16(out + 10, c->redY,   50000.0);
    PutU16(out + 12, c->whiteX, 50000.0); PutU16(out + 14, c->whiteY, 50000.0);
    PutU32(out + 16, c->maxNits, 10000.0);
    PutU32(out + 20, c->minNits, 10000.0);
}

static void EncWhy(wchar_t *why, size_t cch, const wchar_t *fmt, ...)
{
    va_list ap;
    if (!why || !cch)
        return;
    va_start(ap, fmt);
    StringCchVPrintfW(why, cch, fmt, ap);
    va_end(ap);
}

NvencEncoder *Nvenc_Create(ID3D11Device *dev, const HwEncConfig *cfg,
                           wchar_t *why, size_t cch)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open;
    NV_ENC_PRESET_CONFIG                 preset;
    NV_ENC_INITIALIZE_PARAMS             init;
    NV_ENC_CONFIG                        conf;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD        sp;
    HwEncInfo                            info;
    NvencEncoder                        *e;
    GUID                                 codec;
    NVENCSTATUS                          st;
    int                                  i;

    if (why && cch) why[0] = 0;
    if (!dev || !cfg || (cfg->width & 1) || (cfg->height & 1) || cfg->fps <= 0) {
        EncWhy(why, cch, L"bad parameters");
        return NULL;
    }
    e = (NvencEncoder *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*e));
    if (!e)
        return NULL;
    ZeroMemory(&info, sizeof(info));
    if (!LoadApi(NULL, &e->api, &info)) {
        EncWhy(why, cch, L"%s", info.why);
        goto fail;
    }
    e->dev = dev;
    ID3D11Device_AddRef(dev);
    ID3D11Device_GetImmediateContext(dev, &e->ctx);
    if (FAILED(ID3D11DeviceContext_QueryInterface(e->ctx, &IID_ID3D11Multithread,
                                                  (void **)&e->mt)))
        e->mt = NULL;
    e->dxgi     = cfg->input;
    e->fmt      = BufferFormat(cfg->input);
    e->width    = cfg->width;
    e->height   = cfg->height;
    e->duration = 10000000LL / cfg->fps;
    if (e->fmt == NV_ENC_BUFFER_FORMAT_UNDEFINED ||
        (cfg->tenBit && (!cfg->hevc || e->fmt != NV_ENC_BUFFER_FORMAT_YUV420_10BIT))) {
        EncWhy(why, cch, L"unsupported input format %d", (int)cfg->input);
        goto fail;
    }
    if (cfg->hdr && !(cfg->hevc && cfg->tenBit)) {
        EncWhy(why, cch, L"HDR needs HEVC Main10");
        goto fail;
    }
    if (cfg->hdr && cfg->whiteX > 0.0f && cfg->greenX > 0.0f) {
        BuildMasteringSei(cfg, e->seiBytes);
        e->sei.payloadSize = sizeof(e->seiBytes);
        e->sei.payloadType = 137;
        e->sei.payload     = e->seiBytes;
        e->haveSei         = TRUE;
    }

    ZeroMemory(&open, sizeof(open));
    open.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device     = dev;
    open.apiVersion = NVENCAPI_VERSION;
    st = e->api.fn.nvEncOpenEncodeSessionEx(&open, &e->enc);
    if (st != NV_ENC_SUCCESS || !e->enc) {
        e->enc = NULL;
        EncWhy(why, cch, L"no encode session (%d)", (int)st);
        goto fail;
    }

    /*
     * Start from the preset's own configuration for high-quality tuning -
     * that is where its motion search and B-frame decisions live - and
     * change only what has to be ours: profile, GOP, B-frames, rate,
     * lookahead and the colour description.
     */
    codec = cfg->hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    ZeroMemory(&preset, sizeof(preset));
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    st = e->api.fn.nvEncGetEncodePresetConfigEx(e->enc, codec, *PresetGuid(cfg->preset),
                                                NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset);
    if (st != NV_ENC_SUCCESS) {
        EncWhy(why, cch, L"no preset configuration (%d)", (int)st);
        goto fail;
    }
    conf = preset.presetCfg;
    conf.version        = NV_ENC_CONFIG_VER;
    conf.profileGUID    = cfg->hevc ? (cfg->tenBit ? NV_ENC_HEVC_PROFILE_MAIN10_GUID
                                                   : NV_ENC_HEVC_PROFILE_MAIN_GUID)
                                    : NV_ENC_H264_PROFILE_HIGH_GUID;
    /* A key frame every two seconds: what keeps seeking in a player quick. */
    conf.gopLength      = (uint32_t)cfg->fps * 2;
    conf.frameIntervalP = cfg->bFrames + 1;
    conf.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    conf.rcParams.averageBitRate  = cfg->bitrate;
    conf.rcParams.maxBitRate      = cfg->bitrate * 2;
    conf.rcParams.enableLookahead = cfg->lookahead > 0;
    conf.rcParams.lookaheadDepth  = (uint16_t)(cfg->lookahead > 0 ? cfg->lookahead : 0);
    conf.rcParams.enableAQ        = cfg->aq ? 1 : 0;
    if (cfg->hevc) {
        NV_ENC_CONFIG_HEVC *h = &conf.encodeCodecConfig.hevcConfig;
        h->idrPeriod           = conf.gopLength;
        h->chromaFormatIDC     = 1;
        h->pixelBitDepthMinus8 = cfg->tenBit ? 2 : 0;
        if (cfg->bFrames > 1)
            h->useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_MIDDLE;
        VuiColour(&h->hevcVUIParameters, cfg->hdr);
    } else {
        NV_ENC_CONFIG_H264 *h = &conf.encodeCodecConfig.h264Config;
        h->idrPeriod       = conf.gopLength;
        h->chromaFormatIDC = 1;
        VuiColour(&h->h264VUIParameters, FALSE);
    }

    ZeroMemory(&init, sizeof(init));
    init.version          = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID       = codec;
    init.presetGUID       = *PresetGuid(cfg->preset);
    init.encodeWidth      = cfg->width;
    init.encodeHeight     = cfg->height;
    init.darWidth         = cfg->width;
    init.darHeight        = cfg->height;
    init.frameRateNum     = (uint32_t)cfg->fps;
    init.frameRateDen     = 1;
    init.enablePTD        = 1;           /* NVENC decides the picture types */
    init.encodeConfig     = &conf;
    init.maxEncodeWidth   = cfg->width;
    init.maxEncodeHeight  = cfg->height;
    init.tuningInfo       = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    st = e->api.fn.nvEncInitializeEncoder(e->enc, &init);
    if (st != NV_ENC_SUCCESS) {
        EncWhy(why, cch, L"the encoder refused the configuration (%d)", (int)st);
        goto fail;
    }

    /* Output lags input by the B-frames in between, and by the lookahead,
       which only adds latency, not reordering. */
    e->gop   = conf.gopLength;
    e->delay = cfg->bFrames;
    e->hold  = cfg->bFrames + cfg->lookahead;
    e->ring  = e->hold + 4;
    if (e->ring > RING_MAX)
        e->ring = RING_MAX;

    for (i = 0; i < e->ring; ++i) {
        D3D11_TEXTURE2D_DESC        d;
        NV_ENC_REGISTER_RESOURCE    r;
        NV_ENC_CREATE_BITSTREAM_BUFFER b;

        ZeroMemory(&d, sizeof(d));
        d.Width = cfg->width;
        d.Height = cfg->height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = cfg->input;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &e->tex[i]))) {
            EncWhy(why, cch, L"no input texture");
            goto fail;
        }
        ZeroMemory(&r, sizeof(r));
        r.version            = NV_ENC_REGISTER_RESOURCE_VER;
        r.resourceType       = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        r.width              = cfg->width;
        r.height             = cfg->height;
        r.resourceToRegister = e->tex[i];
        r.bufferFormat       = e->fmt;
        r.bufferUsage        = NV_ENC_INPUT_IMAGE;
        st = e->api.fn.nvEncRegisterResource(e->enc, &r);
        if (st != NV_ENC_SUCCESS) {
            EncWhy(why, cch, L"the input texture could not be registered (%d)", (int)st);
            goto fail;
        }
        e->reg[i] = r.registeredResource;

        ZeroMemory(&b, sizeof(b));
        b.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        st = e->api.fn.nvEncCreateBitstreamBuffer(e->enc, &b);
        if (st != NV_ENC_SUCCESS) {
            EncWhy(why, cch, L"no bitstream buffer (%d)", (int)st);
            goto fail;
        }
        e->bits[i] = b.bitstreamBuffer;
    }

    ZeroMemory(&sp, sizeof(sp));
    sp.version              = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    sp.inBufferSize         = sizeof(e->seq);
    sp.spsppsBuffer         = e->seq;
    sp.outSPSPPSPayloadSize = &e->seqLen;
    st = e->api.fn.nvEncGetSequenceParams(e->enc, &sp);
    if (st != NV_ENC_SUCCESS || e->seqLen == 0) {
        EncWhy(why, cch, L"no sequence header (%d)", (int)st);
        goto fail;
    }

    Log_Printf(L"nvenc: %s%s %ux%u at %d fps, P%d, %d B-frame(s), lookahead %d, "
               L"%u kbit/s VBR, %d buffers",
               cfg->hevc ? L"HEVC" : L"H.264 High", cfg->tenBit ? L" Main10" : L"",
               cfg->width, cfg->height, cfg->fps, cfg->preset, cfg->bFrames,
               cfg->lookahead, cfg->bitrate / 1000, e->ring);
    return e;

fail:
    Log_Printf(L"nvenc: encoder not created - %s", why && why[0] ? why : L"?");
    Nvenc_Destroy(e);
    return NULL;
}

LONGLONG Nvenc_StartOffset(const NvencEncoder *e)
{
    return e ? (LONGLONG)e->delay * e->duration : 0;
}

BOOL Nvenc_SequenceHeader(NvencEncoder *e, const BYTE **data, UINT32 *size)
{
    if (!e || !e->seqLen)
        return FALSE;
    *data = e->seq;
    *size = e->seqLen;
    return TRUE;
}

/*
 * Takes every finished picture out, in the order they were encoded. In
 * synchronous mode a successful submission means everything submitted so far
 * is complete, so this drains up to 'upTo'.
 */
static BOOL Drain(NvencEncoder *e, UINT64 upTo, HwPacketFn out, void *ctx)
{
    while (e->got < upTo) {
        int                   i = (int)(e->got % (UINT64)e->ring);
        NV_ENC_LOCK_BITSTREAM lk;
        NVENCSTATUS           st;
        LONGLONG              dts, pts;

        ZeroMemory(&lk, sizeof(lk));
        lk.version         = NV_ENC_LOCK_BITSTREAM_VER;
        lk.outputBitstream = e->bits[i];
        st = e->api.fn.nvEncLockBitstream(e->enc, &lk);
        if (st != NV_ENC_SUCCESS) {
            Log_Printf(L"nvenc: lock failed (%d)", (int)st);
            return FALSE;
        }
        /*
         * The k-th picture out is decoded k-th, so its decode time is the
         * k-th input's time. Its presentation time must not come before
         * that, and with B-frames it would: a B-frame decoded after its
         * anchor shows earlier than the anchor arrived. Holding decode times
         * back instead makes the first few negative, and Media Foundation's
         * MP4 sink silently drops a sample with a negative decode time - the
         * first being the key frame, everything up to the next one became
         * undecodable. Ramping them up from zero in fractions of a frame
         * kept them, but gave ffmpeg-based tools a 120 fps stream to
         * duplicate frames into.
         *
         * So presentation times are moved later by the reorder depth -
         * what Microsoft's own encoder does with its B-frames - and every
         * sample keeps the same duration. Nvenc_StartOffset says by how
         * much, and the recorder moves the sound by the same, which keeps
         * picture and sound together where the Media Foundation encoder
         * leaves them a frame apart.
         */
        dts = e->inPts[i];
        pts = (LONGLONG)lk.outputTimeStamp + (LONGLONG)e->delay * e->duration;
        if (dts > pts)
            dts = pts;                     /* cannot happen; never pass it */
        out(ctx, (const BYTE *)lk.bitstreamBufferPtr, lk.bitstreamSizeInBytes,
            pts, dts,
            lk.pictureType == NV_ENC_PIC_TYPE_IDR || lk.pictureType == NV_ENC_PIC_TYPE_I);
        e->api.fn.nvEncUnlockBitstream(e->enc, e->bits[i]);
        if (e->mapped[i]) {
            e->api.fn.nvEncUnmapInputResource(e->enc, e->mapped[i]);
            e->mapped[i] = NULL;
        }
        e->got++;
    }
    return TRUE;
}

BOOL Nvenc_Encode(NvencEncoder *e, ID3D11Texture2D *tex, LONGLONG pts,
                  HwPacketFn out, void *ctx)
{
    NV_ENC_MAP_INPUT_RESOURCE map;
    NV_ENC_PIC_PARAMS         pic;
    NVENCSTATUS               st;
    int                       i;

    if (!e || !tex || !out || e->failed)
        return FALSE;
    if (e->sent - e->got >= (UINT64)e->ring) {
        /* Cannot happen with the ring sized as above; refuse rather than
           overwrite a texture NVENC still reads. */
        Log_Printf(L"nvenc: all %d buffers still in flight", e->ring);
        return FALSE;
    }
    i = (int)(e->sent % (UINT64)e->ring);

    if (e->mt) ID3D11Multithread_Enter(e->mt);
    ID3D11DeviceContext_CopyResource(e->ctx, (ID3D11Resource *)e->tex[i],
                                     (ID3D11Resource *)tex);
    if (e->mt) ID3D11Multithread_Leave(e->mt);

    ZeroMemory(&map, sizeof(map));
    map.version            = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = e->reg[i];
    st = e->api.fn.nvEncMapInputResource(e->enc, &map);
    if (st != NV_ENC_SUCCESS) {
        Log_Printf(L"nvenc: map failed (%d)", (int)st);
        e->failed = TRUE;
        return FALSE;
    }
    e->mapped[i] = map.mappedResource;
    e->inPts[i]  = pts;

    ZeroMemory(&pic, sizeof(pic));
    pic.version         = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth      = e->width;
    pic.inputHeight     = e->height;
    pic.inputBuffer     = map.mappedResource;
    pic.outputBitstream = e->bits[i];
    pic.bufferFmt       = map.mappedBufferFmt;
    pic.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp  = (uint64_t)pts;
    pic.inputDuration   = (uint64_t)e->duration;
    /*
     * Key frames at fixed places - every GOP from the first frame - rather
     * than wherever NVENC's own decision puts them, so that each one is
     * known in advance and can carry the mastering-display SEI: a player
     * that starts or seeks into the stream then finds it right there.
     */
    if (e->gop && e->sent % e->gop == 0) {
        pic.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
        if (e->haveSei) {
            pic.codecPicParams.hevcPicParams.seiPayloadArrayCnt = 1;
            pic.codecPicParams.hevcPicParams.seiPayloadArray    = &e->sei;
        }
    }
    Crash_Step(L"nvenc: EncodePicture");
    st = e->api.fn.nvEncEncodePicture(e->enc, &pic);
    e->sent++;
    /*
     * Success does not mean everything handed in is done: the lookahead keeps
     * its depth of pictures back on top of the B-frames, and locking one of
     * those is refused as an invalid parameter. Only what is further back
     * than both is taken here; Nvenc_Flush takes the rest.
     */
    if (st == NV_ENC_SUCCESS)
        return e->sent > (UINT64)e->hold ? Drain(e, e->sent - (UINT64)e->hold, out, ctx)
                                         : TRUE;
    if (st == NV_ENC_ERR_NEED_MORE_INPUT)
        return TRUE;
    Log_Printf(L"nvenc: encode failed (%d)", (int)st);
    e->failed = TRUE;
    return FALSE;
}

BOOL Nvenc_Flush(NvencEncoder *e, HwPacketFn out, void *ctx)
{
    NV_ENC_PIC_PARAMS pic;
    NVENCSTATUS       st;

    if (!e || !out)
        return FALSE;
    ZeroMemory(&pic, sizeof(pic));
    pic.version        = NV_ENC_PIC_PARAMS_VER;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    st = e->api.fn.nvEncEncodePicture(e->enc, &pic);
    if (st != NV_ENC_SUCCESS) {
        Log_Printf(L"nvenc: end of stream refused (%d)", (int)st);
        return FALSE;
    }
    return Drain(e, e->sent, out, ctx);
}

void Nvenc_Destroy(NvencEncoder *e)
{
    int i;

    if (!e)
        return;
    if (e->enc) {
        for (i = 0; i < RING_MAX; ++i) {
            if (e->mapped[i]) e->api.fn.nvEncUnmapInputResource(e->enc, e->mapped[i]);
            if (e->reg[i])    e->api.fn.nvEncUnregisterResource(e->enc, e->reg[i]);
            if (e->bits[i])   e->api.fn.nvEncDestroyBitstreamBuffer(e->enc, e->bits[i]);
        }
        e->api.fn.nvEncDestroyEncoder(e->enc);
    }
    for (i = 0; i < RING_MAX; ++i)
        if (e->tex[i])
            ID3D11Texture2D_Release(e->tex[i]);
    if (e->mt)  ID3D11Multithread_Release(e->mt);
    if (e->ctx) ID3D11DeviceContext_Release(e->ctx);
    if (e->dev) ID3D11Device_Release(e->dev);
    UnloadApi(&e->api);
    HeapFree(GetProcessHeap(), 0, e);
}

/* ---------------------------------------------------- as hwenc.h has it */

static void *OpsCreate(ID3D11Device *dev, const HwEncConfig *cfg, wchar_t *why, size_t cch)
{
    return Nvenc_Create(dev, cfg, why, cch);
}

static BOOL OpsHeader(void *e, const BYTE **data, UINT32 *size)
{
    return Nvenc_SequenceHeader((NvencEncoder *)e, data, size);
}

static LONGLONG OpsOffset(const void *e)
{
    return Nvenc_StartOffset((const NvencEncoder *)e);
}

static BOOL OpsEncode(void *e, ID3D11Texture2D *tex, LONGLONG pts, HwPacketFn out, void *ctx)
{
    return Nvenc_Encode((NvencEncoder *)e, tex, pts, out, ctx);
}

static BOOL OpsFlush(void *e, HwPacketFn out, void *ctx)
{
    return Nvenc_Flush((NvencEncoder *)e, out, ctx);
}

static void OpsDestroy(void *e)
{
    Nvenc_Destroy((NvencEncoder *)e);
}

const HwEncOps g_nvencOps = {
    L"NVENC", Nvenc_Query, OpsCreate, OpsHeader, OpsOffset, OpsEncode, OpsFlush, OpsDestroy
};
