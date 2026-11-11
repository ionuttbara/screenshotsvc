/*
 * screenshotservice.h - shared names and types for ScreenshotService.
 */
#ifndef SCREENSHOTSERVICE_H
#define SCREENSHOTSERVICE_H

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#define APP_NAME_W      L"ScreenshotService"
#define APP_TITLE_W     L"ScreenshotService"
#define APP_CLASS_W     L"ScreenshotService.Host"
#define APP_MUTEX_W     L"Local\\ScreenshotService.SingleInstance"

/* What the user is capturing. The order matches the toolbar, left to right. */
typedef enum {
    SNIP_RECT = 0,   /* drag a rectangle                       */
    SNIP_FREE,       /* draw a closed freehand shape           */
    SNIP_WINDOW,     /* pick a window under the cursor         */
    SNIP_FULLSCREEN, /* the monitor under the cursor, or all   */
    SNIP_MODE_COUNT
} SnipMode;

/* Where a capture request came from - only used for diagnostics and for
   deciding which mode a bare PrtScn should start in. */
typedef enum {
    TRIGGER_HOTKEY_SNIP = 0, /* Win+Shift+S            */
    TRIGGER_PRINTSCREEN,     /* PrtScn                 */
    TRIGGER_TRAY,            /* tray menu / double click */
    TRIGGER_SECOND_INSTANCE, /* another copy was started */
    TRIGGER_FULLSCREEN,      /* the tray's "whole screen", no overlay */
    TRIGGER_RECORD_SELECTION, /* Shift+PrtScn: choose an area to record */
    TRIGGER_RECORD_FULLSCREEN, /* Alt+PrtScn: toggle full-monitor video */
    TRIGGER_COLOR_PICKER       /* Win+Shift+X: sample a screen colour */
} TriggerSource;

/* Posted to the host window when a hotkey fires.
   wParam = TriggerSource, lParam = unused. */
#define WM_BSNIP_TRIGGER  (WM_APP + 2)

/* Posted by the save worker when it is done.
   lParam = SaveResult *, which the receiver frees. */
#define WM_BSNIP_SAVED    (WM_APP + 3)

/* Posted by the recording worker when it is done.
   lParam = RecordResult *, which the receiver frees. */
#define WM_BSNIP_RECORDED (WM_APP + 4)

/* Posted by the record bar's stop button. */
#define WM_BSNIP_STOPREC  (WM_APP + 5)
#define WM_BSNIP_PAUSEREC (WM_APP + 6)
#define WM_BSNIP_MICMUTE  (WM_APP + 7)
/* Recording worker progress. wParam = save percentage 0..100. */
#define WM_BSNIP_RECORD_PROGRESS (WM_APP + 8)
#define WM_BSNIP_PEN (WM_APP + 9)
/* Right-click on the live annotation canvas disables drawing mode. */
#define WM_BSNIP_PEN_OFF (WM_APP + 10)
/* Ctrl+Alt+right-click during recording clears all live pen marks. */
#define WM_BSNIP_PEN_CLEAR (WM_APP + 11)
/* Low-level mouse hook forwards captured live-pen input to the UI thread.
   x is in wParam and y is in lParam, both signed screen coordinates. */
#define WM_BSNIP_PEN_MOUSEMOVE (WM_APP + 12)
#define WM_BSNIP_PEN_MOUSEDOWN (WM_APP + 13)
#define WM_BSNIP_PEN_MOUSEUP   (WM_APP + 14)
#define WM_BSNIP_PEN_MOUSELEAVE (WM_APP + 15)

#endif /* SCREENSHOTSERVICE_H */
