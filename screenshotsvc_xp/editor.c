#include "classic.h"

#define ED_PEN 4101
#define ED_RECT 4102
#define ED_CIRCLE 4103
#define ED_HEAL 4104
#define ED_COLOR 4105
#define ED_SAVE 4106
#define ED_CANCEL 4107
#define ED_TEXT_BUTTON 4108
#define ED_TEXT_ENTRY 4110
#define ED_CLASS "ScreenshotServiceClassic.Editor"
#define ED_TOOLBAR 48

typedef enum EditTool { ET_PEN, ET_RECT, ET_CIRCLE, ET_HEAL, ET_TEXT } EditTool;
typedef struct EditState {
    HWND hwnd, text;
    PixelImage *image;
    BYTE *work;
    HDC memdc;
    HBITMAP dib, oldBmp;
    BYTE *dibBits;
    COLORREF color;
    EditTool tool;
    BOOL dragging, accepted;
    POINT anchor, last;
    RECT view;
    double scale;
} EditState;
static EditState g_edit;

static void LayoutEditor(EditState *e)
{
    RECT c;
    int aw, ah, w, h;
    double sx, sy;
    GetClientRect(e->hwnd, &c);
    aw = c.right - 24; ah = c.bottom - ED_TOOLBAR - 16;
    if (aw < 1) aw = 1; if (ah < 1) ah = 1;
    sx = (double)aw / e->image->width; sy = (double)ah / e->image->height;
    e->scale = sx < sy ? sx : sy; if (e->scale > 1.0) e->scale = 1.0; if (e->scale < 0.01) e->scale = 0.01;
    w = (int)(e->image->width * e->scale); h = (int)(e->image->height * e->scale);
    e->view.left = (c.right - w) / 2; e->view.top = ED_TOOLBAR + ((c.bottom - ED_TOOLBAR) - h) / 2;
    e->view.right = e->view.left + w; e->view.bottom = e->view.top + h;
}
static BOOL ToImage(EditState *e, POINT p, POINT *q)
{
    if (!PtInRect(&e->view, p)) return FALSE;
    q->x = (LONG)((p.x - e->view.left) / e->scale + 0.5);
    q->y = (LONG)((p.y - e->view.top) / e->scale + 0.5);
    if (q->x < 0) q->x = 0; if (q->y < 0) q->y = 0;
    if (q->x >= e->image->width) q->x = e->image->width - 1;
    if (q->y >= e->image->height) q->y = e->image->height - 1;
    return TRUE;
}
static void DrawSolidLine(EditState *e, POINT a, POINT b)
{
    HPEN pen = CreatePen(PS_SOLID, (int)(3.0 / e->scale + 0.5), e->color);
    HGDIOBJ old;
    if (!pen) return;
    old = SelectObject(e->memdc, pen); MoveToEx(e->memdc, a.x, a.y, NULL); LineTo(e->memdc, b.x, b.y);
    SelectObject(e->memdc, old); DeleteObject(pen);
}
static int RectW(RECT r) { return r.right - r.left; }
static int RectH(RECT r) { return r.bottom - r.top; }
static void HealRect(EditState *e, RECT r)
{
    int x, y, w, h, sx, sy, mode = -1, best = 0x7fffffff;
    int side, score;
    BYTE *srcPatch;
    size_t bytes;
    if (r.left > r.right) { int t = r.left; r.left = r.right; r.right = t; }
    if (r.top > r.bottom) { int t = r.top; r.top = r.bottom; r.bottom = t; }
    r.left = max(0, r.left); r.top = max(0, r.top);
    r.right = min(e->image->width - 1, r.right); r.bottom = min(e->image->height - 1, r.bottom);
    w = RectW(r) + 1; h = RectH(r) + 1; if (w < 1 || h < 1) return;
    /* Pick the adjacent patch whose outside edge best matches the outside edge of the selection. */
    for (side = 0; side < 4; ++side) {
        long sum = 0; int n = 0;
        if (side == 0 && r.left >= w) {
            for (y = 0; y < h; y += max(1, h / 32)) { BYTE *a = e->dibBits + ((size_t)(r.top+y)*e->image->width + (r.left-1))*4; BYTE *b = e->dibBits + ((size_t)(r.top+y)*e->image->width + (r.left-w))*4; sum += abs((int)a[0]-b[0])+abs((int)a[1]-b[1])+abs((int)a[2]-b[2]); n += 3; }
        } else if (side == 1 && r.right + w < e->image->width) {
            for (y = 0; y < h; y += max(1, h / 32)) { BYTE *a = e->dibBits + ((size_t)(r.top+y)*e->image->width + (r.right+1))*4; BYTE *b = e->dibBits + ((size_t)(r.top+y)*e->image->width + (r.right+1))*4; sum += abs((int)a[0]-b[0])+abs((int)a[1]-b[1])+abs((int)a[2]-b[2]); n += 3; }
        } else if (side == 2 && r.top >= h) {
            for (x = 0; x < w; x += max(1, w / 32)) { BYTE *a = e->dibBits + ((size_t)(r.top-1)*e->image->width + r.left+x)*4; BYTE *b = e->dibBits + ((size_t)(r.top-h)*e->image->width + r.left+x)*4; sum += abs((int)a[0]-b[0])+abs((int)a[1]-b[1])+abs((int)a[2]-b[2]); n += 3; }
        } else if (side == 3 && r.bottom + h < e->image->height) {
            for (x = 0; x < w; x += max(1, w / 32)) { BYTE *a = e->dibBits + ((size_t)(r.bottom+1)*e->image->width + r.left+x)*4; BYTE *b = e->dibBits + ((size_t)(r.bottom+1)*e->image->width + r.left+x)*4; sum += abs((int)a[0]-b[0])+abs((int)a[1]-b[1])+abs((int)a[2]-b[2]); n += 3; }
        } else continue;
        score = n ? (int)(sum / n) : 0x7fffffff;
        if (score < best) { best = score; mode = side; }
    }
    if (mode < 0) return;
    sx = r.left; sy = r.top;
    if (mode == 0) sx = r.left - w;
    else if (mode == 1) sx = r.right + 1;
    else if (mode == 2) sy = r.top - h;
    else if (mode == 3) sy = r.bottom + 1;
    else return;
    bytes = (size_t)w * (size_t)h * 4U; srcPatch = (BYTE *)malloc(bytes); if (!srcPatch) return;
    for (y = 0; y < h; ++y) memcpy(srcPatch + (size_t)y*w*4U, e->dibBits + ((size_t)(sy+y)*e->image->width + sx)*4U, (size_t)w*4U);
    /* Feather only the perimeter, keeping the center clean and avoiding a hard rectangular seam. */
    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) {
        int edge = min(min(x, w-1-x), min(y, h-1-y));
        float mix = edge >= 10 ? 1.0f : 0.18f + 0.082f * edge;
        BYTE *d = e->dibBits + ((size_t)(r.top+y)*e->image->width + r.left+x)*4U;
        BYTE *s = srcPatch + ((size_t)y*w+x)*4U; int k;
        for (k = 0; k < 3; ++k) d[k] = (BYTE)(d[k] * (1.0f-mix) + s[k] * mix + 0.5f);
        d[3] = 255;
    }
    free(srcPatch);
}
static void ChooseEditorColor(EditState *e)
{
    static COLORREF custom[16]; CHOOSECOLORA cc;
    ZeroMemory(&cc, sizeof(cc)); cc.lStructSize = sizeof(cc); cc.hwndOwner = e->hwnd; cc.rgbResult = e->color; cc.lpCustColors = custom; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorA(&cc)) { e->color = cc.rgbResult; InvalidateRect(e->hwnd, NULL, FALSE); }
}
static void AddEditorButton(HWND hwnd, const char *text, int id, int x, int w)
{
    CreateWindowExA(0, "BUTTON", text, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON, x, 7, w, 30, hwnd, (HMENU)(INT_PTR)id, g_instance, NULL);
}
static void EditorPaint(EditState *e, HWND hwnd)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); RECT rc; BITMAPINFO bi; HBRUSH bg;
    GetClientRect(hwnd, &rc); bg = CreateSolidBrush(RGB(38,38,40)); FillRect(dc, &rc, bg); DeleteObject(bg);
    LayoutEditor(e);
    ZeroMemory(&bi, sizeof(bi)); bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = e->image->width; bi.bmiHeader.biHeight = -e->image->height; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, e->view.left, e->view.top, RectW(e->view), RectH(e->view), 0, 0, e->image->width, e->image->height, e->dibBits, &bi, DIB_RGB_COLORS, SRCCOPY);
    if (e->dragging && (e->tool == ET_RECT || e->tool == ET_CIRCLE || e->tool == ET_HEAL)) {
        HPEN p = CreatePen(PS_SOLID, 2, e->tool == ET_HEAL ? RGB(70,220,255) : e->color);
        HGDIOBJ oldp = SelectObject(dc, p), oldb = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        int x1 = e->view.left + (int)(min(e->anchor.x,e->last.x)*e->scale);
        int y1 = e->view.top + (int)(min(e->anchor.y,e->last.y)*e->scale);
        int x2 = e->view.left + (int)(max(e->anchor.x,e->last.x)*e->scale);
        int y2 = e->view.top + (int)(max(e->anchor.y,e->last.y)*e->scale);
        Rectangle(dc, x1,y1,x2+1,y2+1); SelectObject(dc, oldb); SelectObject(dc, oldp); DeleteObject(p);
    }
    EndPaint(hwnd, &ps);
}
static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditState *e = &g_edit; POINT p, q; RECT r;
    switch (msg) {
    case WM_CREATE:
        AddEditorButton(hwnd,"Pen",ED_PEN,8,48); AddEditorButton(hwnd,"Rectangle",ED_RECT,60,74); AddEditorButton(hwnd,"Circle",ED_CIRCLE,138,56);
        AddEditorButton(hwnd,"Heal selection",ED_HEAL,198,94); AddEditorButton(hwnd,"Text",ED_TEXT_BUTTON,296,44); AddEditorButton(hwnd,"Color...",ED_COLOR,344,62);
        e->text = CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,410,10,125,24,hwnd,(HMENU)(INT_PTR)ED_TEXT_ENTRY,g_instance,NULL);
        AddEditorButton(hwnd,"Save",ED_SAVE,541,52); AddEditorButton(hwnd,"Cancel",ED_CANCEL,597,62); return 0;
    case WM_SIZE: InvalidateRect(hwnd,NULL,FALSE); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ED_PEN: e->tool=ET_PEN; break; case ED_RECT: e->tool=ET_RECT; break; case ED_CIRCLE: e->tool=ET_CIRCLE; break; case ED_HEAL: e->tool=ET_HEAL; break;
        case ED_COLOR: ChooseEditorColor(e); return 0;
        case ED_TEXT_BUTTON: e->tool=ET_TEXT; SetFocus(e->text); return 0;
        case ED_SAVE: e->accepted=TRUE; DestroyWindow(hwnd); return 0;
        case ED_CANCEL: e->accepted=FALSE; DestroyWindow(hwnd); return 0;
        default: break;
        }
        return 0;
    case WM_LBUTTONDOWN:
        p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp); LayoutEditor(e);
        if (ToImage(e,p,&q)) {
            if (e->tool == ET_TEXT) {
                char text[512]; HFONT font; HGDIOBJ oldf;
                GetWindowTextA(e->text, text, sizeof(text));
                if (text[0]) {
                    font = CreateFontA(-max(14,(int)(26.0/e->scale)),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,"Arial");
                    if (font) { oldf=SelectObject(e->memdc,font); SetTextColor(e->memdc,e->color); SetBkMode(e->memdc,TRANSPARENT); TextOutA(e->memdc,q.x,q.y,text,lstrlenA(text)); SelectObject(e->memdc,oldf); DeleteObject(font); }
                    InvalidateRect(hwnd,NULL,FALSE);
                }
                return 0;
            }
            e->anchor=e->last=q; e->dragging=TRUE; SetCapture(hwnd); if(e->tool==ET_PEN)DrawSolidLine(e,q,q); InvalidateRect(hwnd,NULL,FALSE);
        } return 0;
    case WM_MOUSEMOVE:
        if (e->dragging) { p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp); if(ToImage(e,p,&q)) { if(e->tool==ET_PEN)DrawSolidLine(e,e->last,q); e->last=q; InvalidateRect(hwnd,NULL,FALSE); } } return 0;
    case WM_LBUTTONUP:
        if(e->dragging) {
            p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp); if(ToImage(e,p,&q))e->last=q;
            e->dragging=FALSE; ReleaseCapture();
            if(e->tool==ET_RECT||e->tool==ET_CIRCLE) {
                HPEN pen=CreatePen(PS_SOLID, max(2,(int)(3.0/e->scale)),e->color); HGDIOBJ oldp=SelectObject(e->memdc,pen), oldb=SelectObject(e->memdc,GetStockObject(HOLLOW_BRUSH));
                r.left=min(e->anchor.x,e->last.x);r.top=min(e->anchor.y,e->last.y);r.right=max(e->anchor.x,e->last.x);r.bottom=max(e->anchor.y,e->last.y);
                if(e->tool==ET_RECT)Rectangle(e->memdc,r.left,r.top,r.right,r.bottom);else Ellipse(e->memdc,r.left,r.top,r.right,r.bottom);
                SelectObject(e->memdc,oldb);SelectObject(e->memdc,oldp);DeleteObject(pen);
            } else if(e->tool==ET_HEAL) { r.left=e->anchor.x;r.top=e->anchor.y;r.right=e->last.x;r.bottom=e->last.y;HealRect(e,r); }
            InvalidateRect(hwnd,NULL,FALSE);
        } return 0;
    case WM_PAINT: EditorPaint(e,hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: e->accepted=FALSE; DestroyWindow(hwnd); return 0;
    case WM_DESTROY: return 0;
    }
    return DefWindowProcA(hwnd,msg,wp,lp);
}

BOOL Editor_Run(HWND owner, PixelImage *img)
{
    WNDCLASSA wc; RECT wr; MSG msg; DWORD bytes; BOOL registered = FALSE;
    ZeroMemory(&g_edit,sizeof(g_edit)); g_edit.image=img; g_edit.color=RGB(255,0,0); g_edit.tool=ET_PEN;
    bytes=(DWORD)((size_t)img->width*(size_t)img->height*4U);
    ZeroMemory(&wc,sizeof(wc)); wc.lpfnWndProc=EditorProc; wc.hInstance=g_instance; wc.lpszClassName=ED_CLASS; wc.hCursor=LoadCursor(NULL,IDC_CROSS); wc.hIcon=LoadIcon(NULL,IDI_APPLICATION); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if(RegisterClassA(&wc))registered=TRUE; else if(GetLastError()==ERROR_CLASS_ALREADY_EXISTS)registered=TRUE;
    if(!registered)return FALSE;
    g_edit.hwnd=CreateWindowExA(WS_EX_APPWINDOW,ED_CLASS,"ScreenshotService Classic - Screenshot preview",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,1000,720,owner,NULL,g_instance,NULL);
    if(!g_edit.hwnd)return FALSE;
    {
        BITMAPINFO bi; ZeroMemory(&bi,sizeof(bi));bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=img->width;bi.bmiHeader.biHeight=-(LONG)img->height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
        g_edit.memdc=CreateCompatibleDC(NULL);
        if(g_edit.memdc)g_edit.dib=CreateDIBSection(g_edit.memdc,&bi,DIB_RGB_COLORS,(void **)&g_edit.dibBits,NULL,0);
        if(!g_edit.memdc||!g_edit.dib||!g_edit.dibBits){if(g_edit.dib)DeleteObject(g_edit.dib);if(g_edit.memdc)DeleteDC(g_edit.memdc);DestroyWindow(g_edit.hwnd);return FALSE;}
        g_edit.oldBmp=(HBITMAP)SelectObject(g_edit.memdc,g_edit.dib);memcpy(g_edit.dibBits,img->bgra,bytes);
    }
    GetWindowRect(g_edit.hwnd,&wr);
    if(img->width+20 < GetSystemMetrics(SM_CXSCREEN)) wr.right=wr.left+img->width+24;
    if(img->height+ED_TOOLBAR+40 < GetSystemMetrics(SM_CYSCREEN)) wr.bottom=wr.top+img->height+ED_TOOLBAR+45;
    SetWindowPos(g_edit.hwnd,NULL,0,0,wr.right-wr.left,wr.bottom-wr.top,SWP_NOMOVE|SWP_NOZORDER|SWP_SHOWWINDOW);
    ShowWindow(g_edit.hwnd,SW_SHOW);UpdateWindow(g_edit.hwnd);EnableWindow(owner,FALSE);
    while(IsWindow(g_edit.hwnd)&&GetMessageA(&msg,NULL,0,0)>0) { if(!IsDialogMessageA(g_edit.hwnd,&msg)){TranslateMessage(&msg);DispatchMessageA(&msg);} }
    EnableWindow(owner,TRUE);SetForegroundWindow(owner);
    if(g_edit.accepted) {
        int i; size_t count=(size_t)img->width*(size_t)img->height;
        for(i=0;i<(int)count;i++)g_edit.dibBits[(size_t)i*4U+3]=255;
        memcpy(img->bgra,g_edit.dibBits,bytes);
    }
    if(g_edit.memdc){if(g_edit.oldBmp)SelectObject(g_edit.memdc,g_edit.oldBmp);DeleteDC(g_edit.memdc);}
    if(g_edit.dib)DeleteObject(g_edit.dib);
    return g_edit.accepted;
}
