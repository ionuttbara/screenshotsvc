/* Native ISO-BMFF FLAC track muxing; no external muxer required. */
#ifndef SCREENSHOTSERVICE_MP4MULTI_H
#define SCREENSHOTSERVICE_MP4MULTI_H

#include "screenshotservice.h"
#include "flacenc.h"

/* Add microphone and system FLAC tracks to a finished MP4 whose combined AAC
   track already exists. The original MP4 is left untouched if muxing fails. */
BOOL Mp4Multi_HasFlacTracks(const wchar_t *mp4);
BOOL Mp4Multi_AddFlacTracks(const wchar_t *mp4,
                            const FlacEncodedTrack *microphone,
                            const FlacEncodedTrack *systemAudio);

#endif /* SCREENSHOTSERVICE_MP4MULTI_H */
