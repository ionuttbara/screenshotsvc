/*
 * hwenc.h - a graphics card's own video encoder, driven directly.
 *
 * Media Foundation reaches each vendor's encoder through a wrapper that
 * exposes only part of it - NVIDIA's writes no B-frames and does not look
 * ahead - so on hardware whose own API is available the recorder encodes
 * through that instead and hands the finished bitstream to the MP4 sink,
 * which then only muxes. One module per vendor (nvenc.c, amfenc.c), each
 * filling in the table at the bottom; the recorder picks by the adapter its
 * capture device is on.
 *
 * Nothing is linked: every vendor's runtime is part of its driver and is
 * loaded from System32 at run time. Where it is missing, too old or refuses
 * the device, the recorder falls back to Media Foundation as before.
 */
#ifndef NITSHOT_HWENC_H
#define NITSHOT_HWENC_H

#include "nitshot.h"

#include <d3d11.h>

/* What an encoder can do, per codec: [0] H.264, [1] HEVC. */
typedef struct {
    BOOL    h264, hevc;        /* the codec is offered at all            */
    BOOL    hevc10;            /* HEVC Main10 from 10-bit input          */
    int     maxB[2];           /* B-frames; 0 = none                     */
    BOOL    lookahead[2];
    BOOL    bRef[2];           /* B-frames usable as references          */
    BOOL    temporalAq[2];
    int     maxWidth, maxHeight; /* HEVC's, which are the larger         */
    int     engines;           /* separate encoder blocks on the chip    */
} HwEncCaps;

typedef struct {
    BOOL      available;       /* loaded, recent enough, session opened  */
    UINT32    driverMajor, driverMinor; /* the runtime's API version     */
    HwEncCaps caps;
    wchar_t   why[128];        /* if not available, the reason, for the log */
} HwEncInfo;

typedef struct {
    BOOL        hevc;           /* else H.264 High                         */
    BOOL        tenBit;         /* HEVC Main10 from P010                   */
    DXGI_FORMAT input;          /* NV12, P010 or B8G8R8A8 textures          */
    UINT32      width, height;  /* even                                    */
    int         fps;
    UINT32      bitrate;        /* average, bits per second                */
    int         preset;         /* 1..7, fastest .. best; NVENC's P1..P7,
                                   mapped onto the vendor's own steps      */
    int         bFrames;        /* 0..what HwEncCaps::maxB allows          */
    int         lookahead;      /* frames; 0 = off                          */
    /* Spatial adaptive quantisation. Off by default, on measurement: it
       moves bits from busy areas to flat ones, and on a screen the busy
       areas are the text - 1 to 2 dB of luma PSNR lost for a hair of SSIM. */
    BOOL        aq;
    /* Intel: hand the frames over as textures in Nitshot's own video memory
       rather than in system memory for the runtime to upload. Faster, but
       the textures-through-an-allocator path failed on an Arc 140V ("device
       failed" on the first frame), so it is off unless asked for. */
    BOOL        videoMemory;

    /* HDR10: BT.2020 primaries and the PQ curve in the stream's colour
       description instead of BT.709. Needs HEVC Main10 from P010. */
    BOOL        hdr;
    /* With hdr, the display the content was made on, written into the
       stream as mastering-display metadata: CIE xy and nits. A zero white
       point leaves it out. (The light levels are only known when the
       recording ends; they go into the container, as on the Media
       Foundation path.) */
    float       redX, redY, greenX, greenY, blueX, blueY, whiteX, whiteY;
    float       maxNits, minNits;
} HwEncConfig;

/*
 * One encoded access unit, Annex-B, in decode order. 'pts' is what was handed
 * in with the frame plus the encoder's start offset; 'dts' is never later
 * than it, starts at the first frame's own time and rises by whole frames -
 * with B-frames the pictures leave in another order than they arrived, and
 * the container needs both. Called from encode / flush on the caller's
 * thread.
 */
typedef void (*HwPacketFn)(void *ctx, const BYTE *data, UINT32 size,
                           LONGLONG pts, LONGLONG dts, BOOL keyFrame);

/*
 * A vendor's encoder, as the recorder uses it. The encoder handle is the
 * module's own type behind a void pointer.
 *
 * query:   whether it can encode for this device, and what it can do. 'dll'
 *          NULL for the vendor's runtime; another name lets a test see the
 *          missing-driver path.
 * create:  NULL with the reason in 'why' if it cannot be set up as asked.
 * header:  SPS/PPS (and VPS for HEVC) as Annex-B, owned by the encoder.
 * offset:  how much later than handed in every picture is presented - the
 *          B-frames' reorder depth, in 100 ns; the sound has to be moved by
 *          the same to stay with it. 0 without B-frames.
 * encode:  one frame; the texture is copied at once on the GPU, so the
 *          caller may reuse it straight away. Takes the device's
 *          ID3D11Multithread lock for the copy. Whatever finished encoding
 *          is handed to 'out' before it returns.
 * flush:   ends the stream and hands out everything still inside.
 */
typedef struct {
    const wchar_t *name;        /* for the log: "NVENC", "AMF"             */
    BOOL     (*query)(ID3D11Device *dev, const wchar_t *dll, HwEncInfo *info);
    void    *(*create)(ID3D11Device *dev, const HwEncConfig *cfg, wchar_t *why, size_t cch);
    BOOL     (*header)(void *e, const BYTE **data, UINT32 *size);
    LONGLONG (*offset)(const void *e);
    BOOL     (*encode)(void *e, ID3D11Texture2D *tex, LONGLONG pts, HwPacketFn out, void *ctx);
    BOOL     (*flush)(void *e, HwPacketFn out, void *ctx);
    void     (*destroy)(void *e);
} HwEncOps;

#endif /* NITSHOT_HWENC_H */
