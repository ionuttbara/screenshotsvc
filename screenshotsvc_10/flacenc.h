#ifndef SCREENSHOTSERVICE_FLACENC_H
#define SCREENSHOTSERVICE_FLACENC_H

#include "screenshotservice.h"

#define FLAC_FRAME_SAMPLES 4096u
#define FLAC_NATIVE_HEADER_BYTES 42u

typedef void (*FlacProgressFn)(void *context, UINT64 done, UINT64 total);

typedef struct {
    UINT64 offset;       /* byte offset of this frame after the 42-byte header */
    UINT32 size;
    UINT32 sampleCount;
} FlacFrameIndex;

typedef struct {
    wchar_t        path[MAX_PATH];
    UINT32         sampleRate;
    UINT32         channels;
    UINT32         channelMask;  /* WAVEFORMATEXTENSIBLE speaker mask, when available */
    UINT32         bitsPerSample;
    UINT32         blockAlign;
    UINT64         totalSamples;
    UINT32         frameCount;
    FlacFrameIndex *frames;
    BYTE           streamInfo[34];
} FlacEncodedTrack;

/* Converts endpoint-native PCM (or IEEE float) samples to a native FLAC file.
   Only the requested [startSample, startSample + sampleCount) range is used;
   gaps beyond the raw file are filled with silence to keep the track aligned. */
BOOL Flac_EncodeRaw(const wchar_t *rawPath,
                    const BYTE *formatBytes, UINT32 formatByteCount,
                    UINT64 startSample, UINT64 sampleCount,
                    UINT32 preferredFloatBits,
                    const wchar_t *flacPath, FlacEncodedTrack *outTrack,
                    FlacProgressFn progress, void *progressContext);
void Flac_FreeTrack(FlacEncodedTrack *track);

#endif /* SCREENSHOTSERVICE_FLACENC_H */
