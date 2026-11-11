/*
 * vpconv.h - BGRA to NV12 on the GPU, through the D3D11 video processor.
 *
 * Every Windows GPU driver has one, and it is what Media Foundation's own
 * colour converter uses underneath. An encoder that does not take BGRA
 * itself - Intel's, for one, depending on the generation - gets NV12 from
 * here, converted as BT.709 limited range, which is what the stream says.
 */
#ifndef SCREENSHOTSERVICE_VPCONV_H
#define SCREENSHOTSERVICE_VPCONV_H

#include "screenshotservice.h"

#include <d3d11.h>

typedef struct VpConv VpConv;

/* NULL if the device's video processor cannot turn BGRA into NV12 at this
   size; 'why' says what refused. */
VpConv *VpConv_Create(ID3D11Device *dev, UINT32 width, UINT32 height,
                      wchar_t *why, size_t cch);

/*
 * Converts 'bgra' (B8G8R8A8, the size given) into 'nv12' (NV12, at least that
 * size). The caller holds the device's ID3D11Multithread lock if others use
 * the immediate context. Views are kept for the last textures seen, so
 * converting into the same few targets over and over costs no allocations.
 */
BOOL VpConv_Run(VpConv *c, ID3D11Texture2D *bgra, ID3D11Texture2D *nv12);

void VpConv_Destroy(VpConv *c);

#endif /* SCREENSHOTSERVICE_VPCONV_H */
