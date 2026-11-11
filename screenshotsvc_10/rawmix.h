#ifndef SCREENSHOTSERVICE_RAWMIX_H
#define SCREENSHOTSERVICE_RAWMIX_H

#include "screenshotservice.h"

typedef void (*RawMixProgressFn)(void *context, UINT64 done, UINT64 total);

/* Offline, timestamp-aligned render of two captured RAW WASAPI streams to
   the PCM WAV source consumed by the native Media Foundation AAC remux path. */
BOOL RawMix_BuildStereoWav(const wchar_t *microphonePath,
                           const BYTE *microphoneFormat, UINT32 microphoneFormatBytes,
                           UINT64 microphoneStartSample,
                           const wchar_t *systemPath,
                           const BYTE *systemFormat, UINT32 systemFormatBytes,
                           UINT64 systemStartSample,
                           UINT32 outputRate, UINT64 outputFrames,
                           const wchar_t *wavPath,
                           RawMixProgressFn progress, void *progressContext);

#endif
