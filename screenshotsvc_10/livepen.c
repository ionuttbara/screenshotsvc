#include "livepen.h"
#include <windowsx.h>
#include <string.h>

#define LIVEPEN_CLASS L"ScreenshotService.LivePen"
#define LIVEPEN_STROKE 4
#define LIVEPEN_MARGIN 8

typedef struct {
    HWND hwnd;
    RECT region;
    HDC dc, cursorDC;
    HBITMAP dib, oldBmp, cursorDib, cursorOld;
    BYTE *bits, *cursorBits;
    COLORREF color;
    BOOL enabled, drawing, cursorValid;
    POINT last, cursorPos, inputCursorPos;
    BOOL inputCursorValid;
    HCURSOR cursorHandle;
    RECT cursorRect;
    UINT32 width, height;
} LivePen;
static LivePen g_lp;

static BOOL ClipToCanvas(RECT *r)
{
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > (LONG)g_lp.width) r->right = (LONG)g_lp.width;
    if (r->bottom > (LONG)g_lp.height) r->bottom = (LONG)g_lp.height;
    return r->left < r->right && r->top < r->bottom;
}

/* Copy only dirty rows. This avoids copying a 4K-sized DIB 60 times per second. */
static void CopyBaseRectToCursor(RECT rect)
{
    LONG y;
    SIZE_T stride = (SIZE_T)g_lp.width * 4;
    if (!g_lp.bits || !g_lp.cursorBits || !ClipToCanvas(&rect)) return;
    for (y = rect.top; y < rect.bottom; ++y) {
        SIZE_T off = (SIZE_T)y * stride + (SIZE_T)rect.left * 4;
        memcpy(g_lp.cursorBits + off, g_lp.bits + off,
               (SIZE_T)(rect.right - rect.left) * 4);
    }
}

static void LivePenClearCursor(HWND hwnd)
{
    if (!g_lp.cursorValid) return;
    CopyBaseRectToCursor(g_lp.cursorRect);
    InvalidateRect(hwnd, &g_lp.cursorRect, FALSE);
    g_lp.cursorValid = FALSE;
    g_lp.cursorHandle = NULL;
}

static void LivePenUpdateCursorAt(HWND hwnd, POINT screenPos)
{
    CURSORINFO ci;
    ICONINFO ii;
    BITMAP bm;
    int iconW = 32, iconH = 32, x, y;
    RECT bounds;
    BOOL visible;

    ZeroMemory(&ci, sizeof(ci));
    ci.cbSize = sizeof(ci);
    visible = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0 &&
              ci.hCursor != NULL;

    if (visible && g_lp.cursorValid && g_lp.cursorHandle == ci.hCursor &&
        g_lp.cursorPos.x == screenPos.x && g_lp.cursorPos.y == screenPos.y)
        return;

    LivePenClearCursor(hwnd);
    if (!visible) return;

    ZeroMemory(&ii, sizeof(ii));
    if (GetIconInfo(ci.hCursor, &ii)) {
        HBITMAP dimBmp = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
        if (dimBmp && GetObjectW(dimBmp, (int)sizeof(bm), &bm) == (int)sizeof(bm)) {
            iconW = bm.bmWidth;
            iconH = bm.bmHeight;
            if (!ii.hbmColor && iconH > 1) iconH /= 2;
        }
        x = screenPos.x - g_lp.region.left - (int)ii.xHotspot;
        y = screenPos.y - g_lp.region.top - (int)ii.yHotspot;
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    } else {
        x = screenPos.x - g_lp.region.left;
        y = screenPos.y - g_lp.region.top;
    }

    SetRect(&bounds, x, y, x + iconW, y + iconH);
    if (!ClipToCanvas(&bounds)) return;

    DrawIconEx(g_lp.cursorDC, x, y, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
    g_lp.cursorRect = bounds;
    g_lp.cursorPos = screenPos;
    g_lp.cursorHandle = ci.hCursor;
    g_lp.cursorValid = TRUE;
    InvalidateRect(hwnd, &bounds, FALSE);
}

static void LivePenUpdateCursor(HWND hwnd)
{
    POINT p;
    if (g_lp.enabled && g_lp.inputCursorValid) {
        p = g_lp.inputCursorPos;
    } else if (!GetCursorPos(&p)) {
        p.x = g_lp.region.left;
        p.y = g_lp.region.top;
    }
    LivePenUpdateCursorAt(hwnd, p);
}

static void LivePenSyncDirty(RECT dirty)
{
    CopyBaseRectToCursor(dirty);
    if (ClipToCanvas(&dirty)) InvalidateRect(g_lp.hwnd, &dirty, FALSE);
}

static void LivePenPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HBRUSH key = CreateSolidBrush(RGB(255, 0, 255));
    int w = ps.rcPaint.right - ps.rcPaint.left;
    int h = ps.rcPaint.bottom - ps.rcPaint.top;
    if (key) {
        FillRect(dc, &ps.rcPaint, key);
        DeleteObject(key);
    }
    if (g_lp.cursorDC && w > 0 && h > 0)
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, w, h,
               g_lp.cursorDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    EndPaint(hwnd, &ps);
}

static void LivePenStroke(POINT from, POINT to)
{
    HPEN pen;
    HGDIOBJ old;
    RECT dirty;
    if (!g_lp.dc) return;
    LivePenClearCursor(g_lp.hwnd);
    pen = CreatePen(PS_SOLID, LIVEPEN_STROKE, g_lp.color);
    if (!pen) return;
    old = SelectObject(g_lp.dc, pen);
    MoveToEx(g_lp.dc, from.x, from.y, NULL);
    LineTo(g_lp.dc, to.x, to.y);
    SelectObject(g_lp.dc, old);
    DeleteObject(pen);
    GdiFlush();
    dirty.left = (from.x < to.x ? from.x : to.x) - LIVEPEN_MARGIN;
    dirty.top = (from.y < to.y ? from.y : to.y) - LIVEPEN_MARGIN;
    dirty.right = (from.x > to.x ? from.x : to.x) + LIVEPEN_MARGIN + 1;
    dirty.bottom = (from.y > to.y ? from.y : to.y) + LIVEPEN_MARGIN + 1;
    LivePenSyncDirty(dirty);
    if (g_lp.hwnd) LivePenUpdateCursor(g_lp.hwnd);
}

static LRESULT CALLBACK LivePenProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT p;
    switch (msg) {
    case WM_PAINT:
        LivePenPaint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == 1 && g_lp.cursorDC && g_lp.dc) LivePenUpdateCursor(hwnd);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, g_lp.enabled ? IDC_CROSS : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (!g_lp.enabled) return 0;
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        g_lp.last = p; g_lp.drawing = TRUE; SetCapture(hwnd);
        LivePenStroke(p, p);
        return 0;
    case WM_MOUSEMOVE:
        if (g_lp.enabled && g_lp.drawing) {
            p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
            LivePenStroke(g_lp.last, p);
            g_lp.last = p;
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_lp.drawing) { g_lp.drawing = FALSE; ReleaseCapture(); }
        return 0;
    case WM_RBUTTONUP:
        /* Gesture modifiers are handled by the low-level mouse hook so they
           also work while this overlay is click-through. Plain right-clicks
           no longer toggle Draw accidentally. */
        return 0;
    case WM_CLOSE:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL LivePen_Start(const RECT *region)
{
    WNDCLASSEXW wc;
    BITMAPINFO bi;
    int w, h;
    HINSTANCE inst = GetModuleHandleW(NULL);
    SIZE_T i, n;
    if (!region) return FALSE;
    LivePen_Stop();
    ZeroMemory(&g_lp, sizeof(g_lp));
    g_lp.region = *region;
    g_lp.color = RGB(255, 0, 0);
    w = region->right - region->left;
    h = region->bottom - region->top;
    if (w < 1 || h < 1) return FALSE;
    g_lp.width = (UINT32)w;
    g_lp.height = (UINT32)h;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.hInstance = inst; wc.lpfnWndProc = LivePenProc;
    wc.lpszClassName = LIVEPEN_CLASS; wc.hCursor = LoadCursorW(NULL, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return FALSE;

    g_lp.dc = CreateCompatibleDC(NULL);
    if (!g_lp.dc) { LivePen_Stop(); return FALSE; }
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g_lp.dib = CreateDIBSection(g_lp.dc, &bi, DIB_RGB_COLORS, (void **)&g_lp.bits, NULL, 0);
    if (!g_lp.dib || !g_lp.bits) { LivePen_Stop(); return FALSE; }
    g_lp.oldBmp = (HBITMAP)SelectObject(g_lp.dc, g_lp.dib);

    g_lp.cursorDC = CreateCompatibleDC(NULL);
    if (!g_lp.cursorDC) { LivePen_Stop(); return FALSE; }
    g_lp.cursorDib = CreateDIBSection(g_lp.cursorDC, &bi, DIB_RGB_COLORS,
                                      (void **)&g_lp.cursorBits, NULL, 0);
    if (!g_lp.cursorDib || !g_lp.cursorBits) { LivePen_Stop(); return FALSE; }
    g_lp.cursorOld = (HBITMAP)SelectObject(g_lp.cursorDC, g_lp.cursorDib);

    n = (SIZE_T)w * (SIZE_T)h * 4;
    for (i = 0; i < n; i += 4) {
        g_lp.bits[i] = 255; g_lp.bits[i + 1] = 0;
        g_lp.bits[i + 2] = 255; g_lp.bits[i + 3] = 255;
    }
    memcpy(g_lp.cursorBits, g_lp.bits, n);
    g_lp.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        LIVEPEN_CLASS, L"ScreenshotService live pen", WS_POPUP,
        region->left, region->top, w, h, NULL, NULL, inst, NULL);
    if (!g_lp.hwnd) { LivePen_Stop(); return FALSE; }
    SetLayeredWindowAttributes(g_lp.hwnd, RGB(255, 0, 255), 0, LWA_COLORKEY);
    ShowWindow(g_lp.hwnd, SW_SHOWNOACTIVATE);
    LivePenUpdateCursor(g_lp.hwnd);
    SetTimer(g_lp.hwnd, 1, 16, NULL);
    return TRUE;
}

void LivePen_Toggle(void)
{
    if (!g_lp.hwnd) return;
    g_lp.enabled = !g_lp.enabled;
    g_lp.drawing = FALSE;
    if (g_lp.enabled) {
        if (GetCursorPos(&g_lp.inputCursorPos)) g_lp.inputCursorValid = TRUE;
    } else {
        g_lp.inputCursorValid = FALSE;
    }
    /* The visual canvas remains click-through. A low-level mouse hook routes
       drawing input to the UI thread and suppresses desktop/game interaction,
       including clicks over transparent pixels. */
    InvalidateRect(g_lp.hwnd, NULL, FALSE);
    LivePenUpdateCursor(g_lp.hwnd);
}


BOOL LivePen_ContainsScreenPoint(POINT screenPoint)
{
    return g_lp.hwnd != NULL && PtInRect(&g_lp.region, screenPoint);
}

static POINT LivePen_ToLocal(POINT screenPoint)
{
    POINT p;
    p.x = screenPoint.x - g_lp.region.left;
    p.y = screenPoint.y - g_lp.region.top;
    return p;
}

void LivePen_MouseDownScreen(POINT screenPoint)
{
    POINT p;
    if (!g_lp.hwnd || !g_lp.enabled) return;
    g_lp.inputCursorPos = screenPoint;
    g_lp.inputCursorValid = TRUE;
    p = LivePen_ToLocal(screenPoint);
    g_lp.last = p;
    g_lp.drawing = TRUE;
    LivePenStroke(p, p);
}

void LivePen_MouseMoveScreen(POINT screenPoint)
{
    POINT p;
    if (!g_lp.hwnd) return;
    g_lp.inputCursorPos = screenPoint;
    g_lp.inputCursorValid = g_lp.enabled;
    p = LivePen_ToLocal(screenPoint);
    if (g_lp.enabled && g_lp.drawing) {
        LivePenStroke(g_lp.last, p);
        g_lp.last = p;
    } else {
        LivePenUpdateCursorAt(g_lp.hwnd, screenPoint);
    }
}

void LivePen_MouseUpScreen(POINT screenPoint)
{
    POINT p;
    if (!g_lp.hwnd) return;
    p = LivePen_ToLocal(screenPoint);
    if (g_lp.enabled && g_lp.drawing &&
        (p.x != g_lp.last.x || p.y != g_lp.last.y))
        LivePenStroke(g_lp.last, p);
    g_lp.drawing = FALSE;
    g_lp.inputCursorPos = screenPoint;
    g_lp.inputCursorValid = g_lp.enabled;
    LivePenUpdateCursor(g_lp.hwnd);
}

void LivePen_MouseLeaveRegion(void)
{
    if (!g_lp.hwnd) return;
    g_lp.inputCursorValid = FALSE;
    LivePenClearCursor(g_lp.hwnd);
    LivePenUpdateCursor(g_lp.hwnd);
}

void LivePen_ClearStrokes(void)
{
    SIZE_T i, n;
    if (!g_lp.bits || !g_lp.cursorBits) return;
    LivePenClearCursor(g_lp.hwnd);
    n = (SIZE_T)g_lp.width * (SIZE_T)g_lp.height * 4;
    /* The layered window uses magenta as its transparent colour key. */
    for (i=0; i<n; i+=4) {
        g_lp.bits[i]=255; g_lp.bits[i+1]=0; g_lp.bits[i+2]=255; g_lp.bits[i+3]=255;
    }
    memcpy(g_lp.cursorBits, g_lp.bits, n);
    g_lp.cursorValid=FALSE;
    if (g_lp.hwnd) {
        InvalidateRect(g_lp.hwnd, NULL, FALSE);
        LivePenUpdateCursor(g_lp.hwnd);
    }
}

BOOL LivePen_IsRunning(void)
{
    return g_lp.hwnd != NULL;
}

void LivePen_Stop(void)
{
    if (g_lp.hwnd) {
        KillTimer(g_lp.hwnd, 1);
        DestroyWindow(g_lp.hwnd);
        g_lp.hwnd = NULL;
    }
    if (g_lp.dc) {
        if (g_lp.oldBmp) SelectObject(g_lp.dc, g_lp.oldBmp);
        DeleteDC(g_lp.dc);
    }
    if (g_lp.cursorDC) {
        if (g_lp.cursorOld) SelectObject(g_lp.cursorDC, g_lp.cursorOld);
        DeleteDC(g_lp.cursorDC);
    }
    if (g_lp.dib) DeleteObject(g_lp.dib);
    if (g_lp.cursorDib) DeleteObject(g_lp.cursorDib);
    ZeroMemory(&g_lp, sizeof(g_lp));
}

BOOL LivePen_IsEnabled(void)
{
    return g_lp.enabled;
}
