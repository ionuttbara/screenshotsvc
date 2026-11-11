/*
 * toolbar.c - layout and CPU rendering of the mode picker.
 */

#include "screenshotservice.h"
#include "toolbar.h"
#include "theme.h"

#define SS            4       /* supersampling factor for the GDI layers */

/* Logical (96 dpi) metrics. */
#define BTN_W         44
#define BTN_H         44
#define PAD           6
#define GAP_BEFORE_X  8       /* extra space before the close button */
#define TOP_MARGIN    16
#define CORNER        10

/* The pill stays dark; active selection uses the current Windows accent. */
#define C_PILL_R      0x20
#define C_PILL_G      0x20
#define C_PILL_B      0x24
#define C_PILL_A      0xF2
#define C_HOT         0x40, 0x42, 0x48, 0xFF
#define C_ICON        0xF2, 0xF4, 0xF8, 0xFF
#define C_CLOSE_HOT   0xC4, 0x2B, 0x1C, 0xFF

static int Scaled(int logical, UINT dpi)
{
    return MulDiv(logical, (int)dpi, 96);
}

void Toolbar_Layout(Toolbar *tb, const RECT *monitor, UINT dpi)
{
    int bh, pad, gap, w, h, x, y, i, cursorX;

    if (!tb || !monitor)
        return;
    if (dpi < 96)
        dpi = 96;

    tb->dpi = dpi;
    bh  = Scaled(BTN_H, dpi);
    pad = Scaled(PAD, dpi);
    gap = Scaled(GAP_BEFORE_X, dpi);

    w = pad * 2 + gap;
    for (i = 0; i < TB_COUNT; i++)
        w += Scaled(i == TB_GIF ? 64 : BTN_W, dpi);
    h = pad * 2 + bh;

    x = monitor->left + ((monitor->right - monitor->left) - w) / 2;
    y = monitor->top + Scaled(TOP_MARGIN, dpi);

    SetRect(&tb->rect, x, y, x + w, y + h);

    cursorX = x + pad;
    for (i = 0; i < TB_COUNT; i++) {
        int bw = Scaled(i == TB_GIF ? 64 : BTN_W, dpi);
        if (i == TB_CLOSE) cursorX += gap;
        SetRect(&tb->button[i], cursorX, y + pad, cursorX + bw, y + pad + bh);
        cursorX += bw;
    }
}

int Toolbar_HitTest(const Toolbar *tb, POINT pt)
{
    int i;

    if (!tb)
        return -1;
    for (i = 0; i < TB_COUNT; i++) {
        if (PtInRect(&tb->button[i], pt))
            return i;
    }
    return -1;
}

BOOL Toolbar_Contains(const Toolbar *tb, POINT pt)
{
    return tb && PtInRect(&tb->rect, pt);
}

/* ------------------------------------------------------- layer machinery */

typedef struct {
    HDC     dc;
    HBITMAP dib;
    HBITMAP old;
    BYTE   *bits;
    int     w, h;          /* supersampled size */
    int     outW, outH;
} Layers;

static void LayersFree(Layers *L)
{
    if (L->dc) {
        if (L->old)
            SelectObject(L->dc, L->old);
        DeleteDC(L->dc);
    }
    if (L->dib)
        DeleteObject(L->dib);
    ZeroMemory(L, sizeof(*L));
}

static BOOL LayersInit(Layers *L, int outW, int outH)
{
    BITMAPINFO bi;

    ZeroMemory(L, sizeof(*L));
    L->outW = outW;
    L->outH = outH;
    L->w    = outW * SS;
    L->h    = outH * SS;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = L->w;
    bi.bmiHeader.biHeight      = -L->h;        /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    L->dc = CreateCompatibleDC(NULL);
    if (!L->dc)
        return FALSE;
    L->dib = CreateDIBSection(L->dc, &bi, DIB_RGB_COLORS, (void **)&L->bits, NULL, 0);
    if (!L->dib || !L->bits) {
        LayersFree(L);
        return FALSE;
    }
    L->old = (HBITMAP)SelectObject(L->dc, L->dib);
    SetBkMode(L->dc, TRANSPARENT);
    return TRUE;
}

/* Start a layer: everything drawn after this counts as coverage. */
static void LayerBegin(Layers *L)
{
    RECT all;
    SetRect(&all, 0, 0, L->w, L->h);
    FillRect(L->dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
}

/*
 * Finish a layer: box-filter the coverage down by SS and composite 'colour'
 * over the destination with that coverage. 'dst' is premultiplied BGRA.
 */
static void LayerEnd(Layers *L, BYTE *dst, int stride,
                     BYTE r, BYTE g, BYTE b, BYTE a)
{
    int x, y, sy, sx;

    GdiFlush();

    for (y = 0; y < L->outH; y++) {
        BYTE *out = dst + (SIZE_T)y * stride;
        for (x = 0; x < L->outW; x++) {
            unsigned sum = 0;
            unsigned cov, sa;

            for (sy = 0; sy < SS; sy++) {
                const BYTE *src = L->bits +
                    (SIZE_T)(y * SS + sy) * (SIZE_T)L->w * 4 + (SIZE_T)(x * SS) * 4;
                for (sx = 0; sx < SS; sx++)
                    sum += src[sx * 4];        /* blue channel: white or black */
            }
            cov = sum / (SS * SS);
            if (cov == 0)
                continue;

            sa = cov * a / 255;                /* effective source alpha */
            if (sa == 0)
                continue;

            /* Premultiplied source over premultiplied destination. */
            out[x * 4 + 0] = (BYTE)(b * sa / 255 + out[x * 4 + 0] * (255 - sa) / 255);
            out[x * 4 + 1] = (BYTE)(g * sa / 255 + out[x * 4 + 1] * (255 - sa) / 255);
            out[x * 4 + 2] = (BYTE)(r * sa / 255 + out[x * 4 + 2] * (255 - sa) / 255);
            out[x * 4 + 3] = (BYTE)(sa          + out[x * 4 + 3] * (255 - sa) / 255);
        }
    }
}

static void WhiteFillRoundRect(Layers *L, const RECT *r, int radius)
{
    HBRUSH brush = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HPEN   pen   = (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ ob = SelectObject(L->dc, brush);
    HGDIOBJ op = SelectObject(L->dc, pen);

    /* RoundRect's outline is drawn with the pen; a null pen leaves the shape
       one pixel short, which the +1 puts back. */
    RoundRect(L->dc, r->left, r->top, r->right + 1, r->bottom + 1,
              radius * 2, radius * 2);

    SelectObject(L->dc, ob);
    SelectObject(L->dc, op);
}

/* ------------------------------------------------------------- the icons */

/* Every icon is drawn inside a square box in supersampled coordinates. */
static void IconRect(Layers *L, const RECT *box, int pen)
{
    HPEN    p  = CreatePen(PS_SOLID, pen, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(L->dc, p);
    HGDIOBJ ob = SelectObject(L->dc, GetStockObject(NULL_BRUSH));

    Rectangle(L->dc, box->left, box->top, box->right, box->bottom);

    SelectObject(L->dc, ob);
    SelectObject(L->dc, op);
    DeleteObject(p);
}

static void IconFree(Layers *L, const RECT *box, int pen)
{
    POINT   pts[9];
    int     w  = box->right - box->left;
    int     h  = box->bottom - box->top;
    HPEN    p  = CreatePen(PS_SOLID, pen, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(L->dc, p);
    HGDIOBJ ob = SelectObject(L->dc, GetStockObject(NULL_BRUSH));

    /* A closed, deliberately irregular blob - it has to read as "hand-drawn"
       at 20 logical pixels. */
    pts[0].x = box->left + w * 12 / 100;  pts[0].y = box->top + h * 42 / 100;
    pts[1].x = box->left + w * 30 / 100;  pts[1].y = box->top + h *  8 / 100;
    pts[2].x = box->left + w * 62 / 100;  pts[2].y = box->top + h * 14 / 100;
    pts[3].x = box->left + w * 92 / 100;  pts[3].y = box->top + h * 38 / 100;
    pts[4].x = box->left + w * 84 / 100;  pts[4].y = box->top + h * 74 / 100;
    pts[5].x = box->left + w * 54 / 100;  pts[5].y = box->top + h * 92 / 100;
    pts[6].x = box->left + w * 24 / 100;  pts[6].y = box->top + h * 82 / 100;
    pts[7].x = box->left + w *  8 / 100;  pts[7].y = box->top + h * 64 / 100;
    pts[8] = pts[0];
    Polyline(L->dc, pts, 9);

    SelectObject(L->dc, ob);
    SelectObject(L->dc, op);
    DeleteObject(p);
}

static void IconWindow(Layers *L, const RECT *box, int pen)
{
    int     h  = box->bottom - box->top;
    int     bar = box->top + h * 28 / 100;
    HPEN    p  = CreatePen(PS_SOLID, pen, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(L->dc, p);
    HGDIOBJ ob = SelectObject(L->dc, GetStockObject(NULL_BRUSH));

    Rectangle(L->dc, box->left, box->top, box->right, box->bottom);
    MoveToEx(L->dc, box->left, bar, NULL);
    LineTo(L->dc, box->right, bar);

    SelectObject(L->dc, ob);
    SelectObject(L->dc, op);
    DeleteObject(p);
}

static void IconFull(Layers *L, const RECT *box, int pen)
{
    int     w   = box->right - box->left;
    int     h   = box->bottom - box->top;
    int     armX = w * 34 / 100;
    int     armY = h * 34 / 100;
    HPEN    p  = CreatePen(PS_SOLID, pen, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(L->dc, p);

    /* Four corner brackets - the usual "whole screen" mark. */
    MoveToEx(L->dc, box->left, box->top + armY, NULL);
    LineTo(L->dc, box->left, box->top);
    LineTo(L->dc, box->left + armX, box->top);

    MoveToEx(L->dc, box->right - armX, box->top, NULL);
    LineTo(L->dc, box->right, box->top);
    LineTo(L->dc, box->right, box->top + armY);

    MoveToEx(L->dc, box->right, box->bottom - armY, NULL);
    LineTo(L->dc, box->right, box->bottom);
    LineTo(L->dc, box->right - armX, box->bottom);

    MoveToEx(L->dc, box->left + armX, box->bottom, NULL);
    LineTo(L->dc, box->left, box->bottom);
    LineTo(L->dc, box->left, box->bottom - armY);

    SelectObject(L->dc, op);
    DeleteObject(p);
}

/* A filled disc: the universal "this records" mark. */
static void IconRecord(Layers *L, const RECT *box, int pen)
{
    HBRUSH  b  = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HGDIOBJ ob = SelectObject(L->dc, b);
    HGDIOBJ op = SelectObject(L->dc, GetStockObject(NULL_PEN));
    int     inset = (box->right - box->left) / 8;

    UNREFERENCED_PARAMETER(pen);
    Ellipse(L->dc, box->left + inset, box->top + inset,
                   box->right - inset, box->bottom - inset);

    SelectObject(L->dc, ob);
    SelectObject(L->dc, op);
}


static void IconGif(Layers *L, const RECT *box, int pen)
{
    HFONT font;
    HGDIOBJ oldFont;
    RECT r = *box;
    int h = box->bottom - box->top;
    font = CreateFontW(h * 52 / 100, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!font) return;
    oldFont = SelectObject(L->dc, font);
    SetBkMode(L->dc, TRANSPARENT);
    SetTextColor(L->dc, RGB(255, 255, 255));
    DrawTextW(L->dc, L"GIF", 3, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(L->dc, oldFont);
    DeleteObject(font);
    UNREFERENCED_PARAMETER(pen);
}


static void IconPen(Layers *L, const RECT *box, int pen)
{
    int w = box->right - box->left, h = box->bottom - box->top;
    HPEN p = CreatePen(PS_SOLID, pen + SS, RGB(255,255,255));
    HGDIOBJ oldP = SelectObject(L->dc, p);
    POINT a, b;
    a.x = box->left + w * 22 / 100; a.y = box->bottom - h * 18 / 100;
    b.x = box->right - w * 20 / 100; b.y = box->top + h * 20 / 100;
    MoveToEx(L->dc, a.x, a.y, NULL); LineTo(L->dc, b.x, b.y);
    MoveToEx(L->dc, a.x - w/12, a.y + h/12, NULL); LineTo(L->dc, a.x + w/12, a.y - h/12);
    SelectObject(L->dc, oldP); DeleteObject(p);
}

static void IconEraser(Layers *L, const RECT *box, int pen)
{
    int w = box->right - box->left, h = box->bottom - box->top;
    HPEN p = CreatePen(PS_SOLID, pen + SS, RGB(255,255,255));
    HGDIOBJ oldP = SelectObject(L->dc, p);
    POINT pts[5];
    pts[0].x = box->left + w*20/100; pts[0].y = box->top + h*58/100;
    pts[1].x = box->left + w*48/100; pts[1].y = box->top + h*22/100;
    pts[2].x = box->right - w*15/100; pts[2].y = box->top + h*55/100;
    pts[3].x = box->left + w*57/100; pts[3].y = box->bottom - h*18/100;
    pts[4] = pts[0];
    Polyline(L->dc, pts, 5);
    MoveToEx(L->dc, box->left + w*41/100, box->bottom - h*18/100, NULL);
    LineTo(L->dc, box->right - w*10/100, box->bottom - h*18/100);
    SelectObject(L->dc, oldP); DeleteObject(p);
}

static void IconFullRecord(Layers *L, const RECT *box, int pen)
{
    RECT dot = *box;
    int w = box->right - box->left;
    int h = box->bottom - box->top;
    IconFull(L, box, pen);
    /* The full-screen record action uses the standard centered record dot,
       not the small badge in the lower-right corner. */
    dot.left = box->left + w * 32 / 100;
    dot.top = box->top + h * 32 / 100;
    dot.right = box->right - w * 32 / 100;
    dot.bottom = box->bottom - h * 32 / 100;
    IconRecord(L, &dot, pen);
}

static void IconClose(Layers *L, const RECT *box, int pen)
{
    HPEN    p  = CreatePen(PS_SOLID, pen, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(L->dc, p);

    MoveToEx(L->dc, box->left, box->top, NULL);
    LineTo(L->dc, box->right, box->bottom);
    MoveToEx(L->dc, box->right, box->top, NULL);
    LineTo(L->dc, box->left, box->bottom);

    SelectObject(L->dc, op);
    DeleteObject(p);
}

/* ------------------------------------------------------------ rendering */

/* A button's icon box, in supersampled coordinates local to the pill. */
static RECT IconBox(const Toolbar *tb, int i, int side)
{
    RECT b = tb->button[i];
    RECT box;
    int  cx = (b.left + b.right)  / 2 - tb->rect.left;
    int  cy = (b.top  + b.bottom) / 2 - tb->rect.top;

    SetRect(&box, (cx - side / 2) * SS, (cy - side / 2) * SS,
                  (cx + side / 2) * SS, (cy + side / 2) * SS);
    return box;
}

static RECT LocalButton(const Toolbar *tb, int i, int inset)
{
    RECT b = tb->button[i];
    SetRect(&b, (b.left  - tb->rect.left + inset) * SS,
                (b.top   - tb->rect.top  + inset) * SS,
                (b.right - tb->rect.left - inset) * SS,
                (b.bottom- tb->rect.top  - inset) * SS);
    return b;
}

BOOL Toolbar_Render(const Toolbar *tb, BYTE *bgra, int stride)
{
    Layers L;
    int    w, h, y, i;
    int    iconSide, pen, radius, btnRadius, inset;

    if (!tb || !bgra)
        return FALSE;
    w = tb->rect.right  - tb->rect.left;
    h = tb->rect.bottom - tb->rect.top;
    if (w <= 0 || h <= 0)
        return FALSE;

    for (y = 0; y < h; y++)
        ZeroMemory(bgra + (SIZE_T)y * stride, (SIZE_T)w * 4);

    if (!LayersInit(&L, w, h))
        return FALSE;

    iconSide  = Scaled(20, tb->dpi);
    pen       = Scaled(2, tb->dpi) * SS;
    if (pen < SS) pen = SS;
    radius    = Scaled(CORNER, tb->dpi) * SS;
    btnRadius = Scaled(7, tb->dpi) * SS;
    inset     = Scaled(3, tb->dpi);

    /* 1. the pill */
    {
        RECT all;
        SetRect(&all, 0, 0, w * SS - 1, h * SS - 1);
        LayerBegin(&L);
        WhiteFillRoundRect(&L, &all, radius);
        LayerEnd(&L, bgra, stride, C_PILL_R, C_PILL_G, C_PILL_B, C_PILL_A);
    }

    /* 2. the active mode, then the hovered button on top of it */
    if (tb->active >= 0 && tb->active < TB_COUNT && tb->active != TB_CLOSE) {
        RECT b = LocalButton(tb, tb->active, inset);
        COLORREF accent = Theme_AccentColor();
        LayerBegin(&L);
        WhiteFillRoundRect(&L, &b, btnRadius);
        LayerEnd(&L, bgra, stride, GetRValue(accent), GetGValue(accent),
                 GetBValue(accent), 0xFF);
    }
    if (tb->hot >= 0 && tb->hot < TB_COUNT && tb->hot != tb->active) {
        RECT b = LocalButton(tb, tb->hot, inset);
        LayerBegin(&L);
        WhiteFillRoundRect(&L, &b, btnRadius);
        if (tb->hot == TB_CLOSE)
            LayerEnd(&L, bgra, stride, C_CLOSE_HOT);
        else
            LayerEnd(&L, bgra, stride, C_HOT);
    }

    /* 3. every icon in one layer - they share a colour, so one pass does */
    LayerBegin(&L);
    for (i = 0; i < TB_COUNT; i++) {
        RECT box = IconBox(tb, i, i == TB_GIF ? Scaled(27, tb->dpi) : iconSide);
        switch (i) {
        case TB_RECT:   IconRect(&L, &box, pen);   break;
        case TB_FREE:   IconFree(&L, &box, pen);   break;
        case TB_WINDOW: IconWindow(&L, &box, pen); break;
        case TB_FULL:   IconFull(&L, &box, pen);   break;
        case TB_RECORD: IconRecord(&L, &box, pen); break;
        case TB_GIF:    IconGif(&L, &box, pen);    break;
        case TB_PEN:    IconPen(&L, &box, pen);    break;
        case TB_ERASER: IconEraser(&L, &box, pen); break;
        case TB_FULLREC: IconFullRecord(&L, &box, pen); break;
        case TB_CLOSE:  IconClose(&L, &box, pen);  break;
        default: break;
        }
    }
    LayerEnd(&L, bgra, stride, C_ICON);

    LayersFree(&L);
    return TRUE;
}
