/*
 * encoder.c - probe-backed encoder selection for the recorder.
 */

#define COBJMACROS

#include "screenshotservice.h"
#include "encoder.h"
#include "settings.h"
#include "log.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <process.h>

/* AV1 subtype GUID (MFVideoFormat_AV1 in newer Windows SDKs). Defining it
   locally keeps builds working with SDKs that predate the AV1 symbolic name. */
static const GUID SCREENSHOTSERVICE_MFVideoFormat_AV1 =
    { 0x31305641, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

/*
 * Define the CodecAPI GUIDs locally instead of relying on import-library GUID
 * objects. Some Windows SDK / MSVC combinations declare these identifiers in
 * the headers but do not resolve them from uuid.lib at link time. These are
 * the standard GUID values from codecapi.h and icodecapi.h.
 */
static const GUID SCREENSHOTSERVICE_IID_ICodecAPI =
    { 0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };
static const GUID SCREENSHOTSERVICE_CODECAPI_AVEncCommonRateControlMode =
    { 0x1c0608e9, 0x370c, 0x4710, { 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23 } };
static const GUID SCREENSHOTSERVICE_CODECAPI_AVEncVideoEncodeQP =
    { 0x2cb5696b, 0x23fb, 0x4ce1, { 0xa0, 0xf9, 0xef, 0x5b, 0x90, 0xfd, 0x55, 0xca } };
static const GUID SCREENSHOTSERVICE_CODECAPI_AVEncCommonMeanBitRate =
    { 0xf7222374, 0x2144, 0x4815, { 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52 } };
static const GUID SCREENSHOTSERVICE_CODECAPI_AVEncCommonMaxBitRate =
    { 0x9651eae4, 0x39b9, 0x4ebf, { 0x85, 0xef, 0xd7, 0xf4, 0x44, 0xec, 0x74, 0x65 } };

/* The probe writes a frame at this size; encoders that refuse odd or tiny
   dimensions are not interesting, and 1280x720 is cheap. */
#define PROBE_W   1280
#define PROBE_H   720
#define PROBE_FPS 30

static EncoderCaps g_caps;
static LONG        g_probing;
static wchar_t     g_probeVendor[16] = L"auto";
static BOOL        g_vendorMftRegistered;
static CLSID       g_vendorMftClsid;
static wchar_t     g_vendorMftHardwareUrl[1024];
static wchar_t     g_vendorMftName[128];

/*
 * Read without the lock on the fast path, so it is read and written as an
 * atomic: the probe thread sets it only after every capability field is in
 * place, and a reader that sees it set must see those fields too.
 */
static BOOL IsProbed(void)
{
    return InterlockedCompareExchange((volatile LONG *)&g_caps.probed, 0, 0) != 0;
}

static void SetProbed(void)
{
    InterlockedExchange((volatile LONG *)&g_caps.probed, 1);
}

/* The warm-up thread and a settings dialog can both ask at once, and they
   would otherwise be filling g_caps.mft[][] at the same time. */
static SRWLOCK     g_lock = SRWLOCK_INIT;

/* ------------------------------------------------------------------ names */

const wchar_t *Encoder_Name(EncoderId id)
{
    switch (id) {
    case ENC_AUTO:   return L"Automatic (best available)";
    case ENC_H264:   return L"H.264, 8-bit";
    case ENC_HEVC8:  return L"HEVC, 8-bit";
    case ENC_HEVC10:     return L"HEVC, 10-bit HDR";
    case ENC_HEVC10_SDR: return L"HEVC, 10-bit SDR";
    case ENC_AV1_8:      return L"AV1, 8-bit";
    case ENC_AV1_10:     return L"AV1, 10-bit SDR";
    case ENC_AV1_10_HDR: return L"AV1, 10-bit HDR";
    default:             return L"(none)";
    }
}

const wchar_t *Encoder_Token(EncoderId id)
{
    switch (id) {
    case ENC_H264:   return L"h264";
    case ENC_HEVC8:  return L"hevc8";
    case ENC_HEVC10:     return L"hevc10";
    case ENC_HEVC10_SDR: return L"hevc10sdr";
    case ENC_AV1_8:      return L"av1_8";
    case ENC_AV1_10:     return L"av1_10";
    case ENC_AV1_10_HDR: return L"av1_10hdr";
    default:             return L"auto";
    }
}

EncoderId Encoder_FromToken(const wchar_t *token)
{
    if (!token || !token[0])            return ENC_AUTO;
    if (!lstrcmpiW(token, L"h264"))     return ENC_H264;
    if (!lstrcmpiW(token, L"hevc8"))    return ENC_HEVC8;
    if (!lstrcmpiW(token, L"hevc10"))   return ENC_HEVC10;
    if (!lstrcmpiW(token, L"hevc10sdr")) return ENC_HEVC10_SDR;
    if (!lstrcmpiW(token, L"av1_8"))     return ENC_AV1_8;
    if (!lstrcmpiW(token, L"av1_10"))    return ENC_AV1_10;
    if (!lstrcmpiW(token, L"av1_10hdr")) return ENC_AV1_10_HDR;
    return ENC_AUTO;
}

BOOL Encoder_IsHdr(EncoderId id)
{
    return id == ENC_HEVC10 || id == ENC_AV1_10_HDR;
}

BOOL Encoder_Is10Bit(EncoderId id)
{
    return Encoder_IsHdr(id) || id == ENC_HEVC10_SDR ||
           id == ENC_AV1_10;
}

BOOL Encoder_IsLossless(EncoderId id)
{
    /* Main10 SDR is now variable-bitrate like all other exposed profiles.
       Do not switch it to QP 0 / quality mode, which conflicts with VBR. */
    (void)id;
    return FALSE;
}

const EncoderCaps *Encoder_Caps(void)
{
    return &g_caps;
}

/* ---------------------------------------------------------- stream set-up */

static void SetPacked(IMFMediaType *t, const GUID *key, UINT32 high, UINT32 low)
{
    IMFMediaType_SetUINT64(t, key, ((UINT64)high << 32) | (UINT64)low);
}

/*
 * Screen content is mostly flat with hard edges, which wants more bits than
 * camera footage of the same size. HEVC buys roughly a third at equal quality,
 * so it is given less rather than being allowed to look better by using the
 * same budget - a recording that is smaller for no visible loss is the point.
 */
static UINT32 PickBitrate(EncoderId id, UINT32 w, UINT32 h, int fps)
{
    double bits = (double)w * h * fps * 0.12;

    if (id == ENC_HEVC8 || id == ENC_HEVC10 || id == ENC_HEVC10_SDR)
        bits *= 0.65;
    if (id == ENC_AV1_8 || id == ENC_AV1_10 || id == ENC_AV1_10_HDR)
        bits *= 0.60;
    /* 10-bit HDR carries more than it looks: PQ puts real detail in the top
       stops that 8-bit never had, and starving it there is very visible. */
    if (id == ENC_HEVC10 || id == ENC_AV1_10_HDR)
        bits *= 1.35;
    if (id == ENC_HEVC10_SDR)
        bits *= 2.0; /* Extra headroom for Main10 SDR; QP 0 ignores this target. */

    if (bits <  2000000.0) bits =  2000000.0;
    if (bits > 40000000.0) bits = 40000000.0;
    return (UINT32)bits;
}

UINT32 Encoder_Bitrate(EncoderId id, UINT32 width, UINT32 height, int fps)
{
    return PickBitrate(id, width, height, fps);
}

BOOL Encoder_AddVideoStream(IMFSinkWriter *writer, EncoderId id,
                            UINT32 width, UINT32 height, int fps,
                            DWORD *streamIndex)
{
    IMFMediaType *outType = NULL, *inType = NULL;
    const BOOL    hdr = Encoder_IsHdr(id);
    const BOOL    tenBit = Encoder_Is10Bit(id);
    const BOOL    av1 = id == ENC_AV1_8 || id == ENC_AV1_10 ||
                        id == ENC_AV1_10_HDR;
    DWORD         index = 0;
    BOOL          ok = FALSE;

    if (!writer || id <= ENC_AUTO || id >= ENC_COUNT)
        return FALSE;

    if (FAILED(MFCreateMediaType(&outType)))
        goto done;
    IMFMediaType_SetGUID(outType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(outType, &MF_MT_SUBTYPE,
                         id == ENC_H264 ? &MFVideoFormat_H264 :
                         av1 ? &SCREENSHOTSERVICE_MFVideoFormat_AV1 : &MFVideoFormat_HEVC);
    IMFMediaType_SetUINT32(outType, &MF_MT_AVG_BITRATE,
                           PickBitrate(id, width, height, fps));
    IMFMediaType_SetUINT32(outType, &MF_MT_INTERLACE_MODE,
                           MFVideoInterlace_Progressive);
    if (tenBit) {
        if (!av1)
            IMFMediaType_SetUINT32(outType, &MF_MT_MPEG2_PROFILE,
                                   eAVEncH265VProfile_Main_420_10);
        if (hdr) {
            /* These tags describe PQ HDR. They must not leak into SDR Main10. */
            IMFMediaType_SetUINT32(outType, &MF_MT_VIDEO_PRIMARIES,
                                   MFVideoPrimaries_BT2020);
            IMFMediaType_SetUINT32(outType, &MF_MT_TRANSFER_FUNCTION,
                                   MFVideoTransFunc_2084);
            IMFMediaType_SetUINT32(outType, &MF_MT_YUV_MATRIX,
                                   MFVideoTransferMatrix_BT2020_10);
            IMFMediaType_SetUINT32(outType, &MF_MT_VIDEO_NOMINAL_RANGE,
                                   MFNominalRange_16_235);
        } else {
            IMFMediaType_SetUINT32(outType, &MF_MT_VIDEO_PRIMARIES,
                                   MFVideoPrimaries_BT709);
            IMFMediaType_SetUINT32(outType, &MF_MT_TRANSFER_FUNCTION,
                                   MFVideoTransFunc_709);
            IMFMediaType_SetUINT32(outType, &MF_MT_YUV_MATRIX,
                                   MFVideoTransferMatrix_BT709);
            IMFMediaType_SetUINT32(outType, &MF_MT_VIDEO_NOMINAL_RANGE,
                                   MFNominalRange_16_235);
        }
    }
    SetPacked(outType, &MF_MT_FRAME_SIZE, width, height);
    SetPacked(outType, &MF_MT_FRAME_RATE, (UINT32)fps, 1);
    SetPacked(outType, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(IMFSinkWriter_AddStream(writer, outType, &index)))
        goto done;

    if (FAILED(MFCreateMediaType(&inType)))
        goto done;
    IMFMediaType_SetGUID(inType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetUINT32(inType, &MF_MT_INTERLACE_MODE,
                           MFVideoInterlace_Progressive);
    if (tenBit) {
        /* 10-bit profiles use P010. SDR profiles use Rec.709; HDR profiles
           use BT.2020/PQ. */
        IMFMediaType_SetGUID(inType, &MF_MT_SUBTYPE, &MFVideoFormat_P010);
        if (hdr) {
            IMFMediaType_SetUINT32(inType, &MF_MT_VIDEO_PRIMARIES,
                                   MFVideoPrimaries_BT2020);
            IMFMediaType_SetUINT32(inType, &MF_MT_TRANSFER_FUNCTION,
                                   MFVideoTransFunc_2084);
            IMFMediaType_SetUINT32(inType, &MF_MT_YUV_MATRIX,
                                   MFVideoTransferMatrix_BT2020_10);
            IMFMediaType_SetUINT32(inType, &MF_MT_VIDEO_NOMINAL_RANGE,
                                   MFNominalRange_16_235);
        } else {
            IMFMediaType_SetUINT32(inType, &MF_MT_VIDEO_PRIMARIES,
                                   MFVideoPrimaries_BT709);
            IMFMediaType_SetUINT32(inType, &MF_MT_TRANSFER_FUNCTION,
                                   MFVideoTransFunc_709);
            IMFMediaType_SetUINT32(inType, &MF_MT_YUV_MATRIX,
                                   MFVideoTransferMatrix_BT709);
            IMFMediaType_SetUINT32(inType, &MF_MT_VIDEO_NOMINAL_RANGE,
                                   MFNominalRange_16_235);
        }
    } else {
        /* RGB32 straight in. It costs a video-processor MFT in front of the
           encoder, but duplication already hands over BGRA and converting it
           ourselves would buy one transform for a whole colour path. */
        IMFMediaType_SetGUID(inType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
        IMFMediaType_SetUINT32(inType, &MF_MT_DEFAULT_STRIDE, width * 4);
    }
    SetPacked(inType, &MF_MT_FRAME_SIZE, width, height);
    SetPacked(inType, &MF_MT_FRAME_RATE, (UINT32)fps, 1);
    SetPacked(inType, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(IMFSinkWriter_SetInputMediaType(writer, index, inType, NULL)))
        goto done;
    ok = TRUE;

done:
    if (streamIndex) *streamIndex = index;
    if (inType)  IMFMediaType_Release(inType);
    if (outType) IMFMediaType_Release(outType);
    return ok;
}

/* Configure peak-constrained VBR on the active hardware encoder. The profile
   probe requires this to succeed, so a driver that only accepts an unspecified
   or CBR mode is not advertised as a VBR-capable profile. */
static BOOL CodecApiSetUInt32(ICodecAPI *api, const GUID *key, UINT32 value)
{
    VARIANT v;
    HRESULT hr;
    VariantInit(&v);
    V_VT(&v) = VT_UI4;
    V_UI4(&v) = value;
    hr = ICodecAPI_SetValue(api, key, &v);
    VariantClear(&v);
    return SUCCEEDED(hr);
}

BOOL Encoder_ConfigureVBR(IMFSinkWriter *writer, DWORD stream, EncoderId id,
                          UINT32 width, UINT32 height, int fps)
{
    IMFSinkWriterEx *wx = NULL;
    DWORD t;
    UINT32 average, peak;
    BOOL configured = FALSE;

    if (!writer || id <= ENC_AUTO || id >= ENC_COUNT ||
        width < 16 || height < 16 || fps < 1)
        return FALSE;

    average = PickBitrate(id, width, height, fps);
    peak = average + average / 2u;
    if (peak < average) peak = 0xFFFFFFFFu;

    if (FAILED(IMFSinkWriter_QueryInterface(writer, &IID_IMFSinkWriterEx,
                                            (void **)&wx)))
        return FALSE;

    for (t = 0; t < 8; ++t) {
        GUID category;
        IMFTransform *mft = NULL;
        ICodecAPI *api = NULL;
        HRESULT hr;
        VARIANT mode;
        BOOL supported;

        hr = IMFSinkWriterEx_GetTransformForStream(wx, stream, t,
                                                    &category, &mft);
        if (FAILED(hr) || !mft) {
            if (mft) IMFTransform_Release(mft);
            break;
        }
        hr = IMFTransform_QueryInterface(mft, &SCREENSHOTSERVICE_IID_ICodecAPI,
                                         (void **)&api);
        IMFTransform_Release(mft);
        if (FAILED(hr) || !api) {
            if (api) ICodecAPI_Release(api);
            continue;
        }

        supported =
            ICodecAPI_IsSupported(api, &SCREENSHOTSERVICE_CODECAPI_AVEncCommonRateControlMode) == S_OK &&
            ICodecAPI_IsSupported(api, &SCREENSHOTSERVICE_CODECAPI_AVEncCommonMeanBitRate) == S_OK &&
            ICodecAPI_IsSupported(api, &SCREENSHOTSERVICE_CODECAPI_AVEncCommonMaxBitRate) == S_OK;
        if (!supported) {
            ICodecAPI_Release(api);
            continue;
        }

        VariantInit(&mode);
        V_VT(&mode) = VT_UI4;
        V_UI4(&mode) = (UINT32)eAVEncCommonRateControlMode_PeakConstrainedVBR;
        hr = ICodecAPI_SetValue(api,
                                &SCREENSHOTSERVICE_CODECAPI_AVEncCommonRateControlMode,
                                &mode);
        VariantClear(&mode);
        if (SUCCEEDED(hr) &&
            CodecApiSetUInt32(api, &SCREENSHOTSERVICE_CODECAPI_AVEncCommonMeanBitRate,
                              average) &&
            CodecApiSetUInt32(api, &SCREENSHOTSERVICE_CODECAPI_AVEncCommonMaxBitRate,
                              peak)) {
            configured = TRUE;
            ICodecAPI_Release(api);
            break;
        }
        ICodecAPI_Release(api);
    }

    IMFSinkWriterEx_Release(wx);
    if (configured) {
        Log_Printf(L"encoder: %s configured for peak-constrained VBR (average=%lu, peak=%lu)",
                   Encoder_Name(id), (unsigned long)average, (unsigned long)peak);
    } else {
        Log_Printf(L"encoder: %s does not expose configurable VBR mode and mean/peak bitrates",
                   Encoder_Name(id));
    }
    return configured;
}

/*
 * Ask the chosen HEVC transform for constant QP 0. This is not raw RGB: the
 * standard Media Foundation HEVC path is still 4:2:0, so chroma subsampling
 * and RGB->YUV conversion remain. It is lossless only in the encoder's coded
 * 4:2:0 domain. Failure to set this property must not hide Main10 SDR itself;
 * callers treat QP 0 as a best-effort enhancement.
 */
BOOL Encoder_ConfigureLossless(IMFSinkWriter *writer, DWORD stream)
{
    IMFSinkWriterEx *wx = NULL;
    IMFTransform *mft = NULL;
    DWORD t;
    BOOL configured = FALSE;
    VARIANT v;

    if (!writer)
        return FALSE;
    VariantInit(&v);

    if (FAILED(IMFSinkWriter_QueryInterface(writer, &IID_IMFSinkWriterEx,
                                            (void **)&wx)))
        return FALSE;
    for (t = 0; t < 8; t++) {
        GUID cat;
        ICodecAPI *api = NULL;
        HRESULT hr;
        if (FAILED(IMFSinkWriterEx_GetTransformForStream(wx, stream, t, &cat, &mft)))
            break;
        hr = IMFTransform_QueryInterface(mft, &SCREENSHOTSERVICE_IID_ICodecAPI, (void **)&api);
        if (SUCCEEDED(hr) && api) {
            BOOL supported = SUCCEEDED(ICodecAPI_IsSupported(api, &SCREENSHOTSERVICE_CODECAPI_AVEncVideoEncodeQP));
            if (supported) {
                /* Rate-control mode is UINT32, while VideoEncodeQP is UINT64. */
                V_VT(&v) = VT_UI4;
                V_UI4(&v) = (UINT32)eAVEncCommonRateControlMode_Quality;
                (void)ICodecAPI_SetValue(api, &SCREENSHOTSERVICE_CODECAPI_AVEncCommonRateControlMode, &v);
                V_VT(&v) = VT_UI8;
                V_UI8(&v) = 0u;
                hr = ICodecAPI_SetValue(api, &SCREENSHOTSERVICE_CODECAPI_AVEncVideoEncodeQP, &v);
                if (SUCCEEDED(hr)) {
                    configured = TRUE;
                    ICodecAPI_Release(api);
                    IMFTransform_Release(mft);
                    mft = NULL;
                    break;
                }
            }
            ICodecAPI_Release(api);
        }
        IMFTransform_Release(mft);
        mft = NULL;
    }
    if (mft) IMFTransform_Release(mft);
    IMFSinkWriterEx_Release(wx);
    VariantClear(&v);
    return configured;
}

/* --------------------------------------------------------- vendor preference */

static const wchar_t *NormalizeVendor(const wchar_t *vendor)
{
    if (!vendor || !vendor[0] || !_wcsicmp(vendor, L"auto")) return L"auto";
    if (!_wcsicmp(vendor, L"amd"))    return L"amd";
    if (!_wcsicmp(vendor, L"intel"))  return L"intel";
    if (!_wcsicmp(vendor, L"nvidia")) return L"nvidia";
    return L"auto";
}

/* Media Foundation specifies this attribute as a string such as "VEN_1002",
   not a UINT32 PCI vendor ID. Drivers may append additional PnP details, so
   compare the standard eight-character VEN_xxxx prefix. */
static const wchar_t *VendorIdToken(const wchar_t *vendor)
{
    vendor = NormalizeVendor(vendor);
    if (!_wcsicmp(vendor, L"amd"))    return L"VEN_1002";
    if (!_wcsicmp(vendor, L"intel"))  return L"VEN_8086";
    if (!_wcsicmp(vendor, L"nvidia")) return L"VEN_10DE";
    return L"";
}

static BOOL GetMftVendorId(IMFAttributes *attrs, wchar_t *out, size_t cch)
{
    LPWSTR actual = NULL;
    UINT32 actualLen = 0;
    HRESULT hr;

    if (!out || cch == 0) return FALSE;
    out[0] = L'\0';
    if (!attrs) return FALSE;

    hr = IMFAttributes_GetAllocatedString(
        attrs, &MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &actual, &actualLen);
    if (SUCCEEDED(hr) && actual) {
        StringCchCopyW(out, cch, actual);
        CoTaskMemFree(actual);
        return out[0] != L'\0';
    }
    if (actual) CoTaskMemFree(actual);
    return FALSE;
}

/* The hardware URL is an attribute of both the MFT activation object and
   the instantiated hardware MFT. It is a safer runtime identity check than
   MFT_TRANSFORM_CLSID_Attribute, which is documented for activation objects. */
static BOOL GetMftHardwareUrl(IMFAttributes *attrs, wchar_t *out, size_t cch)
{
    LPWSTR actual = NULL;
    UINT32 actualLen = 0;
    HRESULT hr;

    if (!out || cch == 0) return FALSE;
    out[0] = L'\0';
    if (!attrs) return FALSE;

    hr = IMFAttributes_GetAllocatedString(
        attrs, &MFT_ENUM_HARDWARE_URL_Attribute, &actual, &actualLen);
    if (SUCCEEDED(hr) && actual && actual[0]) {
        StringCchCopyW(out, cch, actual);
        CoTaskMemFree(actual);
        return out[0] != L'\0';
    }
    if (actual) CoTaskMemFree(actual);
    return FALSE;
}

static BOOL VendorNameMatches(const wchar_t *name, const wchar_t *vendor)
{
    if (!name || !name[0]) return FALSE;
    vendor = NormalizeVendor(vendor);
    if (!lstrcmpiW(vendor, L"auto")) return TRUE;
    if (!_wcsicmp(vendor, L"amd"))
        return StrStrIW(name, L"AMD") != NULL ||
               StrStrIW(name, L"Advanced Micro Devices") != NULL ||
               StrStrIW(name, L"ATI") != NULL || StrStrIW(name, L"AMF") != NULL ||
               StrStrIW(name, L"Radeon") != NULL;
    if (!_wcsicmp(vendor, L"intel"))
        return StrStrIW(name, L"Intel") != NULL || StrStrIW(name, L"Quick Sync") != NULL;
    if (!_wcsicmp(vendor, L"nvidia"))
        return StrStrIW(name, L"NVIDIA") != NULL || StrStrIW(name, L"NVENC") != NULL;
    return FALSE;
}

/* Some OEM drivers publish generic MFT friendly names. Prefer the hardware
   vendor ID attached by Media Foundation over guessing from that name. */
static BOOL VendorAttributesMatch(IMFAttributes *attrs, const wchar_t *vendor)
{
    wchar_t actual[128];
    const wchar_t *wanted = VendorIdToken(vendor);

    if (!wanted[0] || !GetMftVendorId(attrs, actual, ARRAYSIZE(actual)))
        return FALSE;
    /* Usually this is exactly "VEN_10DE". Some driver branches include a
       PnP prefix/suffix, so search for the vendor token instead of assuming
       it begins at character zero. The exact activation URL is verified again
       after the Sink Writer has instantiated the MFT. */
    return StrStrIW(actual, wanted) != NULL;
}

/* A few driver versions omit MFT_ENUM_HARDWARE_VENDOR_ID_Attribute but put
   the PCI vendor token into the device URL. Use that as an independent
   hardware identity source; friendly names are only the last resort. */
static BOOL VendorHardwareUrlMatches(const wchar_t *hardwareUrl,
                                     const wchar_t *vendor)
{
    const wchar_t *token = VendorIdToken(vendor);
    if (!hardwareUrl || !hardwareUrl[0] || !token[0])
        return FALSE;
    return StrStrIW(hardwareUrl, token) != NULL;
}

void Encoder_GetGpuInventory(EncoderGpuInventory *inventory)
{
    IDXGIFactory1 *factory = NULL;
    UINT index;

    if (!inventory)
        return;
    ZeroMemory(inventory, sizeof(*inventory));
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return;

    /* Enumerate every adapter: hybrid laptops and multi-GPU desktops must not
       be reduced to DXGI adapter zero. */
    for (index = 0; index < 32; ++index) {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc;
        HRESULT hr = IDXGIFactory1_EnumAdapters1(factory, index, &adapter);

        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr)) {
            if (adapter) IDXGIAdapter1_Release(adapter);
            continue;
        }
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc)) &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            BOOL *present = NULL;
            wchar_t *name = NULL;

            switch (desc.VendorId) {
            case 0x1002u: present = &inventory->amd; name = inventory->amdName; break;
            case 0x8086u: present = &inventory->intel; name = inventory->intelName; break;
            case 0x10DEu: present = &inventory->nvidia; name = inventory->nvidiaName; break;
            default: break;
            }
            if (present && name && !*present) {
                *present = TRUE;
                StringCchCopyW(name, 128, desc.Description);
                Log_Printf(L"encoder: detected GPU vendor %04X, device %04X: %s",
                           desc.VendorId, desc.DeviceId, desc.Description);
            }
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
}

static BOOL OutputTypeForEncoder(EncoderId id, MFT_REGISTER_TYPE_INFO *type)
{
    if (!type || id <= ENC_AUTO || id >= ENC_COUNT) return FALSE;
    type->guidMajorType = MFMediaType_Video;
    if (id == ENC_H264) {
        type->guidSubtype = MFVideoFormat_H264;
    } else if (id == ENC_AV1_8 || id == ENC_AV1_10 || id == ENC_AV1_10_HDR) {
        type->guidSubtype = SCREENSHOTSERVICE_MFVideoFormat_AV1;
    } else {
        type->guidSubtype = MFVideoFormat_HEVC;
    }
    return TRUE;
}

void Encoder_ClearVendor(void)
{
    if (g_vendorMftRegistered) {
        (void)MFTUnregisterLocalByCLSID(g_vendorMftClsid);
        g_vendorMftRegistered = FALSE;
    }
    ZeroMemory(&g_vendorMftClsid, sizeof(g_vendorMftClsid));
    ZeroMemory(g_vendorMftHardwareUrl, sizeof(g_vendorMftHardwareUrl));
    ZeroMemory(g_vendorMftName, sizeof(g_vendorMftName));
}

/*
 * Sink Writer does not have a documented encoder-by-vendor argument. We can
 * register a matching encoder MFT locally to put it ahead of normal candidates,
 * then verify the transform that Sink Writer actually selected. This avoids
 * silently claiming that a requested vendor was used when Windows ignored it.
 */
static BOOL Encoder_PrepareVendorCandidate(EncoderId id, const wchar_t *vendor,
                                           UINT32 wantedOrdinal,
                                           const wchar_t *wantedHardwareUrl,
                                           const CLSID *wantedClsid,
                                           BOOL *hadCandidate)
{
    MFT_REGISTER_TYPE_INFO outType;
    IMFActivate **acts = NULL;
    UINT32 count = 0, i;
    HRESULT hr;
    const wchar_t *requested = NormalizeVendor(vendor);
    const wchar_t *wanted = requested;
    BOOL found = FALSE;
    BOOL urlPinned = wantedHardwareUrl && wantedHardwareUrl[0];
    UINT32 matchingOrdinal = 0;

    if (hadCandidate) *hadCandidate = FALSE;

    /* In Auto mode, pin the same concrete vendor that passed the full MP4
       probe for this profile. MFTEnumEx ordering is not a vendor preference
       and must not change between probe and recording. */
    if (!_wcsicmp(requested, L"auto") && id > ENC_AUTO && id < ENC_COUNT &&
        g_caps.vendor[id][0] && _wcsicmp(g_caps.vendor[id], L"auto"))
        wanted = NormalizeVendor(g_caps.vendor[id]);

    Encoder_ClearVendor();
    if (!OutputTypeForEncoder(id, &outType)) return FALSE;

    /* Strict hardware-only policy: do not enumerate software MFT categories.
       Auto selects a compatible hardware MFT; a named vendor selects only a
       matching hardware activation. */
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   NULL, &outType, &acts, &count);
    if (FAILED(hr) || !acts || count == 0) {
        if (acts) {
            for (i = 0; i < count; ++i) IMFActivate_Release(acts[i]);
            CoTaskMemFree(acts);
        }
        Log_Printf(L"encoder: no hardware MFTs enumerated for %s / %s",
                   Encoder_Name(id), wanted);
        return FALSE;
    }

    for (i = 0; i < count; ++i) {
        LPWSTR name = NULL;
        LPWSTR hardwareUrl = NULL;
        UINT32 nameLen = 0, urlLen = 0;
        CLSID clsid;
        wchar_t vendorId[128] = L"";
        IMFAttributes *attrs = (IMFAttributes *)acts[i];
        BOOL haveName, haveUrl, nameMatch, vendorIdMatch;

        haveName = SUCCEEDED(IMFAttributes_GetAllocatedString(attrs,
                &MFT_FRIENDLY_NAME_Attribute, &name, &nameLen)) && name && name[0];
        haveUrl = SUCCEEDED(IMFAttributes_GetAllocatedString(attrs,
                &MFT_ENUM_HARDWARE_URL_Attribute, &hardwareUrl, &urlLen)) &&
                hardwareUrl && hardwareUrl[0];
        (void)GetMftVendorId(attrs, vendorId, ARRAYSIZE(vendorId));
        if (!_wcsicmp(wanted, L"auto")) {
            nameMatch = TRUE;
            vendorIdMatch = TRUE;
        } else if (vendorId[0]) {
            /* A reported vendor ID is authoritative. Do not let a friendly
               name override a contradictory PCI vendor ID. */
            nameMatch = FALSE;
            vendorIdMatch = VendorAttributesMatch(attrs, wanted);
        } else if (haveUrl && StrStrIW(hardwareUrl, L"VEN_") != NULL) {
            /* The hardware URL is the next strongest identity source. */
            nameMatch = FALSE;
            vendorIdMatch = VendorHardwareUrlMatches(hardwareUrl, wanted);
        } else {
            /* Last resort for OEMs that omit both the vendor ID and PCI token. */
            nameMatch = haveName && VendorNameMatches(name, wanted);
            vendorIdMatch = FALSE;
        }
        Log_Printf(L"encoder: enumerated HARDWARE MFT '%s', vendor id '%s', hardware URL '%s' for %s; match=%s",
                   haveName ? name : L"(unnamed)",
                   vendorId[0] ? vendorId : L"(not provided)",
                   haveUrl ? hardwareUrl : L"(missing)", Encoder_Name(id),
                   (nameMatch || vendorIdMatch) ? L"yes" : L"no");

        /* A hardware MFT must advertise its device URL. Cache that URL to
           verify the active Sink Writer transform without assuming runtime
           MFT attributes contain the activation object's CLSID. */
        if (haveUrl && (nameMatch || vendorIdMatch) &&
            SUCCEEDED(IMFAttributes_GetGUID(attrs, &MFT_TRANSFORM_CLSID_Attribute,
                                            &clsid))) {
            /* Count only candidates that have all attributes required for a
               deterministic hardware selection. If one NVIDIA MFT advertises
               a codec but cannot initialise, the probe must reach the next
               NVIDIA MFT before dropping to another GPU vendor. */
            {
                UINT32 currentOrdinal = matchingOrdinal++;
                BOOL candidateMatches = urlPinned
                    ? (!_wcsicmp(hardwareUrl, wantedHardwareUrl) &&
                       (!wantedClsid || IsEqualGUID(&clsid, wantedClsid)))
                    : currentOrdinal == wantedOrdinal;
                if (!candidateMatches) {
                    if (name) CoTaskMemFree(name);
                    if (hardwareUrl) CoTaskMemFree(hardwareUrl);
                    IMFActivate_Release(acts[i]);
                    continue;
                }
                if (hadCandidate) *hadCandidate = TRUE;
                LPCWSTR registerName = haveName ? name : Encoder_Name(id);
                hr = MFTRegisterLocalByCLSID(&clsid, &MFT_CATEGORY_VIDEO_ENCODER,
                        (LPWSTR)registerName,
                        MFT_ENUM_FLAG_HARDWARE,
                        0, NULL, 1, &outType);
                if (SUCCEEDED(hr)) {
                    g_vendorMftClsid = clsid;
                    g_vendorMftRegistered = TRUE;
                    StringCchCopyW(g_vendorMftHardwareUrl,
                                   ARRAYSIZE(g_vendorMftHardwareUrl), hardwareUrl);
                    StringCchCopyW(g_vendorMftName, ARRAYSIZE(g_vendorMftName),
                                   registerName);
                    Log_Printf(L"encoder: pinned hardware MFT candidate %lu '%s' (vendor id '%s', URL '%s') for %s%s%s",
                               (unsigned long)currentOrdinal, registerName,
                               vendorId[0] ? vendorId : L"not provided", hardwareUrl,
                               Encoder_Name(id),
                               !_wcsicmp(requested, L"auto") ? L" [Auto]" : L"",
                               urlPinned ? L" [exact cached URL]" : L"");
                    found = TRUE;
                } else {
                    Log_Printf(L"encoder: could not register hardware MFT candidate %lu '%s' (0x%08lX)",
                               (unsigned long)currentOrdinal, registerName,
                               (unsigned long)hr);
                }
            }
            if (name) CoTaskMemFree(name);
            if (hardwareUrl) CoTaskMemFree(hardwareUrl);
            IMFActivate_Release(acts[i]);
            ++i;
            break;
        }

        if (name) CoTaskMemFree(name);
        if (hardwareUrl) CoTaskMemFree(hardwareUrl);
        IMFActivate_Release(acts[i]);
    }
    for (; i < count; ++i) IMFActivate_Release(acts[i]);
    CoTaskMemFree(acts);

    if (!found && (!hadCandidate || !*hadCandidate)) {
        if (urlPinned)
            Log_Printf(L"encoder: cached hardware MFT URL '%s' not present for %s / %s",
                       wantedHardwareUrl, wanted, Encoder_Name(id));
        else
            Log_Printf(L"encoder: no matching HARDWARE MFT candidate %lu with a hardware URL for %s / %s",
                       (unsigned long)wantedOrdinal, wanted, Encoder_Name(id));
    }
    return found;
}

BOOL Encoder_PrepareVendor(EncoderId id, const wchar_t *vendor)
{
    const wchar_t *requested = NormalizeVendor(vendor);
    const wchar_t *wanted = requested;
    const wchar_t *wantedUrl = NULL;
    const CLSID *wantedClsid = NULL;
    UINT32 ordinal = 0;
    if (id > ENC_AUTO && id < ENC_COUNT) {
        if (!_wcsicmp(requested, L"auto") && g_caps.vendor[id][0])
            wanted = NormalizeVendor(g_caps.vendor[id]);
        if (g_caps.vendor[id][0] && !_wcsicmp(g_caps.vendor[id], wanted)) {
            ordinal = g_caps.candidate[id];
            if (g_caps.hardwareUrl[id][0]) {
                wantedUrl = g_caps.hardwareUrl[id];
                if (!IsEqualGUID(&g_caps.mftClsid[id], &GUID_NULL))
                    wantedClsid = &g_caps.mftClsid[id];
            }
        }
    }
    if (id > ENC_AUTO && id < ENC_COUNT)
        Log_Printf(L"encoder: recording request=%s, resolved MFT vendor=%s, candidate=%lu, exact-url+clsid-pinned=%s, profile=%s",
                   requested, wanted, (unsigned long)ordinal,
                   (wantedUrl && wantedClsid) ? L"yes" : (wantedUrl ? L"url-only" : L"no"),
                   Encoder_Name(id));
    return Encoder_PrepareVendorCandidate(id, vendor, ordinal, wantedUrl,
                                           wantedClsid, NULL);
}

BOOL Encoder_WriterMatchesVendor(IMFSinkWriter *writer, DWORD stream,
                                 const wchar_t *vendor)
{
    IMFSinkWriterEx *wx = NULL;
    IMFTransform *mft = NULL;
    wchar_t chosen[128] = L"";
    DWORD t;
    BOOL sawEncoderCategory = FALSE;
    BOOL matchingHardwareEncoder = FALSE;
    const wchar_t *wanted = NormalizeVendor(vendor);

    /* A hardware-only activation and its device URL are mandatory in every
       mode, including Auto. A software MFT has no hardware URL. */
    if (!writer || !g_vendorMftRegistered || !g_vendorMftHardwareUrl[0])
        return FALSE;
    if (FAILED(IMFSinkWriter_QueryInterface(writer, &IID_IMFSinkWriterEx,
                                            (void **)&wx)))
        return FALSE;
    for (t = 0; t < 8; ++t) {
        GUID category;
        IMFAttributes *attrs = NULL;
        BOOL isEncoder;

        if (FAILED(IMFSinkWriterEx_GetTransformForStream(wx, stream, t,
                                                         &category, &mft)))
            break;
        isEncoder = IsEqualGUID(&category, &MFT_CATEGORY_VIDEO_ENCODER);
        if (isEncoder)
            sawEncoderCategory = TRUE;
        if (SUCCEEDED(IMFTransform_GetAttributes(mft, &attrs))) {
            wchar_t actualUrl[1024] = L"";
            LPWSTR name = NULL;
            UINT32 length = 0;
            BOOL urlMatch = GetMftHardwareUrl(attrs, actualUrl, ARRAYSIZE(actualUrl)) &&
                !_wcsicmp(actualUrl, g_vendorMftHardwareUrl);

            if (SUCCEEDED(IMFAttributes_GetAllocatedString(attrs,
                    &MFT_FRIENDLY_NAME_Attribute, &name, &length))) {
                if (name && name[0])
                    StringCchCopyW(chosen, ARRAYSIZE(chosen), name);
                if (name) CoTaskMemFree(name);
            }
            /* Exact hardware URL identifies the device MFT we selected after
               its activation object passed the vendor preference check. */
            if (isEncoder && urlMatch)
                matchingHardwareEncoder = TRUE;
            IMFAttributes_Release(attrs);
        }
        IMFTransform_Release(mft);
        mft = NULL;
    }
    if (mft) IMFTransform_Release(mft);
    IMFSinkWriterEx_Release(wx);

    /* Do not infer hardware encoding from async scheduling or a friendly name.
       If Sink Writer exposes no encoder category, fail closed rather than risk
       accepting a software codec in the transform chain. */
    if (!matchingHardwareEncoder || !sawEncoderCategory)
        Log_Printf(L"encoder: hardware-only check rejected selected transform '%s' for vendor '%s' (hardware URL/category mismatch)",
                   chosen[0] ? chosen : L"(unnamed)", wanted);
    else
        Log_Printf(L"encoder: verified hardware encoder '%s' for vendor '%s' (device URL matched)",
                   chosen[0] ? chosen : g_vendorMftName, wanted);
    return matchingHardwareEncoder && sawEncoderCategory;
}

/* ------------------------------------------------------------- the probe */

/* Names the encoder MFT the writer settled on, so the log and the settings
   list say "NVIDIA HEVC Encoder MFT" rather than just "HEVC". */
static void NameTransform(IMFSinkWriter *writer, DWORD stream, EncoderId id)
{
    IMFSinkWriterEx *wx = NULL;
    IMFTransform    *mft = NULL;
    DWORD            t;
    BOOL             sawEncoderCategory = FALSE;
    BOOL             pinnedEncoderSeen = FALSE;

    if (FAILED(IMFSinkWriter_QueryInterface(writer, &IID_IMFSinkWriterEx,
                                            (void **)&wx)))
        return;
    for (t = 0; t < 8; t++) {
        GUID category;
        IMFAttributes *attrs = NULL;

        if (FAILED(IMFSinkWriterEx_GetTransformForStream(wx, stream, t,
                                                        &category, &mft)))
            break;
        if (IsEqualGUID(&category, &MFT_CATEGORY_VIDEO_ENCODER))
            sawEncoderCategory = TRUE;
        if (SUCCEEDED(IMFTransform_GetAttributes(mft, &attrs))) {
            wchar_t actualUrl[1024] = L"";
            LPWSTR name = NULL;
            UINT32 len = 0;
            BOOL urlMatch = GetMftHardwareUrl(attrs, actualUrl, ARRAYSIZE(actualUrl)) &&
                g_vendorMftHardwareUrl[0] &&
                !_wcsicmp(actualUrl, g_vendorMftHardwareUrl);

            if (urlMatch && IsEqualGUID(&category, &MFT_CATEGORY_VIDEO_ENCODER)) {
                pinnedEncoderSeen = TRUE;
                if (SUCCEEDED(IMFAttributes_GetAllocatedString(attrs,
                        &MFT_FRIENDLY_NAME_Attribute, &name, &len)) && name) {
                    StringCchCopyW(g_caps.mft[id], ARRAYSIZE(g_caps.mft[id]), name);
                    CoTaskMemFree(name);
                }
            }
            IMFAttributes_Release(attrs);
        }
        IMFTransform_Release(mft);
        mft = NULL;
    }
    if (mft) IMFTransform_Release(mft);
    IMFSinkWriterEx_Release(wx);

    if (sawEncoderCategory && pinnedEncoderSeen) {
        g_caps.hardware[id] = TRUE;
        if (!g_caps.mft[id][0])
            StringCchCopyW(g_caps.mft[id], ARRAYSIZE(g_caps.mft[id]),
                           g_vendorMftName);
    }
}

/* One frame of mid-grey in whatever layout this encoder takes. */
static BOOL PushProbeFrame(IMFSinkWriter *writer, DWORD stream, EncoderId id)
{
    IMFMediaBuffer *buf = NULL;
    IMFSample      *sample = NULL;
    DWORD           size;
    BYTE           *p = NULL;
    BOOL            ok = FALSE;

    if (Encoder_Is10Bit(id))
        size = PROBE_W * PROBE_H * 3;               /* P010: 2 B Y + 1 B UV */
    else
        size = PROBE_W * PROBE_H * 4;               /* RGB32 */

    if (FAILED(MFCreateMemoryBuffer(size, &buf)))
        return FALSE;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, NULL))) {
        FillMemory(p, size, 0x80);
        IMFMediaBuffer_Unlock(buf);
    }
    IMFMediaBuffer_SetCurrentLength(buf, size);

    if (SUCCEEDED(MFCreateSample(&sample))) {
        IMFSample_AddBuffer(sample, buf);
        IMFSample_SetSampleTime(sample, 0);
        IMFSample_SetSampleDuration(sample, 10000000LL / PROBE_FPS);
        ok = SUCCEEDED(IMFSinkWriter_WriteSample(writer, stream, sample));
        IMFSample_Release(sample);
    }
    IMFMediaBuffer_Release(buf);
    return ok;
}

/*
 * The whole point: configure exactly as recording would, write a frame and
 * finalise. Anything short of finalising proves nothing - AV1 gets all the way
 * to the last step before the MP4 sink admits it cannot mux it.
 */
static BOOL ProbeOneForVendorCandidate(EncoderId id, const wchar_t *dir,
                                       const wchar_t *vendor, UINT32 candidateOrdinal,
                                       BOOL *hadCandidate)
{
    IMFSinkWriter *writer = NULL;
    IMFAttributes *attrs = NULL;
    wchar_t        path[MAX_PATH];
    DWORD          stream = 0;
    HRESULT        hr = E_FAIL;
    BOOL           ok = FALSE;
    const char    *stage = "create";

    if (FAILED(StringCchPrintfW(path, MAX_PATH, L"%s\\bsnip-probe-%s.mp4",
                                dir, Encoder_Token(id))))
        return FALSE;
    DeleteFileW(path);

    if (!Encoder_PrepareVendorCandidate(id, vendor, candidateOrdinal, NULL, NULL,
                                        hadCandidate)) {
        DeleteFileW(path);
        return FALSE;
    }

    hr = MFCreateAttributes(&attrs, 3);
    if (FAILED(hr)) {
        stage = "attributes";
        goto done;
    }
    IMFAttributes_SetGUID(attrs, &MF_TRANSCODE_CONTAINERTYPE,
                          &MFTranscodeContainerType_MPEG4);
    IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    IMFAttributes_SetUINT32(attrs, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    hr = MFCreateSinkWriterFromURL(path, NULL, attrs, &writer);
    if (FAILED(hr)) goto done;

    stage = "configure";
    if (!Encoder_AddVideoStream(writer, id, PROBE_W, PROBE_H, PROBE_FPS, &stream)) {
        hr = E_FAIL;
        goto done;
    }

    stage = "begin";
    hr = IMFSinkWriter_BeginWriting(writer);
    if (FAILED(hr)) goto done;

    NameTransform(writer, stream, id);
    if (!Encoder_WriterMatchesVendor(writer, stream, vendor)) {
        hr = E_FAIL;
        stage = "vendor verification";
        goto done;
    }
    /* The actual encoder must expose the hardware URL observed on the
       hardware-only activation; ProbeOne must never bless a software encoder. */
    if (!g_caps.hardware[id]) {
        hr = E_FAIL;
        stage = "hardware verification";
        goto done;
    }
    stage = "VBR rate control";
    if (!Encoder_ConfigureVBR(writer, stream, id, PROBE_W, PROBE_H, PROBE_FPS)) {
        hr = E_FAIL;
        goto done;
    }

    stage = "write";
    if (!PushProbeFrame(writer, stream, id)) { hr = E_FAIL; goto done; }

    stage = "finalise";
    hr = IMFSinkWriter_Finalize(writer);
    if (FAILED(hr)) goto done;
    ok = TRUE;

done:
    if (ok)
        Log_Printf(L"encoder: %s usable via %s%s", Encoder_Name(id),
                   g_caps.mft[id][0] ? g_caps.mft[id] : L"(unnamed)",
                   g_caps.hardware[id] ? L" [hardware]" : L"");
    else
        Log_Printf(L"encoder: %s unusable, failed at %hs (0x%08lX)",
                   Encoder_Name(id), stage, (unsigned long)hr);

    if (ok && id > ENC_AUTO && id < ENC_COUNT && g_vendorMftHardwareUrl[0]) {
        StringCchCopyW(g_caps.hardwareUrl[id], ARRAYSIZE(g_caps.hardwareUrl[id]),
                       g_vendorMftHardwareUrl);
        g_caps.mftClsid[id] = g_vendorMftClsid;
    }
    if (!ok && id > ENC_AUTO && id < ENC_COUNT) {
        g_caps.hardwareUrl[id][0] = L'\0';
        ZeroMemory(&g_caps.mftClsid[id], sizeof(g_caps.mftClsid[id]));
    }
    if (writer) IMFSinkWriter_Release(writer);
    if (attrs)  IMFAttributes_Release(attrs);
    Encoder_ClearVendor();
    DeleteFileW(path);
    return ok;
}

/* Try every enumerated hardware MFT for this vendor before giving up on that
   vendor. A single bad/limited MFT should not hide another MFT from the same
   vendor (for example, a distinct NVIDIA driver/codec path). The same ordinal
   is persisted so recording pins the candidate that passed this full test. */
static BOOL ProbeOneForVendor(EncoderId id, const wchar_t *dir, const wchar_t *vendor)
{
    UINT32 ordinal;
    for (ordinal = 0; ordinal < 32; ++ordinal) {
        BOOL hadCandidate = FALSE;
        g_caps.candidate[id] = ordinal;
        g_caps.hardwareUrl[id][0] = L'\0';
        ZeroMemory(&g_caps.mftClsid[id], sizeof(g_caps.mftClsid[id]));
        g_caps.mft[id][0] = L'\0';
        g_caps.hardware[id] = FALSE;
        if (ProbeOneForVendorCandidate(id, dir, vendor, ordinal, &hadCandidate)) {
            g_caps.candidate[id] = ordinal;
            return TRUE;
        }
        if (!hadCandidate)
            break;
        Log_Printf(L"encoder: hardware MFT candidate %lu failed for %s / %s; trying next candidate",
                   (unsigned long)ordinal, Encoder_Name(id), NormalizeVendor(vendor));
    }
    g_caps.candidate[id] = 0;
    g_caps.hardwareUrl[id][0] = L'\0';
    ZeroMemory(&g_caps.mftClsid[id], sizeof(g_caps.mftClsid[id]));
    return FALSE;
}

/* Auto does not mean "use the first transform returned by MFTEnumEx". The
   OS enumeration order varies by driver and adapter arrangement. Probe each
   vendor explicitly, in deterministic preference order, and remember which
   one actually survived AddStream, WriteSample and Finalize. If NVIDIA cannot
   provide a profile, the same profile is tried with AMD and then Intel. */
static BOOL ProbeOne(EncoderId id, const wchar_t *dir, const wchar_t *vendor)
{
    static const wchar_t *const autoOrder[] = { L"nvidia", L"amd", L"intel" };
    const wchar_t *requested = NormalizeVendor(vendor);
    size_t i;

    g_caps.vendor[id][0] = L'\0';
    g_caps.mft[id][0] = L'\0';
    g_caps.hardwareUrl[id][0] = L'\0';
    ZeroMemory(&g_caps.mftClsid[id], sizeof(g_caps.mftClsid[id]));
    g_caps.hardware[id] = FALSE;

    if (_wcsicmp(requested, L"auto")) {
        BOOL ok = ProbeOneForVendor(id, dir, requested);
        if (ok)
            StringCchCopyW(g_caps.vendor[id], ARRAYSIZE(g_caps.vendor[id]), requested);
        return ok;
    }

    for (i = 0; i < ARRAYSIZE(autoOrder); ++i) {
        g_caps.mft[id][0] = L'\0';
        g_caps.hardware[id] = FALSE;
        if (ProbeOneForVendor(id, dir, autoOrder[i])) {
            StringCchCopyW(g_caps.vendor[id], ARRAYSIZE(g_caps.vendor[id]), autoOrder[i]);
            Log_Printf(L"encoder: Auto resolved %s to %s MFT '%s'",
                       Encoder_Name(id), autoOrder[i], g_caps.mft[id]);
            return TRUE;
        }
    }

    /* Keep hardware-only behavior strict. A codec profile is unavailable
       when no named physical-GPU MFT passes the actual MP4 probe. */
    return FALSE;
}

/* ------------------------------------------------------------- the cache */

/* Adapter plus user-mode driver version: a driver update can add or remove an
   encoder, and is the usual reason a cached answer goes stale. */
static void CacheKey(wchar_t *out, size_t cch, const wchar_t *vendor)
{
    IDXGIFactory1 *factory = NULL;
    UINT index;

    StringCchPrintfW(out, cch, L"ScreenshotService-probe-v29 MFT-only Main10-profile-probe vendor-first exact-url-clsid-pinned vendor=%s",
                     NormalizeVendor(vendor));
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return;

    /* Include every adapter's PCI IDs and driver version. A newly added or
       updated secondary GPU must invalidate a previous negative probe. */
    for (index = 0; index < 32; ++index) {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc;
        LARGE_INTEGER driverVersion;
        wchar_t part[80];
        HRESULT hr = IDXGIFactory1_EnumAdapters1(factory, index, &adapter);

        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr)) {
            if (adapter) IDXGIAdapter1_Release(adapter);
            continue;
        }
        ZeroMemory(&driverVersion, sizeof(driverVersion));
        (void)IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice,
                                                 &driverVersion);
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
            StringCchPrintfW(part, ARRAYSIZE(part), L"|%04X:%04X:%08lX%08lX",
                             desc.VendorId, desc.DeviceId,
                             (unsigned long)driverVersion.HighPart,
                             (unsigned long)driverVersion.LowPart);
            (void)StringCchCatW(out, cch, part);
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
}

#define CACHE_SEC L"EncoderCache"

static BOOL LoadCache(const wchar_t *key)
{
    wchar_t stored[2048];
    EncoderId id;

    (void)Settings_RegGetString(CACHE_SEC, L"Key", L"", stored, ARRAYSIZE(stored));
    if (lstrcmpiW(stored, key) != 0)
        return FALSE;

    for (id = ENC_H264; id < ENC_COUNT; id++) {
        wchar_t name[32];
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s", Encoder_Token(id));
        g_caps.mftUsable[id] = Settings_RegGetInt(CACHE_SEC, name, 0) != 0;
        g_caps.usable[id] = g_caps.mftUsable[id];
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.mft", Encoder_Token(id));
        (void)Settings_RegGetString(CACHE_SEC, name, L"", g_caps.mft[id],
                                    ARRAYSIZE(g_caps.mft[id]));
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.hw", Encoder_Token(id));
        g_caps.hardware[id] = Settings_RegGetInt(CACHE_SEC, name, 0) != 0;
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.vendor", Encoder_Token(id));
        (void)Settings_RegGetString(CACHE_SEC, name, L"", g_caps.vendor[id],
                                    ARRAYSIZE(g_caps.vendor[id]));
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.candidate", Encoder_Token(id));
        g_caps.candidate[id] = (UINT32)Settings_RegGetInt(CACHE_SEC, name, 0);
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.url", Encoder_Token(id));
        (void)Settings_RegGetString(CACHE_SEC, name, L"", g_caps.hardwareUrl[id],
                                    ARRAYSIZE(g_caps.hardwareUrl[id]));
        {
            wchar_t clsidText[40] = L"";
            StringCchPrintfW(name, ARRAYSIZE(name), L"%s.clsid", Encoder_Token(id));
            (void)Settings_RegGetString(CACHE_SEC, name, L"", clsidText,
                                        ARRAYSIZE(clsidText));
            ZeroMemory(&g_caps.mftClsid[id], sizeof(g_caps.mftClsid[id]));
            if (clsidText[0] && FAILED(CLSIDFromString(clsidText,
                                                       &g_caps.mftClsid[id])))
                ZeroMemory(&g_caps.mftClsid[id], sizeof(g_caps.mftClsid[id]));
        }
    }
    return TRUE;
}

static void SaveCache(const wchar_t *key)
{
    EncoderId id;

    (void)Settings_RegSetString(CACHE_SEC, L"Key", key);
    for (id = ENC_H264; id < ENC_COUNT; id++) {
        wchar_t name[32];
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s", Encoder_Token(id));
        (void)Settings_RegSetInt(CACHE_SEC, name, g_caps.mftUsable[id] ? 1 : 0);
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.mft", Encoder_Token(id));
        (void)Settings_RegSetString(CACHE_SEC, name, g_caps.mft[id]);
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.hw", Encoder_Token(id));
        (void)Settings_RegSetInt(CACHE_SEC, name, g_caps.hardware[id] ? 1 : 0);
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.vendor", Encoder_Token(id));
        (void)Settings_RegSetString(CACHE_SEC, name, g_caps.vendor[id]);
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.candidate", Encoder_Token(id));
        {
            wchar_t ordinal[16];
            StringCchPrintfW(ordinal, ARRAYSIZE(ordinal), L"%lu",
                             (unsigned long)g_caps.candidate[id]);
            (void)Settings_RegSetString(CACHE_SEC, name, ordinal);
        }
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.url", Encoder_Token(id));
        (void)Settings_RegSetString(CACHE_SEC, name, g_caps.hardwareUrl[id]);
        {
            wchar_t clsidText[40] = L"";
            StringCchPrintfW(name, ARRAYSIZE(name), L"%s.clsid", Encoder_Token(id));
            if (StringFromGUID2(&g_caps.mftClsid[id], clsidText,
                                (int)ARRAYSIZE(clsidText)) <= 0)
                clsidText[0] = L'\0';
            (void)Settings_RegSetString(CACHE_SEC, name, clsidText);
        }
    }
}

/* Apply the vendor-MFT Main10 fallback equally to fresh and cached probes.
   QP 0 is optional and is not part of whether the SDR profile is available. */
static void ExposeMain10SdrFallback(void)
{
    if (!g_caps.usable[ENC_HEVC10_SDR] && g_caps.usable[ENC_HEVC10]) {
        g_caps.usable[ENC_HEVC10_SDR] = TRUE;
        g_caps.mftUsable[ENC_HEVC10_SDR] = g_caps.mftUsable[ENC_HEVC10];
        g_caps.hardware[ENC_HEVC10_SDR] = g_caps.hardware[ENC_HEVC10];
        StringCchCopyW(g_caps.mft[ENC_HEVC10_SDR],
                       ARRAYSIZE(g_caps.mft[ENC_HEVC10_SDR]),
                       g_caps.mft[ENC_HEVC10]);
        StringCchCopyW(g_caps.vendor[ENC_HEVC10_SDR],
                       ARRAYSIZE(g_caps.vendor[ENC_HEVC10_SDR]),
                       g_caps.vendor[ENC_HEVC10]);
        g_caps.candidate[ENC_HEVC10_SDR] = g_caps.candidate[ENC_HEVC10];
        StringCchCopyW(g_caps.hardwareUrl[ENC_HEVC10_SDR],
                       ARRAYSIZE(g_caps.hardwareUrl[ENC_HEVC10_SDR]),
                       g_caps.hardwareUrl[ENC_HEVC10]);
        g_caps.mftClsid[ENC_HEVC10_SDR] = g_caps.mftClsid[ENC_HEVC10];
        Log_Printf(L"encoder: exposing HEVC Main10 SDR through the working Main10 MFT; Rec.709 tags are written after muxing");
    }
}

void Encoder_ProbeForVendor(const wchar_t *vendor, BOOL force)
{
    wchar_t key[2048];
    wchar_t dir[MAX_PATH];
    wchar_t normalized[16];
    EncoderId id;
    ULONGLONG t0;

    StringCchCopyW(normalized, ARRAYSIZE(normalized), NormalizeVendor(vendor));
    if (IsProbed() && !force && !_wcsicmp(g_probeVendor, normalized))
        return;

    AcquireSRWLockExclusive(&g_lock);
    if (IsProbed() && !force && !_wcsicmp(g_probeVendor, normalized)) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    /* A completed cache for a different vendor is not a valid answer for this
       request. Clear the publication flag before touching the capability data. */
    InterlockedExchange((volatile LONG *)&g_caps.probed, 0);
    StringCchCopyW(g_probeVendor, ARRAYSIZE(g_probeVendor), normalized);

    CacheKey(key, ARRAYSIZE(key), normalized);
    if (!force && LoadCache(key)) {
        ExposeMain10SdrFallback();
        SetProbed();
        Log_Printf(L"encoder: using cached %s probe for %s", normalized, key);
        for (id = ENC_H264; id < ENC_COUNT; ++id) {
            if (g_caps.usable[id]) {
                wchar_t clsidText[40] = L"";
                if (StringFromGUID2(&g_caps.mftClsid[id], clsidText,
                                    (int)ARRAYSIZE(clsidText)) <= 0)
                    clsidText[0] = L'\0';
                Log_Printf(L"encoder: cached profile %s -> vendor=%s candidate=%lu MFT='%s' hardware=%s URL='%s' CLSID=%s",
                    Encoder_Name(id), g_caps.vendor[id][0] ? g_caps.vendor[id] : L"(unknown)",
                    (unsigned long)g_caps.candidate[id],
                    g_caps.mft[id][0] ? g_caps.mft[id] : L"(unnamed)",
                    g_caps.hardware[id] ? L"yes" : L"no",
                    g_caps.hardwareUrl[id][0] ? g_caps.hardwareUrl[id] : L"(none)",
                    clsidText[0] ? clsidText : L"(none)");
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    ZeroMemory(g_caps.usable, sizeof(g_caps.usable));
    ZeroMemory(g_caps.mftUsable, sizeof(g_caps.mftUsable));
    ZeroMemory(g_caps.hardware, sizeof(g_caps.hardware));
    ZeroMemory(g_caps.mft, sizeof(g_caps.mft));
    ZeroMemory(g_caps.vendor, sizeof(g_caps.vendor));
    ZeroMemory(g_caps.candidate, sizeof(g_caps.candidate));
    ZeroMemory(g_caps.hardwareUrl, sizeof(g_caps.hardwareUrl));
    ZeroMemory(g_caps.mftClsid, sizeof(g_caps.mftClsid));

    /* The settings dialog can request a different vendor after the startup
       probe thread has called MFShutdown. Hold our own MF startup reference for
       every uncached sweep, including probes requested synchronously by UI. */
    {
        HRESULT startupHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(startupHr)) {
            Log_Printf(L"encoder: MFStartup failed for %s probe (0x%08lX)",
                       normalized, (unsigned long)startupHr);
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }

    if (!GetTempPathW(MAX_PATH, dir))
        StringCchCopyW(dir, MAX_PATH, L".");

    t0 = GetTickCount64();
    for (id = ENC_H264; id < ENC_COUNT; id++) {
        g_caps.usable[id] = ProbeOne(id, dir, normalized);
        g_caps.mftUsable[id] = g_caps.usable[id];
    }

    /* Some vendor HEVC MFTs reject Rec.709 colour tags on AddStream even
       though their Main10/HDR path works. Expose the SDR variant only for the
       same selected vendor, and let the recording code write Rec.709 tags. */
    ExposeMain10SdrFallback();
    if (lstrcmpiW(normalized, L"auto") != 0) {
        BOOL anyUsable = FALSE;
        for (id = ENC_H264; id < ENC_COUNT; ++id) {
            if (g_caps.usable[id]) {
                anyUsable = TRUE;
                break;
            }
        }
        if (!anyUsable)
            Log_Printf(L"encoder: no usable Media Foundation encoder found for %s; check the GPU driver and hardware MFT registration",
                       normalized);
    }
    MFShutdown();

    /* Cache only profiles which passed the Media Foundation hardware-MFT probe. */
    SaveCache(key);
    SetProbed();
    Log_Printf(L"encoder: %s hardware probe took %lu ms on %s", normalized,
               (unsigned long)(GetTickCount64() - t0), key);
    ReleaseSRWLockExclusive(&g_lock);
}

void Encoder_Probe(BOOL force)
{
    Encoder_ProbeForVendor(L"auto", force);
}

static unsigned __stdcall ProbeThread(void *arg)
{
    UNREFERENCED_PARAMETER(arg);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    Encoder_Probe(FALSE);
    MFShutdown();
    CoUninitialize();
    InterlockedExchange(&g_probing, 0);
    return 0;
}

void Encoder_ProbeAsync(void)
{
    uintptr_t th;

    if (IsProbed())
        return;
    if (InterlockedCompareExchange(&g_probing, 1, 0) != 0)
        return;
    th = _beginthreadex(NULL, 0, ProbeThread, NULL, 0, NULL);
    if (!th)
        InterlockedExchange(&g_probing, 0);
    else
        CloseHandle((HANDLE)th);
}

/* ----------------------------------------------------------- the ranking */

/*
 * Automatic favours the best hardware path that passed the full MP4 probe.
 * On SDR screens it prefers HEVC Main10 SDR, then HEVC 8-bit, then working
 * AV1 profiles, with H.264 as the broad-compatibility fallback. HDR prefers
 * HEVC Main10 PQ and uses AV1 Main10 HDR only when the full probe succeeds.
 */
static const EncoderId g_rankHdr[] = {
    ENC_HEVC10, ENC_AV1_10_HDR, ENC_HEVC10_SDR, ENC_AV1_10,
    ENC_HEVC8, ENC_AV1_8, ENC_H264
};
static const EncoderId g_rankSdr[] = {
    ENC_HEVC10_SDR, ENC_HEVC8, ENC_AV1_10, ENC_AV1_8,
    ENC_H264, ENC_HEVC10, ENC_AV1_10_HDR
};

EncoderId Encoder_Resolve(EncoderId want, BOOL hdr)
{
    const EncoderId *rank = hdr ? g_rankHdr : g_rankSdr;
    const int n = hdr ? (int)ARRAYSIZE(g_rankHdr) : (int)ARRAYSIZE(g_rankSdr);
    int i;

    if (!IsProbed())
        Encoder_Probe(FALSE);

    /* An explicit codec/profile is honoured only when its Media Foundation
       hardware MFT passed the same MP4 probe used for recording. */
    if (want > ENC_AUTO && want < ENC_COUNT &&
        (hdr || !Encoder_IsHdr(want))) {
        if (g_caps.mftUsable[want])
            return want;
        Log_Printf(L"encoder: explicit profile %s has no working hardware MFT",
                   Encoder_Name(want));
    }

    /* Rank only vendor-pinned Media Foundation hardware MFTs. */
    if (!_wcsicmp(g_probeVendor, L"auto")) {
        static const wchar_t *const vendorOrder[] = { L"nvidia", L"amd", L"intel" };
        size_t v;
        for (v = 0; v < ARRAYSIZE(vendorOrder); ++v) {
            for (i = 0; i < n; ++i) {
                EncoderId candidate = rank[i];
                if (g_caps.mftUsable[candidate] &&
                    (hdr || !Encoder_IsHdr(candidate)) &&
                    !_wcsicmp(g_caps.vendor[candidate], vendorOrder[v])) {
                    Log_Printf(L"encoder: Auto resolved %s to %s hardware MFT (rank %d)",
                               Encoder_Name(candidate), vendorOrder[v], i);
                    return candidate;
                }
            }
        }
        /* Some vendor MFTs do not expose a recognised vendor token. Do not
           hide a profile that nevertheless passed the hardware-MFT probe. */
        for (i = 0; i < n; ++i)
            if (g_caps.mftUsable[rank[i]] && (hdr || !Encoder_IsHdr(rank[i])))
                return rank[i];
    } else {
        for (i = 0; i < n; ++i)
            if (g_caps.mftUsable[rank[i]] && (hdr || !Encoder_IsHdr(rank[i])))
                return rank[i];
    }
    return ENC_COUNT;
}

EncoderId Encoder_Fallback(EncoderId id)
{
    /* Keep the resolved physical GPU when another compatible profile from
       that GPU works; only cross to another vendor after its profiles are
       exhausted. There is deliberately no software-encoder fallback. */
    static const EncoderId chain[] = {
        ENC_H264, ENC_HEVC8, ENC_AV1_8, ENC_HEVC10_SDR, ENC_AV1_10,
        ENC_HEVC10, ENC_AV1_10_HDR
    };
    const wchar_t *preferred = (id > ENC_AUTO && id < ENC_COUNT)
        ? g_caps.vendor[id] : L"";
    int i;

    if (preferred && preferred[0]) {
        for (i = 0; i < (int)ARRAYSIZE(chain); i++)
            if (chain[i] != id && g_caps.usable[chain[i]] &&
                !_wcsicmp(g_caps.vendor[chain[i]], preferred))
                return chain[i];
    }
    for (i = 0; i < (int)ARRAYSIZE(chain); i++)
        if (chain[i] != id && g_caps.usable[chain[i]])
            return chain[i];
    return ENC_COUNT;
}
