#include "classic.h"
#include "giflegacy.h"

HINSTANCE g_instance = NULL;
HWND g_main = NULL;
BOOL g_recording = FALSE;
CaptureSelection g_recordSelection;

static HWND g_selector = NULL;
static BOOL g_selectingRecord = FALSE;
static BOOL g_selectingGif = FALSE;
static BOOL g_gifRecording = FALSE;
static int g_selectorHot = -1;
static int g_virtualX = 0, g_virtualY = 0, g_virtualW = 0, g_virtualH = 0;
static RECT g_frameRect;
static int g_outputWidth = 0, g_outputHeight = 0;
static DWORD g_recordStartTick = 0;
static LONG g_recordFrameCount = 0;
static BYTE *g_lastRecordFrame = NULL;
static BOOL g_haveLastRecordFrame = FALSE;
static NOTIFYICONDATAA g_nid;
static BOOL g_quitAfterRecord = FALSE;
static BOOL g_lowResourceMode = FALSE;
static int g_recordFps = 30;
static DWORD g_lastFrameElapsed = 0;
static HWND g_recordBar = NULL;
static HWND g_recordOutline[4] = { NULL, NULL, NULL, NULL };
static BOOL g_recordingFullScreen = FALSE;
#define RECORD_BAR_CLASS "ScreenshotServiceClassic.RecordBar"
#define RECORD_OUTLINE_CLASS "ScreenshotServiceClassic.RecordOutline"
#define RECORD_BAR_STOP_ID 410

static void ShowRecordNotice(const char *text);
static void PositionRecordOutline(int x, int y, int width, int height);
static void RestoreRecordingOutlinePixels(BYTE *bits, int width, int height);

/* Forward declaration: StartRecording captures/caches the first frame immediately. */
static void WriteRecordingFrame(void);

static int VirtualLeft(void) { return GetSystemMetrics(SM_XVIRTUALSCREEN); }
static int VirtualTop(void) { return GetSystemMetrics(SM_YVIRTUALSCREEN); }
static int VirtualWidth(void) { return GetSystemMetrics(SM_CXVIRTUALSCREEN); }
static int VirtualHeight(void) { return GetSystemMetrics(SM_CYVIRTUALSCREEN); }

BOOL EnsureOutputDirectory(void)
{
    DWORD a = GetFileAttributesA(OUTPUT_DIR);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return TRUE;
    if (CreateDirectoryA(OUTPUT_DIR, NULL)) return TRUE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

BOOL MakeOutputPath(char *out, size_t cch, const char *prefix, const char *ext)
{
    SYSTEMTIME st;
    int n;
    GetLocalTime(&st);
    n = sprintf(out, "%s\\%s_%04u%02u%02u_%02u%02u%02u_%03u.%s", OUTPUT_DIR, prefix,
        (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, (unsigned)st.wMilliseconds, ext);
    if (n < 0 || (size_t)n >= cch) return FALSE;
    return TRUE;
}

void FreePixelImage(PixelImage *img)
{
    if (!img) return;
    if (img->bgra) free(img->bgra);
    img->bgra = NULL; img->width = 0; img->height = 0;
}

static void DrawCursorOnDC(HDC dc, int xOrigin, int yOrigin, int scaleX, int scaleY)
{
    CURSORINFO ci;
    ICONINFO ii;
    int x, y;
    ZeroMemory(&ci, sizeof(ci)); ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    ZeroMemory(&ii, sizeof(ii));
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    x = MulDiv(ci.ptScreenPos.x - xOrigin, scaleX, 1) - (int)ii.xHotspot;
    y = MulDiv(ci.ptScreenPos.y - yOrigin, scaleY, 1) - (int)ii.yHotspot;
    DrawIconEx(dc, x, y, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
}

BOOL CaptureRectBGRA(const RECT *rc, PixelImage *out, BOOL drawCursor)
{
    HDC screen = NULL, mem = NULL;
    HBITMAP dib = NULL, old = NULL;
    BITMAPINFO bi;
    BYTE *bits = NULL;
    size_t bytes;
    int w, h;
    BOOL ok = FALSE;
    if (!rc || !out) return FALSE;
    ZeroMemory(out, sizeof(*out));
    w = rc->right - rc->left; h = rc->bottom - rc->top;
    if (w < 1 || h < 1 || (size_t)w > (size_t)-1 / (size_t)h / 4U) return FALSE;
    bytes = (size_t)w * (size_t)h * 4U;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(NULL); if (!screen) goto done;
    mem = CreateCompatibleDC(screen); if (!mem) goto done;
    dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!dib || !bits) goto done;
    old = (HBITMAP)SelectObject(mem, dib);
    if (!BitBlt(mem, 0, 0, w, h, screen, rc->left, rc->top, SRCCOPY | CAPTUREBLT)) goto done;
    if (drawCursor) DrawCursorOnDC(mem, rc->left, rc->top, 1, 1);
    out->bgra = (BYTE *)malloc(bytes); if (!out->bgra) goto done;
    memcpy(out->bgra, bits, bytes); out->width = w; out->height = h;
    ok = TRUE;
done:
    if (!ok) FreePixelImage(out);
    if (old) SelectObject(mem, old);
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(NULL, screen);
    return ok;
}

BOOL CaptureVirtualDesktop(PixelImage *out, BOOL drawCursor)
{
    RECT rc;
    rc.left = VirtualLeft(); rc.top = VirtualTop();
    rc.right = rc.left + VirtualWidth(); rc.bottom = rc.top + VirtualHeight();
    return CaptureRectBGRA(&rc, out, drawCursor);
}

/* Capture directly into the output-size DIB. This avoids a full-desktop
   allocation plus a separate crop, important on 256 MB machines. */
static BOOL CaptureRecordingFrame(PixelImage *out)
{
    HDC screen = NULL, mem = NULL;
    HBITMAP dib = NULL, old = NULL;
    BITMAPINFO bi;
    BYTE *bits = NULL;
    size_t bytes;
    int vx, vy, vw, vh, srcX, srcY, srcW, srcH, dstW, dstH;
    BOOL ok = FALSE;
    CURSORINFO ci;
    ICONINFO ii;
    if (!out) return FALSE;
    ZeroMemory(out, sizeof(*out));
    dstW = g_outputWidth; dstH = g_outputHeight;
    if (dstW < 1 || dstH < 1 || g_virtualW < 1 || g_virtualH < 1) return FALSE;
    if ((size_t)dstW > (size_t)-1 / (size_t)dstH / 4U) return FALSE;
    bytes = (size_t)dstW * (size_t)dstH * 4U;

    vx = VirtualLeft(); vy = VirtualTop(); vw = VirtualWidth(); vh = VirtualHeight();
    if (vw < 1 || vh < 1) return FALSE;
    if (g_recordBar && IsWindowVisible(g_recordBar)) {
        RECT barNow;
        int barX = vx + vw - 284, barY = vy + 8;
        if (vw < 284) barX = vx + 2;
        if (vh < 44) barY = vy + 2;
        if (GetWindowRect(g_recordBar, &barNow) && (barNow.left != barX || barNow.top != barY))
            SetWindowPos(g_recordBar, HWND_TOPMOST, barX, barY, 280, 42, SWP_NOACTIVATE);
    }
    srcX = vx + MulDiv(g_frameRect.left, vw, g_virtualW);
    srcY = vy + MulDiv(g_frameRect.top, vh, g_virtualH);
    srcW = MulDiv(g_frameRect.right - g_frameRect.left, vw, g_virtualW);
    srcH = MulDiv(g_frameRect.bottom - g_frameRect.top, vh, g_virtualH);
    if (srcW < 1) srcW = 1;
    if (srcH < 1) srcH = 1;

    /* Highlight the selected capture crop while recording region video/GIF.
       Follow the crop if display metrics change during the recording. */
    if (g_recordOutline[0] && !g_recordingFullScreen)
        PositionRecordOutline(srcX, srcY, srcW, srcH);

    /* Keep the stop control visible. Its pixels are replaced below with the
       previous clean frame so the controls are not recorded and don't flicker. */
    screen = GetDC(NULL); if (!screen) goto done;
    mem = CreateCompatibleDC(screen); if (!mem) goto done;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dstW; bi.bmiHeader.biHeight = -dstH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!dib || !bits) goto done;
    old = (HBITMAP)SelectObject(mem, dib);
    SetStretchBltMode(mem, COLORONCOLOR);
    if (dstW == srcW && dstH == srcH) {
        if (!BitBlt(mem, 0, 0, dstW, dstH, screen, srcX, srcY, SRCCOPY | CAPTUREBLT)) goto done;
    } else {
        if (!StretchBlt(mem, 0, 0, dstW, dstH, screen, srcX, srcY, srcW, srcH, SRCCOPY | CAPTUREBLT)) goto done;
    }

    ZeroMemory(&ci, sizeof(ci)); ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
        ZeroMemory(&ii, sizeof(ii));
        if (GetIconInfo(ci.hCursor, &ii)) {
            int cx, cy, cw, ch;
            cx = MulDiv(ci.ptScreenPos.x - srcX, dstW, srcW) - MulDiv((int)ii.xHotspot, dstW, srcW);
            cy = MulDiv(ci.ptScreenPos.y - srcY, dstH, srcH) - MulDiv((int)ii.yHotspot, dstH, srcH);
            cw = MulDiv(GetSystemMetrics(SM_CXCURSOR), dstW, srcW);
            ch = MulDiv(GetSystemMetrics(SM_CYCURSOR), dstH, srcH);
            if (cw < 1) cw = 1;
            if (ch < 1) ch = 1;
            DrawIconEx(mem, cx, cy, ci.hCursor, cw, ch, 0, NULL, DI_NORMAL);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
    }
    if (g_recordBar && IsWindowVisible(g_recordBar) && g_haveLastRecordFrame &&
        g_lastRecordFrame && g_outputWidth == dstW && g_outputHeight == dstH) {
        RECT barRc;
        if (GetWindowRect(g_recordBar, &barRc)) {
            int bx0 = MulDiv(barRc.left - srcX, dstW, srcW);
            int by0 = MulDiv(barRc.top - srcY, dstH, srcH);
            int bx1 = MulDiv(barRc.right - srcX, dstW, srcW);
            int by1 = MulDiv(barRc.bottom - srcY, dstH, srcH);
            int yy;
            if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0;
            if (bx1 > dstW) bx1 = dstW; if (by1 > dstH) by1 = dstH;
            if (bx1 > bx0 && by1 > by0) {
                for (yy = by0; yy < by1; ++yy)
                    memcpy(bits + ((size_t)yy * (size_t)dstW + (size_t)bx0) * 4U,
                           g_lastRecordFrame + ((size_t)yy * (size_t)dstW + (size_t)bx0) * 4U,
                           (size_t)(bx1 - bx0) * 4U);
            }
        }
    }
    RestoreRecordingOutlinePixels(bits, dstW, dstH);
    out->bgra = (BYTE *)malloc(bytes);
    if (!out->bgra) goto done;
    memcpy(out->bgra, bits, bytes); out->width = dstW; out->height = dstH;
    ok = TRUE;
done:
    if (!ok) FreePixelImage(out);
    if (old) SelectObject(mem, old);
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(NULL, screen);
    return ok;
}

static void DetectLowResourceMode(void)
{
    MEMORYSTATUS ms;
    SYSTEM_INFO si;
    ZeroMemory(&ms, sizeof(ms)); ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    GetSystemInfo(&si);
    /* Practical profile for Pentium 4 / 256 MB class computers. */
    g_lowResourceMode = (ms.dwTotalPhys <= (DWORD)(512U * 1024U * 1024U) || si.dwNumberOfProcessors <= 1);
}

static void CreateRecordBar(void)
{
    int x, y, sw, sh;
    if (g_recordBar) return;
    sw = VirtualWidth(); sh = VirtualHeight();
    x = VirtualLeft() + sw - 284; y = VirtualTop() + 8;
    if (sw < 284) x = VirtualLeft() + 2;
    if (sh < 44) y = VirtualTop() + 2;
    g_recordBar = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        RECORD_BAR_CLASS, "Recording", WS_POPUP, x, y, 280, 42, g_main, NULL, g_instance, NULL);
    if (g_recordBar) {
        SetWindowPos(g_recordBar, HWND_TOPMOST, x, y, 280, 42, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(g_recordBar, SW_SHOWNOACTIVATE);
        UpdateWindow(g_recordBar);
    }
}

static LRESULT CALLBACK RecordBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateWindowExA(0, "BUTTON", "STOP", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            204, 7, 68, 28, hwnd, (HMENU)(INT_PTR)RECORD_BAR_STOP_ID, g_instance, NULL);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == RECORD_BAR_STOP_ID) { StopRecording(); return 0; }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r;
            HBRUSH bg = CreateSolidBrush(RGB(32, 32, 36));
            char text[96];
            DWORD elapsed = GetTickCount() - g_recordStartTick;
            unsigned long seconds = (unsigned long)(elapsed / 1000U);
            GetClientRect(hwnd, &r); FillRect(dc, &r, bg); DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            wsprintfA(text, "%s %02lu:%02lu", g_gifRecording ? "GIF" : "AVI", seconds / 60UL, seconds % 60UL);
            SetTextColor(dc, RGB(255, 90, 90)); TextOutA(dc, 10, 14, "REC", 3);
            SetTextColor(dc, RGB(245, 245, 245)); TextOutA(dc, 45, 14, text, lstrlenA(text));
            EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: StopRecording(); return 0;
    case WM_DESTROY: if (g_recordBar == hwnd) g_recordBar = NULL; return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void PositionOneOutlineStrip(int index, const RECT *r)
{
    RECT oldRect;
    BOOL created = FALSE;
    HWND insertAfter;
    int width, height;
    if (index < 0 || index >= 4 || !r) return;
    width = r->right - r->left;
    height = r->bottom - r->top;
    if (width < 1 || height < 1) return;
    insertAfter = (g_recordBar && IsWindow(g_recordBar)) ? g_recordBar : HWND_TOPMOST;
    if (!g_recordOutline[index]) {
        g_recordOutline[index] = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_LAYERED | WS_EX_TRANSPARENT,
            RECORD_OUTLINE_CLASS, "Recording region", WS_POPUP,
            r->left, r->top, width, height, g_main, NULL, g_instance, (LPVOID)(INT_PTR)index);
        if (!g_recordOutline[index]) return;
        SetLayeredWindowAttributes(g_recordOutline[index], RGB(255, 0, 255), 0, LWA_COLORKEY);
        created = TRUE;
    }
    if (created || !GetWindowRect(g_recordOutline[index], &oldRect) ||
        oldRect.left != r->left || oldRect.top != r->top ||
        oldRect.right - oldRect.left != width || oldRect.bottom - oldRect.top != height ||
        !IsWindowVisible(g_recordOutline[index])) {
        SetWindowPos(g_recordOutline[index], insertAfter, r->left, r->top, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(g_recordOutline[index], NULL, TRUE);
        UpdateWindow(g_recordOutline[index]);
    }
}

static void PositionRecordOutline(int x, int y, int width, int height)
{
    RECT r;
    const int t = 3;
    if (width < t * 2 + 1 || height < t * 2 + 1) return;
    /* Four narrow layered windows avoid allocating a full-size transparent
       surface on very low-RAM machines (e.g. 256 MB / Pentium 4). */
    r.left = x; r.top = y; r.right = x + width; r.bottom = y + t;
    PositionOneOutlineStrip(0, &r);              /* top */
    r.left = x; r.top = y + height - t; r.right = x + width; r.bottom = y + height;
    PositionOneOutlineStrip(1, &r);              /* bottom */
    r.left = x; r.top = y + t; r.right = x + t; r.bottom = y + height - t;
    PositionOneOutlineStrip(2, &r);              /* left */
    r.left = x + width - t; r.top = y + t; r.right = x + width; r.bottom = y + height - t;
    PositionOneOutlineStrip(3, &r);              /* right */
}

static LRESULT CALLBACK RecordOutlineProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        {
            CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
            SetWindowLongA(hwnd, GWL_USERDATA, (LONG)(INT_PTR)cs->lpCreateParams);
        }
        return 0;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r, fill;
            HBRUSH keyBrush = CreateSolidBrush(RGB(255, 0, 255));
            HBRUSH whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            HBRUSH redBrush = CreateSolidBrush(RGB(255, 62, 62));
            int index = (int)(INT_PTR)GetWindowLongA(hwnd, GWL_USERDATA);
            GetClientRect(hwnd, &r);
            FillRect(dc, &r, keyBrush);
            /* One white pixel plus two red pixels at the crop edge. The strips
               are only 3px thick, keeping layered-window memory negligible. */
            if (index == 0) {
                fill.left=0; fill.right=r.right; fill.top=0; fill.bottom=1; FillRect(dc,&fill,whiteBrush);
                fill.top=1; fill.bottom=3; FillRect(dc,&fill,redBrush);
            } else if (index == 1) {
                fill.left=0; fill.right=r.right; fill.top=0; fill.bottom=2; FillRect(dc,&fill,redBrush);
                fill.top=2; fill.bottom=3; FillRect(dc,&fill,whiteBrush);
            } else if (index == 2) {
                fill.left=0; fill.right=1; fill.top=0; fill.bottom=r.bottom; FillRect(dc,&fill,whiteBrush);
                fill.left=1; fill.right=3; FillRect(dc,&fill,redBrush);
            } else {
                fill.left=0; fill.right=2; fill.top=0; fill.bottom=r.bottom; FillRect(dc,&fill,redBrush);
                fill.left=2; fill.right=3; FillRect(dc,&fill,whiteBrush);
            }
            DeleteObject(keyBrush); DeleteObject(whiteBrush); DeleteObject(redBrush);
            EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DESTROY:
        {
            int i;
            for (i = 0; i < 4; ++i) if (g_recordOutline[i] == hwnd) g_recordOutline[i] = NULL;
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void RestoreRecordingOutlinePixels(BYTE *bits, int width, int height)
{
    int y, edge = 4;
    size_t rowBytes;
    if (g_recordingFullScreen || !g_recordOutline[0] || !IsWindowVisible(g_recordOutline[0]) ||
        !g_haveLastRecordFrame || !g_lastRecordFrame || !bits || width < 1 || height < 1) return;
    if (edge * 2 > width) edge = width / 2;
    if (edge * 2 > height) edge = height / 2;
    if (edge < 1) return;
    rowBytes = (size_t)width * 4U;
    /* The outline is a UI affordance only. Restore the perimeter from the
       previous clean frame so its red/white border is never baked into AVI/GIF. */
    for (y = 0; y < edge; ++y) {
        memcpy(bits + (size_t)y * rowBytes, g_lastRecordFrame + (size_t)y * rowBytes, rowBytes);
        memcpy(bits + (size_t)(height - 1 - y) * rowBytes,
               g_lastRecordFrame + (size_t)(height - 1 - y) * rowBytes, rowBytes);
    }
    for (y = edge; y < height - edge; ++y) {
        memcpy(bits + ((size_t)y * (size_t)width) * 4U,
               g_lastRecordFrame + ((size_t)y * (size_t)width) * 4U, (size_t)edge * 4U);
        memcpy(bits + ((size_t)y * (size_t)width + (size_t)(width - edge)) * 4U,
               g_lastRecordFrame + ((size_t)y * (size_t)width + (size_t)(width - edge)) * 4U, (size_t)edge * 4U);
    }
}

static BOOL SaveScreenshot(PixelImage *img)
{
    char path[MAX_PATH];
    BOOL saved = FALSE;
    if (!img || !img->bgra || img->width < 1 || img->height < 1) return FALSE;
    if (!EnsureOutputDirectory()) {
        MessageBoxA(g_main, "Cannot create C:\\Screenshots. Check permissions.", APP_NAME, MB_ICONERROR);
        return FALSE;
    }
    if (!MakeOutputPath(path, sizeof(path), "Screenshot", "png")) return FALSE;
    saved = SavePNG(path, img->bgra, img->width, img->height);
    if (!saved) {
        /* Direct autosave: BMP remains a recovery path when PNG writing fails. */
        if (MakeOutputPath(path, sizeof(path), "Screenshot", "bmp"))
            saved = SaveBMP(path, img->bgra, img->width, img->height);
    }
    if (saved) {
        ShowRecordNotice("Screenshot automatically saved in C:\\Screenshots.");
    } else {
        MessageBoxA(g_main, "Could not save the screenshot to C:\\Screenshots.", APP_NAME, MB_OK | MB_ICONERROR);
    }
    return saved;
}

static void DoScreenshot(const RECT *rc)
{
    PixelImage img;
    BOOL ok;
    if (rc) ok = CaptureRectBGRA(rc, &img, TRUE);
    else ok = CaptureVirtualDesktop(&img, TRUE);
    if (!ok) { MessageBoxA(g_main, "Could not capture the screen.", APP_NAME, MB_OK | MB_ICONERROR); return; }
    SaveScreenshot(&img);
    FreePixelImage(&img);
}

static void ShowRecordNotice(const char *text)
{
    /* Update the tray tooltip rather than using balloon notifications, which
       do not exist on Windows 2000. NOTIFYICONDATA_V1_SIZE is intentional. */
    g_nid.uFlags = NIF_TIP;
    lstrcpynA(g_nid.szTip, text, sizeof(g_nid.szTip));
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

static void StartRecordingCommon(RECT rc, BOOL fullScreen, BOOL gif)
{
    char path[MAX_PATH];
    int left, top, width, height;
    int sinkWidth, sinkHeight, maxW, maxH, fps, timerMs;
    double scale;
    if (g_recording) return;
    if (!EnsureOutputDirectory()) { MessageBoxA(g_main, "Cannot create C:\\Screenshots.", APP_NAME, MB_ICONERROR); return; }
    g_virtualX = VirtualLeft(); g_virtualY = VirtualTop();
    g_virtualW = VirtualWidth(); g_virtualH = VirtualHeight();
    if (g_virtualW < 1 || g_virtualH < 1) return;
    if (fullScreen) { left = 0; top = 0; width = g_virtualW; height = g_virtualH; }
    else {
        left = rc.left - g_virtualX; top = rc.top - g_virtualY;
        width = rc.right - rc.left; height = rc.bottom - rc.top;
        if (left < 0) { width += left; left = 0; }
        if (top < 0) { height += top; top = 0; }
        if (left + width > g_virtualW) width = g_virtualW - left;
        if (top + height > g_virtualH) height = g_virtualH - top;
    }
    if (width < 16 || height < 16) { MessageBoxA(g_main, "The selected recording area is too small.", APP_NAME, MB_ICONWARNING); return; }
    g_frameRect.left = left; g_frameRect.top = top; g_frameRect.right = left + width; g_frameRect.bottom = top + height;
    g_recordingFullScreen = fullScreen ? TRUE : FALSE;

    /* Automatically reduce work and frame buffers on <=512 MB or single-core systems. */
    if (g_lowResourceMode) {
        maxW = gif ? 800 : 1024;
        maxH = gif ? 600 : 768;
    } else {
        maxW = gif ? 1920 : width;
        maxH = gif ? 1080 : height;
    }
    scale = 1.0;
    if (width > maxW) scale = (double)maxW / (double)width;
    if (height > maxH && (double)maxH / (double)height < scale) scale = (double)maxH / (double)height;
    if (scale > 1.0) scale = 1.0;
    sinkWidth = (int)((double)width * scale + 0.5);
    sinkHeight = (int)((double)height * scale + 0.5);
    if (sinkWidth < 16) sinkWidth = width < 16 ? width : 16;
    if (sinkHeight < 16) sinkHeight = height < 16 ? height : 16;
    g_outputWidth = sinkWidth; g_outputHeight = sinkHeight;
    fps = gif ? (g_lowResourceMode ? 8 : 15) : (g_lowResourceMode ? 10 : 30);
    g_recordFps = fps;
    if (!MakeOutputPath(path, sizeof(path), "Recording", gif ? "gif" : "avi")) return;
    if (gif) {
        if (!GIF_Start(path, sinkWidth, sinkHeight, fps)) {
            MessageBoxA(g_main, "Could not initialize GIF recording.", APP_NAME, MB_ICONERROR);
            return;
        }
    } else {
        if (!AVI_Start(g_main, path, sinkWidth, sinkHeight, fps)) {
            MessageBoxA(g_main, "Could not initialize AVI recording.", APP_NAME, MB_ICONERROR);
            return;
        }
    }
    g_lastRecordFrame = (BYTE *)malloc((size_t)sinkWidth * (size_t)sinkHeight * 4U);
    if (!g_lastRecordFrame) {
        if (gif) GIF_Stop(); else AVI_Stop();
        MessageBoxA(g_main, "Not enough memory for the recording frame buffer.", APP_NAME, MB_ICONERROR);
        return;
    }
    g_recordFrameCount = 0; g_haveLastRecordFrame = FALSE; g_lastFrameElapsed = 0;
    g_recordStartTick = GetTickCount();
    g_gifRecording = gif;
    g_recording = TRUE;
    WriteRecordingFrame();
    if (!g_recording) return;
    timerMs = (1000 + fps / 2) / fps;
    if (timerMs < 1) timerMs = 1;
    if (!SetTimer(g_main, TIMER_RECORD, (UINT)timerMs, NULL)) {
        StopRecording();
        MessageBoxA(g_main, "Could not start the recording timer.", APP_NAME, MB_ICONERROR);
        return;
    }
    CreateRecordBar();
    if (!g_recordingFullScreen) {
        int ox = g_virtualX + g_frameRect.left;
        int oy = g_virtualY + g_frameRect.top;
        PositionRecordOutline(ox, oy, g_frameRect.right - g_frameRect.left, g_frameRect.bottom - g_frameRect.top);
    }
    ShowRecordNotice(gif ? (g_lowResourceMode ? "Recording GIF 8 FPS - Stop button available; max 15 seconds" : "Recording GIF 15 FPS - Stop button available; max 15 seconds") :
        (g_lowResourceMode ? "Recording AVI 10 FPS - Stop button available" : "Recording AVI 30 FPS - Stop button available"));
}

void StartRecording(RECT rc, BOOL fullScreen)
{
    StartRecordingCommon(rc, fullScreen, FALSE);
}

void StartGifRecording(RECT rc, BOOL fullScreen)
{
    StartRecordingCommon(rc, fullScreen, TRUE);
}

void StopRecording(void)
{
    BOOL wasGif;
    if (!g_recording) return;
    wasGif = g_gifRecording;
    KillTimer(g_main, TIMER_RECORD);
    if (wasGif && g_haveLastRecordFrame) {
        DWORD elapsed = GetTickCount() - g_recordStartTick;
        DWORD delta = elapsed >= g_lastFrameElapsed ? elapsed - g_lastFrameElapsed : (DWORD)(1000 / g_recordFps);
        unsigned int delayCs = (unsigned int)((delta + 5U) / 10U);
        if (delayCs < 1U) delayCs = 1U;
        if (delayCs > 65535U) delayCs = 65535U;
        GIF_WriteFrameTimed(g_lastRecordFrame, g_outputWidth, g_outputHeight, delayCs);
    }
    if (wasGif) GIF_Stop(); else AVI_Stop();
    g_recording = FALSE; g_gifRecording = FALSE;
    if (g_recordBar && IsWindow(g_recordBar)) DestroyWindow(g_recordBar);
    g_recordBar = NULL;
    {
        int outlineIndex;
        for (outlineIndex = 0; outlineIndex < 4; ++outlineIndex) {
            if (g_recordOutline[outlineIndex] && IsWindow(g_recordOutline[outlineIndex]))
                DestroyWindow(g_recordOutline[outlineIndex]);
            g_recordOutline[outlineIndex] = NULL;
        }
    }
    g_recordingFullScreen = FALSE;
    if (g_lastRecordFrame) { free(g_lastRecordFrame); g_lastRecordFrame = NULL; }
    g_haveLastRecordFrame = FALSE; g_recordFrameCount = 0; g_lastFrameElapsed = 0;
    ShowRecordNotice(wasGif ? "GIF saved in C:\\Screenshots." : "Recording saved in C:\\Screenshots.");
    if (g_quitAfterRecord) PostMessage(g_main, WM_CLOSE, 0, 0);
}

static void WriteRecordingFrame(void)
{
    PixelImage frame;
    int w, h;
    DWORD elapsed, targetCount;
    if (!g_recording) return;
    elapsed = GetTickCount() - g_recordStartTick;
    if (g_gifRecording && elapsed >= 15000U) { StopRecording(); return; }
    w = g_outputWidth; h = g_outputHeight;
    if (!g_gifRecording) {
        targetCount = (DWORD)((elapsed / 1000U) * (DWORD)g_recordFps +
            (((elapsed % 1000U) * (DWORD)g_recordFps + 500U) / 1000U) + 1U);
        if (targetCount <= (DWORD)g_recordFrameCount) return;
    }

    if (!CaptureRecordingFrame(&frame)) {
        if (!g_gifRecording && g_haveLastRecordFrame) {
            targetCount = (DWORD)((elapsed / 1000U) * (DWORD)g_recordFps +
                (((elapsed % 1000U) * (DWORD)g_recordFps + 500U) / 1000U) + 1U);
            while ((DWORD)g_recordFrameCount < targetCount) {
                if (!AVI_WriteFrame(g_lastRecordFrame, w, h)) { StopRecording(); return; }
                ++g_recordFrameCount;
            }
        }
        return;
    }

    if (g_gifRecording) {
        /* Buffer one GIF frame; write it on the next tick with a delay matching
           real elapsed time. This avoids encoding repeated frames on slow CPUs. */
        if (g_haveLastRecordFrame) {
            DWORD delta = elapsed >= g_lastFrameElapsed ? elapsed - g_lastFrameElapsed : (DWORD)(1000 / g_recordFps);
            unsigned int delayCs = (unsigned int)((delta + 5U) / 10U);
            if (delayCs < 1U) delayCs = 1U;
            if (delayCs > 65535U) delayCs = 65535U;
            if (!GIF_WriteFrameTimed(g_lastRecordFrame, w, h, delayCs)) {
                FreePixelImage(&frame); StopRecording();
                MessageBoxA(g_main, "GIF encoding failed. Recording was stopped.", APP_NAME, MB_ICONERROR);
                return;
            }
        }
        memcpy(g_lastRecordFrame, frame.bgra, (size_t)w * (size_t)h * 4U);
        g_haveLastRecordFrame = TRUE; g_lastFrameElapsed = elapsed; ++g_recordFrameCount;
        FreePixelImage(&frame);
        if (g_recordBar) InvalidateRect(g_recordBar, NULL, FALSE);
        return;
    }

    targetCount = (DWORD)((elapsed / 1000U) * (DWORD)g_recordFps +
        (((elapsed % 1000U) * (DWORD)g_recordFps + 500U) / 1000U) + 1U);
    if (targetCount <= (DWORD)g_recordFrameCount) { FreePixelImage(&frame); return; }
    if (g_haveLastRecordFrame) {
        while ((DWORD)(g_recordFrameCount + 1) < targetCount) {
            if (!AVI_WriteFrame(g_lastRecordFrame, w, h)) {
                FreePixelImage(&frame); StopRecording();
                MessageBoxA(g_main, "AVI writing failed. Recording was stopped.", APP_NAME, MB_ICONERROR);
                return;
            }
            ++g_recordFrameCount;
        }
    }
    if (!AVI_WriteFrame(frame.bgra, w, h)) {
        FreePixelImage(&frame); StopRecording();
        MessageBoxA(g_main, "AVI writing failed. Recording was stopped.", APP_NAME, MB_ICONERROR);
        return;
    }
    memcpy(g_lastRecordFrame, frame.bgra, (size_t)w * (size_t)h * 4U);
    g_haveLastRecordFrame = TRUE; ++g_recordFrameCount;
    FreePixelImage(&frame);
    if (g_recordBar) InvalidateRect(g_recordBar, NULL, FALSE);
}

static void AddTrayIcon(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid)); g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main; g_nid.uID = 1; g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconA(g_instance, MAKEINTRESOURCEA(101));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpynA(g_nid.szTip, APP_NAME, sizeof(g_nid.szTip));
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void ShowTrayMenu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT p;
    UINT off = g_recording ? MF_GRAYED : 0;
    if (!menu) return;
    AppendMenuA(menu, MF_STRING | off, CMD_SCREENSHOT, "Screenshot toolbar...\tPrtScn");
    AppendMenuA(menu, MF_STRING | off, CMD_REGION_SHOT, "Select screenshot area...\tWin+Shift+S");
    AppendMenuA(menu, MF_STRING | off, CMD_SCREENSHOT_FULL, "Capture full desktop now");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING | off, CMD_RECORD_FULL, "Record full desktop (AVI)\tAlt+PrtScn");
    AppendMenuA(menu, MF_STRING | off, CMD_RECORD_REGION, "Record selected area (AVI)\tShift+PrtScn");
    AppendMenuA(menu, MF_STRING | off, CMD_GIF_REGION, g_lowResourceMode ? "Record selected area as GIF (8 FPS, max 15s)" : "Record selected area as GIF (15 FPS, max 15s)");
    AppendMenuA(menu, MF_STRING | off, CMD_GIF_FULL, g_lowResourceMode ? "Record full desktop as GIF (8 FPS, max 15s)" : "Record full desktop as GIF (15 FPS, max 15s)");
    AppendMenuA(menu, MF_STRING | (g_recording ? 0 : MF_GRAYED), CMD_STOP_RECORD, "Stop recording now");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, CMD_ABOUT, "About");
    AppendMenuA(menu, MF_STRING, CMD_EXIT, "Exit");
    GetCursorPos(&p); SetForegroundWindow(g_main);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, g_main, NULL);
    PostMessage(g_main, WM_NULL, 0, 0); DestroyMenu(menu);
}

enum {
    SELBTN_REGION = 0,
    SELBTN_FULLSHOT,
    SELBTN_RECORD,
    SELBTN_FULLREC,
    SELBTN_GIF,
    SELBTN_GIFFULL,
    SELBTN_CLOSE,
    SELBTN_COUNT
};

static const int g_selectorButtonWidths[SELBTN_COUNT] = { 58, 74, 62, 72, 64, 68, 34 };
static const char *g_selectorButtonLabels[SELBTN_COUNT] = { "Region", "Full shot", "Record", "Full rec", "GIF area", "GIF full", "X" };

static RECT SelectorToolbarRect(HWND hwnd)
{
    RECT c, r;
    int width = 0, i;
    GetClientRect(hwnd, &c);
    for (i = 0; i < SELBTN_COUNT; ++i) width += g_selectorButtonWidths[i];
    width += (SELBTN_COUNT - 1) * 4 + 16;
    r.left = (c.right - width) / 2;
    if (r.left < 8) r.left = 8;
    r.right = r.left + width;
    r.top = 10; r.bottom = 52;
    return r;
}

static BOOL SelectorButtonRect(HWND hwnd, int button, RECT *out)
{
    RECT tb;
    int i, x;
    if (button < 0 || button >= SELBTN_COUNT || !out) return FALSE;
    tb = SelectorToolbarRect(hwnd);
    x = tb.left + 8;
    for (i = 0; i < button; ++i) x += g_selectorButtonWidths[i] + 4;
    out->left = x; out->right = x + g_selectorButtonWidths[button];
    out->top = tb.top + 6; out->bottom = tb.bottom - 6;
    return TRUE;
}

static int SelectorHitButton(HWND hwnd, POINT p)
{
    int i; RECT r;
    for (i = 0; i < SELBTN_COUNT; ++i) {
        SelectorButtonRect(hwnd, i, &r);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

static void SelectorSetMode(HWND hwnd, BOOL recording, BOOL gif)
{
    g_selectingRecord = recording;
    g_selectingGif = gif;
    InvalidateRect(hwnd, NULL, FALSE);
}

static void SelectorButtonAction(HWND hwnd, int button)
{
    UINT command = 0;
    switch (button) {
    case SELBTN_REGION:
        SelectorSetMode(hwnd, FALSE, FALSE);
        return;
    case SELBTN_FULLSHOT: command = CMD_SCREENSHOT_FULL; break;
    case SELBTN_RECORD:
        SelectorSetMode(hwnd, TRUE, FALSE);
        return;
    case SELBTN_FULLREC: command = CMD_RECORD_FULL; break;
    case SELBTN_GIF:
        SelectorSetMode(hwnd, FALSE, TRUE);
        return;
    case SELBTN_GIFFULL: command = CMD_GIF_FULL; break;
    case SELBTN_CLOSE:
        DestroyWindow(hwnd);
        PostMessage(g_main, WM_SELECTION_CANCEL, 0, 0);
        return;
    default: return;
    }
    /* Destroy the dimming overlay before capturing/recording the full desktop. */
    DestroyWindow(hwnd);
    PostMessage(g_main, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

static void PaintSelectorToolbar(HWND hwnd, HDC dc)
{
    RECT tb, r;
    HBRUSH bg, buttonBrush;
    HPEN edge;
    HGDIOBJ oldBrush, oldPen;
    int i, active;
    tb = SelectorToolbarRect(hwnd);
    bg = CreateSolidBrush(RGB(28, 28, 30));
    edge = CreatePen(PS_SOLID, 1, RGB(90, 90, 94));
    oldBrush = SelectObject(dc, bg); oldPen = SelectObject(dc, edge);
    RoundRect(dc, tb.left, tb.top, tb.right, tb.bottom, 10, 10);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(bg); DeleteObject(edge);
    active = g_selectingGif ? SELBTN_GIF : (g_selectingRecord ? SELBTN_RECORD : SELBTN_REGION);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < SELBTN_COUNT; ++i) {
        SelectorButtonRect(hwnd, i, &r);
        if (i == active) buttonBrush = CreateSolidBrush(RGB(55, 90, 130));
        else if (i == g_selectorHot) buttonBrush = CreateSolidBrush(RGB(72, 72, 78));
        else buttonBrush = CreateSolidBrush(RGB(44, 44, 48));
        edge = CreatePen(PS_SOLID, 1, RGB(98, 98, 104));
        oldBrush = SelectObject(dc, buttonBrush); oldPen = SelectObject(dc, edge);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, 5, 5);
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(buttonBrush); DeleteObject(edge);
        SetTextColor(dc, RGB(250, 250, 252));
        DrawTextA(dc, g_selectorButtonLabels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static LRESULT CALLBACK SelectorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static POINT start;
    static POINT current;
    static BOOL dragging = FALSE;
    RECT rc, tb;
    HDC dc;
    PAINTSTRUCT ps;
    switch (msg) {
    case WM_CREATE:
        dragging = FALSE; g_selectorHot = -1;
        return 0;
    case WM_LBUTTONDOWN:
        current.x = GET_X_LPARAM(lp); current.y = GET_Y_LPARAM(lp);
        {
            int hit = SelectorHitButton(hwnd, current);
            if (hit >= 0) { SelectorButtonAction(hwnd, hit); return 0; }
        }
        tb = SelectorToolbarRect(hwnd);
        if (PtInRect(&tb, current)) return 0;
        start = current; dragging = TRUE; SetCapture(hwnd); InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEMOVE:
        if (dragging) {
            current.x = GET_X_LPARAM(lp); current.y = GET_Y_LPARAM(lp);
            InvalidateRect(hwnd, NULL, FALSE);
        } else {
            POINT p; int hot;
            p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
            hot = SelectorHitButton(hwnd, p);
            if (hot != g_selectorHot) { g_selectorHot = hot; tb = SelectorToolbarRect(hwnd); InvalidateRect(hwnd, &tb, FALSE); }
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging) {
            dragging = FALSE; ReleaseCapture(); current.x = GET_X_LPARAM(lp); current.y = GET_Y_LPARAM(lp);
            GetWindowRect(hwnd, &rc);
            g_recordSelection.rect.left = (start.x < current.x ? start.x : current.x) + rc.left;
            g_recordSelection.rect.top = (start.y < current.y ? start.y : current.y) + rc.top;
            g_recordSelection.rect.right = (start.x > current.x ? start.x : current.x) + rc.left;
            g_recordSelection.rect.bottom = (start.y > current.y ? start.y : current.y) + rc.top;
            g_recordSelection.recording = g_selectingRecord;
            g_recordSelection.gif = g_selectingGif;
            DestroyWindow(hwnd); g_selector = NULL;
            PostMessage(g_main, WM_SELECTION_DONE, 0, 0);
        }
        return 0;
    case WM_RBUTTONUP:
        if (dragging) ReleaseCapture(); dragging = FALSE;
        DestroyWindow(hwnd); g_selector = NULL; PostMessage(g_main, WM_SELECTION_CANCEL, 0, 0); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (dragging) ReleaseCapture(); dragging = FALSE;
            DestroyWindow(hwnd); g_selector = NULL; PostMessage(g_main, WM_SELECTION_CANCEL, 0, 0); return 0;
        }
        if (wp == '1') { SelectorButtonAction(hwnd, SELBTN_REGION); return 0; }
        if (wp == '2') { SelectorButtonAction(hwnd, SELBTN_FULLSHOT); return 0; }
        if (wp == '3') { SelectorButtonAction(hwnd, SELBTN_RECORD); return 0; }
        if (wp == '4') { SelectorButtonAction(hwnd, SELBTN_FULLREC); return 0; }
        if (wp == '5') { SelectorButtonAction(hwnd, SELBTN_GIF); return 0; }
        if (wp == '6') { SelectorButtonAction(hwnd, SELBTN_GIFFULL); return 0; }
        break;
    case WM_PAINT:
        dc = BeginPaint(hwnd, &ps); GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        PaintSelectorToolbar(hwnd, dc);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255,255,255));
        if (g_selectingGif) {
            const char *hint = "Drag to select GIF area (15 FPS, max 15 seconds) - Esc cancels";
            TextOutA(dc, 18, 62, hint, lstrlenA(hint));
        } else if (g_selectingRecord) {
            const char *hint = "Drag to choose AVI recording area - Esc cancels";
            TextOutA(dc, 18, 62, hint, lstrlenA(hint));
        } else {
            const char *hint = "Drag to capture a region - choose Full shot for the whole desktop";
            TextOutA(dc, 18, 62, hint, lstrlenA(hint));
        }
        if (dragging) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 70, 70));
            HGDIOBJ oldPen = SelectObject(dc, pen); HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, (start.x < current.x ? start.x : current.x), (start.y < current.y ? start.y : current.y),
                (start.x > current.x ? start.x : current.x), (start.y > current.y ? start.y : current.y));
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
        }
        EndPaint(hwnd, &ps); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: if (g_selector == hwnd) g_selector = NULL; return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void BeginSelectionMode(BOOL recording, BOOL gif)
{
    int x = VirtualLeft(), y = VirtualTop(), w = VirtualWidth(), h = VirtualHeight();
    if (g_recording) return;
    if (g_selector) { SetForegroundWindow(g_selector); return; }
    g_selectingRecord = recording;
    g_selectingGif = gif;
    g_selector = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        "ScreenshotServiceClassic.Selector", "ScreenshotService Classic - Capture", WS_POPUP,
        x, y, w, h, g_main, NULL, g_instance, NULL);
    if (!g_selector) return;
    SetLayeredWindowAttributes(g_selector, RGB(0,0,0), 155, LWA_ALPHA);
    ShowWindow(g_selector, SW_SHOW); SetForegroundWindow(g_selector); SetFocus(g_selector);
}

void BeginSelection(BOOL recording)
{
    BeginSelectionMode(recording, FALSE);
}

void BeginGifSelection(void)
{
    BeginSelectionMode(FALSE, TRUE);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        RegisterHotKey(hwnd, HK_SCREENSHOT, 0, VK_SNAPSHOT);
        RegisterHotKey(hwnd, HK_RECORD_AREA, MOD_SHIFT, VK_SNAPSHOT);
        RegisterHotKey(hwnd, HK_RECORD_FULL, MOD_ALT, VK_SNAPSHOT);
        RegisterHotKey(hwnd, HK_REGION_SHOT, MOD_WIN | MOD_SHIFT, 'S');
        return 0;
    case WM_HOTKEY:
        if (wp == HK_SCREENSHOT) HandleCaptureHotkey(FALSE);
        else if (wp == HK_RECORD_AREA) HandleCaptureHotkey(TRUE);
        else if (wp == HK_RECORD_FULL) {
            if (g_recording) StopRecording();
            else { RECT rc; rc.left = VirtualLeft(); rc.top = VirtualTop(); rc.right = rc.left + VirtualWidth(); rc.bottom = rc.top + VirtualHeight(); StartRecording(rc, TRUE); }
        } else if (wp == HK_REGION_SHOT) BeginSelection(FALSE);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_RECORD) WriteRecordingFrame();
        return 0;
    case WM_SELECTION_DONE:
        if (g_recordSelection.gif) StartGifRecording(g_recordSelection.rect, FALSE);
        else if (g_recordSelection.recording) StartRecording(g_recordSelection.rect, FALSE);
        else DoScreenshot(&g_recordSelection.rect);
        return 0;
    case WM_SELECTION_CANCEL: return 0;
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) ShowTrayMenu();
        else if (lp == WM_LBUTTONDBLCLK) HandleCaptureHotkey(FALSE);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case CMD_SCREENSHOT: HandleCaptureHotkey(FALSE); break;
        case CMD_REGION_SHOT: BeginSelection(FALSE); break;
        case CMD_SCREENSHOT_FULL: DoScreenshot(NULL); break;
        case CMD_GIF_REGION: if (!g_recording) BeginGifSelection(); break;
        case CMD_GIF_FULL:
            if (!g_recording) { RECT grc; grc.left = VirtualLeft(); grc.top = VirtualTop(); grc.right = grc.left + VirtualWidth(); grc.bottom = grc.top + VirtualHeight(); StartGifRecording(grc, TRUE); }
            break;
        case CMD_RECORD_FULL:
            if (g_recording) StopRecording();
            else { RECT rc; rc.left = VirtualLeft(); rc.top = VirtualTop(); rc.right = rc.left + VirtualWidth(); rc.bottom = rc.top + VirtualHeight(); StartRecording(rc, TRUE); }
            break;
        case CMD_RECORD_REGION: if (!g_recording) BeginSelection(TRUE); break;
        case CMD_STOP_RECORD: StopRecording(); break;
        case CMD_ABOUT:
            MessageBoxA(hwnd, g_lowResourceMode ? "ScreenshotService Classic\nLow-resource profile: AVI 10 FPS max 1024x768; GIF 8 FPS max 800x600.\nPrtScn opens the capture toolbar; Alt+PrtScn toggles full AVI.\nShift+PrtScn selects AVI area. GIF max 15 seconds.\nSystem audio depends on the default WinMM recording source." : "ScreenshotService Classic\nLegacy Win32 edition for Windows XP, Vista, 7 and 8.\nPrtScn opens the capture toolbar; Alt+PrtScn toggles full AVI.\nShift+PrtScn selects AVI area. GIF 15 FPS, max 15 seconds.\nA Stop button is shown during recording.\nSystem audio depends on the default WinMM recording source.", APP_NAME, MB_OK | MB_ICONINFORMATION);
            break;
        case CMD_EXIT:
            if (g_recording) { int r = MessageBoxA(hwnd, "Stop and save the current recording before exiting?", APP_NAME, MB_YESNO | MB_ICONQUESTION); if (r == IDYES) { g_quitAfterRecord = TRUE; StopRecording(); } }
            else PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        }
        return 0;
    case WM_CLOSE:
        if (g_recording) { g_quitAfterRecord = TRUE; StopRecording(); return 0; }
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, HK_SCREENSHOT); UnregisterHotKey(hwnd, HK_RECORD_AREA); UnregisterHotKey(hwnd, HK_RECORD_FULL); UnregisterHotKey(hwnd, HK_REGION_SHOT);
        Shell_NotifyIconA(NIM_DELETE, &g_nid); PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void HandleCaptureHotkey(BOOL region)
{
    if (g_recording) { StopRecording(); return; }
    BeginSelection(region ? TRUE : FALSE);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    HANDLE mutex;
    (void)prev; (void)cmd; (void)show;
    g_instance = inst;
    DetectLowResourceMode();
    mutex = CreateMutexA(NULL, TRUE, "ScreenshotServiceClassic.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    ZeroMemory(&wc, sizeof(wc)); wc.lpfnWndProc = MainProc; wc.hInstance = inst;
    wc.lpszClassName = APP_CLASS; wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(101));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wc);
    wc.lpfnWndProc = SelectorProc; wc.lpszClassName = "ScreenshotServiceClassic.Selector"; wc.hCursor = LoadCursor(NULL, IDC_CROSS); RegisterClassA(&wc);
    wc.lpfnWndProc = RecordBarProc; wc.lpszClassName = RECORD_BAR_CLASS; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); RegisterClassA(&wc);
    wc.lpfnWndProc = RecordOutlineProc; wc.lpszClassName = RECORD_OUTLINE_CLASS; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassA(&wc);
    wc.lpfnWndProc = MainProc; wc.lpszClassName = APP_CLASS;
    g_main = CreateWindowA(APP_CLASS, APP_NAME, WS_POPUP, 0,0,1,1, NULL,NULL,inst,NULL);
    if (!g_main) { if (mutex) CloseHandle(mutex); return 1; }
    EnsureOutputDirectory(); AddTrayIcon();
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (g_recording) StopRecording();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return (int)msg.wParam;
}
