/*
 * settings.h - the user-visible options, backed by a plain INI.
 *
 * The file lives at %APPDATA%\Nitshot\settings.ini so the exe stays
 * portable-looking but never needs write access next to itself. Every field
 * has a sane default, so a missing or half-written file is not an error.
 */
#ifndef NITSHOT_SETTINGS_H
#define NITSHOT_SETTINGS_H

#include "nitshot.h"

typedef struct {
    BOOL     copyToClipboard;   /* put every snip on the clipboard        */
    BOOL     saveToDisk;        /* also write a file                      */
    wchar_t  saveFolder[MAX_PATH]; /* empty = Pictures\Nitshot */
    wchar_t  videoFolder[MAX_PATH];/* empty = Videos\Nitshot   */

    BOOL     hookWinShiftS;     /* take over Win+Shift+S                  */
    BOOL     hookPrintScreen;   /* take over PrtScn                       */
    SnipMode defaultMode;       /* mode the overlay opens in              */
    BOOL     prtScnFullscreen;  /* PrtScn grabs everything, no overlay    */

    int      dimPercent;        /* how far the overlay dims the desktop   */
    BOOL     showToast;         /* tray balloon naming the saved file     */

    BOOL     hdrSaveJxr;        /* HDR snips go to .jxr                   */
    BOOL     hdrSdrSidecar;     /* ...plus a tone-mapped .png next to it   */
    BOOL     hdrSdrRollOff;     /* keep highlight detail in the 8-bit copy,
                                   at the cost of SDR white landing near 233 */
    int      hdrJxrQuality;     /* 1..100; 100 means lossless, which on busy
                                   content costs around 19 s for a 4K frame  */

    /* The two sound sources, each an audio.h device spec - none, a default,
       or an endpoint id - and its name when chosen, to show while the device
       is unplugged. */
    wchar_t  recordAudioDev[2][256];
    wchar_t  recordAudioName[2][128];
    BOOL     recordAudioMix;    /* two sources become one track, not two */
    int      recordFps;
    BOOL     recordRawAudio;    /* bypass the endpoint's effects when capturing */
    wchar_t  recordEncoder[16]; /* EncoderId token: auto / h264 / hevc8 / hevc10 */
    BOOL     recordHdr;         /* capture 10-bit HDR when the display is in it */
    BOOL     recordWavSidecar;  /* also write the untouched PCM beside the MP4 */
    BOOL     recordZeroCopy;    /* frames stay on the GPU into the encoder;
                                   off forces the older CPU path */
    /* The graphics card's own encoder, driven directly (hwenc.h): NVENC on
       NVIDIA, AMF on AMD. */
    BOOL     recordDirect;      /* on: use it where available              */
    int      recordHwPreset;    /* 1..7, fastest .. best (NVENC's P1..P7)   */
    int      recordHwBFrames;   /* -1 automatic (2 H.264, 3 HEVC), 0..5   */
    int      recordHwLookahead; /* frames, 0..32; 0 = off                   */
    BOOL     recordIntelVideoMemory; /* Intel: textures, not system memory */
    BOOL     recordHwAq;        /* spatial adaptive quantisation; off, as it
                                   takes bits from text                     */
    BOOL     recordMuxFromWav;  /* ...and build the MP4's AAC from that WAV
                                   afterwards, so nothing encodes audio live */
    BOOL     recordKeepSourceWavs; /* ...but when mixing, keep each source's
                                   own WAV rather than deleting it too */

    /* Where the settings dialog was last left. SETTINGS_POS_UNSET means it has
       never been moved, and it keeps being centred. */
    int      settingsX, settingsY;

    wchar_t  language[16];      /* "auto", or a tag with a langs\<tag>.ini */

    int      idleReleaseMinutes;/* minutes without a snip before the capture
                                   machinery is let go entirely; 0 = never */
    BOOL     forceSdr;          /* ignore advanced colour, capture 8-bit  */
    BOOL     debugLog;          /* append diagnostics to Nitshot.log */
    BOOL     forceGdi;          /* skip duplication - diagnostics, and an
                                   escape hatch on displays where it misbehaves */
} Settings;

/* Far outside any real desktop, so it cannot collide with a saved position. */
#define SETTINGS_POS_UNSET  (-1000000)

/* The one live instance. Valid after Settings_Load(). */
extern Settings g_cfg;

void Settings_Load(void);
void Settings_Save(void);

/* Full path of the INI. Other modules keep their own sections in it - the
   encoder probe cache is one - rather than inventing a second file. */
const wchar_t *Settings_IniPath(void);

/* Full path of the folder snips go into, creating it if needed.
   Returns FALSE only if the folder could not be created. */
BOOL Settings_ResolveSaveFolder(wchar_t *out, size_t cch);

/* The same for recordings. Unlike snips, a recording is always written to
   disk, so this does not depend on saveToDisk. */
BOOL Settings_ResolveVideoFolder(wchar_t *out, size_t cch);

/* The same, for a folder taken from a settings snapshot rather than g_cfg.
   The save and record workers resolve their folders this way, so a change
   made in the settings dialog meanwhile can never reach them half-written. */
BOOL Settings_ResolveFolderFrom(const wchar_t *custom, BOOL videos,
                                wchar_t *out, size_t cch);

/* The built-in default for either folder, ignoring any custom one - what a
   setting is compared against to decide whether it is really a custom path. */
BOOL Settings_DefaultFolder(BOOL videos, wchar_t *out, size_t cch);

/* HKCU ..\CurrentVersion\Run entry. */
BOOL Settings_GetAutostart(void);
BOOL Settings_SetAutostart(BOOL enable);

/*
 * A recording through the graphics card's own encoder marks itself in the
 * INI while it runs and clears the mark when it ends. A mark still there at
 * the next start means the process died mid-recording - for an encoder
 * runtime that cannot be tested here, most likely in it - and the direct
 * path is switched off so that recording works again at once.
 * NULL clears the mark.
 */
void Settings_MarkDirectRecording(const wchar_t *encoder);

/* TRUE, with the encoder's name, if a mark was left; clears it either way. */
BOOL Settings_TakeDirectCrash(wchar_t *encoder, size_t cch);

#endif /* NITSHOT_SETTINGS_H */
