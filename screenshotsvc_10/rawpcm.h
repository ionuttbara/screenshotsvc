/*
 * rawpcm.h - timestamp-aware raw PCM capture sink used by the multitrack path.
 * The file deliberately has no container header: source format lives in the
 * recording state and the captured packet bytes remain untouched.
 */
#ifndef SCREENSHOTSERVICE_RAWPCM_H
#define SCREENSHOTSERVICE_RAWPCM_H

#include "screenshotservice.h"

typedef struct RawPcmSink RawPcmSink;

RawPcmSink *RawPcm_Open(const wchar_t *path, UINT32 sampleRate,
                        UINT32 blockAlign, UINT64 origin100ns);
BOOL RawPcm_WriteTimed(RawPcmSink *sink, const BYTE *data, UINT32 frames,
                       BOOL silent, UINT64 packetQpc100ns);
BOOL RawPcm_PadTo(RawPcmSink *sink, UINT64 targetFrames);
void RawPcm_SetSilenceByte(RawPcmSink *sink, BYTE value);
BOOL RawPcm_Close(RawPcmSink *sink, UINT64 *framesWritten);

#endif /* SCREENSHOTSERVICE_RAWPCM_H */
