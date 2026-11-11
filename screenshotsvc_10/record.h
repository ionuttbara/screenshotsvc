/*
 * record.h - screen recording to MP4.
 *
 * Video comes from Desktop Duplication and audio from WASAPI. If enabled and
 * supported, the selected adapter's native NVIDIA NVENC, AMD AMF, or modern
 * Intel oneVPL encoder produces the video packets; otherwise a Media Foundation
 * hardware MFT encodes it. Media Foundation's sink writer muxes the MP4.
 *
 * On a display in advanced colour the recorder captures scRGB half-float,
 * converts it to 10-bit BT.2020 PQ and requests HEVC Main10; otherwise it uses
 * the configured SDR profile. Encoder selection is based on runtime probing and
 * native API capability checks, not friendly names alone - see encoder.h/hwenc.h.
 */
#ifndef SCREENSHOTSERVICE_RECORD_H
#define SCREENSHOTSERVICE_RECORD_H

#include "screenshotservice.h"
#include "audio.h"

typedef struct {
    RECT        region;     /* screen coordinates */
    AudioSource audio;
    int         fps;
    BOOL        syncToDisplay;
    BOOL        makeGif;          /* silent GIF output, limited to 15 seconds */
} RecordOptions;

typedef struct {
    BOOL    ok;
    BOOL    usedDirect;        /* native vendor encoder actually added to MP4 */
    wchar_t path[MAX_PATH];       /* final path on success, .partial.mp4 on failure */
    wchar_t wavPath[MAX_PATH]; /* the lossless sidecar, empty if not written */
    DWORD   seconds;
    wchar_t error[128];     /* empty unless something went wrong */
    /* Native encoder died before its first packet: start the same recording
       again via Media Foundation hardware MFT. */
    BOOL    retryWithoutDirect;
    wchar_t directName[32];
    RecordOptions retryOptions;
} RecordResult;

/*
 * Starts recording on a worker thread. On completion (Stop or a failure),
 * 'doneMsg' is posted to 'notify' with a heap-allocated RecordResult as lParam,
 * freed with Record_FreeResult().
 */
BOOL  Record_Start(HWND notify, UINT doneMsg, const RecordOptions *opt);

/* Shutdown-only wait: requests stop and waits until post-processing is complete. */
void  Record_Stop(void);
/* Asynchronous UI stop: requests finalisation without blocking the message loop. */
void  Record_RequestStop(void);
/* Signal from WM_DISPLAYCHANGE; the worker finalises at the original dimensions. */
void  Record_NotifyDisplayChange(void);
/* Best-effort shutdown/crash hook. Never waits from the recorder thread itself. */
void  Record_HandleCrash(void);
/* Toggle a timeline-aware pause/resume; returns TRUE when paused after the call. */
BOOL  Record_TogglePause(void);
BOOL  Record_IsPaused(void);

BOOL  Record_IsActive(void);

/* True from Record_Start until the worker has finished for good - including
   the setup and finalisation stretches, where Record_IsActive() reads false
   although the thread is very much alive. */
BOOL  Record_IsBusy(void);
DWORD Record_ElapsedMs(void);

void  Record_FreeResult(RecordResult *res);

#endif /* SCREENSHOTSERVICE_RECORD_H */
