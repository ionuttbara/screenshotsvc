#ifndef SCREENSHOT_SERVICE_CLASSIC_H
#define SCREENSHOT_SERVICE_CLASSIC_H
#define WINVER 0x0500
#define _WIN32_WINNT 0x0500
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <vfw.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "ScreenshotService Classic"
#define APP_CLASS "ScreenshotServiceClassic.Main"
#define OUTPUT_DIR "C:\\Screenshots"
#define WM_TRAYICON (WM_APP + 40)
#define WM_SELECTION_DONE (WM_APP + 41)
#define WM_SELECTION_CANCEL (WM_APP + 42)
#define HK_SCREENSHOT 101
#define HK_RECORD_AREA 102
#define HK_RECORD_FULL 103
#define HK_REGION_SHOT 104
#define TIMER_RECORD 201
#define CMD_SCREENSHOT 301
#define CMD_REGION_SHOT 302
#define CMD_RECORD_FULL 303
#define CMD_RECORD_REGION 304
#define CMD_STOP_RECORD 305
#define CMD_ABOUT 306
#define CMD_EXIT 307
#define CMD_SCREENSHOT_FULL 308
#define CMD_GIF_REGION 309
#define CMD_GIF_FULL 310

typedef struct PixelImage {
    int width;
    int height;
    BYTE *bgra;
} PixelImage;

typedef struct CaptureSelection {
    RECT rect;
    BOOL recording;
    BOOL gif;
} CaptureSelection;

extern HINSTANCE g_instance;
extern HWND g_main;
extern BOOL g_recording;
extern CaptureSelection g_recordSelection;

BOOL EnsureOutputDirectory(void);
BOOL MakeOutputPath(char *out, size_t cch, const char *prefix, const char *ext);
BOOL CaptureRectBGRA(const RECT *rc, PixelImage *out, BOOL drawCursor);
BOOL CaptureVirtualDesktop(PixelImage *out, BOOL drawCursor);
void FreePixelImage(PixelImage *img);
BOOL SavePNG(const char *path, const BYTE *bgra, int width, int height);
BOOL SaveBMP(const char *path, const BYTE *bgra, int width, int height);
BOOL Editor_Run(HWND owner, PixelImage *img);
BOOL AVI_Start(HWND owner, const char *path, int width, int height, int fps);
BOOL AVI_WriteFrame(const BYTE *bgra, int width, int height);
void AVI_Stop(void);
BOOL AVI_IsRecording(void);
void BeginSelection(BOOL recording);
void BeginGifSelection(void);
void StartRecording(RECT rc, BOOL fullScreen);
void StartGifRecording(RECT rc, BOOL fullScreen);
void StopRecording(void);
void HandleCaptureHotkey(BOOL region);

#endif
