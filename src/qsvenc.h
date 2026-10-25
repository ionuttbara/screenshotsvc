/*
 * qsvenc.h - Intel's encoder, driven directly through oneVPL.
 *
 * For Intel Arc and Xe graphics, whose driver brings the oneVPL GPU runtime
 * (libmfx64-gen.dll, API 2.x). Older Intel GPUs have only the legacy Media
 * SDK runtime and stay on Media Foundation. The runtime lives in the
 * driver's DriverStore folder, not System32; it is found the way Intel's
 * dispatcher finds it - through the DriverStorePathForVPL value of the
 * adapter's driver key - and loaded only from under System32\DriverStore.
 * Nothing is linked. The types and the way the recorder uses it are in
 * hwenc.h.
 *
 * Frames go to the encoder in video memory through a frame allocator of
 * Nitshot's own (D3D11 textures it creates when the encoder asks), so it
 * always knows which texture and slice a surface is, and copies each frame
 * there on the GPU. BGRA is converted to NV12 by the D3D11 video processor
 * (vpconv.c) on the way, since not every Intel encoder takes RGB.
 *
 * Written against the oneVPL 2.13 headers without Intel hardware to test on,
 * so every step goes into the diagnostic log: where the runtime was found,
 * its implementations and API version, every parameter the runtime changed
 * when queried, the allocations it asked for, and each status that was not
 * plain success.
 */
#ifndef NITSHOT_QSVENC_H
#define NITSHOT_QSVENC_H

#include "hwenc.h"

typedef struct QsvEncoder QsvEncoder;

BOOL        Qsv_Query(ID3D11Device *dev, const wchar_t *dll, HwEncInfo *info);
QsvEncoder *Qsv_Create(ID3D11Device *dev, const HwEncConfig *cfg, wchar_t *why, size_t cch);
LONGLONG    Qsv_StartOffset(const QsvEncoder *e);
BOOL        Qsv_SequenceHeader(QsvEncoder *e, const BYTE **data, UINT32 *size);
BOOL        Qsv_Encode(QsvEncoder *e, ID3D11Texture2D *tex, LONGLONG pts,
                       HwPacketFn out, void *ctx);
BOOL        Qsv_Flush(QsvEncoder *e, HwPacketFn out, void *ctx);
void        Qsv_Destroy(QsvEncoder *e);

/* Whether 'dir' is a folder under %SystemRoot%\System32\DriverStore - the
   only place a runtime path from the registry is trusted. Public for the
   tests. */
BOOL Qsv_IsDriverStorePath(const wchar_t *dir);

/* Appends one parameter set to 'out' as Annex-B, adding the start code if
   the runtime handed it over without one. Public for the tests. */
BOOL Qsv_AppendNal(const BYTE *nal, UINT32 len, BYTE *out, UINT32 cap, UINT32 *used);

extern const HwEncOps g_qsvOps;

#endif /* NITSHOT_QSVENC_H */
