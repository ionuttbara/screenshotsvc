/*
 * main.c - ScreenshotService.
 *
 * A tray-resident host: it owns the hotkey hook, the tray icon and its menu,
 * and hands every capture request to the capture/overlay code. It deliberately
 * has no visible window of its own - the only UI in normal use is the overlay
 * that appears when a hotkey fires.
 */

#include "screenshotservice.h"
#include "resource.h"
#include "settings.h"
#include "hotkey.h"
#include "theme.h"
#include "capture.h"
#include "save.h"
#include "overlay.h"
#include "settings_ui.h"
#include "record.h"
#include "recovery.h"
#include "encoder.h"
#include "lang.h"
#include "recordbar.h"
#include "log.h"
#include "colorpicker.h"
#include "annotate.h"
#include "livepen.h"
#include "monitorpicker.h"
#include "crash.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <string.h>

#define WM_TRAYICON     (WM_APP + 1)
#define WM_APP_PREWARM  (WM_APP + 20)
#define WM_APP_SMOKE_RECORD (WM_APP + 21)
#define TIMER_HOOKHEAL  1
#define TIMER_IDLETRIM  2     /* give back the grab's memory after a snip     */
#define TIMER_DEEPIDLE  3     /* let the capture machinery go after a while   */
#define TIMER_SMOKE_STOP 4
#define TIMER_SMOKE_START 5
/*
 * A low-level hook can stop working in two ways, neither of which can be
 * detected: Windows drops one whose callback ever failed to answer within
 * LowLevelHooksTimeout, and anything installing a hook after us is called
 * ahead of us and can swallow the keys first. Reinstalling costs two API calls
 * and bounds how long the hotkeys can be silently dead.
 *
 * This is the safety net, not the fix. The failure that prompted it was the
 * first kind and was self-inflicted: a 4K HDR snip spent seconds in the
 * encoder on this thread, the hook could not be dispatched while that ran, and
 * Win+Shift+S went to the shell - which opened Snip & Sketch, the one thing
 * this program exists to prevent. Saving runs on a worker thread because of
 * it; see Save_SnipAsync.
 */
#define HOOKHEAL_MS     10000

/*
 * Memory while waiting. A grab leaves a full desktop of CPU-readable memory
 * behind - 66 MB at 4K HDR - which the display driver frees only some time
 * after it is released, so it is given back twice, a couple of seconds apart.
 * After IdleReleaseMinutes without a snip the whole capture machinery goes too;
 * the next snip then rebuilds it, which costs about a tenth of a second once.
 * Measured at 4K HDR: 115 MB idle after a snip before this, 51 MB with the
 * trim, 17 MB after the long idle.
 */
#define IDLETRIM_MS     2000

#define IDM_SNIP        0x100
#define IDM_FULLSCREEN  0x101
#define IDM_OPENFOLDER  0x102
#define IDM_AUTOSTART   0x103
#define IDM_SETTINGS    0x104
#define IDM_ABOUT       0x105
#define IDM_EXIT        0x106
#define IDM_RECORDTOGGLE 0x107
#define IDM_COLORPICKER  0x108

static HWND      g_host;
static HICON     g_icon;
static UINT      g_msgTaskbarCreated;
static BOOL      g_trayShown;
static wchar_t   g_lastFile[MAX_PATH];   /* what the last balloon points at */
static BOOL      g_snipBusy;             /* a snip, its overlay, is under way */
static BOOL      g_exitWhenRecorded;      /* defer Exit until post-processing completes */
static int       g_idleTrims;
static BOOL      g_cliSmokeTest;
static BOOL      g_cliSmokeMftOnly;
static BOOL      g_cliSmokeNativeOnly;
static DWORD     g_cliSmokeSeconds = 4;
static int       g_cliExitCode = 2;
static wchar_t   g_directCrashed[64];

/* Restarts both idle timers; called whenever capture has just been used. */
static void App_ArmIdle(void)
{
    g_idleTrims = 0;
    SetTimer(g_host, TIMER_IDLETRIM, IDLETRIM_MS, NULL);
    if (g_cfg.idleReleaseMinutes > 0)
        SetTimer(g_host, TIMER_DEEPIDLE, (UINT)g_cfg.idleReleaseMinutes * 60000u, NULL);
    else
        KillTimer(g_host, TIMER_DEEPIDLE);
}

/* ------------------------------- automated recording smoke test */
typedef struct { RECT rect; BOOL found; } SmokePrimary;

static BOOL CALLBACK SmokeFindPrimary(HMONITOR mon, HDC dc, LPRECT rc, LPARAM data)
{
    MONITORINFO mi;
    SmokePrimary *out = (SmokePrimary *)data;
    UNREFERENCED_PARAMETER(dc);
    UNREFERENCED_PARAMETER(rc);
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
        out->rect = mi.rcMonitor;
        out->found = TRUE;
        return FALSE;
    }
    return TRUE;
}

static BOOL SmokePrimaryRect(RECT *out)
{
    SmokePrimary p;
    ZeroMemory(&p, sizeof(p));
    EnumDisplayMonitors(NULL, NULL, SmokeFindPrimary, (LPARAM)&p);
    if (!p.found) return FALSE;
    *out = p.rect;
    return TRUE;
}

static void SmokeWaitForActive(void)
{
    if (g_cliSmokeTest)
        SetTimer(g_host, TIMER_SMOKE_START, 200, NULL);
}

static BOOL SmokeValidateMp4(const wchar_t *path, ULONGLONG *fileBytes)
{
    HANDLE f;
    LARGE_INTEGER size, pos;
    BOOL ftyp = FALSE, moov = FALSE, mdat = FALSE;
    unsigned boxes = 0;

    if (fileBytes) *fileBytes = 0;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 24) {
        CloseHandle(f);
        return FALSE;
    }
    if (fileBytes) *fileBytes = (ULONGLONG)size.QuadPart;
    pos.QuadPart = 0;
    while (pos.QuadPart + 8 <= size.QuadPart && boxes++ < 1000) {
        BYTE head[16];
        DWORD got = 0;
        ULONGLONG boxSize;
        DWORD header = 8;
        if (!SetFilePointerEx(f, pos, NULL, FILE_BEGIN) ||
            !ReadFile(f, head, 8, &got, NULL) || got != 8)
            break;
        boxSize = ((ULONGLONG)head[0] << 24) | ((ULONGLONG)head[1] << 16) |
                  ((ULONGLONG)head[2] << 8) | (ULONGLONG)head[3];
        if (boxSize == 1) {
            unsigned i;
            if (!ReadFile(f, head + 8, 8, &got, NULL) || got != 8)
                break;
            boxSize = 0;
            for (i = 8; i < 16; ++i) boxSize = (boxSize << 8) | head[i];
            header = 16;
        } else if (boxSize == 0) {
            boxSize = (ULONGLONG)(size.QuadPart - pos.QuadPart);
        }
        if (boxSize < header || boxSize > (ULONGLONG)(size.QuadPart - pos.QuadPart))
            break;
        if (memcmp(head + 4, "ftyp", 4) == 0 && boxSize >= 16) ftyp = TRUE;
        if (memcmp(head + 4, "moov", 4) == 0 && boxSize > header) moov = TRUE;
        if (memcmp(head + 4, "mdat", 4) == 0 && boxSize > header) mdat = TRUE;
        pos.QuadPart += (LONGLONG)boxSize;
    }
    CloseHandle(f);
    return ftyp && moov && mdat;
}

/* ------------------------------------------------------------------ tray */

static void TrayAdd(void)
{
    NOTIFYICONDATAW nid;

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_host;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = g_icon;
    StringCchPrintfW(nid.szTip, ARRAYSIZE(nid.szTip), L"%s - %s",
                   APP_TITLE_W, Lang_Str(STR_TRAY_TIP));

    g_trayShown = Shell_NotifyIconW(NIM_ADD, &nid);
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid;

    if (!g_trayShown)
        return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_host;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayShown = FALSE;
}

static void TrayBalloon(const wchar_t *title, const wchar_t *text)
{
    NOTIFYICONDATAW nid;

    if (!g_trayShown || !g_cfg.showToast)
        return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = g_host;
    nid.uID         = 1;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_NONE | NIIF_NOSOUND;
    StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    StringCchCopyW(nid.szInfo,      ARRAYSIZE(nid.szInfo),      text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ---------------------------------------------------------- recording */

static void App_StartRecording(const RECT *screenRect, BOOL makeGif)
{
    RecordOptions opt;
    /* The selection overlay passes the exact chosen rectangle. Full-screen
       recording is an explicit toolbar action that passes a monitor rect;
       never silently replace a selected region using a legacy preference. */
    RECT region = *screenRect;

    ZeroMemory(&opt, sizeof(opt));
    opt.region = region;
    opt.audio  = makeGif ? AUDIO_NONE : (AudioSource)g_cfg.recordAudio;
    opt.fps    = makeGif ? 15 : g_cfg.recordFps;
    opt.makeGif = makeGif;
    opt.syncToDisplay = g_cfg.recordVsync;

    Log_Printf(L"record: starting %ldx%ld at %d fps%s, audio=%d, output=%s",
               region.right - region.left,
               region.bottom - region.top, opt.fps,
               opt.syncToDisplay ? L" (monitor refresh pacing)" : L"", (int)opt.audio,
               makeGif ? L"GIF (max 15 seconds)" : L"MP4");

    /*
     * The still-capture machinery keeps a duplication open on every output for
     * the whole run, and DXGI will not hand a second one for the same output to
     * the same process - it fails the recorder's DuplicateOutput with
     * E_INVALIDARG. Letting go of it for the duration is the price of
     * recording; the next snip re-creates it lazily.
     */
    Capture_Shutdown();

    if (!Record_Start(g_host, WM_BSNIP_RECORDED, &opt)) {
        Capture_Init();
        TrayBalloon(APP_TITLE_W, Lang_Str(STR_MSG_RECORD_START));
        return;
    }
    /* The bar is created here rather than by the worker: it is a window, and
       windows belong to the thread that pumps messages for them. */
    Hotkey_SetLivePenActive(LivePen_Start(&region));
    RecordBar_Show(g_host, &region, WM_BSNIP_STOPREC,
                   !makeGif && (g_cfg.recordAudio == AUDIO_MICROPHONE ||
                                g_cfg.recordAudio == AUDIO_BOTH));
}

static void App_Recorded(RecordResult *res)
{
    wchar_t text[512];

    Hotkey_SetLivePenActive(FALSE);
    LivePen_Stop();
    RecordBar_Hide();

    /* If a card-native encoder dies before its first packet, retry the same
       settings immediately with the tested Media Foundation hardware MFT.
       Do this before reopening Desktop Duplication, and scope the setting
       override to Record_Start so the user's preference is not changed. */
    if (res && res->retryWithoutDirect && g_cliSmokeNativeOnly) {
        Log_Printf(L"smoke: native encoder failed before its first packet; native-only test fails without MFT retry");
        g_cliExitCode = 6;
        Record_FreeResult(res);
        DestroyWindow(g_host);
        return;
    }

    if (res && res->retryWithoutDirect && !g_exitWhenRecorded) {
        RecordOptions retry = res->retryOptions;
        BOOL savedDirect = g_cfg.recordDirect;
        wchar_t failedName[32];
        StringCchCopyW(failedName, ARRAYSIZE(failedName),
                       res->directName[0] ? res->directName : L"native encoder");
        Log_Printf(L"record: retrying %s recording via Media Foundation hardware MFT",
                   failedName);
        if (!g_cliSmokeTest) {
            wchar_t notice[512];
            Lang_Format(notice, ARRAYSIZE(notice), STR_MSG_DIRECT_FALLBACK, failedName);
            TrayBalloon(APP_TITLE_W, notice);
        }
        Record_FreeResult(res);
        g_cfg.recordDirect = FALSE;
        if (!Record_Start(g_host, WM_BSNIP_RECORDED, &retry)) {
            g_cfg.recordDirect = savedDirect;
            Log_Printf(L"record: could not start Media Foundation retry");
            if (g_cliSmokeTest) {
                g_cliExitCode = 3;
                DestroyWindow(g_host);
            } else {
                if (!g_cfg.forceGdi) Capture_Init();
                App_ArmIdle();
                TrayBalloon(APP_TITLE_W, Lang_Str(STR_MSG_RECORD_START));
            }
            return;
        }
        g_cfg.recordDirect = savedDirect;
        if (!g_cliSmokeTest)
            Hotkey_SetLivePenActive(LivePen_Start(&retry.region));
        if (!g_cliSmokeTest)
            RecordBar_Show(g_host, &retry.region, WM_BSNIP_STOPREC,
                           retry.audio == AUDIO_MICROPHONE || retry.audio == AUDIO_BOTH);
        SmokeWaitForActive();
        return;
    }

    if (g_cliSmokeTest) {
        if (res) {
            ULONGLONG size = 0;
            BOOL mp4ok = res->ok && res->path[0] && SmokeValidateMp4(res->path, &size);
            BOOL backendok = !g_cliSmokeNativeOnly || res->usedDirect;
            g_cliExitCode = mp4ok && backendok ? 0 : (backendok ? 2 : 6);
            Log_Printf(L"smoke: result ok=%d used_native=%d mp4_valid=%d file=%s size=%llu exit=%d error=%s",
                       (int)res->ok, (int)res->usedDirect, (int)mp4ok, res->path,
                       (unsigned long long)size, g_cliExitCode, res->error);
            Record_FreeResult(res);
        } else {
            g_cliExitCode = 2;
            Log_Printf(L"smoke: recorder returned no result");
        }
        KillTimer(g_host, TIMER_SMOKE_START);
        KillTimer(g_host, TIMER_SMOKE_STOP);
        DestroyWindow(g_host);
        return;
    }

    /* Warm capture again only when we are staying in the tray app. During an
       Exit request, let the worker finish first and then tear down normally. */
    if (!g_exitWhenRecorded) {
        if (!g_cfg.forceGdi) Capture_Init();
        App_ArmIdle();
    }
    if (!res) {
        if (g_exitWhenRecorded) DestroyWindow(g_host);
        return;
    }

    Log_Printf(L"record: finished ok=%d %lus %s %s", (int)res->ok,
               (unsigned long)res->seconds, res->path, res->error);

    if (res->ok) {
        StringCchCopyW(g_lastFile, ARRAYSIZE(g_lastFile), res->path);
        wchar_t len[16], head[400];
        StringCchPrintfW(len, ARRAYSIZE(len), L"%lu:%02lu",
                         (unsigned long)(res->seconds / 60),
                         (unsigned long)(res->seconds % 60));
        Lang_Format(head, ARRAYSIZE(head), STR_TOAST_RECORDED, len,
                    PathFindFileNameW(res->path));
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s%s%s%s", head,
                         res->wavPath[0] ? Lang_Str(STR_TOAST_RECORD_WAV) : L"",
                         res->error[0] ? L"\n" : L"", res->error);
    } else {
        Lang_Format(text, ARRAYSIZE(text), STR_TOAST_RECORD_FAIL, res->error);
    }
    if (!g_exitWhenRecorded)
        TrayBalloon(APP_TITLE_W, text);
    Record_FreeResult(res);
    if (g_exitWhenRecorded)
        DestroyWindow(g_host);
}

/* ------------------------------------------------------------- the action */

/*
 * Freeze the desktop, let the user pick a region, then publish the result.
 * PrtScn can be configured to skip the overlay entirely and take everything.
 */
static void App_SnipOnce(TriggerSource src);

/* Every way out of a snip - taken, cancelled, failed, a recording stopped -
   ends here, so the idle timers always restart from the last use. */
static void App_Snip(TriggerSource src)
{
    if (g_snipBusy)
        return;                      /* a second hotkey while the overlay is up */
    g_snipBusy = TRUE;
    App_SnipOnce(src);
    g_snipBusy = FALSE;
    App_ArmIdle();
}

static void App_SnipOnce(TriggerSource src)
{
    SnipImage     frozen, shot;
    SnipSelection sel;
    RECT          bounds;
    LARGE_INTEGER t0, t1, freq;
    double        grabMs;
    BOOL          wholeScreen;
    SnipMode      startMode;
    UINT32        w, h;

    /* While a recording runs the hotkey is the way to end it - starting a
       second capture on top would be the wrong thing to want. Busy rather
       than active, so a press during setup or while the file is being
       finalised lands here too instead of opening the overlay. */
    if (src == TRIGGER_COLOR_PICKER) {
        COLORREF color; wchar_t description[160];
        if (ColorPicker_Run(g_host, &color, description, ARRAYSIZE(description)))
            TrayBalloon(APP_TITLE_W, description);
        return;
    }
    if (Record_IsBusy()) {
        Log_Printf(L"snip: trigger=%d while recording -> stop", (int)src);
        RecordBar_SetSaving(0);
        Record_RequestStop();
        return;
    }
    if (src == TRIGGER_RECORD_FULLSCREEN) {
        RECT region;
        /* With multiple displays Alt+PrtScn asks which physical monitor to
           record. A single connected display is implicit, unless Force-last
           must recover from a saved physical target that disappeared. */
        if (MonitorPicker_Select(g_host, &region)) {
            Log_Printf(L"hotkey: Alt+PrtScn starts full-monitor recording %ldx%ld",
                       region.right-region.left, region.bottom-region.top);
            App_StartRecording(&region, FALSE);
        } else {
            Log_Printf(L"hotkey: Alt+PrtScn monitor selection cancelled or unavailable");
        }
        return;
    }

    Log_Printf(L"snip: trigger=%d", (int)src);

    /* GetTickCount's 15 ms granularity cannot see this at all - the warm
       duplication path is well under one frame. */
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    if (!Capture_VirtualDesktop(&frozen, &bounds)) {
        Log_Printf(L"snip: capture FAILED");
        TrayBalloon(APP_TITLE_W, Lang_Str(STR_MSG_CAPTURE_FAILED));
        return;
    }
    /* Make the pointer part of the saved SDR screenshot, not just the live desktop. */
    Capture_DrawCursor(&frozen, &bounds);
    QueryPerformanceCounter(&t1);
    grabMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    /* PrintScreen always opens the preview so the user can draw/erase before
       committing the crop. A full-screen preference changes the initial mode,
       not whether the preview and its tools are shown. The tray's explicit
       Fullscreen command remains a direct capture. */
    wholeScreen = (src == TRIGGER_FULLSCREEN);
    startMode = (src == TRIGGER_PRINTSCREEN && g_cfg.prtScnFullscreen)
              ? SNIP_FULLSCREEN : g_cfg.defaultMode;

    ZeroMemory(&sel, sizeof(sel));
    if (wholeScreen) {
        shot = frozen;                       /* borrowed, not owned */
    } else {
        /* Keep eating the hotkeys while the overlay is up, but act on none of
           them: letting them through would hand Win+Shift+S to the shell. */
        Hotkey_SetSuppressed(TRUE);
        Overlay_Run(&frozen, &bounds, startMode,
                    src == TRIGGER_RECORD_SELECTION, &sel);
        Hotkey_SetSuppressed(FALSE);

        if (!sel.taken) {
            Log_Printf(L"snip: cancelled");
            Overlay_FreeSelection(&sel);
            Snip_FreeImage(&frozen);
            return;
        }
        if (sel.record) {
            RECT screen = sel.rect;
            BOOL makeGif = sel.gif;
            OffsetRect(&screen, bounds.left, bounds.top);
            /* The toolbar's full-monitor button uses the monitor beneath the
               pointer, rather than trying to record the virtual desktop as a
               single DXGI output. */
            if (sel.mode == SNIP_FULLSCREEN) {
                RECT picked;
                /* Toolbar full-monitor record (or full-screen GIF) uses the
                   same themed picker and remembered-display policy as
                   Alt+PrtScn. Region recording remains untouched. */
                if (!MonitorPicker_Select(g_host, &picked)) {
                    Overlay_FreeSelection(&sel);
                    Snip_FreeImage(&frozen);
                    Log_Printf(L"record: full-monitor selection cancelled");
                    return;
                }
                screen = picked;
            }
            Overlay_FreeSelection(&sel);
            Snip_FreeImage(&frozen);
            App_StartRecording(&screen, makeGif);
            return;
        }

        /* Remember the mode for next time, the way the Snipping Tool does.
           Fullscreen is a one-off action, not a mode worth returning to. */
        if (sel.mode != SNIP_FULLSCREEN)
            g_cfg.defaultMode = sel.mode;

        if (!Overlay_ExtractSelection(&frozen, &sel, &shot)) {
            Overlay_FreeSelection(&sel);
            Snip_FreeImage(&frozen);
            return;
        }
    }

    /* In the whole-screen case 'shot' is the frozen image itself; hand that
       over rather than copying eight megapixels to say the same thing. */
    if (wholeScreen)
        ZeroMemory(&frozen, sizeof(frozen));

    w = shot.width;
    h = shot.height;
    Overlay_FreeSelection(&sel);
    Snip_FreeImage(&frozen);

    Log_Printf(L"snip: %ux%u via %s, grab %.1f ms - encoding in the background",
               w, h, Capture_LastMethod(), grabMs);

    /* Captures are saved immediately. Do not open the annotation/editor window
       after any screenshot trigger; the selection overlay above is retained
       only when needed to choose a region. */

    /*
     * Everything expensive happens on a worker from here. The message loop has
     * to stay free: the keyboard hook is dispatched on this thread, and a UI
     * thread busy in the encoder means Windows times the hook out and hands
     * Win+Shift+S to the shell instead.
     */
    if (!Save_SnipAsync(g_host, WM_BSNIP_SAVED, &shot))
        TrayBalloon(APP_TITLE_W, Lang_Str(STR_MSG_SAVE_FAILED));

}

/* The worker is finished; 'res' is ours to report and free. */
static void App_SnipSaved(SaveResult *res)
{
    wchar_t text[512];
    wchar_t tail[192];

    if (!res)
        return;

    Log_Printf(L"snip: saved clip=%d disk=%d sidecar=%d hdr=%d encode %ld ms %s",
               (int)res->clipboardOk, (int)res->diskOk, (int)res->sidecarOk,
               (int)res->hdr, res->encodeMs, res->diskOk ? res->path : L"");

    StringCchCopyW(g_lastFile, ARRAYSIZE(g_lastFile), res->diskOk ? res->path : L"");

    /* Both destinations are optional, so the balloon has to say which of them
       actually happened rather than claiming a saved file that is not there. */
    if (res->diskOk && res->clipboardOk)
        Lang_Format(tail, ARRAYSIZE(tail), STR_TOAST_COPIED_SAVED,
                    PathFindFileNameW(res->path));
    else if (res->diskOk)
        Lang_Format(tail, ARRAYSIZE(tail), STR_TOAST_SAVED,
                    PathFindFileNameW(res->path));
    else if (res->clipboardOk)
        StringCchCopyW(tail, ARRAYSIZE(tail), Lang_Str(STR_TOAST_COPIED));
    else
        StringCchCopyW(tail, ARRAYSIZE(tail), Lang_Str(STR_TOAST_NOTHING));

    {
        wchar_t ms[24], encoded[64];
        StringCchPrintfW(ms, ARRAYSIZE(ms), L"%ld", res->encodeMs);
        Lang_Format(encoded, ARRAYSIZE(encoded), STR_TOAST_ENCODE, ms);
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s%s%s", tail,
                         res->hdr ? Lang_Str(res->sidecarOk ? STR_TOAST_HDR_SDR
                                                            : STR_TOAST_HDR)
                                  : L"",
                         encoded);
    }
    TrayBalloon(APP_TITLE_W, text);
    Save_FreeResult(res);
}

static void App_OpenFolder(void)
{
    wchar_t dir[MAX_PATH];

    if (Settings_ResolveSaveFolder(dir, ARRAYSIZE(dir)))
        ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
    else
        MessageBoxW(NULL, Lang_Str(STR_MSG_FOLDER_FAILED),
                    APP_TITLE_W, MB_OK | MB_ICONWARNING);
}

/* Clicking the balloon shows the file it just named. */
static void App_RevealLastFile(void)
{
    wchar_t args[MAX_PATH + 16];

    if (!g_lastFile[0]) {
        App_OpenFolder();
        return;
    }
    StringCchPrintfW(args, ARRAYSIZE(args), L"/select,\"%s\"", g_lastFile);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

static void App_About(void)
{
    wchar_t dir[MAX_PATH] = L"(unavailable)";
    wchar_t text[768];

    wchar_t body[640];

    Settings_ResolveSaveFolder(dir, ARRAYSIZE(dir));
    Lang_Format(body, ARRAYSIZE(body), STR_ABOUT_BODY, dir);
    /* The name and version line is product identity, not prose, and stays put
       in every language. */
    StringCchPrintfW(text, ARRAYSIZE(text), L"%s  %s\n%s",
                     APP_TITLE_W, APP_VER_WSTRING, body);
    MessageBoxW(NULL, text, APP_TITLE_W, MB_OK | MB_ICONINFORMATION);
}

static void ShowTrayMenu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;

    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, IDM_SNIP,       Lang_Str(STR_MENU_SNIP));
    AppendMenuW(menu, MF_STRING, IDM_FULLSCREEN, Lang_Str(STR_MENU_FULLSCREEN));
    AppendMenuW(menu, MF_STRING, IDM_RECORDTOGGLE, L"Start/stop full-monitor video  (Alt+PrtScn)");
    AppendMenuW(menu, MF_STRING, IDM_COLORPICKER,  L"Color picker  (Win+Shift+X)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_OPENFOLDER, Lang_Str(STR_MENU_OPENFOLDER));
    AppendMenuW(menu, MF_STRING | (Settings_GetAutostart() ? MF_CHECKED : 0u),
                IDM_AUTOSTART, Lang_Str(STR_MENU_AUTOSTART));
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, Lang_Str(STR_MENU_SETTINGS));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, Lang_Str(STR_MENU_ABOUT));
    AppendMenuW(menu, MF_STRING, IDM_EXIT,  Lang_Str(STR_MENU_EXIT));
    SetMenuDefaultItem(menu, IDM_SNIP, FALSE);

    /* The menu only dismisses on an outside click while its owner is the
       foreground window, and a tray owner never is by itself. */
    GetCursorPos(&pt);
    SetForegroundWindow(g_host);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_host, NULL);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* ---------------------------------------------------------- window proc */

static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        g_trayShown = FALSE;      /* Explorer restarted; our icon went with it */
        TrayAdd();
        return 0;
    }

    switch (msg) {
    case WM_BSNIP_TRIGGER:
        App_Snip((TriggerSource)wp);
        return 0;

    case WM_BSNIP_SAVED:
        App_SnipSaved((SaveResult *)lp);
        return 0;

    case WM_BSNIP_RECORDED:
        App_Recorded((RecordResult *)lp);
        return 0;

    case WM_BSNIP_STOPREC:
        if (Record_IsBusy()) {
            RecordBar_SetSaving(0);
            Record_RequestStop();
        }
        return 0;

    case WM_BSNIP_RECORD_PROGRESS:
        RecordBar_SetSaving((int)wp);
        return 0;

    case WM_BSNIP_PAUSEREC:
        Record_TogglePause();
        RecordBar_SetPaused(Record_IsPaused());
        return 0;

    case WM_BSNIP_MICMUTE:
        Audio_SetMicrophoneMuted(!Audio_IsMicrophoneMuted());
        RecordBar_SetMicMuted(Audio_IsMicrophoneMuted());
        return 0;

    case WM_BSNIP_PEN:
        LivePen_Toggle();
        Hotkey_SetLivePenDrawing(LivePen_IsEnabled());
        RecordBar_SetPenEnabled(LivePen_IsEnabled());
        return 0;

    case WM_BSNIP_PEN_OFF:
        Hotkey_SetLivePenDrawing(FALSE);
        RecordBar_SetPenEnabled(FALSE);
        return 0;

    case WM_BSNIP_PEN_CLEAR:
        LivePen_ClearStrokes();
        return 0;

    case WM_BSNIP_PEN_MOUSEMOVE: {
        POINT p; p.x = (LONG)(LONG_PTR)wp; p.y = (LONG)(LONG_PTR)lp;
        LivePen_MouseMoveScreen(p);
        return 0;
    }
    case WM_BSNIP_PEN_MOUSEDOWN: {
        POINT p; p.x = (LONG)(LONG_PTR)wp; p.y = (LONG)(LONG_PTR)lp;
        LivePen_MouseDownScreen(p);
        return 0;
    }
    case WM_BSNIP_PEN_MOUSEUP: {
        POINT p; p.x = (LONG)(LONG_PTR)wp; p.y = (LONG)(LONG_PTR)lp;
        LivePen_MouseUpScreen(p);
        return 0;
    }
    case WM_BSNIP_PEN_MOUSELEAVE:
        LivePen_MouseLeaveRegion();
        return 0;

    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK:      App_Snip(TRIGGER_TRAY);  return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:        ShowTrayMenu();          return 0;
        case NIN_BALLOONUSERCLICK:  App_RevealLastFile();    return 0;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SNIP:       App_Snip(TRIGGER_TRAY);       break;
        case IDM_FULLSCREEN: App_Snip(TRIGGER_FULLSCREEN); break;
        case IDM_RECORDTOGGLE: App_Snip(TRIGGER_RECORD_FULLSCREEN); break;
        case IDM_COLORPICKER: App_Snip(TRIGGER_COLOR_PICKER); break;
        case IDM_OPENFOLDER: App_OpenFolder();       break;
        case IDM_AUTOSTART:  Settings_SetAutostart(!Settings_GetAutostart()); break;
        case IDM_SETTINGS:   SettingsUI_Show(NULL);  break;
        case IDM_ABOUT:      App_About();            break;
        case IDM_EXIT:
            if (Record_IsBusy()) {
                /* Keep the message loop alive so Saving recording NN% remains
                   responsive and the post-processing worker can finish. */
                g_exitWhenRecorded = TRUE;
                RecordBar_SetSaving(0);
                Record_RequestStop();
            } else {
                DestroyWindow(hwnd);
            }
            break;
        default:                                     break;
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_HOOKHEAL) {
            Hotkey_Refresh();
        } else if (wp == TIMER_IDLETRIM) {
            /* Timers are delivered inside the overlay's own message loop as
               well, and the overlay draws with the capture device: never
               touch it mid-snip. The timer stays armed and tries again. */
            if (g_snipBusy)
                return 0;
            if (!Record_IsBusy())
                Capture_Idle();
            if (++g_idleTrims >= 2)
                KillTimer(hwnd, TIMER_IDLETRIM);
        } else if (wp == TIMER_SMOKE_START) {
            if (Record_IsActive()) {
                KillTimer(hwnd, TIMER_SMOKE_START);
                Log_Printf(L"smoke: recording loop active, capturing for %lu s",
                           (unsigned long)g_cliSmokeSeconds);
                SetTimer(hwnd, TIMER_SMOKE_STOP, g_cliSmokeSeconds * 1000u, NULL);
            }
        } else if (wp == TIMER_SMOKE_STOP) {
            KillTimer(hwnd, TIMER_SMOKE_STOP);
            Log_Printf(L"smoke: requesting stop after active capture interval");
            Record_RequestStop();
        } else if (wp == TIMER_DEEPIDLE) {
            if (g_snipBusy)
                return 0;
            KillTimer(hwnd, TIMER_DEEPIDLE);
            if (!Record_IsBusy()) {
                /* The overlay holds a reference to the capture device; both
                   have to let go for the device to be freed. */
                Overlay_Idle();
                Capture_Shutdown();
                Log_Printf(L"idle: %d min without a snip, capture released",
                           g_cfg.idleReleaseMinutes);
            }
        }
        return 0;

    case WM_APP_SMOKE_RECORD: {
        RECT region;
        RecordOptions opt;
        Log_Printf(L"smoke: starting %lu-second primary-monitor MP4 test",
                   (unsigned long)g_cliSmokeSeconds);
        if (!SmokePrimaryRect(&region)) {
            Log_Printf(L"smoke: no primary display was found");
            g_cliExitCode = 4;
            DestroyWindow(hwnd);
            return 0;
        }
        ZeroMemory(&opt, sizeof(opt));
        opt.region = region;
        opt.audio = AUDIO_NONE;
        opt.fps = 15;
        opt.syncToDisplay = FALSE;
        opt.makeGif = FALSE;
        Capture_Shutdown();
        {
            BOOL savedDirect = g_cfg.recordDirect;
            if (g_cliSmokeMftOnly)
                g_cfg.recordDirect = FALSE;
            else if (g_cliSmokeNativeOnly)
                g_cfg.recordDirect = TRUE;
            if (!Record_Start(hwnd, WM_BSNIP_RECORDED, &opt)) {
                g_cfg.recordDirect = savedDirect;
                Log_Printf(L"smoke: Record_Start failed");
                g_cliExitCode = 3;
                DestroyWindow(hwnd);
                return 0;
            }
            g_cfg.recordDirect = savedDirect;
        }
        Log_Printf(L"smoke: requested backend=%s",
                   g_cliSmokeMftOnly ? L"MFT-only" : g_cliSmokeNativeOnly ? L"native-only" : L"configured/auto-fallback");
        SmokeWaitForActive();
        return 0;
    }

    case WM_APP_PREWARM:
        Overlay_Prewarm();
        return 0;

    case WM_DISPLAYCHANGE:
        /* Desktop Duplication is invalidated by mode changes. Ask the worker
           to reacquire it and scale new frames into the original video canvas. */
        if (Record_IsBusy())
            Record_NotifyDisplayChange();
        else
            Capture_Shutdown();
        return 0;

    case WM_SETTINGCHANGE:
        if (Theme_IsColorSchemeChange(msg, lp))
            Theme_SetDark(Theme_SystemPrefersDark());
        return 0;

    case WM_DESTROY:
        Record_Stop();
        Hotkey_SetLivePenActive(FALSE);
        LivePen_Stop();
        RecordBar_Hide();
        KillTimer(hwnd, TIMER_HOOKHEAL);
        KillTimer(hwnd, TIMER_IDLETRIM);
        KillTimer(hwnd, TIMER_DEEPIDLE);
        KillTimer(hwnd, TIMER_SMOKE_START);
        KillTimer(hwnd, TIMER_SMOKE_STOP);
        Hotkey_Uninstall();
        TrayRemove();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------- startup */

static LONG WINAPI App_UnhandledException(EXCEPTION_POINTERS *ep)
{
    UNREFERENCED_PARAMETER(ep);
    OutputDebugStringW(L"ScreenshotService: unhandled exception; attempting to finalize active recording.\n");
    /* Best effort only: a hard termination or a fault on the capture worker
       itself cannot promise a playable MP4. In that case .partial/raw files
       remain available for recovery instead of being deleted. */
    Record_HandleCrash();
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    HANDLE      mutex;
    MSG         msg;

    const wchar_t *smokeArg;
    UNREFERENCED_PARAMETER(prev);
    UNREFERENCED_PARAMETER(show);

    SetUnhandledExceptionFilter(App_UnhandledException);
    g_cliSmokeMftOnly = cmdline && StrStrIW(cmdline, L"--record-smoke-test-mft") != NULL;
    g_cliSmokeNativeOnly = cmdline && StrStrIW(cmdline, L"--record-smoke-test-native") != NULL;
    smokeArg = cmdline ? StrStrIW(cmdline, L"--record-smoke-test") : NULL;
    if (smokeArg) {
        wchar_t *end = NULL;
        unsigned long seconds;
        g_cliSmokeTest = TRUE;
        if (g_cliSmokeMftOnly)
            smokeArg += lstrlenW(L"--record-smoke-test-mft");
        else if (g_cliSmokeNativeOnly)
            smokeArg += lstrlenW(L"--record-smoke-test-native");
        else
            smokeArg += lstrlenW(L"--record-smoke-test");
        while (*smokeArg == L' ' || *smokeArg == L'\t') ++smokeArg;
        seconds = *smokeArg ? wcstoul(smokeArg, &end, 10) : 4;
        if (!end || end == smokeArg) seconds = 4;
        if (seconds < 2) seconds = 2;
        if (seconds > 15) seconds = 15;
        g_cliSmokeSeconds = (DWORD)seconds;
    }

    /* A second copy is a request to capture, not a reason to run twice. */
    mutex = CreateMutexW(NULL, TRUE, APP_MUTEX_W);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(APP_CLASS_W, NULL);
        if (other)
            PostMessageW(other, WM_BSNIP_TRIGGER, TRIGGER_SECOND_INSTANCE, 0);
        CloseHandle(mutex);
        return g_cliSmokeTest ? 5 : 0;
    }

    Crash_Install();

    /* WIC and the shell verbs both need an STA. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    Settings_Load();
    Log_Enable(g_cliSmokeTest ? TRUE : g_cfg.debugLog);
    Log_Printf(L"---- start ----");
    if (!Settings_RegistryWritable()) {
        static const wchar_t storageWarning[] =
            L"ScreenshotService stores settings in HKLM\\Software\\Gallery Inc\\ScreenshotSvc, "
            L"but this registry key is not writable. Close the app and run "
            L"setup-registry-settings.bat as Administrator once. The app can continue now, "
            L"but changes may not persist until permissions are fixed.";
        Log_Always(L"settings: HKLM configuration store is not writable; run setup-registry-settings.bat as administrator");
        if (g_cliSmokeTest)
            return 6;
        MessageBoxW(NULL, storageWarning, APP_TITLE_W, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
    {
        wchar_t encoder[64];
        if (Settings_TakeDirectCrash(encoder, ARRAYSIZE(encoder))) {
            Log_Always(L"record: previous run ended while native encoder %s was active; disabling native encoding once", encoder);
            g_cfg.recordDirect = FALSE;
            if (!Crash_LogWindowsRecord(NULL, 7u * 24u * 3600u * 1000u))
                Log_Always(L"crash: no recent Application Error/Hang entry for ScreenshotService");
            StringCchCopyW(g_directCrashed, ARRAYSIZE(g_directCrashed), encoder);
        }
    }
    Settings_Save();   /* persist defaults and normalized values to the registry */
    /* Before any window exists, so nothing is ever built in the wrong
       language and then corrected. */
    Lang_Init(g_cfg.language);
    Theme_Init();
    Theme_SetDark(Theme_SystemPrefersDark());

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                               0, 0, LR_DEFAULTSIZE | LR_SHARED);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = HostProc;
    wc.hInstance     = inst;
    wc.hIcon         = g_icon;
    wc.lpszClassName = APP_CLASS_W;
    if (!RegisterClassExW(&wc))
        return 1;

    /* Never shown: it exists to receive the tray callback and the hotkey post.
       HWND_MESSAGE would be tidier but cannot receive the TaskbarCreated
       broadcast, which is how the icon comes back when Explorer restarts. */
    g_host = CreateWindowExW(0, APP_CLASS_W, APP_TITLE_W, WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_host)
        return 1;

    TrayAdd();
    if (g_directCrashed[0]) {
        wchar_t notice[512];
        Lang_Format(notice, ARRAYSIZE(notice), STR_MSG_DIRECT_CRASHED, g_directCrashed);
        TrayBalloon(APP_TITLE_W, notice);
    }
    if (!g_cliSmokeTest) {
        wchar_t videoFolder[MAX_PATH], recoveredText[24], attentionText[24], notice[512];
        RecoverySummary recovery;
        if (Settings_ResolveVideoFolder(videoFolder, ARRAYSIZE(videoFolder)) &&
            Recovery_ScanFolder(videoFolder, &recovery)) {
            unsigned recovered = recovery.recoveredMp4 + recovery.recoveredWav +
                                 recovery.recoveredFlac + recovery.recoveredGif +
                                 recovery.recoveredRawAudio;
            if (recovered || recovery.needsAttention) {
                StringCchPrintfW(recoveredText, ARRAYSIZE(recoveredText), L"%u", recovered);
                StringCchPrintfW(attentionText, ARRAYSIZE(attentionText), L"%u",
                                 recovery.needsAttention);
                Lang_Format(notice, ARRAYSIZE(notice), STR_MSG_RECOVERY_SUMMARY,
                            recoveredText, attentionText);
                TrayBalloon(APP_TITLE_W, notice);
            }
        }
    }
    Save_Init();
    /* A smoke test must not keep a second Desktop Duplication open or run the
       asynchronous MFT probe against the encoder it is about to exercise. */
    if (!g_cliSmokeTest) {
        if (!g_cfg.forceGdi) Capture_Init();
        Encoder_ProbeAsync();
    }

    Log_Printf(L"capture outputs ready, hook installing");
    if (!g_cliSmokeTest && !Hotkey_Install(g_host)) {
        Log_Printf(L"hook install FAILED (%lu)", GetLastError());
        wchar_t warn[512];
        Lang_Format(warn, ARRAYSIZE(warn), STR_MSG_HOOK_FAILED, APP_TITLE_W);
        MessageBoxW(NULL, warn, APP_TITLE_W, MB_OK | MB_ICONWARNING);
    }
    if (!g_cliSmokeTest) {
        SetTimer(g_host, TIMER_HOOKHEAL, HOOKHEAL_MS, NULL);
        App_ArmIdle();
        /* Posted, so it runs once the tray icon is up and startup is otherwise
           done: building the overlay's device costs a few hundred milliseconds
           that would otherwise land on the very first snip. */
        PostMessageW(g_host, WM_APP_PREWARM, 0, 0);
    } else {
        Log_Printf(L"smoke: diagnostic logging forced on; profile settings are not changed");
        PostMessageW(g_host, WM_APP_SMOKE_RECORD, 0, 0);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* A snip saves on a worker thread, and a lossless 4K JPEG XR takes about
       twenty seconds. Quitting out from under one would leave a truncated
       file, so the last thing this program does is wait for it. */
    if (!Save_WaitForPending(40000))
        Log_Printf(L"exit: an encode did not finish in time");

    Settings_Save();     /* the mode the user last picked */
    Capture_Shutdown();
    Save_Shutdown();
    Overlay_Shutdown();
    CoUninitialize();
    if (mutex)
        CloseHandle(mutex);
    return g_cliSmokeTest ? g_cliExitCode : (int)msg.wParam;
}
