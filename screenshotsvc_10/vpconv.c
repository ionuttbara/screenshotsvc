/*
 * vpconv.c - BGRA to NV12 through the D3D11 video processor.
 */

#define COBJMACROS

#include "screenshotservice.h"
#include "vpconv.h"
#include "log.h"

#include <d3d11_1.h>
#include <strsafe.h>

#define OUT_VIEWS 64       /* encoder surface pools are smaller than this */

struct VpConv {
    ID3D11VideoDevice               *vdev;
    ID3D11VideoContext              *vctx;
    ID3D11VideoProcessorEnumerator  *en;
    ID3D11VideoProcessor            *vp;
    UINT32                           width, height;
    ID3D11Texture2D                 *inTex;     /* the texture inView is for */
    ID3D11VideoProcessorInputView   *inView;
    ID3D11Texture2D                 *outTex[OUT_VIEWS];
    ID3D11VideoProcessorOutputView  *outView[OUT_VIEWS];
    int                              outCount, outNext;
};

static void Why(wchar_t *why, size_t cch, const wchar_t *what, HRESULT hr)
{
    if (why && cch)
        StringCchPrintfW(why, cch, L"%s (0x%08lX)", what, (unsigned long)hr);
}

VpConv *VpConv_Create(ID3D11Device *dev, UINT32 width, UINT32 height, wchar_t *why, size_t cch)
{
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11VideoContext1 *vctx1 = NULL;
    VpConv              *c;
    UINT                 flags = 0;
    HRESULT              hr;

    if (why && cch) why[0] = 0;
    c = (VpConv *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c)
        return NULL;
    c->width = width;
    c->height = height;
    hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11VideoDevice, (void **)&c->vdev);
    if (FAILED(hr)) { Why(why, cch, L"no video device", hr); goto fail; }
    ID3D11Device_GetImmediateContext(dev, &ctx);
    hr = ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11VideoContext, (void **)&c->vctx);
    ID3D11DeviceContext_Release(ctx);
    if (FAILED(hr)) { Why(why, cch, L"no video context", hr); goto fail; }

    ZeroMemory(&cd, sizeof(cd));
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = width;   cd.InputHeight = height;
    cd.OutputWidth = width;  cd.OutputHeight = height;
    cd.InputFrameRate.Numerator = 30;  cd.InputFrameRate.Denominator = 1;
    cd.OutputFrameRate.Numerator = 30; cd.OutputFrameRate.Denominator = 1;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(c->vdev, &cd, &c->en);
    if (FAILED(hr)) { Why(why, cch, L"no video processor for this size", hr); goto fail; }
    if (FAILED(ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(c->en,
               DXGI_FORMAT_B8G8R8A8_UNORM, &flags)) ||
        !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        Why(why, cch, L"the video processor does not take BGRA", E_FAIL);
        goto fail;
    }
    flags = 0;
    if (FAILED(ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(c->en,
               DXGI_FORMAT_NV12, &flags)) ||
        !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        Why(why, cch, L"the video processor does not write NV12", E_FAIL);
        goto fail;
    }
    hr = ID3D11VideoDevice_CreateVideoProcessor(c->vdev, c->en, 0, &c->vp);
    if (FAILED(hr)) { Why(why, cch, L"no video processor", hr); goto fail; }

    /*
     * Desktop pixels are full-range sRGB with BT.709 primaries; the stream
     * is BT.709 limited range. The DXGI colour spaces say exactly that where
     * the driver knows them (Windows 10's ID3D11VideoContext1); the older
     * description says the same in its coarser terms.
     */
    if (SUCCEEDED(ID3D11VideoContext_QueryInterface(c->vctx, &IID_ID3D11VideoContext1,
                                                    (void **)&vctx1))) {
        ID3D11VideoContext1_VideoProcessorSetStreamColorSpace1(vctx1, c->vp, 0,
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        ID3D11VideoContext1_VideoProcessorSetOutputColorSpace1(vctx1, c->vp,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        ID3D11VideoContext1_Release(vctx1);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in, out;
        ZeroMemory(&in, sizeof(in));
        ZeroMemory(&out, sizeof(out));
        in.RGB_Range = 0;                     /* 0-255 */
        out.YCbCr_Matrix = 1;                 /* BT.709 */
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        ID3D11VideoContext_VideoProcessorSetStreamColorSpace(c->vctx, c->vp, 0, &in);
        ID3D11VideoContext_VideoProcessorSetOutputColorSpace(c->vctx, c->vp, &out);
    }
    /* No denoising, no edge enhancement, no "improvements" of the driver's:
       a screen recording is to look like the screen. */
    ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(c->vctx, c->vp, 0, FALSE);
    ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(c->vctx, c->vp, 0,
        D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    return c;

fail:
    Log_Printf(L"vpconv: %ux%u BGRA to NV12 not available - %s", width, height,
               why && why[0] ? why : L"?");
    VpConv_Destroy(c);
    return NULL;
}

static ID3D11VideoProcessorOutputView *OutView(VpConv *c, ID3D11Texture2D *tex)
{
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od;
    int i;

    for (i = 0; i < c->outCount; ++i)
        if (c->outTex[i] == tex)
            return c->outView[i];
    /* A new target: take the next slot, round robin once all are used. */
    i = c->outCount < OUT_VIEWS ? c->outCount++ : (c->outNext++ % OUT_VIEWS);
    if (c->outView[i]) {
        ID3D11VideoProcessorOutputView_Release(c->outView[i]);
        ID3D11Texture2D_Release(c->outTex[i]);
        c->outView[i] = NULL;
        c->outTex[i] = NULL;
    }
    ZeroMemory(&od, sizeof(od));
    od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11VideoDevice_CreateVideoProcessorOutputView(c->vdev, (ID3D11Resource *)tex,
               c->en, &od, &c->outView[i])))
        return NULL;
    c->outTex[i] = tex;
    ID3D11Texture2D_AddRef(tex);
    return c->outView[i];
}

BOOL VpConv_Run(VpConv *c, ID3D11Texture2D *bgra, ID3D11Texture2D *nv12)
{
    D3D11_VIDEO_PROCESSOR_STREAM stream;
    ID3D11VideoProcessorOutputView *out;
    RECT    r;
    HRESULT hr;

    if (!c || !bgra || !nv12)
        return FALSE;
    if (c->inTex != bgra) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id;
        if (c->inView) {
            ID3D11VideoProcessorInputView_Release(c->inView);
            ID3D11Texture2D_Release(c->inTex);
            c->inView = NULL;
            c->inTex = NULL;
        }
        ZeroMemory(&id, sizeof(id));
        id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(ID3D11VideoDevice_CreateVideoProcessorInputView(c->vdev, (ID3D11Resource *)bgra,
                   c->en, &id, &c->inView)))
            return FALSE;
        c->inTex = bgra;
        ID3D11Texture2D_AddRef(bgra);
    }
    out = OutView(c, nv12);
    if (!out)
        return FALSE;
    SetRect(&r, 0, 0, (int)c->width, (int)c->height);
    ID3D11VideoContext_VideoProcessorSetStreamSourceRect(c->vctx, c->vp, 0, TRUE, &r);
    ID3D11VideoContext_VideoProcessorSetStreamDestRect(c->vctx, c->vp, 0, TRUE, &r);
    ID3D11VideoContext_VideoProcessorSetOutputTargetRect(c->vctx, c->vp, TRUE, &r);
    ZeroMemory(&stream, sizeof(stream));
    stream.Enable = TRUE;
    stream.pInputSurface = c->inView;
    hr = ID3D11VideoContext_VideoProcessorBlt(c->vctx, c->vp, out, 0, 1, &stream);
    return SUCCEEDED(hr);
}

void VpConv_Destroy(VpConv *c)
{
    int i;
    if (!c)
        return;
    for (i = 0; i < OUT_VIEWS; ++i) {
        if (c->outView[i]) ID3D11VideoProcessorOutputView_Release(c->outView[i]);
        if (c->outTex[i])  ID3D11Texture2D_Release(c->outTex[i]);
    }
    if (c->inView) ID3D11VideoProcessorInputView_Release(c->inView);
    if (c->inTex)  ID3D11Texture2D_Release(c->inTex);
    if (c->vp)     ID3D11VideoProcessor_Release(c->vp);
    if (c->en)     ID3D11VideoProcessorEnumerator_Release(c->en);
    if (c->vctx)   ID3D11VideoContext_Release(c->vctx);
    if (c->vdev)   ID3D11VideoDevice_Release(c->vdev);
    HeapFree(GetProcessHeap(), 0, c);
}
