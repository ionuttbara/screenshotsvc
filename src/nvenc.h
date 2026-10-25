/*
 * nvenc.h - NVIDIA's encoder, driven directly.
 *
 * Media Foundation's NVIDIA encoder writes neither B-frames nor anything but
 * its default quality settings: it reports the B-frame count as not
 * implemented and ignores it as an encoding parameter. The NVENC API itself
 * offers both, plus lookahead and the P1..P7 quality presets. The types and
 * the way the recorder uses it are in hwenc.h.
 *
 * Nothing is linked: the driver's nvEncodeAPI64.dll is loaded from System32
 * at run time.
 */
#ifndef NITSHOT_NVENC_H
#define NITSHOT_NVENC_H

#include "hwenc.h"

typedef struct NvencEncoder NvencEncoder;

/* Opens and closes one encode session to find out, which takes a few
   milliseconds. */
BOOL          Nvenc_Query(ID3D11Device *dev, const wchar_t *dll, HwEncInfo *info);
NvencEncoder *Nvenc_Create(ID3D11Device *dev, const HwEncConfig *cfg,
                           wchar_t *why, size_t cch);
LONGLONG      Nvenc_StartOffset(const NvencEncoder *e);
BOOL          Nvenc_SequenceHeader(NvencEncoder *e, const BYTE **data, UINT32 *size);
/* NVENC holds on to its inputs while it looks ahead and reorders, which is
   why the frame is copied into a texture of the encoder's own. */
BOOL          Nvenc_Encode(NvencEncoder *e, ID3D11Texture2D *tex, LONGLONG pts,
                           HwPacketFn out, void *ctx);
BOOL          Nvenc_Flush(NvencEncoder *e, HwPacketFn out, void *ctx);
void          Nvenc_Destroy(NvencEncoder *e);

extern const HwEncOps g_nvencOps;

#endif /* NITSHOT_NVENC_H */
