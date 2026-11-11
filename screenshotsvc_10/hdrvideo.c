/*
 * hdrvideo.c - the scRGB -> BT.2020 PQ -> P010 conversion pipeline.
 */

#define COBJMACROS

#include "screenshotservice.h"
#include "hdrvideo.h"
#include "log.h"

#include <d3dcompiler.h>
#include <math.h>

struct HdrVideo {
    ID3D11Device        *dev;
    ID3D11DeviceContext *ctx;
    UINT32               width, height;

    ID3D11Texture2D          *src;      /* fixed-size input canvas */
    ID3D11ShaderResourceView *srv;
    ID3D11RenderTargetView   *srcRtv;   /* used only for resolution-change scaling */
    ID3D11VertexShader       *scaleVs;
    ID3D11PixelShader        *scalePs;
    ID3D11SamplerState       *scaleSampler;
    ID3D11Buffer             *scaleCb;
    ID3D11Texture2D          *scaleTex; /* temporary SRV copy of current Duplication frame */
    ID3D11ShaderResourceView *scaleSrv;
    UINT32                    scaleSourceWidth, scaleSourceHeight;
    DXGI_FORMAT               scaleSourceFormat;

    ID3D11Texture2D        *texY,  *texUV;    /* render targets  */
    ID3D11RenderTargetView *rtvY,  *rtvUV;
    ID3D11Texture2D        *stgY,  *stgUV;    /* readback        */

    ID3D11VertexShader *vs;
    ID3D11PixelShader  *psY, *psUV, *psSdr;
    ID3D11PixelShader  *psYSdr, *psUVSdr;
    BOOL                sdrInput;  /* source is gamma-coded SDR RGB, output Rec.709 */

    /* The SDR path, built only when it is asked for. */
    ID3D11Texture2D        *texSdr, *stgSdr;
    ID3D11RenderTargetView *rtvSdr;
    ID3D11Buffer           *cbSdr;
    float                   sdrScale;
    BOOL                    rollOff;

    UINT16              lastPeak;     /* luma codes, from the last frame */
    double              lastMean;

    /*
     * Light statistics on the GPU, for the zero-copy path where the luma
     * plane never reaches the CPU. A compute shader reduces each 16x16 tile
     * to its brightest code and its sum; the tile results are copied into a
     * small ring of readback buffers and read a frame or two later, when the
     * GPU is done with them, so measuring never stalls the recording.
     */
    ID3D11ComputeShader       *csStats;
    ID3D11Buffer              *statsBuf;     /* one uint2 per tile        */
    ID3D11UnorderedAccessView *statsUav;
    ID3D11Buffer              *statsCb;      /* width, height, tiles across */
    ID3D11Buffer              *statsStage[3];
    BOOL                       statsPending[3];
    int                        statsNext;
    UINT32                     tilesX, tilesY;
    UINT32                    *tileScratch;  /* one readback, for summing */
};

/* Separate from the pixel shaders: it binds its own t0. */
static const char g_hlslScale[] =
"Texture2D<float4> ScaleSource : register(t0);\n"
"SamplerState ScaleSampler : register(s0);\n"
"cbuffer ScaleCrop : register(b0) { float4 Crop; };\n"
"struct ScaleVSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"ScaleVSOut VSScale(uint id : SV_VertexID)\n"
"{\n"
"    ScaleVSOut o;\n"
"    float2 t = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
"    o.uv = t;\n"
"    return o;\n"
"}\n"
"float4 PSScale(ScaleVSOut i) : SV_Target\n"
"{\n"
"    float2 uv = lerp(Crop.xy, Crop.zw, i.uv);\n"
"    return ScaleSource.SampleLevel(ScaleSampler, uv, 0);\n"
"}\n";

static const char g_hlslStats[] =
"Texture2D<float> Luma : register(t0);\n"
"RWStructuredBuffer<uint2> Tiles : register(u0);\n"
"cbuffer Dims : register(b0) { uint gW; uint gH; uint gTilesX; uint gPad; };\n"
"groupshared uint sMax[256];\n"
"groupshared uint sSum[256];\n"
"[numthreads(16, 16, 1)]\n"
"void CSStats(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID,\n"
"             uint gi : SV_GroupIndex)\n"
"{\n"
"    uint2 p = gid.xy * 16 + tid.xy;\n"
"    uint  v = 0;\n"
"    if (p.x < gW && p.y < gH)\n"
"        v = (uint)(Luma.Load(int3(p, 0)) * 65535.0 + 0.5);\n"
"    sMax[gi] = v;\n"
"    sSum[gi] = v;\n"
"    GroupMemoryBarrierWithGroupSync();\n"
"    [unroll] for (uint s = 128; s > 0; s >>= 1) {\n"
"        if (gi < s) {\n"
"            sMax[gi] = max(sMax[gi], sMax[gi + s]);\n"
"            sSum[gi] += sSum[gi + s];\n"
"        }\n"
"        GroupMemoryBarrierWithGroupSync();\n"
"    }\n"
"    /* 256 codes of at most 65535 sum to under 2^24: no overflow. */\n"
"    if (gi == 0)\n"
"        Tiles[gid.y * gTilesX + gid.x] = uint2(sMax[0], sSum[0]);\n"
"}\n";

/*
 * Two passes over the same maths. Chroma is averaged after the transfer
 * function rather than before it, which is what 4:2:0 subsampling is defined
 * to do; averaging linear light instead shifts saturated edges.
 */
static const char g_hlsl[] =
"Texture2D<float4> Src : register(t0);\n"
"\n"
"struct VSOut { float4 pos : SV_Position; };\n"
"\n"
"VSOut VSMain(uint id : SV_VertexID)\n"
"{\n"
"    VSOut o;\n"
"    float2 t = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
"    return o;\n"
"}\n"
"\n"
"static const float3x3 kRgb709To2020 = {\n"
"    0.627403914, 0.329283028, 0.043313058,\n"
"    0.069097289, 0.919540395, 0.011362316,\n"
"    0.016391439, 0.088013308, 0.895595253\n"
"};\n"
"\n"
"float3 PQ(float3 L)\n"
"{\n"
"    const float m1 = 0.1593017578125;\n"
"    const float m2 = 78.84375;\n"
"    const float c1 = 0.8359375;\n"
"    const float c2 = 18.8515625;\n"
"    const float c3 = 18.6875;\n"
"    float3 Lm = pow(max(L, 0.0), m1);\n"
"    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);\n"
"}\n"
"\n"
"float3 ToYuv(int2 p)\n"
"{\n"
"    float3 rgb = Src.Load(int3(p, 0)).rgb;\n"
"    /* scRGB is linear Rec.709 with 1.0 meaning 80 nits. The matrix runs on\n"
"       signed values on purpose: a negative scRGB component is a colour\n"
"       outside Rec.709 that BT.2020 can hold, and clamping first loses it. */\n"
"    float3 bt2020 = max(mul(kRgb709To2020, rgb * 80.0), 0.0);\n"
"    float3 pq = PQ(min(bt2020, 10000.0) / 10000.0);\n"
"\n"
"    const float Kr = 0.2627;\n"
"    const float Kb = 0.0593;\n"
"    float Y  = Kr * pq.r + (1.0 - Kr - Kb) * pq.g + Kb * pq.b;\n"
"    float Cb = (pq.b - Y) / 1.8814;\n"
"    float Cr = (pq.r - Y) / 1.4746;\n"
"    return float3(Y, Cb, Cr);\n"
"}\n"
"\n"
"/* 10-bit limited range, then shifted into the top of a 16-bit word, which is\n"
"   how P010 stores it. 1023 * 64 = 65472, hence the 65535 divisor. */\n"
"float Code(float v) { return clamp(v, 0.0, 1023.0) * 64.0 / 65535.0; }\n"
"\n"
"float PSLuma(VSOut i) : SV_Target\n"
"{\n"
"    float3 yuv = ToYuv(int2(i.pos.xy));\n"
"    return Code(64.0 + yuv.x * 876.0);\n"
"}\n"
"\n"
"cbuffer Sdr : register(b0)\n"
"{\n"
"    float gSdrScale;   /* 80 / SDR white, so white lands on 1.0 */\n"
"    float gRollOff;    /* non-zero to keep highlight detail     */\n"
"    float2 gPad;\n"
"};\n"
"\n"
"/* The same shoulder as hdr.c: identity to the knee, then an exponential that\n"
"   meets it with matching slope so there is no crease. */\n"
"float RollOff(float x)\n"
"{\n"
"    const float knee = 0.75;\n"
"    const float span = 1.0 - knee;\n"
"    const float s    = (4.0 - knee) / 3.0;\n"
"    return x <= knee ? x : knee + span * (1.0 - exp(-(x - knee) / s));\n"
"}\n"
"\n"
"float LinearToSrgb(float l)\n"
"{\n"
"    return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;\n"
"}\n"
"\n"
"float4 PSSdr(VSOut i) : SV_Target\n"
"{\n"
"    float3 v = Src.Load(int3(int2(i.pos.xy), 0)).rgb * gSdrScale;\n"
"    int c;\n"
"    v = max(v, 0.0);\n"
"    for (c = 0; c < 3; c++)\n"
"        v[c] = gRollOff != 0.0 ? RollOff(v[c]) : min(v[c], 1.0);\n"
"    for (c = 0; c < 3; c++)\n"
"        v[c] = saturate(LinearToSrgb(v[c]));\n"
"    return float4(v, 1.0);\n"
"}\n"
"\n"
"float2 PSChroma(VSOut i) : SV_Target\n"
"{\n"
"    int2 p = int2(i.pos.xy) * 2;\n"
"    float3 a = ToYuv(p);\n"
"    float3 b = ToYuv(p + int2(1, 0));\n"
"    float3 c = ToYuv(p + int2(0, 1));\n"
"    float3 d = ToYuv(p + int2(1, 1));\n"
"    float2 cbcr = (a.yz + b.yz + c.yz + d.yz) * 0.25;\n"
"    return float2(Code(512.0 + cbcr.x * 896.0),\n"
"                  Code(512.0 + cbcr.y * 896.0));\n"
"}\n"
"\n"
"/* SDR Main10: the source is already display-referred gamma-coded RGB. Keep\n"
"   Rec.709 primaries/transfer; do not apply the HDR matrix or PQ curve. */\n"
"float3 ToYuv709(int2 p)\n"
"{\n"
"    float3 rgb = saturate(Src.Load(int3(p, 0)).rgb);\n"
"    float Y = 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;\n"
"    float Cb = (rgb.b - Y) / 1.8556;\n"
"    float Cr = (rgb.r - Y) / 1.5748;\n"
"    return float3(Y, Cb, Cr);\n"
"}\n"
"float PSLuma709(VSOut i) : SV_Target\n"
"{\n"
"    float3 yuv = ToYuv709(int2(i.pos.xy));\n"
"    return Code(64.0 + yuv.x * 876.0);\n"
"}\n"
"float2 PSChroma709(VSOut i) : SV_Target\n"
"{\n"
"    int2 p = int2(i.pos.xy) * 2;\n"
"    float3 a = ToYuv709(p);\n"
"    float3 b = ToYuv709(p + int2(1, 0));\n"
"    float3 c = ToYuv709(p + int2(0, 1));\n"
"    float3 d = ToYuv709(p + int2(1, 1));\n"
"    float2 cbcr = (a.yz + b.yz + c.yz + d.yz) * 0.25;\n"
"    return float2(Code(512.0 + cbcr.x * 896.0), Code(512.0 + cbcr.y * 896.0));\n"
"}\n";

static BOOL CompileShaders(HdrVideo *hv)
{
    HMODULE     lib;
    pD3DCompile compile;
    ID3DBlob   *vsb = NULL, *psa = NULL, *psb = NULL, *psc = NULL,
               *psd = NULL, *pse = NULL, *svb = NULL, *spb = NULL, *err = NULL;
    BOOL        ok = FALSE;
    const UINT  flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;

    /* From System32 only: a plain LoadLibrary would look in the exe's own
       folder first and load whatever d3dcompiler_47.dll was planted there. */
    lib = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib)
        return FALSE;
    compile = (pD3DCompile)(void *)GetProcAddress(lib, "D3DCompile");
    if (!compile) {
        FreeLibrary(lib);
        return FALSE;
    }

    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "VSMain", "vs_4_0", flags, 0, &vsb, &err)))   goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSLuma", "ps_4_0", flags, 0, &psa, &err)))   goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSChroma", "ps_4_0", flags, 0, &psb, &err))) goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSSdr", "ps_4_0", flags, 0, &psc, &err)))    goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSLuma709", "ps_4_0", flags, 0, &psd, &err))) goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSChroma709", "ps_4_0", flags, 0, &pse, &err))) goto done;
    if (FAILED(compile(g_hlslScale, sizeof(g_hlslScale) - 1, NULL, NULL, NULL,
                       "VSScale", "vs_4_0", flags, 0, &svb, &err))) goto done;
    if (FAILED(compile(g_hlslScale, sizeof(g_hlslScale) - 1, NULL, NULL, NULL,
                       "PSScale", "ps_4_0", flags, 0, &spb, &err))) goto done;

    if (FAILED(ID3D11Device_CreateVertexShader(hv->dev,
            ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
            NULL, &hv->vs)))  goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(hv->dev,
            ID3D10Blob_GetBufferPointer(psa), ID3D10Blob_GetBufferSize(psa),
            NULL, &hv->psY))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(hv->dev,
            ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
            NULL, &hv->psUV))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(hv->dev,
            ID3D10Blob_GetBufferPointer(psc), ID3D10Blob_GetBufferSize(psc),
            NULL, &hv->psSdr))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(hv->dev,
            ID3D10Blob_GetBufferPointer(psd), ID3D10Blob_GetBufferSize(psd),
            NULL, &hv->psYSdr))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(hv->dev,
            ID3D10Blob_GetBufferPointer(pse), ID3D10Blob_GetBufferSize(pse),
            NULL, &hv->psUVSdr))) goto done;
    if (FAILED(ID3D11Device_CreateVertexShader(hv->dev,
            ID3D10Blob_GetBufferPointer(svb), ID3D10Blob_GetBufferSize(svb),
            NULL, &hv->scaleVs))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(hv->dev,
            ID3D10Blob_GetBufferPointer(spb), ID3D10Blob_GetBufferSize(spb),
            NULL, &hv->scalePs))) goto done;
    ok = TRUE;

done:
    if (err) {
        Log_Printf(L"hdrvideo: shader compile said %hs",
                   (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    if (vsb) ID3D10Blob_Release(vsb);
    if (psa) ID3D10Blob_Release(psa);
    if (psb) ID3D10Blob_Release(psb);
    if (psc) ID3D10Blob_Release(psc);
    if (psd) ID3D10Blob_Release(psd);
    if (pse) ID3D10Blob_Release(pse);
    if (svb) ID3D10Blob_Release(svb);
    if (spb) ID3D10Blob_Release(spb);
    /* Only after the blobs: their code lives in the compiler DLL. Every
       recording used to take another reference to it and never give it back. */
    FreeLibrary(lib);
    return ok;
}

static BOOL MakeTex(HdrVideo *hv, UINT32 w, UINT32 h, DXGI_FORMAT fmt,
                    UINT bind, D3D11_USAGE usage, UINT cpu,
                    ID3D11Texture2D **out)
{
    D3D11_TEXTURE2D_DESC d;

    ZeroMemory(&d, sizeof(d));
    d.Width            = w;
    d.Height           = h;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = fmt;
    d.SampleDesc.Count = 1;
    d.Usage            = usage;
    d.BindFlags        = bind;
    d.CPUAccessFlags   = cpu;

    return SUCCEEDED(ID3D11Device_CreateTexture2D(hv->dev, &d, NULL, out));
}

static HdrVideo *HdrVideo_CreateInternal(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                         UINT32 width, UINT32 height,
                                         DXGI_FORMAT sourceFormat, BOOL sdrInput)
{
    HdrVideo *hv;

    if (!dev || !ctx || (width & 1) || (height & 1) || width < 2 || height < 2)
        return NULL;

    hv = (HdrVideo *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*hv));
    if (!hv)
        return NULL;
    hv->dev    = dev;
    hv->ctx    = ctx;
    hv->width  = width;
    hv->height = height;
    hv->sdrInput = sdrInput;

    if (!MakeTex(hv, width, height, sourceFormat,
                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                 D3D11_USAGE_DEFAULT, 0, &hv->src))
        goto fail;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev,
            (ID3D11Resource *)hv->src, NULL, &hv->srv)))
        goto fail;

    if (!MakeTex(hv, width, height, DXGI_FORMAT_R16_UNORM,
                 D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, &hv->texY))
        goto fail;
    if (!MakeTex(hv, width / 2, height / 2, DXGI_FORMAT_R16G16_UNORM,
                 D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, &hv->texUV))
        goto fail;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev,
            (ID3D11Resource *)hv->texY, NULL, &hv->rtvY)))
        goto fail;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev,
            (ID3D11Resource *)hv->texUV, NULL, &hv->rtvUV)))
        goto fail;

    if (!MakeTex(hv, width, height, DXGI_FORMAT_R16_UNORM, 0,
                 D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &hv->stgY))
        goto fail;
    if (!MakeTex(hv, width / 2, height / 2, DXGI_FORMAT_R16G16_UNORM, 0,
                 D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &hv->stgUV))
        goto fail;

    if (!CompileShaders(hv))
        goto fail;

    return hv;

fail:
    HdrVideo_Destroy(hv);
    return NULL;
}

HdrVideo *HdrVideo_Create(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                          UINT32 width, UINT32 height)
{
    return HdrVideo_CreateInternal(dev, ctx, width, height,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT, FALSE);
}

HdrVideo *HdrVideo_CreateSdr(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                             UINT32 width, UINT32 height, DXGI_FORMAT sourceFormat)
{
    if (sourceFormat != DXGI_FORMAT_B8G8R8A8_UNORM &&
        sourceFormat != DXGI_FORMAT_R10G10B10A2_UNORM)
        return NULL;
    return HdrVideo_CreateInternal(dev, ctx, width, height, sourceFormat, TRUE);
}

void HdrVideo_Destroy(HdrVideo *hv)
{
    if (!hv)
        return;
    {
        int i;
        for (i = 0; i < 3; i++)
            if (hv->statsStage[i]) ID3D11Buffer_Release(hv->statsStage[i]);
    }
    if (hv->tileScratch) HeapFree(GetProcessHeap(), 0, hv->tileScratch);
    if (hv->scaleSrv) ID3D11ShaderResourceView_Release(hv->scaleSrv);
    if (hv->scaleTex) ID3D11Texture2D_Release(hv->scaleTex);
    if (hv->scaleCb) ID3D11Buffer_Release(hv->scaleCb);
    if (hv->scaleSampler) ID3D11SamplerState_Release(hv->scaleSampler);
    if (hv->scalePs) ID3D11PixelShader_Release(hv->scalePs);
    if (hv->scaleVs) ID3D11VertexShader_Release(hv->scaleVs);
    if (hv->srcRtv) ID3D11RenderTargetView_Release(hv->srcRtv);
    if (hv->statsCb)  ID3D11Buffer_Release(hv->statsCb);
    if (hv->statsUav) ID3D11UnorderedAccessView_Release(hv->statsUav);
    if (hv->statsBuf) ID3D11Buffer_Release(hv->statsBuf);
    if (hv->csStats)  ID3D11ComputeShader_Release(hv->csStats);
    if (hv->cbSdr)  ID3D11Buffer_Release(hv->cbSdr);
    if (hv->stgSdr) ID3D11Texture2D_Release(hv->stgSdr);
    if (hv->rtvSdr) ID3D11RenderTargetView_Release(hv->rtvSdr);
    if (hv->texSdr) ID3D11Texture2D_Release(hv->texSdr);
    if (hv->psSdr) ID3D11PixelShader_Release(hv->psSdr);
    if (hv->psYSdr) ID3D11PixelShader_Release(hv->psYSdr);
    if (hv->psUVSdr) ID3D11PixelShader_Release(hv->psUVSdr);
    if (hv->psUV) ID3D11PixelShader_Release(hv->psUV);
    if (hv->psY)  ID3D11PixelShader_Release(hv->psY);
    if (hv->vs)   ID3D11VertexShader_Release(hv->vs);
    if (hv->stgUV) ID3D11Texture2D_Release(hv->stgUV);
    if (hv->stgY)  ID3D11Texture2D_Release(hv->stgY);
    if (hv->rtvUV) ID3D11RenderTargetView_Release(hv->rtvUV);
    if (hv->rtvY)  ID3D11RenderTargetView_Release(hv->rtvY);
    if (hv->texUV) ID3D11Texture2D_Release(hv->texUV);
    if (hv->texY)  ID3D11Texture2D_Release(hv->texY);
    if (hv->srv)   ID3D11ShaderResourceView_Release(hv->srv);
    if (hv->src)   ID3D11Texture2D_Release(hv->src);
    HeapFree(GetProcessHeap(), 0, hv);
}

ID3D11Texture2D *HdrVideo_Input(HdrVideo *hv)
{
    return hv ? hv->src : NULL;
}

static BOOL HdrVideo_EnsureScaleResources(HdrVideo *hv, UINT32 sourceWidth,
                                           UINT32 sourceHeight, DXGI_FORMAT sourceFormat)
{
    D3D11_SAMPLER_DESC sd;
    D3D11_BUFFER_DESC bd;

    if (!hv || !sourceWidth || !sourceHeight)
        return FALSE;

    if (!hv->srcRtv && FAILED(ID3D11Device_CreateRenderTargetView(
            hv->dev, (ID3D11Resource *)hv->src, NULL, &hv->srcRtv)))
        return FALSE;

    if (!hv->scaleTex || hv->scaleSourceWidth != sourceWidth ||
        hv->scaleSourceHeight != sourceHeight || hv->scaleSourceFormat != sourceFormat) {
        if (hv->scaleSrv) { ID3D11ShaderResourceView_Release(hv->scaleSrv); hv->scaleSrv = NULL; }
        if (hv->scaleTex) { ID3D11Texture2D_Release(hv->scaleTex); hv->scaleTex = NULL; }
        if (!MakeTex(hv, sourceWidth, sourceHeight, sourceFormat,
                     D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0,
                     &hv->scaleTex))
            return FALSE;
        if (FAILED(ID3D11Device_CreateShaderResourceView(hv->dev,
                (ID3D11Resource *)hv->scaleTex, NULL, &hv->scaleSrv))) {
            ID3D11Texture2D_Release(hv->scaleTex);
            hv->scaleTex = NULL;
            return FALSE;
        }
        hv->scaleSourceWidth = sourceWidth;
        hv->scaleSourceHeight = sourceHeight;
        hv->scaleSourceFormat = sourceFormat;
    }

    if (!hv->scaleSampler) {
        ZeroMemory(&sd, sizeof(sd));
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(ID3D11Device_CreateSamplerState(hv->dev, &sd, &hv->scaleSampler)))
            return FALSE;
    }
    if (!hv->scaleCb) {
        ZeroMemory(&bd, sizeof(bd));
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(hv->dev, &bd, NULL, &hv->scaleCb)))
            return FALSE;
    }
    return hv->scaleVs && hv->scalePs;
}

BOOL HdrVideo_ScaleInput(HdrVideo *hv, ID3D11Texture2D *source,
                         const RECT *sourceRect)
{
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT vp;
    ID3D11RenderTargetView *rtv[1];
    ID3D11ShaderResourceView *srv[1];
    ID3D11SamplerState *sampler[1];
    ID3D11Buffer *cb[1];
    RECT crop;
    float *f;

    if (!hv || !source || !sourceRect)
        return FALSE;
    ID3D11Texture2D_GetDesc(source, &d);
    crop = *sourceRect;
    if (crop.left < 0) crop.left = 0;
    if (crop.top < 0) crop.top = 0;
    if (crop.right > (LONG)d.Width) crop.right = (LONG)d.Width;
    if (crop.bottom > (LONG)d.Height) crop.bottom = (LONG)d.Height;
    if (crop.right <= crop.left || crop.bottom <= crop.top)
        return FALSE;
    if (!HdrVideo_EnsureScaleResources(hv, d.Width, d.Height, d.Format))
        return FALSE;

    /* Avoid read/write hazards from a previous conversion pass. */
    srv[0] = NULL;
    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, srv);
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(hv->ctx,
        (ID3D11Resource *)hv->scaleTex, (ID3D11Resource *)source);

    if (FAILED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->scaleCb,
            0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return FALSE;
    f = (float *)map.pData;
    /* Sample between the centres of the first and last pixels in the crop.
       This prevents linear filtering from bleeding adjacent desktop pixels
       into a selected-area recording after a resolution change. */
    f[0] = ((float)crop.left + 0.5f) / (float)d.Width;
    f[1] = ((float)crop.top + 0.5f) / (float)d.Height;
    f[2] = ((float)crop.right - 0.5f) / (float)d.Width;
    f[3] = ((float)crop.bottom - 0.5f) / (float)d.Height;
    ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->scaleCb, 0);

    ZeroMemory(&vp, sizeof(vp));
    vp.Width = (FLOAT)hv->width;
    vp.Height = (FLOAT)hv->height;
    vp.MaxDepth = 1.0f;
    rtv[0] = hv->srcRtv;
    srv[0] = hv->scaleSrv;
    sampler[0] = hv->scaleSampler;
    cb[0] = hv->scaleCb;
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 1, rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(hv->ctx, 1, &vp);
    ID3D11DeviceContext_VSSetShader(hv->ctx, hv->scaleVs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(hv->ctx, hv->scalePs, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, srv);
    ID3D11DeviceContext_PSSetSamplers(hv->ctx, 0, 1, sampler);
    ID3D11DeviceContext_PSSetConstantBuffers(hv->ctx, 0, 1, cb);
    ID3D11DeviceContext_IASetPrimitiveTopology(hv->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(hv->ctx, NULL);
    ID3D11DeviceContext_Draw(hv->ctx, 3, 0);
    srv[0] = NULL;
    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, srv);
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 0, NULL, NULL);
    return TRUE;
}

/* Renders one full-screen pass; defined below, used by both paths. */
static void Pass(HdrVideo *hv, ID3D11RenderTargetView *rtv,
                 ID3D11PixelShader *ps, UINT32 w, UINT32 h);

void HdrVideo_SetSdrWhite(HdrVideo *hv, float sdrWhiteNits, BOOL rollOff)
{
    if (!hv)
        return;
    if (sdrWhiteNits < 1.0f)
        sdrWhiteNits = 80.0f;
    /* scRGB 1.0 is 80 nits by definition, so this is what puts the level the
       display currently calls white back at 1.0, where sRGB expects it. */
    hv->sdrScale = 80.0f / sdrWhiteNits;
    hv->rollOff  = rollOff;
}

/* Built on first use: an HDR recording never needs any of it. */
static BOOL EnsureSdr(HdrVideo *hv)
{
    D3D11_BUFFER_DESC bd;

    if (hv->texSdr)
        return TRUE;

    if (!MakeTex(hv, hv->width, hv->height, DXGI_FORMAT_B8G8R8A8_UNORM,
                 D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, &hv->texSdr))
        return FALSE;
    if (FAILED(ID3D11Device_CreateRenderTargetView(hv->dev,
            (ID3D11Resource *)hv->texSdr, NULL, &hv->rtvSdr)))
        return FALSE;
    if (!MakeTex(hv, hv->width, hv->height, DXGI_FORMAT_B8G8R8A8_UNORM, 0,
                 D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &hv->stgSdr))
        return FALSE;

    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth      = 16;          /* one float4, the smallest cbuffer */
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(ID3D11Device_CreateBuffer(hv->dev, &bd, NULL, &hv->cbSdr));
}

BOOL HdrVideo_ConvertToBgra(HdrVideo *hv, BYTE *dst, UINT32 stride)
{
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11ShaderResourceView *none[1] = { NULL };
    UINT32 y;

    if (!hv || !dst || !EnsureSdr(hv))
        return FALSE;
    if (hv->sdrScale <= 0.0f)
        hv->sdrScale = 1.0f;

    if (SUCCEEDED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->cbSdr, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        float *p = (float *)map.pData;
        p[0] = hv->sdrScale;
        p[1] = hv->rollOff ? 1.0f : 0.0f;
        p[2] = p[3] = 0.0f;
        ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->cbSdr, 0);
    }
    ID3D11DeviceContext_PSSetConstantBuffers(hv->ctx, 0, 1, &hv->cbSdr);

    Pass(hv, hv->rtvSdr, hv->psSdr, hv->width, hv->height);

    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, none);
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(hv->ctx, (ID3D11Resource *)hv->stgSdr,
                                     (ID3D11Resource *)hv->texSdr);

    if (FAILED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->stgSdr, 0,
                                       D3D11_MAP_READ, 0, &map)))
        return FALSE;
    for (y = 0; y < hv->height; y++)
        memcpy(dst + (SIZE_T)y * stride,
               (const BYTE *)map.pData + (SIZE_T)y * map.RowPitch,
               (SIZE_T)hv->width * 4);
    ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->stgSdr, 0);
    return TRUE;
}

/* The inverse of the shader's PQ, on a 10-bit limited-range luma code. */
double HdrVideo_CodeToNits(double code)
{
    const double m1 = 0.1593017578125, m2 = 78.84375;
    const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    double e = (code - 64.0) / 876.0;
    double p, num, den;

    if (e <= 0.0)
        return 0.0;
    if (e > 1.0)
        e = 1.0;
    p   = pow(e, 1.0 / m2);
    num = p - c1;
    den = c2 - c3 * p;
    if (num <= 0.0 || den <= 0.0)
        return 0.0;
    return 10000.0 * pow(num / den, 1.0 / m1);
}

void HdrVideo_LastFrameLight(const HdrVideo *hv, double *peakNits,
                             double *averageNits)
{
    /* The codes live in the top ten bits of a sixteen-bit word. */
    if (peakNits)
        *peakNits = hv ? HdrVideo_CodeToNits(hv->lastPeak / 64.0) : 0.0;
    if (averageNits)
        *averageNits = hv ? HdrVideo_CodeToNits(hv->lastMean / 64.0) : 0.0;
}

static void Pass(HdrVideo *hv, ID3D11RenderTargetView *rtv,
                 ID3D11PixelShader *ps, UINT32 w, UINT32 h)
{
    D3D11_VIEWPORT vp;
    ID3D11RenderTargetView *rtvs[1];

    ZeroMemory(&vp, sizeof(vp));
    vp.Width    = (FLOAT)w;
    vp.Height   = (FLOAT)h;
    vp.MaxDepth = 1.0f;

    rtvs[0] = rtv;
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 1, rtvs, NULL);
    ID3D11DeviceContext_RSSetViewports(hv->ctx, 1, &vp);
    ID3D11DeviceContext_VSSetShader(hv->ctx, hv->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(hv->ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, &hv->srv);
    ID3D11DeviceContext_IASetPrimitiveTopology(hv->ctx,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(hv->ctx, NULL);
    ID3D11DeviceContext_Draw(hv->ctx, 3, 0);
}

BOOL HdrVideo_ConvertToP010(HdrVideo *hv, BYTE *dst)
{
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11ShaderResourceView *none[1] = { NULL };
    UINT32 y;
    BYTE  *uvPlane;

    if (!hv || !dst)
        return FALSE;

    Pass(hv, hv->rtvY,  hv->sdrInput ? hv->psYSdr : hv->psY,
         hv->width, hv->height);
    Pass(hv, hv->rtvUV, hv->sdrInput ? hv->psUVSdr : hv->psUV,
         hv->width / 2, hv->height / 2);

    /* The source is bound for reading and is about to be written again by the
       next frame's copy; leaving it bound makes the debug layer complain and
       the driver insert a barrier of its own. */
    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, none);
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 0, NULL, NULL);

    ID3D11DeviceContext_CopyResource(hv->ctx, (ID3D11Resource *)hv->stgY,
                                     (ID3D11Resource *)hv->texY);
    ID3D11DeviceContext_CopyResource(hv->ctx, (ID3D11Resource *)hv->stgUV,
                                     (ID3D11Resource *)hv->texUV);

    if (FAILED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->stgY, 0,
                                       D3D11_MAP_READ, 0, &map)))
        return FALSE;
    {
        UINT16 peak = 0;
        UINT64 sum  = 0;

        for (y = 0; y < hv->height; y++) {
            UINT16 *row = (UINT16 *)(dst + (SIZE_T)y * hv->width * 2);
            UINT32  x;

            memcpy(row, (const BYTE *)map.pData + (SIZE_T)y * map.RowPitch,
                   (SIZE_T)hv->width * 2);
            /* The row is hot in cache from the copy, so measuring it here is
               far cheaper than a second pass later would be. */
            for (x = 0; x < hv->width; x++) {
                if (row[x] > peak) peak = row[x];
                sum += row[x];
            }
        }
        hv->lastPeak = peak;
        hv->lastMean = (double)sum / ((double)hv->width * hv->height);
    }
    ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->stgY, 0);

    uvPlane = dst + (SIZE_T)hv->width * hv->height * 2;
    if (FAILED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->stgUV, 0,
                                       D3D11_MAP_READ, 0, &map)))
        return FALSE;
    for (y = 0; y < hv->height / 2; y++)
        memcpy(uvPlane + (SIZE_T)y * hv->width * 2,
               (const BYTE *)map.pData + (SIZE_T)y * map.RowPitch,
               (SIZE_T)hv->width * 2);
    ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->stgUV, 0);

    return TRUE;
}

/* ------------------------------------------------------------ zero-copy */

/* Compiled and allocated on first use: the CPU path never needs any of it. */
static BOOL EnsureStats(HdrVideo *hv)
{
    pD3DCompile  compile;
    HMODULE      lib;
    ID3DBlob    *code = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    UINT32       tiles;
    int          i;
    BOOL         ok = FALSE;

    if (hv->csStats)
        return TRUE;

    lib = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib)
        return FALSE;
    compile = (pD3DCompile)(void *)GetProcAddress(lib, "D3DCompile");
    if (compile &&
        SUCCEEDED(compile(g_hlslStats, sizeof(g_hlslStats) - 1, NULL, NULL, NULL,
                          "CSStats", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                          &code, &err)) &&
        SUCCEEDED(ID3D11Device_CreateComputeShader(hv->dev,
                      ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                      NULL, &hv->csStats)))
        ok = TRUE;
    else if (err)
        Log_Printf(L"hdrvideo: stats shader failed: %S",
                   (const char *)ID3D10Blob_GetBufferPointer(err));
    if (code) ID3D10Blob_Release(code);
    if (err)  ID3D10Blob_Release(err);
    FreeLibrary(lib);
    if (!ok)
        return FALSE;

    hv->tilesX = (hv->width  + 15) / 16;
    hv->tilesY = (hv->height + 15) / 16;
    tiles = hv->tilesX * hv->tilesY;

    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth           = tiles * 8;
    bd.Usage               = D3D11_USAGE_DEFAULT;
    bd.BindFlags           = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 8;
    if (FAILED(ID3D11Device_CreateBuffer(hv->dev, &bd, NULL, &hv->statsBuf)))
        return FALSE;

    ZeroMemory(&ud, sizeof(ud));
    ud.Format              = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements  = tiles;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(hv->dev,
            (ID3D11Resource *)hv->statsBuf, &ud, &hv->statsUav)))
        return FALSE;

    for (i = 0; i < 3; i++) {
        ZeroMemory(&bd, sizeof(bd));
        bd.ByteWidth      = tiles * 8;
        bd.Usage          = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(hv->dev, &bd, NULL, &hv->statsStage[i])))
            return FALSE;
    }

    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth      = 16;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(hv->dev, &bd, NULL, &hv->statsCb)))
        return FALSE;
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->statsCb,
                                              0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            UINT32 *d = (UINT32 *)m.pData;
            d[0] = hv->width; d[1] = hv->height; d[2] = hv->tilesX; d[3] = 0;
            ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->statsCb, 0);
        }
    }
    hv->tileScratch = (UINT32 *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)tiles * 8);
    return hv->tileScratch != NULL;
}

/*
 * Reads one finished slot and folds it into the running maxima. 'wait' is for
 * the end of the recording and for a ring that has come all the way round;
 * otherwise a slot the GPU is still writing is simply left for later.
 */
static BOOL HarvestSlot(HdrVideo *hv, int slot, BOOL wait)
{
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr;
    UINT32  tiles = hv->tilesX * hv->tilesY, i, peak = 0;
    UINT64  sum = 0;

    if (!hv->statsPending[slot])
        return TRUE;
    hr = ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->statsStage[slot], 0,
                                 D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return FALSE;
    hv->statsPending[slot] = FALSE;
    if (FAILED(hr))
        return TRUE;                  /* lost: a gap in the stats, not an error */
    memcpy(hv->tileScratch, m.pData, (SIZE_T)tiles * 8);
    ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->statsStage[slot], 0);

    for (i = 0; i < tiles; i++) {
        if (hv->tileScratch[i * 2] > peak)
            peak = hv->tileScratch[i * 2];
        sum += hv->tileScratch[i * 2 + 1];
    }
    /* The caller keeps maxima, so reporting the brightest of the frames that
       came in since it last asked loses nothing. */
    if (peak > hv->lastPeak)
        hv->lastPeak = (UINT16)peak;
    {
        double mean = (double)sum / ((double)hv->width * hv->height);
        if (mean > hv->lastMean)
            hv->lastMean = mean;
    }
    return TRUE;
}

BOOL HdrVideo_RenderP010Into(HdrVideo *hv, ID3D11Texture2D *p010)
{
    D3D11_RENDER_TARGET_VIEW_DESC   rd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    ID3D11RenderTargetView    *rtvY = NULL, *rtvUV = NULL;
    ID3D11ShaderResourceView  *luma = NULL;
    ID3D11ShaderResourceView  *none[1]  = { NULL };
    ID3D11UnorderedAccessView *noUav[1] = { NULL };
    BOOL ok = FALSE;
    int  i;

    if (!hv || !p010)
        return FALSE;

    /* The planes as plain textures: luma as R16, interleaved chroma as R16G16. */
    ZeroMemory(&rd, sizeof(rd));
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = DXGI_FORMAT_R16_UNORM;
    if (FAILED(ID3D11Device_CreateRenderTargetView(hv->dev, (ID3D11Resource *)p010, &rd, &rtvY)))
        goto done;
    rd.Format = DXGI_FORMAT_R16G16_UNORM;
    if (FAILED(ID3D11Device_CreateRenderTargetView(hv->dev, (ID3D11Resource *)p010, &rd, &rtvUV)))
        goto done;

    Pass(hv, rtvY,  hv->sdrInput ? hv->psYSdr : hv->psY,
         hv->width, hv->height);
    Pass(hv, rtvUV, hv->sdrInput ? hv->psUVSdr : hv->psUV,
         hv->width / 2, hv->height / 2);
    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, none);
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 0, NULL, NULL);

    /* Reset the per-call maxima; harvested frames raise them again. */
    hv->lastPeak = 0;
    hv->lastMean = 0.0;

    if (EnsureStats(hv)) {
        ZeroMemory(&sd, sizeof(sd));
        sd.Format = DXGI_FORMAT_R16_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(hv->dev,
                (ID3D11Resource *)p010, &sd, &luma))) {
            int slot = hv->statsNext;

            /* Harvest whatever has finished, oldest first, without waiting. */
            for (i = 1; i <= 3; i++)
                HarvestSlot(hv, (slot + i) % 3, FALSE);
            /* The slot about to be reused must be read first, even if that
               means waiting - its frame would otherwise go unmeasured. */
            HarvestSlot(hv, slot, TRUE);

            ID3D11DeviceContext_CSSetShader(hv->ctx, hv->csStats, NULL, 0);
            ID3D11DeviceContext_CSSetShaderResources(hv->ctx, 0, 1, &luma);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(hv->ctx, 0, 1, &hv->statsUav, NULL);
            ID3D11DeviceContext_CSSetConstantBuffers(hv->ctx, 0, 1, &hv->statsCb);
            ID3D11DeviceContext_Dispatch(hv->ctx, hv->tilesX, hv->tilesY, 1);
            ID3D11DeviceContext_CSSetShaderResources(hv->ctx, 0, 1, none);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(hv->ctx, 0, 1, noUav, NULL);
            ID3D11DeviceContext_CSSetShader(hv->ctx, NULL, NULL, 0);

            ID3D11DeviceContext_CopyResource(hv->ctx, (ID3D11Resource *)hv->statsStage[slot],
                                             (ID3D11Resource *)hv->statsBuf);
            hv->statsPending[slot] = TRUE;
            hv->statsNext = (slot + 1) % 3;
            ID3D11ShaderResourceView_Release(luma);
        }
    }
    ok = TRUE;

done:
    if (rtvY)  ID3D11RenderTargetView_Release(rtvY);
    if (rtvUV) ID3D11RenderTargetView_Release(rtvUV);
    return ok;
}

void HdrVideo_DrainLight(HdrVideo *hv)
{
    int i;
    if (!hv || !hv->csStats)
        return;
    hv->lastPeak = 0;
    hv->lastMean = 0.0;
    for (i = 0; i < 3; i++)
        HarvestSlot(hv, (hv->statsNext + i) % 3, TRUE);
}

ID3D11Texture2D *HdrVideo_RenderBgra(HdrVideo *hv)
{
    D3D11_MAPPED_SUBRESOURCE  map;
    ID3D11ShaderResourceView *none[1] = { NULL };

    if (!hv || !EnsureSdr(hv))
        return NULL;
    if (hv->sdrScale <= 0.0f)
        hv->sdrScale = 1.0f;
    if (SUCCEEDED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->cbSdr, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        float *p = (float *)map.pData;
        p[0] = hv->sdrScale;
        p[1] = hv->rollOff ? 1.0f : 0.0f;
        p[2] = p[3] = 0.0f;
        ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->cbSdr, 0);
    }
    ID3D11DeviceContext_PSSetConstantBuffers(hv->ctx, 0, 1, &hv->cbSdr);
    Pass(hv, hv->rtvSdr, hv->psSdr, hv->width, hv->height);
    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, none);
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 0, NULL, NULL);
    return hv->texSdr;
}
