/*
 * audio.h - WASAPI capture for the recorder.
 *
 * Captures system loopback, the microphone, or both. The combined mode mixes
 * two independent WASAPI capture endpoints into a single stereo AAC stream.
 */
#ifndef SCREENSHOTSERVICE_AUDIO_H
#define SCREENSHOTSERVICE_AUDIO_H

#include "screenshotservice.h"
#include <mmreg.h>

typedef enum {
    AUDIO_NONE = 0,
    AUDIO_SYSTEM,        /* loopback of the default playback device */
    AUDIO_MICROPHONE,
    AUDIO_BOTH           /* system loopback mixed with microphone */
} AudioSource;

typedef struct {
    UINT32 sampleRate;
    UINT32 channels;     /* 1 or 2 after downmixing */
} AudioFormat;

/*
 * Called from an audio worker with interleaved 16-bit PCM. 'frames' is the
 * count per channel. Must not block for long - the audio engine is waiting.
 */
typedef void (*AudioSinkFn)(void *ctx, const BYTE *pcm16, UINT32 frames);

/* Optional native endpoint callbacks used by the post-recording multitrack
   path. The format callback runs before capture threads start; the packet
   callback receives the original WASAPI packet before any conversion or mix. */
typedef void (*AudioNativeFormatFn)(void *ctx, AudioSource source,
                                    const BYTE *formatBytes, UINT32 formatBytesCount,
                                    UINT32 preferredPcmBits, UINT64 clockOrigin100ns);
typedef void (*AudioNativeSinkFn)(void *ctx, AudioSource source,
                                  const BYTE *data, UINT32 frames, BOOL silent,
                                  UINT64 packetQpc100ns);

/*
 * Starts capturing. 'fmt' receives the format the callback will deliver.
 * Native sidecar callbacks preserve each endpoint's negotiated shared-mode
 * format (8-384 kHz where supported). Ordinary combined capture mixes live;
 * multitrack recording defers the combined render until capture has stopped.
 */
BOOL Audio_Start(AudioSource src, AudioFormat *fmt, AudioSinkFn sink, void *ctx,
                 BOOL deferCombinedMix,
                 AudioNativeFormatFn nativeFormat, AudioNativeSinkFn nativeSink,
                 void *nativeCtx);
void Audio_Stop(void);
/* Runtime microphone mute used by the recording control overlay. The native
   mic track receives silence while muted; system loopback is unaffected. */
void Audio_SetMicrophoneMuted(BOOL muted);
BOOL Audio_IsMicrophoneMuted(void);

#endif /* SCREENSHOTSERVICE_AUDIO_H */
