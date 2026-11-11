/*
 * settings.h - user-visible options backed by HKLM\Software\Gallery Inc\ScreenshotSvc.
 * An older settings.ini is read once during migration; the app never writes it.
 */
#ifndef SCREENSHOTSERVICE_SETTINGS_H
#define SCREENSHOTSERVICE_SETTINGS_H

#include "screenshotservice.h"

typedef struct {
    BOOL     copyToClipboard;   /* put every snip on the clipboard        */
    BOOL     saveToDisk;        /* also write a file                      */
    wchar_t  saveFolder[MAX_PATH]; /* empty = C:\Screenshots */
    wchar_t  videoFolder[MAX_PATH];/* empty = C:\Screenshots   */

    BOOL     hookWinShiftS;     /* take over Win+Shift+S                  */
    BOOL     hookPrintScreen;   /* take over PrtScn                       */
    SnipMode defaultMode;       /* mode the overlay opens in              */
    BOOL     prtScnFullscreen;  /* PrtScn grabs everything, no overlay    */

    int      dimPercent;        /* how far the overlay dims the desktop   */
    BOOL     showToast;         /* tray balloon naming the saved file     */

    BOOL     hdrSaveJxr;        /* HDR snips go to .jxr                   */
    BOOL     sdrSaveJxr;        /* save SDR snips as JXR when DXGI supplies a 10-bit surface */
    BOOL     hdrSdrSidecar;     /* ...plus a tone-mapped .png next to it   */
    BOOL     hdrSdrRollOff;     /* keep highlight detail in the 8-bit copy,
                                   at the cost of SDR white landing near 233 */
    int      hdrJxrQuality;     /* 1..100; 100 means lossless, which on busy
                                   content costs around 19 s for a 4K frame  */

    int      recordAudio;       /* AudioSource: 0 none, 1 system, 2 mic, 3 both */
    int      recordFps;
    BOOL     recordRawAudio;    /* bypass the endpoint's effects when capturing */
    wchar_t  recordEncoder[16]; /* auto / h264 / hevc8 / hevc10 / hevc10sdr / AV1 */
    wchar_t  recordEncoderVendor[16]; /* auto / amd / intel / nvidia */
    BOOL     recordHdr;         /* capture 10-bit HDR when the display is in it */
    BOOL     recordWavSidecar;  /* also write the untouched PCM beside the MP4 */
    BOOL     recordZeroCopy;    /* frames stay on the GPU into the encoder;
                                   off forces the older CPU path */
    /* Prefer the graphics card's own encoder (NVENC / AMF / oneVPL) where
       supported. Media Foundation hardware MFT remains the fallback. */
    BOOL     recordDirect;
    int      recordHwPreset;       /* 1..7, fastest .. best */
    int      recordHwBFrames;      /* -1 automatic, 0..5 */
    int      recordHwLookahead;    /* frames, 0..32 */
    BOOL     recordHwAq;            /* spatial adaptive quantisation */
    BOOL     recordIntelVideoMemory;/* use Intel video-memory surfaces when supported */
    BOOL     recordMuxFromWav;  /* ...and build the MP4's AAC from that WAV
                                   afterwards, so nothing encodes audio live */
    BOOL     recordMultiTrack;  /* defaults on: embed AAC mix + system/microphone FLAC */
    BOOL     recordFullscreen;  /* legacy setting; full monitor now uses an explicit overlay action */
    BOOL     recordVsync;       /* pace CFR output to the monitor refresh rate */

    /* Full-monitor recording target. Ask when multiple displays are connected;
       when forcing, reuse the last physical display while it remains connected. */
    BOOL     monitorForceLast;
    wchar_t  monitorTargetPath[MAX_PATH]; /* stable DisplayConfig target path */
    wchar_t  monitorDeviceName[32];       /* last GDI name for diagnostics only */

    /* Where the settings dialog was last left. SETTINGS_POS_UNSET means it has
       never been moved, and it keeps being centred. */
    int      settingsX, settingsY;

    wchar_t  language[16];      /* "auto", or a tag with a langs\<tag>.ini */

    int      idleReleaseMinutes;/* minutes without a snip before the capture
                                   machinery is let go entirely; 0 = never */
    BOOL     forceSdr;          /* ignore advanced colour, capture 8-bit  */
    BOOL     debugLog;          /* append diagnostics to ScreenshotService.log */
    BOOL     forceGdi;          /* skip duplication - diagnostics, and an
                                   escape hatch on displays where it misbehaves */
} Settings;

/* Far outside any real desktop, so it cannot collide with a saved position. */
#define SETTINGS_POS_UNSET  (-1000000)

/* The one live instance. Valid after Settings_Load(). */
extern Settings g_cfg;

void Settings_Load(void);
void Settings_Save(void);

/* Persist active native-encoder state so a crash can disable that backend once. */
void Settings_MarkDirectRecording(const wchar_t *encoder);
BOOL Settings_TakeDirectCrash(wchar_t *encoder, size_t cch);

/* Registry helpers used by the encoder capability cache. A NULL subkey means
   the ScreenshotSvc key itself; cache values live under EncoderCache. */
BOOL Settings_RegGetString(const wchar_t *subKey, const wchar_t *name,
                           const wchar_t *def, wchar_t *out, size_t cch);
int Settings_RegGetInt(const wchar_t *subKey, const wchar_t *name, int def);
BOOL Settings_RegSetString(const wchar_t *subKey, const wchar_t *name,
                           const wchar_t *value);
BOOL Settings_RegSetInt(const wchar_t *subKey, const wchar_t *name, int value);
BOOL Settings_RegistryWritable(void);

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

#endif /* SCREENSHOTSERVICE_SETTINGS_H */
