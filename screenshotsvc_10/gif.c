/* Native MP4 -> animated GIF converter. No FFmpeg or external executables. */
#define COBJMACROS
#include "gif.h"
#include "log.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wincodec.h>
#include <propvarutil.h>
#include <string.h>
#include <shlwapi.h>
#include <strsafe.h>

#define GIF_FRAME_INTERVAL_100NS 666667LL /* 66.67 ms ~= 15 FPS; GIF delays are centiseconds */
#define GIF_MAX_WIDTH 1920u


static void Gif_SetLooping(IWICBitmapEncoder *encoder)
{
    IWICMetadataQueryWriter *meta = NULL;
    if (FAILED(IWICBitmapEncoder_GetMetadataQueryWriter(encoder, &meta)) || !meta)
        return;
    {
        static const BYTE appBytes[11] = {
            'N','E','T','S','C','A','P','E','2','.','0'
        };
        static const BYTE loopBytes[4] = { 3, 1, 0, 0 };
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_VECTOR | VT_UI1;
        value.caub.cElems = ARRAYSIZE(appBytes);
        value.caub.pElems = (BYTE *)CoTaskMemAlloc(sizeof(appBytes));
        if (value.caub.pElems) {
            memcpy(value.caub.pElems, appBytes, sizeof(appBytes));
            (void)IWICMetadataQueryWriter_SetMetadataByName(meta, L"/appext/Application", &value);
            PropVariantClear(&value);
        }
        PropVariantInit(&value);
        value.vt = VT_VECTOR | VT_UI1;
        value.caub.cElems = ARRAYSIZE(loopBytes);
        value.caub.pElems = (BYTE *)CoTaskMemAlloc(sizeof(loopBytes));
        if (value.caub.pElems) {
            memcpy(value.caub.pElems, loopBytes, sizeof(loopBytes));
            (void)IWICMetadataQueryWriter_SetMetadataByName(meta, L"/appext/Data", &value);
            PropVariantClear(&value);
        }
    }
    IWICMetadataQueryWriter_Release(meta);
}

static BOOL Gif_WriteFrame(IWICImagingFactory *factory, IWICBitmapEncoder *encoder,
                           IWICPalette **palette, UINT32 sourceW, UINT32 sourceH,
                           const BYTE *pixels, UINT32 bufferLength, UINT32 stride, UINT32 frameIndex,
                           UINT32 *outW, UINT32 *outH, BOOL *paletteReady)
{
    IWICBitmap *bitmap = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *converter = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *props = NULL;
    IWICBitmapSource *source = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat8bppIndexed;
    HRESULT hr;
    BOOL ok = FALSE;

    if (!sourceW || !sourceH || !pixels || bufferLength < stride * sourceH)
        return FALSE;

    if (!*outW || !*outH) {
        *outW = sourceW;
        *outH = sourceH;
        if (*outW > GIF_MAX_WIDTH) {
            *outH = (UINT32)(((UINT64)sourceH * GIF_MAX_WIDTH + sourceW / 2) / sourceW);
            *outW = GIF_MAX_WIDTH;
        }
        if (!*outW) *outW = 1;
        if (!*outH) *outH = 1;
    }

    hr = IWICImagingFactory_CreateBitmapFromMemory(factory, sourceW, sourceH,
            &GUID_WICPixelFormat32bppBGR, stride, bufferLength, (BYTE *)pixels, &bitmap);
    if (FAILED(hr)) goto done;
    hr = IWICImagingFactory_CreateBitmapScaler(factory, &scaler);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource *)bitmap, *outW, *outH,
                                     WICBitmapInterpolationModeFant);
    if (FAILED(hr)) goto done;
    source = (IWICBitmapSource *)scaler;

    if (!*paletteReady) {
        hr = IWICImagingFactory_CreatePalette(factory, palette);
        if (FAILED(hr)) goto done;
        hr = IWICPalette_InitializeFromBitmap(*palette, source, 256, FALSE);
        if (FAILED(hr)) goto done;
        *paletteReady = TRUE;
    }

    hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(hr)) goto done;
    hr = IWICFormatConverter_Initialize(converter, source, &GUID_WICPixelFormat8bppIndexed,
            WICBitmapDitherTypeErrorDiffusion, *palette, 0.0,
            WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) goto done;

    hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &props);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_Initialize(frame, props);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_SetSize(frame, *outW, *outH);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &pf);
    if (FAILED(hr) || !IsEqualGUID(&pf, &GUID_WICPixelFormat8bppIndexed)) goto done;
    hr = IWICBitmapFrameEncode_SetPalette(frame, *palette);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_SetResolution(frame, 96.0, 96.0);
    if (FAILED(hr)) goto done;
    /* GIF delays are centiseconds. 7,7,6 averages 15 FPS over each three-frame cycle. */
    {
        IWICMetadataQueryWriter *meta = NULL;
        if (SUCCEEDED(IWICBitmapFrameEncode_GetMetadataQueryWriter(frame, &meta))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            value.vt = VT_UI2;
            value.uiVal = (USHORT)((frameIndex % 3u == 2u) ? 6u : 7u);
            IWICMetadataQueryWriter_SetMetadataByName(meta, L"/grctlext/Delay", &value);
            value.uiVal = 2; /* restore to background */
            IWICMetadataQueryWriter_SetMetadataByName(meta, L"/grctlext/Disposal", &value);
            PropVariantClear(&value);
            IWICMetadataQueryWriter_Release(meta);
        }
    }
    hr = IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)converter, NULL);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) ok = TRUE;

done:
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (props) IPropertyBag2_Release(props);
    if (converter) IWICFormatConverter_Release(converter);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (bitmap) IWICBitmap_Release(bitmap);
    return ok;
}

BOOL Gif_ConvertMp4ToGif(const wchar_t *inputPath, const wchar_t *outputPath,
                         DWORD maxDurationMs, GifProgressFn progress, void *context)
{
    IMFAttributes *readerAttrs = NULL;
    IMFSourceReader *reader = NULL;
    IMFMediaType *nativeType = NULL, *rgbType = NULL;
    IWICImagingFactory *factory = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICPalette *palette = NULL;
    IStream *stream = NULL;
    UINT64 frameSize = 0;
    UINT32 sourceW = 0, sourceH = 0, outW = 0, outH = 0;
    UINT64 frameBytes64;
    DWORD streamIndex = 0;
    LONGLONG firstTime = -1, nextFrame = 0;
    UINT32 frames = 0;
    BOOL paletteReady = FALSE, ok = FALSE;
    HRESULT hr;

    if (!inputPath || !outputPath || !*inputPath || !*outputPath)
        return FALSE;
    if (maxDurationMs == 0 || maxDurationMs > 15000)
        maxDurationMs = 15000;

    hr = MFCreateAttributes(&readerAttrs, 2);
    if (FAILED(hr)) goto done;
    IMFAttributes_SetUINT32(readerAttrs, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    IMFAttributes_SetUINT32(readerAttrs, &MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    hr = MFCreateSourceReaderFromURL(inputPath, readerAttrs, &reader);
    if (FAILED(hr)) goto done;

    IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr)) goto done;

    hr = MFCreateMediaType(&rgbType);
    if (FAILED(hr)) goto done;
    IMFMediaType_SetGUID(rgbType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(rgbType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    hr = IMFSourceReader_SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                              NULL, rgbType);
    if (FAILED(hr)) goto done;
    hr = IMFSourceReader_GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                              &nativeType);
    if (FAILED(hr)) goto done;
    hr = IMFMediaType_GetUINT64(nativeType, &MF_MT_FRAME_SIZE, &frameSize);
    if (FAILED(hr)) goto done;
    sourceW = (UINT32)(frameSize >> 32);
    sourceH = (UINT32)(frameSize & 0xffffffffu);
    if (!sourceW || !sourceH || sourceW > 16384 || sourceH > 16384) goto done;
    frameBytes64 = (UINT64)sourceW * sourceH * 4u;
    if (frameBytes64 > MAXDWORD) goto done;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr)) goto done;
    hr = SHCreateStreamOnFileEx(outputPath, STGM_CREATE | STGM_WRITE,
                                FILE_ATTRIBUTE_NORMAL, TRUE, NULL, &stream);
    if (FAILED(hr)) goto done;
    hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatGif, NULL, &encoder);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapEncoder_Initialize(encoder, stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) goto done;
    Gif_SetLooping(encoder);

    for (;;) {
        IMFSample *sample = NULL;
        IMFMediaBuffer *buffer = NULL;
        DWORD flags = 0;
        LONGLONG time = 0, relative;
        BYTE *pixels = NULL;
        DWORD maxLength = 0, currentLength = 0;
        HRESULT readHr;

        readHr = IMFSourceReader_ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                             0, &streamIndex, &flags, &time, &sample);
        if (FAILED(readHr)) { if (sample) IMFSample_Release(sample); goto done; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) IMFSample_Release(sample);
            break;
        }
        if (!sample) continue;
        if (firstTime < 0) firstTime = time;
        relative = time - firstTime;
        if (relative < 0) relative = 0;
        if (relative >= (LONGLONG)maxDurationMs * 10000LL) {
            IMFSample_Release(sample);
            break;
        }
        if (relative < nextFrame && frames > 0) {
            IMFSample_Release(sample);
            continue;
        }
        hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
        IMFSample_Release(sample);
        if (FAILED(hr)) goto done;
        hr = IMFMediaBuffer_Lock(buffer, &pixels, &maxLength, &currentLength);
        if (FAILED(hr)) { IMFMediaBuffer_Release(buffer); goto done; }
        if (currentLength >= (DWORD)frameBytes64 && maxLength >= currentLength) {
            if (!Gif_WriteFrame(factory, encoder, &palette, sourceW, sourceH, pixels,
                                currentLength, sourceW * 4u, frames, &outW, &outH,
                                &paletteReady)) {
                IMFMediaBuffer_Unlock(buffer);
                IMFMediaBuffer_Release(buffer);
                goto done;
            }
            frames++;
            nextFrame = (LONGLONG)frames * GIF_FRAME_INTERVAL_100NS;
            if (progress) {
                int pct = (int)((relative * 100) /
                                  ((LONGLONG)maxDurationMs * 10000LL));
                if (pct > 100) pct = 100;
                progress(pct, context);
            }
        }
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_Release(buffer);
    }
    if (!frames) goto done;
    hr = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(hr)) goto done;
    IStream_Commit(stream, STGC_DEFAULT);
    ok = TRUE;
    if (progress) progress(100, context);

done:
    if (!ok && outputPath) DeleteFileW(outputPath);
    if (palette) IWICPalette_Release(palette);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    if (nativeType) IMFMediaType_Release(nativeType);
    if (rgbType) IMFMediaType_Release(rgbType);
    if (reader) IMFSourceReader_Release(reader);
    if (readerAttrs) IMFAttributes_Release(readerAttrs);
    Log_Printf(L"gif: conversion %s frames=%u output=%s", ok ? L"ok" : L"failed", frames, outputPath);
    return ok;
}
