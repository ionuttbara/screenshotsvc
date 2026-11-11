#include "classic.h"

typedef struct AviState {
    PAVIFILE file;
    PAVISTREAM rawVideo;
    PAVISTREAM video;
    PAVISTREAM audio;
    HWAVEIN wave;
    WAVEFORMATEX waveFormat;
    WAVEHDR waveHeaders[4];
    BYTE waveBuffers[4][32768];
    CRITICAL_SECTION lock;
    BOOL lockReady;
    volatile LONG stopping;
    LONG audioSamples;
    LONG frameNumber;
    int width, height, fps;
    BYTE *bgr24;
    DWORD bgrBytes;
} AviState;
static AviState g_avi;

static void CALLBACK WaveInProc(HWAVEIN h, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR p2)
{
    AviState *s = (AviState *)instance;
    WAVEHDR *wh = (WAVEHDR *)p1;
    DWORD written, samples;
    LONG start;
    (void)h; (void)p2;
    if (!s || msg != WIM_DATA || !wh) return;
    if (wh->dwBytesRecorded && s->audio && s->lockReady) {
        samples = wh->dwBytesRecorded / s->waveFormat.nBlockAlign;
        EnterCriticalSection(&s->lock);
        start = s->audioSamples;
        if (AVIStreamWrite(s->audio, start, (LONG)samples, wh->lpData,
                           (LONG)wh->dwBytesRecorded, 0, NULL, (LONG *)&written) == AVIERR_OK)
            s->audioSamples += (LONG)samples;
        LeaveCriticalSection(&s->lock);
    }
    if (!InterlockedCompareExchange(&s->stopping, 0, 0) && s->wave) {
        wh->dwBytesRecorded = 0;
        waveInAddBuffer(s->wave, wh, sizeof(*wh));
    }
}

static void AviRelease(AviState *s)
{
    int i;
    if (s->wave) {
        InterlockedExchange(&s->stopping, 1);
        waveInStop(s->wave);
        waveInReset(s->wave);
        for (i = 0; i < 4; ++i) {
            if (s->waveHeaders[i].dwFlags & WHDR_PREPARED)
                waveInUnprepareHeader(s->wave, &s->waveHeaders[i], sizeof(WAVEHDR));
        }
        waveInClose(s->wave); s->wave = NULL;
    }
    if (s->audio) { AVIStreamRelease(s->audio); s->audio = NULL; }
    if (s->video && s->video != s->rawVideo) { AVIStreamRelease(s->video); s->video = NULL; }
    if (s->rawVideo) { AVIStreamRelease(s->rawVideo); s->rawVideo = NULL; }
    if (s->file) { AVIFileRelease(s->file); s->file = NULL; }
    if (s->bgr24) { free(s->bgr24); s->bgr24 = NULL; }
    if (s->lockReady) { DeleteCriticalSection(&s->lock); s->lockReady = FALSE; }
    AVIFileExit();
}

BOOL AVI_Start(HWND owner, const char *path, int width, int height, int fps)
{
    AVISTREAMINFOA si;
    BITMAPINFOHEADER bi;
    AVICOMPRESSOPTIONS opt;
    ICINFO codecInfo;
    DWORD codecs[4];
    HRESULT hr;
    int i;
    ZeroMemory(&g_avi, sizeof(g_avi));
    (void)owner;
    if (width < 1 || height < 1 || !path) return FALSE;
    g_avi.width = width; g_avi.height = height; g_avi.fps = fps > 0 ? fps : 30;
    g_avi.bgrBytes = (DWORD)(((width * 3L + 3L) & ~3L) * height);
    g_avi.bgr24 = (BYTE *)malloc(g_avi.bgrBytes);
    if (!g_avi.bgr24) return FALSE;
    InitializeCriticalSection(&g_avi.lock); g_avi.lockReady = TRUE;
    AVIFileInit();
    hr = AVIFileOpenA(&g_avi.file, path, OF_CREATE | OF_WRITE, NULL);
    if (hr != AVIERR_OK) { AviRelease(&g_avi); return FALSE; }

    ZeroMemory(&si, sizeof(si));
    si.fccType = streamtypeVIDEO; si.fccHandler = 0;
    si.dwScale = 1; si.dwRate = (DWORD)g_avi.fps;
    si.dwSuggestedBufferSize = g_avi.bgrBytes;
    SetRect(&si.rcFrame, 0, 0, width, height);
    hr = AVIFileCreateStreamA(g_avi.file, &g_avi.rawVideo, &si);
    if (hr != AVIERR_OK) { AviRelease(&g_avi); return FALSE; }

    /* Do not show a codec dialog on the recording hotkey: it would steal focus
       from fullscreen games. Prefer common legacy VfW codecs, then fall back to
       uncompressed DIB AVI. Codecs are discovered at runtime on each target OS. */
    codecs[0] = mmioFOURCC('M','S','V','C'); /* Microsoft Video 1 */
    codecs[1] = mmioFOURCC('M','J','P','G');
    codecs[2] = mmioFOURCC('C','V','I','D'); /* Cinepak */
    codecs[3] = mmioFOURCC('I','V','5','0'); /* Indeo 5, if installed */
    ZeroMemory(&bi, sizeof(bi)); bi.biSize = sizeof(bi); bi.biWidth = width; bi.biHeight = height;
    bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB; bi.biSizeImage = g_avi.bgrBytes;
    g_avi.video = NULL;
    for (i = 0; i < 4; ++i) {
        ZeroMemory(&codecInfo, sizeof(codecInfo));
        if (!ICInfo(ICTYPE_VIDEO, codecs[i], &codecInfo)) continue;
        ZeroMemory(&opt, sizeof(opt)); opt.fccType = streamtypeVIDEO; opt.fccHandler = codecs[i];
        opt.dwQuality = ICQUALITY_DEFAULT;
        if (AVIMakeCompressedStream(&g_avi.video, g_avi.rawVideo, &opt, NULL) == AVIERR_OK) {
            if (AVIStreamSetFormat(g_avi.video, 0, &bi, sizeof(bi)) == AVIERR_OK) break;
            AVIStreamRelease(g_avi.video); g_avi.video = NULL;
        }
    }
    if (!g_avi.video) {
        g_avi.video = g_avi.rawVideo;
        if (AVIStreamSetFormat(g_avi.video, 0, &bi, sizeof(bi)) != AVIERR_OK) { AviRelease(&g_avi); return FALSE; }
    }

    /* Legacy audio capture uses the default WinMM recording device. On Windows
       2000/XP, system sound can only be captured if the driver exposes Stereo Mix
       and it is selected as the default recording source. */
    ZeroMemory(&si, sizeof(si));
    g_avi.waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    g_avi.waveFormat.nChannels = 2; g_avi.waveFormat.nSamplesPerSec = 44100;
    g_avi.waveFormat.wBitsPerSample = 16; g_avi.waveFormat.nBlockAlign = 4;
    g_avi.waveFormat.nAvgBytesPerSec = 176400; g_avi.waveFormat.cbSize = 0;
    /* Older built-in microphone devices often support mono PCM only. Query the
       mapper before creating the AVI stream so its format matches the input. */
    if (waveInOpen(NULL, WAVE_MAPPER, &g_avi.waveFormat, 0, 0, WAVE_FORMAT_QUERY) != MMSYSERR_NOERROR) {
        g_avi.waveFormat.nChannels = 1;
        g_avi.waveFormat.nBlockAlign = 2;
        g_avi.waveFormat.nAvgBytesPerSec = 88200;
    }
    si.fccType = streamtypeAUDIO; si.dwScale = g_avi.waveFormat.nBlockAlign; si.dwRate = g_avi.waveFormat.nAvgBytesPerSec;
    si.dwSampleSize = g_avi.waveFormat.nBlockAlign; si.dwSuggestedBufferSize = sizeof(g_avi.waveBuffers[0]);
    if (AVIFileCreateStreamA(g_avi.file, &g_avi.audio, &si) == AVIERR_OK) {
        if (AVIStreamSetFormat(g_avi.audio, 0, &g_avi.waveFormat, sizeof(g_avi.waveFormat)) != AVIERR_OK) {
            AVIStreamRelease(g_avi.audio); g_avi.audio = NULL;
        }
    }
    if (g_avi.audio && waveInOpen(&g_avi.wave, WAVE_MAPPER, &g_avi.waveFormat,
            (DWORD_PTR)WaveInProc, (DWORD_PTR)&g_avi, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
        for (i = 0; i < 4; ++i) {
            ZeroMemory(&g_avi.waveHeaders[i], sizeof(WAVEHDR));
            g_avi.waveHeaders[i].lpData = (LPSTR)g_avi.waveBuffers[i];
            g_avi.waveHeaders[i].dwBufferLength = sizeof(g_avi.waveBuffers[i]);
            if (waveInPrepareHeader(g_avi.wave, &g_avi.waveHeaders[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
                waveInAddBuffer(g_avi.wave, &g_avi.waveHeaders[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                InterlockedExchange(&g_avi.stopping, 1); break;
            }
        }
        if (i == 4) waveInStart(g_avi.wave);
        else { waveInReset(g_avi.wave); }
    } else if (g_avi.wave) { waveInClose(g_avi.wave); g_avi.wave = NULL; }
    InterlockedExchange(&g_avi.stopping, 0);
    return TRUE;
}

BOOL AVI_WriteFrame(const BYTE *bgra, int width, int height)
{
    LONG y, x;
    DWORD row = (DWORD)(((width * 3L + 3L) & ~3L));
    LONG samples, bytes;
    HRESULT hr;
    if (!g_avi.file || !g_avi.video || !bgra || width != g_avi.width || height != g_avi.height) return FALSE;
    for (y = 0; y < height; ++y) {
        BYTE *dst = g_avi.bgr24 + (size_t)(height - 1 - y) * row;
        const BYTE *src = bgra + (size_t)y * (size_t)width * 4U;
        for (x = 0; x < width; ++x) { dst[x * 3] = src[x * 4]; dst[x * 3 + 1] = src[x * 4 + 1]; dst[x * 3 + 2] = src[x * 4 + 2]; }
        memset(dst + width * 3, 0, row - width * 3U);
    }
    EnterCriticalSection(&g_avi.lock);
    hr = AVIStreamWrite(g_avi.video, g_avi.frameNumber, 1, g_avi.bgr24,
                        (LONG)g_avi.bgrBytes, (g_avi.video == g_avi.rawVideo ? AVIIF_KEYFRAME : 0), &samples, &bytes);
    if (hr == AVIERR_OK) ++g_avi.frameNumber;
    LeaveCriticalSection(&g_avi.lock);
    return hr == AVIERR_OK;
}

void AVI_Stop(void)
{
    if (!g_avi.file && !g_avi.bgr24) return;
    AviRelease(&g_avi);
    ZeroMemory(&g_avi, sizeof(g_avi));
}
BOOL AVI_IsRecording(void) { return g_avi.file != NULL; }
