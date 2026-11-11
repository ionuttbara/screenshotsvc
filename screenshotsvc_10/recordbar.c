/*
 * recordbar.c - compact Game-Bar-like controls while a recording runs.
 * The bar floats at the top-left of the captured monitor and stays out of the
 * tab order; buttons send commands to the owning UI thread.
 */
#include "screenshotservice.h"
#include "recordbar.h"
#include "record.h"
#include "lang.h"

#include <windowsx.h>
#include <commctrl.h>
#include <strsafe.h>

#define BAR_CLASS    L"ScreenshotService.RecordBar"
#define FRAME_CLASS  L"ScreenshotService.RecordFrame"
#define TIMER_TICK   1
#define FRAME_PX     3
#define HOT_NONE     0
#define HOT_PAUSE    1
#define HOT_MIC      2
#define HOT_PEN      3
#define HOT_STOP     4

static HWND  g_bar;
static HWND  g_frame;
static HWND  g_owner;
static UINT  g_stopMsg;
static HFONT g_font;
static RECT  g_pauseRect;
static RECT  g_micRect;
static RECT  g_stopRect;
static RECT  g_penRect;
static BOOL  g_pauseHot;
static BOOL  g_micHot;
static BOOL  g_stopHot;
static BOOL  g_penHot;
static BOOL  g_penEnabled;
static HWND  g_tip;
static BOOL  g_micAvailable;
static BOOL  g_paused;
static BOOL  g_micMuted;
static BOOL  g_saving;
static int   g_savingPercent;

static int Scaled(int logical, UINT dpi) { return MulDiv(logical, (int)dpi, 96); }

static void ExcludeFromCapture(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *PFNSetWindowDisplayAffinity)(HWND, DWORD);
    PFNSetWindowDisplayAffinity setAffinity = user32
        ? (PFNSetWindowDisplayAffinity)(void *)GetProcAddress(user32, "SetWindowDisplayAffinity")
        : NULL;
    /* WDA_EXCLUDEFROMCAPTURE is available on Windows 10 2004 and newer. */
    if (setAffinity && hwnd)
        (void)setAffinity(hwnd, 0x00000011u);
}

static UINT DpiFor(HWND hwnd)
{
    HMODULE user = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI *PFN)(HWND);
    PFN f = user ? (PFN)(void *)GetProcAddress(user, "GetDpiForWindow") : NULL;
    UINT dpi = f ? f(hwnd) : 96;
    return dpi ? dpi : 96;
}

static BOOL Hit(const RECT *r, POINT p)
{
    return r->right > r->left && r->bottom > r->top && PtInRect(r, p);
}

/* ------------------------------------------------------------- the frame */
static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        HBRUSH br = CreateSolidBrush(RGB(0xE0, 0x30, 0x30));
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CreateFrame(const RECT *region)
{
    RECT outer = *region;
    HRGN all, hole;
    int w, h;

    InflateRect(&outer, FRAME_PX, FRAME_PX);
    w = outer.right - outer.left;
    h = outer.bottom - outer.top;
    g_frame = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                              FRAME_CLASS, L"", WS_POPUP,
                              outer.left, outer.top, w, h, NULL, NULL,
                              GetModuleHandleW(NULL), NULL);
    if (!g_frame)
        return;
    all = CreateRectRgn(0, 0, w, h);
    hole = CreateRectRgn(FRAME_PX, FRAME_PX, w - FRAME_PX, h - FRAME_PX);
    CombineRgn(all, all, hole, RGN_DIFF);
    SetWindowRgn(g_frame, all, TRUE);
    DeleteObject(hole);
    ExcludeFromCapture(g_frame);
    ShowWindow(g_frame, SW_SHOWNOACTIVATE);
}

/* --------------------------------------------------------------- the bar */
static void PaintButton(HDC dc, const RECT *r, const wchar_t *label, BOOL hot)
{
    HBRUSH back = CreateSolidBrush(hot ? RGB(0x4A, 0x4A, 0x53) : RGB(0x31, 0x31, 0x37));
    HBRUSH edge = CreateSolidBrush(RGB(0x55, 0x55, 0x5E));
    FillRect(dc, r, back);
    FrameRect(dc, r, edge);
    SetTextColor(dc, RGB(0xF0, 0xF2, 0xF6));
    DrawTextW(dc, label, -1, (RECT *)r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    DeleteObject(back);
    DeleteObject(edge);
}

static void PaintBar(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc, dot, timerRc;
    UINT dpi = DpiFor(hwnd);
    HBRUSH back = CreateSolidBrush(RGB(0x20, 0x20, 0x24));
    HBRUSH red = CreateSolidBrush(RGB(0xE0, 0x30, 0x30));
    HGDIOBJ oldFont;
    wchar_t time[32];
    DWORD ms = Record_ElapsedMs();

    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, back);

    dot.left = Scaled(10, dpi);
    dot.top = (rc.bottom - Scaled(9, dpi)) / 2;
    dot.right = dot.left + Scaled(9, dpi);
    dot.bottom = dot.top + Scaled(9, dpi);
    {
        HGDIOBJ oldBrush = SelectObject(dc, red);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, dot.left, dot.top, dot.right + 1, dot.bottom + 1);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
    }

    oldFont = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    if (g_saving) {
        StringCchPrintfW(time, ARRAYSIZE(time), L"Saving recording %d%%", g_savingPercent);
        SetTextColor(dc, RGB(0xF0, 0xF2, 0xF6));
        timerRc = rc;
        timerRc.left = dot.right + Scaled(7, dpi);
        timerRc.right = rc.right - Scaled(14, dpi);
        DrawTextW(dc, time, -1, &timerRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        DeleteObject(back);
        DeleteObject(red);
        EndPaint(hwnd, &ps);
        return;
    }
    StringCchPrintfW(time, ARRAYSIZE(time), L"%lu:%02lu",
                     (unsigned long)(ms / 60000), (unsigned long)((ms / 1000) % 60));
    timerRc = rc;
    timerRc.left = dot.right + Scaled(7, dpi);
    timerRc.right = g_pauseRect.left - Scaled(5, dpi);
    SetTextColor(dc, g_paused ? RGB(0xFF, 0xD0, 0x70) : RGB(0xF0, 0xF2, 0xF6));
    DrawTextW(dc, time, -1, &timerRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    PaintButton(dc, &g_pauseRect, g_paused ? L"Resume" : L"Pause", g_pauseHot);
    if (g_micAvailable)
        PaintButton(dc, &g_micRect, g_micMuted ? L"Mic: Off" : L"Mic: On", g_micHot);
    PaintButton(dc, &g_penRect, g_penEnabled ? L"Draw: On" : L"Draw", g_penHot);
    PaintButton(dc, &g_stopRect, L"Stop", g_stopHot);

    SelectObject(dc, oldFont);
    DeleteObject(back);
    DeleteObject(red);
    EndPaint(hwnd, &ps);
}

static int HotAt(POINT p)
{
    if (g_saving) return HOT_NONE;
    if (Hit(&g_pauseRect, p)) return HOT_PAUSE;
    if (g_micAvailable && Hit(&g_micRect, p)) return HOT_MIC;
    if (Hit(&g_penRect, p)) return HOT_PEN;
    if (Hit(&g_stopRect, p)) return HOT_STOP;
    return HOT_NONE;
}

static LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_tip && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
        MSG m;
        m.hwnd = hwnd; m.message = msg; m.wParam = wp; m.lParam = lp;
        m.time = GetMessageTime();
        GetCursorPos(&m.pt);
        SendMessageW(g_tip, TTM_RELAYEVENT, 0, (LPARAM)&m);
    }
    switch (msg) {
    case WM_PAINT:
        PaintBar(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == TIMER_TICK) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        POINT p;
        int hot;
        BOOL changed;
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        hot = HotAt(p);
        changed = g_pauseHot != (hot == HOT_PAUSE) ||
                  g_micHot != (hot == HOT_MIC) ||
                  g_stopHot != (hot == HOT_STOP) ||
                  g_penHot != (hot == HOT_PEN);
        g_pauseHot = hot == HOT_PAUSE;
        g_micHot = hot == HOT_MIC;
        g_stopHot = hot == HOT_STOP;
        g_penHot = hot == HOT_PEN;
        if (changed) InvalidateRect(hwnd, NULL, FALSE);
        {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd; tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_pauseHot = g_micHot = g_stopHot = g_penHot = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        POINT p;
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        if (!g_owner) return 0;
        switch (HotAt(p)) {
        case HOT_PAUSE: PostMessageW(g_owner, WM_BSNIP_PAUSEREC, 0, 0); break;
        case HOT_MIC:   if (g_micAvailable) PostMessageW(g_owner, WM_BSNIP_MICMUTE, 0, 0); break;
        case HOT_PEN:   PostMessageW(g_owner, WM_BSNIP_PEN, 0, 0); break;
        case HOT_STOP:  PostMessageW(g_owner, g_stopMsg, 0, 0); break;
        default: break;
        }
        return 0;
    }
    case WM_NCHITTEST: {
        POINT p;
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        ScreenToClient(hwnd, &p);
        return HotAt(p) != HOT_NONE ? HTCLIENT : HTCAPTION;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RegisterClasses(void)
{
    static BOOL done;
    WNDCLASSEXW wc;
    if (done) return;
    done = TRUE;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpfnWndProc = BarProc;
    wc.lpszClassName = BAR_CLASS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = FrameProc;
    wc.lpszClassName = FRAME_CLASS;
    RegisterClassExW(&wc);
}

BOOL RecordBar_Show(HWND owner, const RECT *region, UINT stopMsg, BOOL microphoneAvailable)
{
    MONITORINFO mi;
    UINT dpi = 96;
    int w, h, x, y;
    HMONITOR mon;
    BOOL haveMonitorInfo, fullMonitor;

    if (!region) return FALSE;
    RecordBar_Hide();
    RegisterClasses();
    g_owner = owner;
    g_stopMsg = stopMsg;
    g_micAvailable = microphoneAvailable;
    g_paused = FALSE;
    g_micMuted = FALSE;
    g_saving = FALSE;
    g_savingPercent = 0;
    g_pauseHot = g_micHot = g_stopHot = g_penHot = FALSE;
    g_penEnabled = FALSE;

    mon = MonitorFromRect(region, MONITOR_DEFAULTTONEAREST);
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    haveMonitorInfo = GetMonitorInfoW(mon, &mi);
    if (!haveMonitorInfo) {
        SetRect(&mi.rcWork, region->left, region->top, region->right, region->bottom);
        mi.rcMonitor = mi.rcWork;
    }
    fullMonitor = region->left == mi.rcMonitor.left &&
                  region->top == mi.rcMonitor.top &&
                  region->right == mi.rcMonitor.right &&
                  region->bottom == mi.rcMonitor.bottom;
    {
        HMODULE shc = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (shc) {
            typedef HRESULT (WINAPI *PFN)(HMONITOR, int, UINT *, UINT *);
            PFN f = (PFN)(void *)GetProcAddress(shc, "GetDpiForMonitor");
            UINT dx = 96, dy = 96;
            if (f && SUCCEEDED(f(mon, 0, &dx, &dy)) && dx) dpi = dx;
            FreeLibrary(shc);
        }
    }

    w = Scaled(microphoneAvailable ? 500 : 424, dpi);
    h = Scaled(38, dpi);
    SetRect(&g_stopRect, w - Scaled(58, dpi), Scaled(5, dpi),
            w - Scaled(6, dpi), h - Scaled(5, dpi));
    SetRect(&g_penRect, g_stopRect.left - Scaled(72, dpi), Scaled(5, dpi),
            g_stopRect.left - Scaled(5, dpi), h - Scaled(5, dpi));
    if (microphoneAvailable) {
        SetRect(&g_micRect, g_penRect.left - Scaled(68, dpi), Scaled(5, dpi),
                g_penRect.left - Scaled(5, dpi), h - Scaled(5, dpi));
        SetRect(&g_pauseRect, g_micRect.left - Scaled(68, dpi), Scaled(5, dpi),
                g_micRect.left - Scaled(5, dpi), h - Scaled(5, dpi));
    } else {
        SetRectEmpty(&g_micRect);
        SetRect(&g_pauseRect, g_penRect.left - Scaled(68, dpi), Scaled(5, dpi),
                g_penRect.left - Scaled(5, dpi), h - Scaled(5, dpi));
    }

    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-Scaled(12, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    if (fullMonitor) {
        /* In full-screen capture the controls are tucked into the top-left. */
        x = mi.rcMonitor.left + Scaled(8, dpi);
        y = mi.rcMonitor.top + Scaled(8, dpi);
    } else {
        /* For a selected region, place controls just below the caret/frame.
           If there is not enough room, place them just above instead. Clamp
           horizontally to the monitor work area so the bar stays reachable. */
        int workLeft = mi.rcWork.left + Scaled(4, dpi);
        int workRight = mi.rcWork.right - w - Scaled(4, dpi);
        x = region->left + ((region->right - region->left) - w) / 2;
        if (x < workLeft) x = workLeft;
        if (x > workRight) x = workRight;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (workRight < mi.rcWork.left) x = mi.rcWork.left;

        if (region->bottom + Scaled(8, dpi) + h <= mi.rcWork.bottom)
            y = region->bottom + Scaled(8, dpi);
        else if (region->top - Scaled(8, dpi) - h >= mi.rcWork.top)
            y = region->top - Scaled(8, dpi) - h;
        else {
            y = region->bottom + Scaled(4, dpi);
            if (y + h > mi.rcWork.bottom)
                y = mi.rcWork.bottom - h;
            if (y < mi.rcWork.top) y = mi.rcWork.top;
        }
    }
    g_bar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            BAR_CLASS, L"", WS_POPUP, x, y, w, h, NULL, NULL,
                            GetModuleHandleW(NULL), NULL);
    if (!g_bar) {
        RecordBar_Hide();
        return FALSE;
    }
    {
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1,
                                      Scaled(9, dpi), Scaled(9, dpi));
        if (rgn) SetWindowRgn(g_bar, rgn, TRUE);
    }
    /* Keep the controls visible to the user but out of the captured video. */
    ExcludeFromCapture(g_bar);
    if (!fullMonitor)
        CreateFrame(region);
    {
        INITCOMMONCONTROLSEX icc;
        TOOLINFOW ti;
        icc.dwSize = sizeof(icc); icc.dwICC = ICC_WIN95_CLASSES;
        InitCommonControlsEx(&icc);
        g_tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, NULL,
                                WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                g_bar, NULL, GetModuleHandleW(NULL), NULL);
        if (g_tip) {
            ZeroMemory(&ti, sizeof(ti));
            ti.cbSize = sizeof(ti); ti.hwnd = g_bar; ti.uId = 1;
            ti.rect = g_stopRect; ti.lpszText = (LPWSTR)Lang_Str(STR_TIP_REC_STOP);
            SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    SetTimer(g_bar, TIMER_TICK, 250, NULL);
    return TRUE;
}

void RecordBar_SetPaused(BOOL paused)
{
    g_paused = paused;
    if (g_bar) InvalidateRect(g_bar, NULL, FALSE);
}

void RecordBar_SetMicMuted(BOOL muted)
{
    g_micMuted = muted;
    if (g_bar) InvalidateRect(g_bar, NULL, FALSE);
}

void RecordBar_SetSaving(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_saving = TRUE;
    g_savingPercent = percent;
    if (g_bar) InvalidateRect(g_bar, NULL, FALSE);
}

void RecordBar_Hide(void)
{
    if (g_tip) { DestroyWindow(g_tip); g_tip = NULL; }
    if (g_bar) {
        KillTimer(g_bar, TIMER_TICK);
        DestroyWindow(g_bar);
        g_bar = NULL;
    }
    if (g_frame) {
        DestroyWindow(g_frame);
        g_frame = NULL;
    }
    if (g_font) {
        DeleteObject(g_font);
        g_font = NULL;
    }
    g_owner = NULL;
    g_micAvailable = FALSE;
    g_saving = FALSE;
    g_savingPercent = 0;
    g_paused = g_micMuted = FALSE;
    SetRectEmpty(&g_pauseRect);
    SetRectEmpty(&g_micRect);
    SetRectEmpty(&g_stopRect);
    SetRectEmpty(&g_penRect);
    g_penEnabled = FALSE;
}

void RecordBar_SetPenEnabled(BOOL enabled)
{
    g_penEnabled = enabled;
    if (g_bar) InvalidateRect(g_bar, NULL, FALSE);
}


/* Used by the low-level live-pen mouse hook so the recording controls remain
   clickable while interactions with the captured desktop are blocked. */
BOOL RecordBar_ContainsPoint(POINT screenPoint)
{
    RECT r;
    return g_bar && IsWindow(g_bar) && GetWindowRect(g_bar, &r) &&
           PtInRect(&r, screenPoint);
}
