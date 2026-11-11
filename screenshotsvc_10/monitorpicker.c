/* monitorpicker.c - themed, remembered monitor picker for full-monitor recording. */
#define COBJMACROS
#include "screenshotservice.h"
#include "monitorpicker.h"
#include "settings.h"
#include "resource.h"
#include "theme.h"
#include "lang.h"
#include "log.h"

#include <strsafe.h>
#include <stdlib.h>
#include <limits.h>
#include <wchar.h>
#include <windowsx.h>

#define PICKER_MAX_MONITORS 32u
#define PICKER_BUTTON_BASE  4100u
#define PICKER_CLOSE_ID     (PICKER_BUTTON_BASE + PICKER_MAX_MONITORS + 1u)
#define PICKER_WIDTH        620   /* logical px; scaled for the system DPI */
#define PICKER_MARGIN       18
#define PICKER_HEADER_HEIGHT 44
#define PICKER_HINT_TOP     54
#define PICKER_HINT_HEIGHT  22
#define PICKER_ROWS_TOP     84
#define PICKER_ROW_HEIGHT   64
#define PICKER_ROW_GAP      8
#define PICKER_BOTTOM       16
#define PICKER_LABEL_LEN    384
#define MONITOR_PATH_CHARS  260
#define MONITOR_FRIENDLY_CHARS 128

#ifndef MONITORINFOF_PRIMARY
#define MONITORINFOF_PRIMARY 1
#endif

typedef struct MonitorPickerItem {
    HMONITOR hmon;
    MONITORINFOEXW info;
    int deviceNo;
    wchar_t targetPath[MONITOR_PATH_CHARS];
    wchar_t friendlyName[MONITOR_FRIENDLY_CHARS];
    wchar_t rowTitle[MONITOR_FRIENDLY_CHARS];
    wchar_t rowDetail[PICKER_LABEL_LEN];
} MonitorPickerItem;

static MonitorPickerItem g_items[PICKER_MAX_MONITORS];
static UINT g_count;
static HWND g_window;
static BOOL g_done;
static BOOL g_chosen;
static UINT g_selectedIndex;
static RECT g_selected;
static RECT g_primaryWork;
static UINT g_dpi = 96;
static int g_pickerWidth;
static int g_headerHeight;
static int g_margin;
static int g_hintTop;
static int g_hintHeight;
static int g_rowsTop;
static int g_rowHeight;
static int g_rowGap;
static int g_bottomMargin;
static int g_closeSize;
static HFONT g_fontHeader;
static HFONT g_fontRowTitle;
static HFONT g_fontDetail;

static int PickerScale(int value)
{
    return MulDiv(value, (int)(g_dpi ? g_dpi : 96), 96);
}

static void PickerCreateFonts(void)
{
    g_fontHeader = CreateFontW(-PickerScale(16), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");
    g_fontRowTitle = CreateFontW(-PickerScale(14), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");
    g_fontDetail = CreateFontW(-PickerScale(12), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");
}

static void PickerDeleteFonts(void)
{
    if (g_fontHeader) { DeleteObject(g_fontHeader); g_fontHeader = NULL; }
    if (g_fontRowTitle) { DeleteObject(g_fontRowTitle); g_fontRowTitle = NULL; }
    if (g_fontDetail) { DeleteObject(g_fontDetail); g_fontDetail = NULL; }
}

/* Match the GDI source name used by EnumDisplayMonitors to the stable target
   path reported by DisplayConfig. This keeps a saved physical monitor from
   silently turning into a different display when DISPLAY1/2 numbering shifts. */
static BOOL QueryTargetForDisplay(const wchar_t *gdiName,
                                  wchar_t *pathOut, size_t pathCch,
                                  wchar_t *friendlyOut, size_t friendlyCch)
{
    UINT attempt;
    if (!pathOut || pathCch == 0) return FALSE;
    pathOut[0] = 0;
    if (friendlyOut && friendlyCch) friendlyOut[0] = 0;

    for (attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0, i;
        DISPLAYCONFIG_PATH_INFO *paths = NULL;
        DISPLAYCONFIG_MODE_INFO *modes = NULL;
        LONG rc;

        rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (rc != ERROR_SUCCESS) return FALSE;

        paths = (DISPLAYCONFIG_PATH_INFO *)HeapAlloc(GetProcessHeap(), 0,
            (SIZE_T)(pathCount ? pathCount : 1) * sizeof(*paths));
        modes = (DISPLAYCONFIG_MODE_INFO *)HeapAlloc(GetProcessHeap(), 0,
            (SIZE_T)(modeCount ? modeCount : 1) * sizeof(*modes));
        if (!paths || !modes) {
            if (paths) HeapFree(GetProcessHeap(), 0, paths);
            if (modes) HeapFree(GetProcessHeap(), 0, modes);
            return FALSE;
        }

        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths,
                                &modeCount, modes, NULL);
        if (rc == ERROR_INSUFFICIENT_BUFFER) {
            HeapFree(GetProcessHeap(), 0, paths);
            HeapFree(GetProcessHeap(), 0, modes);
            continue;
        }
        if (rc != ERROR_SUCCESS) {
            HeapFree(GetProcessHeap(), 0, paths);
            HeapFree(GetProcessHeap(), 0, modes);
            return FALSE;
        }

        for (i = 0; i < pathCount; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target;

            ZeroMemory(&source, sizeof(source));
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = paths[i].sourceInfo.adapterId;
            source.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
                continue;
            if (_wcsicmp(source.viewGdiDeviceName, gdiName) != 0)
                continue;

            ZeroMemory(&target, sizeof(target));
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = paths[i].targetInfo.adapterId;
            target.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
                continue;

            if (target.monitorDevicePath[0])
                StringCchCopyW(pathOut, pathCch, target.monitorDevicePath);
            if (friendlyOut && friendlyCch && target.monitorFriendlyDeviceName[0])
                StringCchCopyW(friendlyOut, friendlyCch,
                               target.monitorFriendlyDeviceName);
            break;
        }

        HeapFree(GetProcessHeap(), 0, paths);
        HeapFree(GetProcessHeap(), 0, modes);
        return pathOut[0] != 0;
    }
    return FALSE;
}

static int DeviceNumber(const wchar_t *name)
{
    const wchar_t *p;
    unsigned long n;
    if (!name) return 0x7fffffff;
    p = wcsstr(name, L"DISPLAY");
    if (!p) return 0x7fffffff;
    p += 7;
    n = wcstoul(p, NULL, 10);
    if (!n || n > 0x7fffffffUL) return 0x7fffffff;
    return (int)n;
}

static BOOL CALLBACK EnumMonitor(HMONITOR mon, HDC dc, LPRECT clip, LPARAM lp)
{
    MONITORINFOEXW mi;
    MonitorPickerItem *item;
    (void)dc; (void)clip; (void)lp;
    if (g_count >= PICKER_MAX_MONITORS) return TRUE;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return TRUE;
    item = &g_items[g_count++];
    ZeroMemory(item, sizeof(*item));
    item->hmon = mon;
    item->info = mi;
    item->deviceNo = DeviceNumber(mi.szDevice);
    QueryTargetForDisplay(mi.szDevice, item->targetPath, ARRAYSIZE(item->targetPath),
                          item->friendlyName, ARRAYSIZE(item->friendlyName));
    if (mi.dwFlags & MONITORINFOF_PRIMARY)
        g_primaryWork = mi.rcWork;
    return TRUE;
}

static void SortMonitors(void)
{
    UINT i, j;
    for (i = 0; i < g_count; ++i) {
        for (j = i + 1; j < g_count; ++j) {
            BOOL swap = g_items[j].deviceNo < g_items[i].deviceNo;
            if (g_items[j].deviceNo == g_items[i].deviceNo) {
                if (g_items[j].info.rcMonitor.left < g_items[i].info.rcMonitor.left)
                    swap = TRUE;
                else if (g_items[j].info.rcMonitor.left == g_items[i].info.rcMonitor.left &&
                         g_items[j].info.rcMonitor.top < g_items[i].info.rcMonitor.top)
                    swap = TRUE;
            }
            if (swap) {
                MonitorPickerItem tmp = g_items[i];
                g_items[i] = g_items[j];
                g_items[j] = tmp;
            }
        }
    }
}

static COLORREF MixColor(COLORREF base, COLORREF accent, unsigned percent)
{
    unsigned r = (GetRValue(base) * (100 - percent) + GetRValue(accent) * percent) / 100;
    unsigned g = (GetGValue(base) * (100 - percent) + GetGValue(accent) * percent) / 100;
    unsigned b = (GetBValue(base) * (100 - percent) + GetBValue(accent) * percent) / 100;
    return RGB(r, g, b);
}

static COLORREF AccentTextColor(COLORREF bg)
{
    unsigned luminance = (GetRValue(bg) * 299u + GetGValue(bg) * 587u +
                          GetBValue(bg) * 114u) / 1000u;
    return luminance >= 145u ? RGB(18, 18, 18) : RGB(255, 255, 255);
}

static void DrawPickerButton(const DRAWITEMSTRUCT *dis)
{
    RECT rc, titleRc, detailRc, numberRc, focusRc;
    wchar_t accessible[PICKER_LABEL_LEN];
    COLORREF accent, face, border, textColor, detailColor;
    HBRUSH brush;
    HPEN pen;
    HGDIOBJ oldBrush, oldPen;
    BOOL pressed, hot, focused;
    int saved, id, index;

    if (!dis || dis->CtlType != ODT_BUTTON) return;
    id = GetDlgCtrlID(dis->hwndItem);
    rc = dis->rcItem;
    accent = Theme_AccentColor();
    pressed = (dis->itemState & ODS_SELECTED) != 0;
    hot = (dis->itemState & ODS_HOTLIGHT) != 0;
    focused = (dis->itemState & ODS_FOCUS) != 0;

    saved = SaveDC(dis->hDC);
    SetBkMode(dis->hDC, TRANSPARENT);

    /* The close control lives in the custom-drawn header. */
    if (id == (int)PICKER_CLOSE_ID) {
        face = (pressed || hot) ? MixColor(Theme_Surface(), accent, pressed ? 28u : 14u)
                                : Theme_Surface();
        brush = CreateSolidBrush(face);
        oldBrush = SelectObject(dis->hDC, brush);
        if (hot || pressed || focused) {
            pen = CreatePen(PS_SOLID, 1, accent);
            oldPen = SelectObject(dis->hDC, pen);
            RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom,
                      PickerScale(7), PickerScale(7));
            SelectObject(dis->hDC, oldPen);
            DeleteObject(pen);
        } else {
            PatBlt(dis->hDC, rc.left, rc.top, rc.right - rc.left,
                   rc.bottom - rc.top, PATCOPY);
        }
        SelectObject(dis->hDC, oldBrush);
        DeleteObject(brush);
        pen = CreatePen(PS_SOLID, PickerScale(1), (hot || pressed) ? accent : Theme_Text());
        oldPen = SelectObject(dis->hDC, pen);
        MoveToEx(dis->hDC, rc.left + PickerScale(10), rc.top + PickerScale(10), NULL);
        LineTo(dis->hDC, rc.right - PickerScale(10), rc.bottom - PickerScale(10));
        MoveToEx(dis->hDC, rc.right - PickerScale(10), rc.top + PickerScale(10), NULL);
        LineTo(dis->hDC, rc.left + PickerScale(10), rc.bottom - PickerScale(10));
        SelectObject(dis->hDC, oldPen);
        DeleteObject(pen);
        if (saved) RestoreDC(dis->hDC, saved);
        return;
    }

    index = id - (int)PICKER_BUTTON_BASE;
    if (index < 0 || (UINT)index >= g_count) {
        if (saved) RestoreDC(dis->hDC, saved);
        return;
    }

    face = pressed ? accent : (hot ? MixColor(Theme_Surface(), accent, 10u)
                                   : Theme_Surface());
    textColor = pressed ? AccentTextColor(accent) : Theme_Text();
    detailColor = pressed ? textColor : Theme_DimText();
    border = accent; /* Accent colour is used consistently, but the idle outline is 1px. */
    InflateRect(&rc, -1, -1);
    brush = CreateSolidBrush(face);
    pen = CreatePen(PS_SOLID, (hot || pressed || focused) ? PickerScale(2) : 1, border);
    oldBrush = SelectObject(dis->hDC, brush);
    oldPen = SelectObject(dis->hDC, pen);
    RoundRect(dis->hDC, rc.left, rc.top, rc.right + 1, rc.bottom + 1,
              PickerScale(9), PickerScale(9));
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    /* Number chip makes keyboard shortcuts easy to scan without forcing all
       monitor metadata into one long, visually noisy line. */
    numberRc.left = rc.left + PickerScale(12);
    numberRc.top = rc.top + (rc.bottom - rc.top - PickerScale(30)) / 2;
    numberRc.right = numberRc.left + PickerScale(30);
    numberRc.bottom = numberRc.top + PickerScale(30);
    brush = CreateSolidBrush(pressed ? MixColor(accent, RGB(255,255,255), 18u)
                                     : MixColor(Theme_Surface(), accent, 18u));
    oldBrush = SelectObject(dis->hDC, brush);
    pen = CreatePen(PS_SOLID, 1, pressed ? textColor : accent);
    oldPen = SelectObject(dis->hDC, pen);
    RoundRect(dis->hDC, numberRc.left, numberRc.top, numberRc.right,
              numberRc.bottom, PickerScale(7), PickerScale(7));
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
    SetTextColor(dis->hDC, pressed ? textColor : accent);
    (void)SelectObject(dis->hDC, g_fontDetail ? g_fontDetail : GetStockObject(DEFAULT_GUI_FONT));
    StringCchPrintfW(accessible, ARRAYSIZE(accessible), L"%d", index + 1);
    DrawTextW(dis->hDC, accessible, -1, &numberRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    titleRc.left = numberRc.right + PickerScale(12);
    titleRc.right = rc.right - PickerScale(14);
    titleRc.top = rc.top + PickerScale(9);
    titleRc.bottom = titleRc.top + PickerScale(22);
    detailRc.left = titleRc.left;
    detailRc.right = titleRc.right;
    detailRc.top = titleRc.bottom + PickerScale(1);
    detailRc.bottom = rc.bottom - PickerScale(7);
    SetTextColor(dis->hDC, textColor);
    SelectObject(dis->hDC, g_fontRowTitle ? g_fontRowTitle : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dis->hDC, g_items[index].rowTitle, -1, &titleRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SetTextColor(dis->hDC, detailColor);
    SelectObject(dis->hDC, g_fontDetail ? g_fontDetail : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dis->hDC, g_items[index].rowDetail, -1, &detailRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (focused) {
        focusRc = dis->rcItem;
        InflateRect(&focusRc, -PickerScale(5), -PickerScale(5));
        DrawFocusRect(dis->hDC, &focusRc);
    }
    if (saved) RestoreDC(dis->hDC, saved);
}

static void RememberMonitor(const MonitorPickerItem *item)
{
    if (!item) return;
    StringCchCopyW(g_cfg.monitorTargetPath, ARRAYSIZE(g_cfg.monitorTargetPath),
                   item->targetPath);
    StringCchCopyW(g_cfg.monitorDeviceName, ARRAYSIZE(g_cfg.monitorDeviceName),
                   item->info.szDevice);
    Settings_Save();
    Log_Printf(L"monitor-picker: remembered device=%s target=%s",
               g_cfg.monitorDeviceName,
               g_cfg.monitorTargetPath[0] ? g_cfg.monitorTargetPath : L"(no stable target path)");
}

static int FindRememberedMonitor(void)
{
    UINT i;
    if (g_cfg.monitorTargetPath[0]) {
        for (i = 0; i < g_count; ++i) {
            if (g_items[i].targetPath[0] &&
                _wcsicmp(g_items[i].targetPath, g_cfg.monitorTargetPath) == 0)
                return (int)i;
        }
        return -1; /* Do not risk silently selecting a different physical monitor. */
    }
    /* DISPLAYn is an enumeration name, not a physical identity. If Windows
       cannot provide a stable target path, prefer asking again over silently
       recording a different monitor that inherited the same DISPLAY number. */
    return -1;
}

static void PickerChoose(UINT index)
{
    if (index >= g_count || g_done) return;
    g_selected = g_items[index].info.rcMonitor;
    g_selectedIndex = index;
    g_chosen = TRUE;
    g_done = TRUE;
    Log_Printf(L"monitor-picker: selected item %u device=%s target=%s rect=(%ld,%ld)-(%ld,%ld)",
               index + 1, g_items[index].info.szDevice,
               g_items[index].targetPath[0] ? g_items[index].targetPath : L"(unknown)",
               g_selected.left, g_selected.top, g_selected.right, g_selected.bottom);
    if (g_window && IsWindow(g_window)) DestroyWindow(g_window);
    g_window = NULL;
}

static void PickerCancel(void)
{
    if (g_done) return;
    g_done = TRUE;
    g_chosen = FALSE;
    Log_Printf(L"monitor-picker: cancelled");
    if (g_window && IsWindow(g_window)) DestroyWindow(g_window);
    g_window = NULL;
}

static LRESULT CALLBACK PickerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        UINT i;
        int y = g_hintTop;
        HWND hint = CreateWindowExW(0, L"STATIC", Lang_Str(STR_MONITOR_PICK_HINT),
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                    g_margin, y, g_pickerWidth - g_margin * 2, g_hintHeight,
                    hwnd, (HMENU)(INT_PTR)1, GetModuleHandleW(NULL), NULL);
        if (hint && g_fontDetail) SendMessageW(hint, WM_SETFONT, (WPARAM)g_fontDetail, TRUE);

        {
            HWND closeButton = CreateWindowExW(0, L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                g_pickerWidth - g_margin - g_closeSize,
                (g_headerHeight - g_closeSize) / 2,
                g_closeSize, g_closeSize, hwnd,
                (HMENU)(INT_PTR)PICKER_CLOSE_ID, GetModuleHandleW(NULL), NULL);
            if (closeButton && g_fontDetail)
                SendMessageW(closeButton, WM_SETFONT, (WPARAM)g_fontDetail, TRUE);
        }

        y = g_rowsTop;
        for (i = 0; i < g_count; ++i) {
            wchar_t accessible[PICKER_LABEL_LEN];
            const MONITORINFOEXW *mi = &g_items[i].info;
            LONG width = mi->rcMonitor.right - mi->rcMonitor.left;
            LONG height = mi->rcMonitor.bottom - mi->rcMonitor.top;
            const wchar_t *primary = (mi->dwFlags & MONITORINFOF_PRIMARY)
                                   ? Lang_Str(STR_MONITOR_PRIMARY) : L"";
            const wchar_t *title = g_items[i].friendlyName[0]
                                 ? g_items[i].friendlyName : mi->szDevice;
            StringCchCopyW(g_items[i].rowTitle, ARRAYSIZE(g_items[i].rowTitle), title);
            if (primary[0])
                StringCchPrintfW(g_items[i].rowDetail, ARRAYSIZE(g_items[i].rowDetail),
                    L"%s  |  %ld x %ld  |  %s", mi->szDevice,
                    width, height, primary);
            else
                StringCchPrintfW(g_items[i].rowDetail, ARRAYSIZE(g_items[i].rowDetail),
                    L"%s  |  %ld x %ld", mi->szDevice, width, height);
            StringCchPrintfW(accessible, ARRAYSIZE(accessible), L"%u. %s, %s",
                             i + 1, g_items[i].rowTitle, g_items[i].rowDetail);
            HWND button = CreateWindowExW(0, L"BUTTON", accessible,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    g_margin, y, g_pickerWidth - g_margin * 2,
                    g_rowHeight, hwnd,
                    (HMENU)(INT_PTR)(PICKER_BUTTON_BASE + i),
                    GetModuleHandleW(NULL), NULL);
            if (button && g_fontDetail) SendMessageW(button, WM_SETFONT, (WPARAM)g_fontDetail, TRUE);
            y += g_rowHeight + g_rowGap;
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        HBRUSH brush = CreateSolidBrush(Theme_Bg());
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client, headerRc, titleRc;
        HBRUSH brush;
        HPEN pen;
        HGDIOBJ oldBrush, oldPen, oldFont;
        GetClientRect(hwnd, &client);
        headerRc.left = 1; headerRc.top = 1;
        headerRc.right = client.right - 1; headerRc.bottom = g_headerHeight;
        brush = CreateSolidBrush(Theme_Surface());
        FillRect(dc, &client, Theme_BgBrush());
        FillRect(dc, &headerRc, brush);
        DeleteObject(brush);

        titleRc.left = g_margin;
        titleRc.top = 0;
        titleRc.right = g_pickerWidth - g_margin - g_closeSize - PickerScale(10);
        titleRc.bottom = g_headerHeight;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Theme_Text());
        oldFont = SelectObject(dc, g_fontHeader ? g_fontHeader : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, Lang_Str(STR_MONITOR_PICK_TITLE), -1, &titleRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, oldFont);

        pen = CreatePen(PS_SOLID, 1, Theme_Line());
        oldPen = SelectObject(dc, pen);
        oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, client.right, client.bottom, PickerScale(12), PickerScale(12));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT point;
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        point.x = GET_X_LPARAM(lp);
        point.y = GET_Y_LPARAM(lp);
        if (ScreenToClient(hwnd, &point) && point.y >= 0 && point.y < g_headerHeight &&
            point.x >= 0 && point.x < g_pickerWidth - g_margin - g_closeSize - PickerScale(8))
            return HTCAPTION;
        return hit;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, Theme_DimText());
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)Theme_BgBrush();
    }
    case WM_DRAWITEM:
        DrawPickerButton((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_COMMAND:
        if ((UINT)LOWORD(wp) == PICKER_CLOSE_ID) {
            PickerCancel();
            return 0;
        }
        if ((UINT)LOWORD(wp) >= PICKER_BUTTON_BASE &&
            (UINT)LOWORD(wp) < PICKER_BUTTON_BASE + g_count) {
            PickerChoose((UINT)LOWORD(wp) - PICKER_BUTTON_BASE);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            PickerCancel();
            return 0;
        }
        if (wp >= (WPARAM)'1' && wp <= (WPARAM)'9') {
            UINT index = (UINT)(wp - (WPARAM)'1');
            if (index < g_count) PickerChoose(index);
            return 0;
        }
        if (wp >= (WPARAM)VK_NUMPAD1 && wp <= (WPARAM)VK_NUMPAD9) {
            UINT index = (UINT)(wp - VK_NUMPAD1);
            if (index < g_count) PickerChoose(index);
            return 0;
        }
        break;
    case WM_SETTINGCHANGE:
        if (Theme_IsColorSchemeChange(msg, lp)) {
            Theme_SetDark(Theme_SystemPrefersDark());
            Theme_ApplyToMainWindow(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            {
                UINT i;
                for (i = 0; i < g_count; ++i)
                    InvalidateRect(GetDlgItem(hwnd, PICKER_BUTTON_BASE + (int)i), NULL, TRUE);
                InvalidateRect(GetDlgItem(hwnd, (int)PICKER_CLOSE_ID), NULL, TRUE);
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        PickerCancel();
        return 0;
    case WM_DESTROY:
        if (!g_done) {
            g_done = TRUE;
            g_chosen = FALSE;
        }
        if (g_window == hwnd) g_window = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL MonitorPicker_Select(HWND owner, RECT *screenRect)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    HMONITOR primary;
    MONITORINFO mi;
    POINT origin;
    int width, height, x, y;
    int remembered;
    HINSTANCE instance = GetModuleHandleW(NULL);
    const wchar_t *className = L"ScreenshotService.MonitorPicker";
    BOOL ownerWasEnabled = FALSE;

    if (!screenRect) return FALSE;
    ZeroMemory(g_items, sizeof(g_items));
    g_count = 0; g_done = FALSE; g_chosen = FALSE;
    g_selectedIndex = UINT_MAX;
    ZeroMemory(&g_selected, sizeof(g_selected));
    SetRectEmpty(&g_primaryWork);
    if (!EnumDisplayMonitors(NULL, NULL, EnumMonitor, 0) || !g_count) {
        Log_Printf(L"monitor-picker: EnumDisplayMonitors found no display");
        return FALSE;
    }
    SortMonitors();

    if (g_cfg.monitorForceLast) {
        remembered = FindRememberedMonitor();
        if (remembered >= 0) {
            *screenRect = g_items[remembered].info.rcMonitor;
            Log_Printf(L"monitor-picker: forced remembered display %s target=%s (%ldx%ld)",
                       g_items[remembered].info.szDevice,
                       g_items[remembered].targetPath[0] ? g_items[remembered].targetPath : L"(unknown)",
                       screenRect->right - screenRect->left,
                       screenRect->bottom - screenRect->top);
            return TRUE;
        }
        /* If a previously selected display is gone, ask again even when only
           one other screen remains. Otherwise a stale target would be silently
           replaced instead of asking the user to confirm the new target. */
        if (g_cfg.monitorTargetPath[0])
            Log_Printf(L"monitor-picker: saved physical target is missing; reopening picker for explicit replacement");
        else if (g_cfg.monitorDeviceName[0])
            Log_Printf(L"monitor-picker: saved GDI name has no stable physical target; cannot force it safely");
        else
            Log_Printf(L"monitor-picker: no remembered display yet");
    }

    /* One connected display is unambiguous. Skip the dialog unless force-last
       is recovering from a saved physical display that has disappeared. */
    if (g_count == 1 &&
        !(g_cfg.monitorForceLast && g_cfg.monitorTargetPath[0])) {
        *screenRect = g_items[0].info.rcMonitor;
        /* When Force last display is enabled but there was no stable target
           to reuse, the only connected display becomes the remembered target
           for the next recording. */
        if (g_cfg.monitorForceLast && !g_cfg.monitorTargetPath[0])
            RememberMonitor(&g_items[0]);
        Log_Printf(L"monitor-picker: one display (%s), selection implicit (%ldx%ld)",
                   g_items[0].info.szDevice,
                   screenRect->right - screenRect->left,
                   screenRect->bottom - screenRect->top);
        return TRUE;
    }

    /* Ask-every-time shows the picker for multiple displays. Force-last also
       opens it when the remembered physical target has disconnected. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = PickerWndProc;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; /* WM_ERASEBKGND paints using the active app theme. */
    wc.hIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON),
                                        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log_Printf(L"monitor-picker: RegisterClassEx failed (%lu)",
                   (unsigned long)GetLastError());
        return FALSE;
    }

    /* Scale the custom dialog to Windows' effective system DPI. The old
       calculation included a native caption in its height although the child
       buttons were laid out in client coordinates, clipping the bottom row. */
    {
        typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        PFN_GetDpiForSystem getDpi = user32
            ? (PFN_GetDpiForSystem)(void *)GetProcAddress(user32, "GetDpiForSystem") : NULL;
        g_dpi = getDpi ? getDpi() : 96u;
        if (g_dpi < 96u) g_dpi = 96u;
    }
    g_pickerWidth = PickerScale(PICKER_WIDTH);
    g_headerHeight = PickerScale(PICKER_HEADER_HEIGHT);
    g_margin = PickerScale(PICKER_MARGIN);
    g_hintTop = PickerScale(PICKER_HINT_TOP);
    g_hintHeight = PickerScale(PICKER_HINT_HEIGHT);
    g_rowsTop = PickerScale(PICKER_ROWS_TOP);
    g_rowHeight = PickerScale(PICKER_ROW_HEIGHT);
    g_rowGap = PickerScale(PICKER_ROW_GAP);
    g_bottomMargin = PickerScale(PICKER_BOTTOM);
    g_closeSize = PickerScale(28);
    PickerCreateFonts();

    width = g_pickerWidth;
    height = g_rowsTop + (int)g_count * g_rowHeight + (int)(g_count - 1) * g_rowGap + g_bottomMargin;
    if (IsRectEmpty(&g_primaryWork)) {
        origin.x = origin.y = 0;
        primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        ZeroMemory(&mi, sizeof(mi)); mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(primary, &mi)) g_primaryWork = mi.rcWork;
        else SetRect(&g_primaryWork, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN));
    }
    if (width > g_primaryWork.right - g_primaryWork.left - PickerScale(24))
        width = g_primaryWork.right - g_primaryWork.left - PickerScale(24);
    g_pickerWidth = width; /* Child controls and custom header use the actual width. */
    if (height > g_primaryWork.bottom - g_primaryWork.top - PickerScale(24)) {
        /* Shorten rows only as a last resort on unusually short desktops. */
        int available = g_primaryWork.bottom - g_primaryWork.top - PickerScale(24);
        int rowSpace = available - g_rowsTop - g_bottomMargin - (int)(g_count - 1) * g_rowGap;
        int compactRow = g_count ? rowSpace / (int)g_count : g_rowHeight;
        if (compactRow >= PickerScale(44)) {
            g_rowHeight = compactRow;
            height = g_rowsTop + (int)g_count * g_rowHeight + (int)(g_count - 1) * g_rowGap + g_bottomMargin;
        } else {
            height = available;
        }
    }
    x = g_primaryWork.left + ((g_primaryWork.right - g_primaryWork.left) - width) / 2;
    y = g_primaryWork.top + ((g_primaryWork.bottom - g_primaryWork.top) - height) / 2;
    if (x < g_primaryWork.left) x = g_primaryWork.left;
    if (y < g_primaryWork.top) y = g_primaryWork.top;

    /* The host is normally a hidden tray window. Draw the title bar ourselves
       so title text, close affordance, and background stay legible in both themes. */
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                           className, Lang_Str(STR_MONITOR_PICK_TITLE),
                           WS_POPUP,
                           x, y, width, height, NULL, NULL, instance, NULL);
    if (!hwnd) {
        PickerDeleteFonts();
        Log_Printf(L"monitor-picker: CreateWindowEx failed (%lu)",
                   (unsigned long)GetLastError());
        return FALSE;
    }
    g_window = hwnd;
    Theme_ApplyToMainWindow(hwnd);
    if (owner && IsWindow(owner) && IsWindowVisible(owner) && IsWindowEnabled(owner)) {
        ownerWasEnabled = TRUE;
        EnableWindow(owner, FALSE);
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    if (g_count) {
        SendMessageW(hwnd, DM_SETDEFID, (WPARAM)PICKER_BUTTON_BASE, 0);
        SetFocus(GetDlgItem(hwnd, PICKER_BUTTON_BASE));
    }

    while (!g_done && IsWindow(hwnd)) {
        MSG msg;
        int gm = GetMessageW(&msg, NULL, 0, 0);
        if (gm <= 0) {
            if (gm == 0) PostQuitMessage((int)msg.wParam);
            PickerCancel();
            break;
        }
        /* Also catch numeric keys while a child button owns keyboard focus. */
        if ((msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd)) &&
            (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
            WPARAM key = msg.wParam;
            UINT index = UINT_MAX;
            if (key == VK_RETURN) {
                int focusedId = msg.hwnd == hwnd ? -1 : GetDlgCtrlID(msg.hwnd);
                if (focusedId >= (int)PICKER_BUTTON_BASE &&
                    focusedId < (int)(PICKER_BUTTON_BASE + g_count))
                    PickerChoose((UINT)(focusedId - (int)PICKER_BUTTON_BASE));
                else
                    PickerChoose(0);
                continue;
            }
            if (key >= (WPARAM)'1' && key <= (WPARAM)'9') index = (UINT)(key - (WPARAM)'1');
            else if (key >= (WPARAM)VK_NUMPAD1 && key <= (WPARAM)VK_NUMPAD9)
                index = (UINT)(key - (WPARAM)VK_NUMPAD1);
            else if (key == VK_ESCAPE) { PickerCancel(); continue; }
            if (index < g_count) { PickerChoose(index); continue; }
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    PickerDeleteFonts();
    if (ownerWasEnabled) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (!g_chosen || g_selectedIndex >= g_count) return FALSE;

    /* Remember all successful choices, even when the current mode asks every
       time; users can switch to forced reuse later in Settings. */
    RememberMonitor(&g_items[g_selectedIndex]);
    *screenRect = g_selected;
    return TRUE;
}
