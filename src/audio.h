/*
 * audio.h - WASAPI capture for the recorder.
 *
 * A capture records one endpoint: either what a playback device is playing (a
 * loopback capture of a render endpoint) or a recording device such as a
 * microphone. Several captures can run at once; each is its own object with
 * its own thread, and each runs on its device's own clock.
 *
 * Devices are named by a "spec" string, which is what the settings store:
 * one of the AUDIO_DEV_* tokens below, or a Windows endpoint id.
 */
#ifndef NITSHOT_AUDIO_H
#define NITSHOT_AUDIO_H

#include "nitshot.h"

#define AUDIO_DEV_CCH   256   /* endpoint ids are about 55 characters */
#define AUDIO_NAME_CCH  128

#define AUDIO_DEV_NONE              L"none"
#define AUDIO_DEV_DEFAULT_PLAYBACK  L"default-playback" /* follows Windows */
#define AUDIO_DEV_DEFAULT_INPUT     L"default-input"

/* Every capture delivers this, whatever the device runs at - see audio.c. */
#define AUDIO_RATE      48000
#define AUDIO_CHANNELS  2

typedef enum {
    AUDIO_FLOW_PLAYBACK = 0, /* recorded as what it plays */
    AUDIO_FLOW_INPUT         /* microphones, line in, stereo mix... */
} AudioFlow;

typedef struct {
    wchar_t   id[AUDIO_DEV_CCH];
    wchar_t   name[AUDIO_NAME_CCH];
    AudioFlow flow;
} AudioDevice;

typedef struct {
    UINT32 sampleRate;
    UINT32 channels;     /* 1 or 2 */
} AudioFormat;

typedef struct AudioCapture AudioCapture;

/*
 * Called from the capture thread with interleaved 16-bit PCM. 'frames' is the
 * count per channel. Must not block for long - the audio engine is waiting.
 */
typedef void (*AudioSinkFn)(void *ctx, const BYTE *pcm16, UINT32 frames);

/*
 * The active devices, playback devices first, each group in Windows' order.
 * Returns the count and a list to free with Audio_FreeDevices (NULL if none).
 */
UINT Audio_ListDevices(AudioDevice **list);
void Audio_FreeDevices(AudioDevice *list);

BOOL Audio_IsNone(const wchar_t *spec);

/*
 * The device's current name - for the defaults, the name of whichever device
 * is the default right now. FALSE if the spec names no device that is present.
 */
BOOL Audio_DeviceName(const wchar_t *spec, wchar_t *name, size_t cch);

/*
 * Starts capturing. 'fmt' receives the format the callback will deliver:
 * AUDIO_RATE stereo wherever Windows can convert to it, which on Windows 10
 * and later is every device; otherwise the device's own rate, refused unless
 * it is one AAC can take. NULL if the device is missing or will not open.
 */
AudioCapture *Audio_Start(const wchar_t *spec, AudioFormat *fmt,
                          AudioSinkFn sink, void *ctx);

/* The device's name as Windows shows it - for a default, the device it
   turned out to be. */
const wchar_t *Audio_Name(const AudioCapture *cap);

/* Stops and frees. No callback runs once this returns, except from a capture
   thread that failed to stop in time, which is logged. NULL is ignored. */
void Audio_Stop(AudioCapture *cap);

#endif /* NITSHOT_AUDIO_H */
