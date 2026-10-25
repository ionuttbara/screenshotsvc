/*
 * record.h - screen recording to MP4.
 *
 * Video comes from Desktop Duplication and audio from WASAPI; both are handed
 * to a Media Foundation sink writer that muxes an MP4.
 *
 * On a display in advanced colour the recorder captures scRGB half-float,
 * converts it to 10-bit BT.2020 PQ and encodes HEVC Main10; otherwise it takes
 * the 8-bit desktop and encodes H.264. Which encoder is available is settled by
 * probing rather than enumeration - see encoder.h.
 */
#ifndef NITSHOT_RECORD_H
#define NITSHOT_RECORD_H

#include "nitshot.h"
#include "audio.h"

#define RECORD_SOURCES 2

typedef struct {
    RECT        region;     /* screen coordinates */
    /* Two sound sources, each a device spec (see audio.h) and its name for
       messages if it is missing. Either may be "none". */
    wchar_t     audioDev[RECORD_SOURCES][AUDIO_DEV_CCH];
    wchar_t     audioName[RECORD_SOURCES][AUDIO_NAME_CCH];
    BOOL        mixAudio;   /* two sources become one track, not two */
    int         fps;
} RecordOptions;

/*
 * The lossless sidecars. One source: "<video>.wav". Two: "<video>.track1.wav"
 * and ".track2.wav", one per source, plus ".track0.wav" for the mix when
 * they are mixed.
 */
#define RECORD_MAX_WAVS 3

typedef struct {
    BOOL    ok;
    wchar_t path[MAX_PATH];
    wchar_t wavPath[RECORD_MAX_WAVS][MAX_PATH]; /* empty where not written */
    int     wavCount;          /* how many of them exist */
    DWORD   seconds;
    wchar_t error[128];     /* empty unless something went wrong */
    wchar_t warning[192];   /* recorded, but not everything that was asked */
    /* The card's own encoder failed before it produced a single picture:
       the file holds nothing worth keeping, and the caller is meant to
       switch that encoder off and record again. 'directName' names it. */
    BOOL    retryWithoutDirect;
    wchar_t directName[32];
} RecordResult;

/* Tests only: the card's encoder "fails" once this many frames have gone
   in (-1, the default: never), for the paths no working card takes. */
extern volatile LONG g_recordFailDirectAfter;

/*
 * Starts recording on a worker thread. When it finishes - because Record_Stop
 * was called or because something failed - 'doneMsg' is posted to 'notify'
 * with a heap-allocated RecordResult as lParam, freed with Record_FreeResult().
 */
BOOL  Record_Start(HWND notify, UINT doneMsg, const RecordOptions *opt);

/* Asks the worker to finish. Returns once it has, so the file is complete. */
void  Record_Stop(void);

BOOL  Record_IsActive(void);

/* True from Record_Start until the worker has finished for good - including
   the setup and finalisation stretches, where Record_IsActive() reads false
   although the thread is very much alive. */
BOOL  Record_IsBusy(void);
DWORD Record_ElapsedMs(void);

void  Record_FreeResult(RecordResult *res);

#endif /* NITSHOT_RECORD_H */
