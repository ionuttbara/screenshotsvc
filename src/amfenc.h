/*
 * amfenc.h - AMD's encoder, driven directly through AMF.
 *
 * The AMD Advanced Media Framework runtime (amfrt64.dll) is part of the AMD
 * driver and is loaded from System32 at run time; nothing is linked. The
 * types and the way the recorder uses it are in hwenc.h.
 *
 * What AMF offers depends on the chip and the driver, and is asked for, not
 * assumed: B-frames only for H.264 and only where the encoder reports them
 * (this SDK version has none for HEVC), pre-analysis as lookahead where
 * reported, HEVC Main10 where the maximum profile allows it.
 *
 * Written against the AMF SDK 1.4.35 headers without AMD hardware to test
 * on, so it logs every step - the runtime's version, each capability, each
 * property set and its result, and AMF's own warnings and errors - into the
 * diagnostic log, for a user with an AMD card to send in.
 */
#ifndef NITSHOT_AMFENC_H
#define NITSHOT_AMFENC_H

#include "hwenc.h"

typedef struct AmfEncoder AmfEncoder;

BOOL        Amf_Query(ID3D11Device *dev, const wchar_t *dll, HwEncInfo *info);
AmfEncoder *Amf_Create(ID3D11Device *dev, const HwEncConfig *cfg, wchar_t *why, size_t cch);
LONGLONG    Amf_StartOffset(const AmfEncoder *e);
BOOL        Amf_SequenceHeader(AmfEncoder *e, const BYTE **data, UINT32 *size);
BOOL        Amf_Encode(AmfEncoder *e, ID3D11Texture2D *tex, LONGLONG pts,
                       HwPacketFn out, void *ctx);
BOOL        Amf_Flush(AmfEncoder *e, HwPacketFn out, void *ctx);
void        Amf_Destroy(AmfEncoder *e);

/* The parameter sets AMF reports, as Annex-B: a configuration record (avcC,
   hvcC) is unpacked, Annex-B passed through. FALSE if neither or truncated.
   Public so the tests can feed it records without AMD hardware. */
BOOL Amf_ToAnnexB(const BYTE *in, UINT32 len, BOOL hevc, BYTE *out, UINT32 cap, UINT32 *outLen);

extern const HwEncOps g_amfOps;

#endif /* NITSHOT_AMFENC_H */
