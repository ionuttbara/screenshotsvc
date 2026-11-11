/*
 * encoder.h - which video encoder the recorder can actually use.
 *
 * Enumerating MFTs says what a driver advertises, which is not the same thing
 * as what the MP4 sink will accept. Some Windows configurations let an AV1
 * MFT configure and start, then fail at Finalize with
 * MF_E_SINK_HEADERS_NOT_FOUND because the in-box MP4 sink cannot mux AV1. So
 * the list offered in Settings is built by *probing* - writing and finalising a
 * one-frame file through the same configuration code recording uses - and not
 * by enumeration. AV1 profiles are offered only when that probe succeeds.
 *
 * Probing costs around 300 ms per candidate, far too much for startup, so the
 * result is cached in the registry against the adapter and its driver version and
 * only redone when either changes.
 */
#ifndef SCREENSHOTSERVICE_ENCODER_H
#define SCREENSHOTSERVICE_ENCODER_H

#include "screenshotservice.h"

#include <mfidl.h>
#include <mfreadwrite.h>

typedef enum {
    ENC_AUTO = 0,   /* pick the best that probed clean; never a probe slot */
    ENC_H264,       /* 8-bit, RGB32 in                                     */
    ENC_HEVC8,      /* 8-bit, RGB32 in                                     */
    ENC_HEVC10,     /* Main10, P010 in, BT.2020 PQ - the HDR one           */
    ENC_HEVC10_SDR, /* Main10 SDR, P010 Rec.709 in; QP 0 is optional         */
    ENC_AV1_8,      /* AV1 Main, 8-bit                                         */
    ENC_AV1_10,     /* AV1 Main, 10-bit SDR                                   */
    ENC_AV1_10_HDR, /* AV1 Main, 10-bit BT.2020 PQ                            */
    ENC_COUNT
} EncoderId;

typedef struct {
    BOOL    probed;                 /* the probe has run at least once */
    BOOL    usable[ENC_COUNT];             /* full hardware-MFT MP4 probe passed */
    BOOL    mftUsable[ENC_COUNT];          /* retained for cache/probe clarity */
    BOOL    hardware[ENC_COUNT];
    wchar_t mft[ENC_COUNT][96];     /* friendly name of the encoder that ran */
    wchar_t vendor[ENC_COUNT][16];  /* concrete MFT vendor resolved from Auto */
    UINT32  candidate[ENC_COUNT];   /* diagnostic ordinal within the chosen vendor */
    wchar_t hardwareUrl[ENC_COUNT][1024]; /* exact activation URL that passed the probe */
    CLSID   mftClsid[ENC_COUNT];    /* exact encoder CLSID tested for each profile */
} EncoderCaps;

/* Physical graphics adapters reported by DXGI. The device names are kept so
   Settings can identify the actual GPU rather than offering hard-coded vendors. */
typedef struct {
    BOOL    amd;
    BOOL    intel;
    BOOL    nvidia;
    wchar_t amdName[128];
    wchar_t intelName[128];
    wchar_t nvidiaName[128];
} EncoderGpuInventory;

void Encoder_GetGpuInventory(EncoderGpuInventory *inventory);

/* The cached result. Never NULL; everything is FALSE before the first probe. */
const EncoderCaps *Encoder_Caps(void);

/* Runs the probe unless a matching cache entry exists. Blocking, seconds. */
void Encoder_Probe(BOOL force);
/* Same probe, scoped to Auto or one selected GPU vendor (amd/intel/nvidia). */
void Encoder_ProbeForVendor(const wchar_t *vendor, BOOL force);
/* Temporarily prefer a vendor's local encoder MFT for the requested profile. */
BOOL Encoder_PrepareVendor(EncoderId id, const wchar_t *vendor);
void Encoder_ClearVendor(void);
BOOL Encoder_WriterMatchesVendor(IMFSinkWriter *writer, DWORD stream,
                                 const wchar_t *vendor);

/* Same, on a worker thread, at most one at a time. Used to warm the cache
   without holding anything up. */
void Encoder_ProbeAsync(void);

/*
 * Turns a setting into the encoder that will actually be used: applies the
 * ranking for ENC_AUTO, and walks down to the next usable one when the chosen
 * encoder did not probe clean. Returns ENC_COUNT when nothing works at all.
 */
EncoderId Encoder_Resolve(EncoderId want, BOOL hdr);

/* The next usable encoder below 'id', for recovering at record time. */
EncoderId Encoder_Fallback(EncoderId id);

BOOL           Encoder_IsHdr(EncoderId id);     /* BT.2020 PQ HDR only */
BOOL           Encoder_Is10Bit(EncoderId id);  /* P010 / 10-bit profile */
BOOL           Encoder_IsLossless(EncoderId id); /* QP 0 required for this mode */
BOOL           Encoder_ConfigureVBR(IMFSinkWriter *writer, DWORD stream, EncoderId id,
                                  UINT32 width, UINT32 height, int fps);
BOOL           Encoder_ConfigureLossless(IMFSinkWriter *writer, DWORD stream);
const wchar_t *Encoder_Name(EncoderId id);      /* for the UI and the log */
const wchar_t *Encoder_Token(EncoderId id);     /* for the registry cache */
UINT32         Encoder_Bitrate(EncoderId id, UINT32 width, UINT32 height, int fps);
EncoderId      Encoder_FromToken(const wchar_t *token);

/*
 * Adds the video stream and sets the input type. These are what the probe
 * exercises, so a combination that probes clean is one that records.
 * 'streamIndex' receives the sink writer's stream index.
 */
BOOL Encoder_AddVideoStream(IMFSinkWriter *writer, EncoderId id,
                            UINT32 width, UINT32 height, int fps,
                            DWORD *streamIndex);

#endif /* SCREENSHOTSERVICE_ENCODER_H */
