/*
 * recovery.h - startup discovery and best-effort recovery of interrupted
 * recordings. Original RAW data is retained whenever a playable derivative is
 * written, and incomplete video containers are never renamed as complete.
 */
#ifndef SCREENSHOTSERVICE_RECOVERY_H
#define SCREENSHOTSERVICE_RECOVERY_H

#include "screenshotservice.h"

typedef struct {
    unsigned detected;
    unsigned recoveredMp4;
    unsigned recoveredWav;
    unsigned recoveredFlac;
    unsigned recoveredGif;
    unsigned recoveredRawAudio;
    unsigned needsAttention;
} RecoverySummary;

/* Persist the exact audio format needed to turn a future orphaned RAW stream
   into a WAV. The metadata is written atomically before capture continues. */
BOOL Recovery_WriteRawMetadata(const wchar_t *rawPath,
                               const BYTE *formatBytes,
                               UINT32 formatByteCount,
                               UINT64 origin100ns);
void Recovery_DeleteRawMetadata(const wchar_t *rawPath);

/* Scan only the configured recording folder. This is intended for startup,
   before the recorder is started. Existing source files are preserved unless
   a structurally valid recoverable artifact is promoted to a recovered name. */
BOOL Recovery_ScanFolder(const wchar_t *folder, RecoverySummary *summary);

#endif /* SCREENSHOTSERVICE_RECOVERY_H */
