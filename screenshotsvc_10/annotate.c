#define COBJMACROS
#include "annotate.h"
#include "hdr.h"
#include "colorpicker.h"
#include "log.h"
#include <commdlg.h>
#include <strsafe.h>
#include <string.h>
#include <math.h>
#include <windowsx.h>

#define ED_CLASS L"ScreenshotService.AnnotationEditor"
#define ED_TOOLBAR_H 50
#define ED_PEN 4101
#define ED_RECT 4102
#define ED_CIRCLE 4103
#define ED_TEXT 4104
#define ED_ERASER 4105
#define ED_COLOR 4106
#define ED_PICK 4107
#define ED_SAVE 4108
#define ED_CANCEL 4109

typedef enum { AT_PEN, AT_RECT, AT_CIRCLE, AT_TEXT, AT_HEAL } AnnotTool;
typedef struct {
    HWND hwnd, edit;
    SnipImage *img;
    BYTE *base;
    BYTE *pixels;
    HDC dc;
    HBITMAP dib, oldBmp;
    COLORREF color;
    AnnotTool tool;
    BOOL done, commit, dragging;
    POINT anchor, last;
    RECT view;
    double scale;
    int brushWidth;
} Editor;
static Editor g_ed;

static void EditorLayout(Editor *e)
{
    RECT c; double sx,sy; int w,h;
    GetClientRect(e->hwnd,&c);
    w=c.right-c.left-24; h=c.bottom-c.top-ED_TOOLBAR_H-18;
    if (w<1) w=1; if (h<1) h=1;
    sx=(double)w/e->img->width; sy=(double)h/e->img->height;
    e->scale=sx<sy?sx:sy; if (e->scale>1.0) e->scale=1.0;
    if (e->scale<=0) e->scale=0.01;
    w=(int)(e->img->width*e->scale); h=(int)(e->img->height*e->scale);
    e->view.left=(c.right-w)/2;
    e->view.top=ED_TOOLBAR_H+((c.bottom-ED_TOOLBAR_H)-h)/2;
    e->view.right=e->view.left+w;
    e->view.bottom=e->view.top+h;
    e->brushWidth=(int)(4.0/e->scale+0.5); if(e->brushWidth<3)e->brushWidth=3; if(e->brushWidth>100)e->brushWidth=100;
}
static BOOL ClientToImage(Editor *e, POINT p, POINT *out)
{
    if (!PtInRect(&e->view,p)) return FALSE;
    out->x=(LONG)((p.x-e->view.left)/e->scale+0.5);
    out->y=(LONG)((p.y-e->view.top)/e->scale+0.5);
    if(out->x<0)out->x=0;if(out->y<0)out->y=0;
    if(out->x>=(LONG)e->img->width)out->x=(LONG)e->img->width-1;
    if(out->y>=(LONG)e->img->height)out->y=(LONG)e->img->height-1;
    return TRUE;
}
static BITMAPINFO *EditorBmi(BITMAPINFO *bi, UINT32 w, UINT32 h)
{
    ZeroMemory(bi,sizeof(*bi)); bi->bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi->bmiHeader.biWidth=(LONG)w; bi->bmiHeader.biHeight=-(LONG)h;
    bi->bmiHeader.biPlanes=1; bi->bmiHeader.biBitCount=32; bi->bmiHeader.biCompression=BI_RGB;
    return bi;
}
static BOOL EditorMakeDib(Editor *e)
{
    BITMAPINFO bi; SIZE_T bytes=(SIZE_T)e->img->width*e->img->height*4;
    e->dc=CreateCompatibleDC(NULL); if(!e->dc)return FALSE;
    e->dib=CreateDIBSection(e->dc,EditorBmi(&bi,e->img->width,e->img->height),DIB_RGB_COLORS,(void **)&e->pixels,NULL,0);
    if(!e->dib||!e->pixels)return FALSE;
    e->oldBmp=(HBITMAP)SelectObject(e->dc,e->dib); SetBkMode(e->dc,TRANSPARENT);
    e->base=(BYTE *)HeapAlloc(GetProcessHeap(),0,bytes); if(!e->base)return FALSE;
    if(e->img->hdr) {
        Hdr_ToneMapScRgbToSrgb((const UINT16 *)e->img->pixels,e->img->stride/2,
                               e->pixels,e->img->width*4,e->img->width,e->img->height,
                               e->img->sdrWhiteNits,FALSE);
    } else memcpy(e->pixels,e->img->pixels,bytes);
    memcpy(e->base,e->pixels,bytes);
    return TRUE;
}
static void EditorCleanup(Editor *e)
{
    if(e->dc){if(e->oldBmp)SelectObject(e->dc,e->oldBmp);DeleteDC(e->dc);}
    if(e->dib)DeleteObject(e->dib);
    if(e->base)HeapFree(GetProcessHeap(),0,e->base);
    ZeroMemory(e,sizeof(*e));
}
static void HealAt(Editor *e, POINT p)
{
    int r=(int)(18.0/e->scale+0.5), dx,dy; BYTE *tmp; SIZE_T n;
    if(r<8)r=8;if(r>64)r=64;
    n=(SIZE_T)(2*r+1)*(2*r+1)*4; tmp=(BYTE *)HeapAlloc(GetProcessHeap(),0,n); if(!tmp)return;
    /* Sample an adjacent patch, then feather it into the target. This behaves
       like a soft clone/heal brush: no blur or pixelation blocks are introduced. */
    for(dy=-r;dy<=r;dy++) for(dx=-r;dx<=r;dx++) {
        int sx=p.x+dx-2*r, sy=p.y+dy;
        BYTE *q=tmp+((SIZE_T)(dy+r)*(2*r+1)+(dx+r))*4;
        if(sx<0)sx=0;if(sx>=(int)e->img->width)sx=(int)e->img->width-1;
        if(sy<0)sy=0;if(sy>=(int)e->img->height)sy=(int)e->img->height-1;
        memcpy(q,e->pixels+(SIZE_T)sy*e->img->width*4+(SIZE_T)sx*4,4);
    }
    for(dy=-r;dy<=r;dy++) for(dx=-r;dx<=r;dx++) {
        int d2=dx*dx+dy*dy; float t; BYTE *dst,*src; int k;
        if(d2>r*r)continue;
        t=1.0f-sqrtf((float)d2)/(float)r; t=0.88f*t*t;
        dst=e->pixels+(SIZE_T)(p.y+dy<0?0:p.y+dy>=(LONG)e->img->height?e->img->height-1:p.y+dy)*e->img->width*4+
            (SIZE_T)(p.x+dx<0?0:p.x+dx>=(LONG)e->img->width?e->img->width-1:p.x+dx)*4;
        src=tmp+((SIZE_T)(dy+r)*(2*r+1)+(dx+r))*4;
        for(k=0;k<3;k++) dst[k]=(BYTE)(dst[k]*(1.0f-t)+src[k]*t+0.5f);
    }
    HeapFree(GetProcessHeap(),0,tmp);
}
static void EditorDrawLine(Editor *e, POINT a, POINT b, BOOL dotted)
{
    HPEN pen=CreatePen(dotted?PS_DOT:PS_SOLID,e->brushWidth,e->color); HGDIOBJ old;
    if(!pen)return; old=SelectObject(e->dc,pen); MoveToEx(e->dc,a.x,a.y,NULL); LineTo(e->dc,b.x,b.y);
    SelectObject(e->dc,old); DeleteObject(pen);
}
static void EditorText(Editor *e, POINT p)
{
    wchar_t text[512]; HFONT font; HGDIOBJ oldf; int fs=(int)(26.0/e->scale+0.5);
    if(fs<16)fs=16;if(fs>160)fs=160;
    GetWindowTextW(e->edit,text,ARRAYSIZE(text)); if(!text[0])return;
    font=CreateFontW(-fs,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    if(!font)return; oldf=SelectObject(e->dc,font); SetTextColor(e->dc,e->color); SetBkMode(e->dc,TRANSPARENT);
    TextOutW(e->dc,p.x,p.y,text,lstrlenW(text)); SelectObject(e->dc,oldf); DeleteObject(font);
}
static void EditorChooseColor(Editor *e)
{
    static COLORREF custom[16]; CHOOSECOLORW cc;
    ZeroMemory(&cc,sizeof(cc));cc.lStructSize=sizeof(cc);cc.hwndOwner=e->hwnd;
    cc.rgbResult=e->color;cc.lpCustColors=custom;cc.Flags=CC_FULLOPEN|CC_RGBINIT;
    if(ChooseColorW(&cc)){e->color=cc.rgbResult;InvalidateRect(e->hwnd,NULL,FALSE);}
}
static void EditorPickColor(Editor *e)
{
    COLORREF c; wchar_t desc[160];
    if(ColorPicker_Run(e->hwnd,&c,desc,ARRAYSIZE(desc))){e->color=c;InvalidateRect(e->hwnd,NULL,FALSE);}
}
static void EditorCommit(Editor *e)
{
    SIZE_T i, pixels=(SIZE_T)e->img->width*e->img->height;
    /* GDI does not promise to maintain the alpha byte while drawing into a
       32-bit DIB. The screenshot is opaque, so restore it before encoding. */
    for(i=0;i<pixels;i++) e->pixels[i*4+3]=255;
    if(!e->img->hdr) memcpy(e->img->pixels,e->pixels,pixels*4);
    else {
        UINT16 *dst=(UINT16 *)e->img->pixels;
        float scale=(e->img->sdrWhiteNits>0?e->img->sdrWhiteNits:80.0f)/80.0f;
        for(i=0;i<pixels;i++) {
            BYTE *a=e->pixels+i*4, *b=e->base+i*4; UINT16 *d=dst+i*4;
            if(a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2])continue;
            d[0]=Hdr_FloatToHalf(Hdr_SrgbToLinear(a[2]/255.0f)*scale);
            d[1]=Hdr_FloatToHalf(Hdr_SrgbToLinear(a[1]/255.0f)*scale);
            d[2]=Hdr_FloatToHalf(Hdr_SrgbToLinear(a[0]/255.0f)*scale);
            d[3]=Hdr_FloatToHalf(1.0f);
        }
    }
    if(e->img->sdr10Pixels){HeapFree(GetProcessHeap(),0,e->img->sdr10Pixels);e->img->sdr10Pixels=NULL;}
    e->img->sdr10Stride=0;e->img->sdr10Valid=FALSE;
}
static void EditorCreateButton(HWND w,const wchar_t *label,int id,int x,int width)
{
    CreateWindowExW(0,L"BUTTON",label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
        x,8,width,30,w,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
}
static void EditorPaint(Editor *e, HWND hwnd)
{
    PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); RECT c; BITMAPINFO bi; HBRUSH bg;
    GetClientRect(hwnd,&c); bg=CreateSolidBrush(RGB(30,30,32));FillRect(dc,&c,bg);DeleteObject(bg);
    EditorLayout(e);
    SetStretchBltMode(dc,HALFTONE); EditorBmi(&bi,e->img->width,e->img->height);
    StretchDIBits(dc,e->view.left,e->view.top,e->view.right-e->view.left,e->view.bottom-e->view.top,
        0,0,e->img->width,e->img->height,e->pixels,&bi,DIB_RGB_COLORS,SRCCOPY);
    if(e->dragging&&(e->tool==AT_RECT||e->tool==AT_CIRCLE)) {
        POINT a={e->view.left+(LONG)(e->anchor.x*e->scale),e->view.top+(LONG)(e->anchor.y*e->scale)};
        POINT b={e->view.left+(LONG)(e->last.x*e->scale),e->view.top+(LONG)(e->last.y*e->scale)};
        HPEN p=CreatePen(PS_SOLID,2,e->color);HGDIOBJ oldp=SelectObject(dc,p),oldb=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
        if(e->tool==AT_RECT)Rectangle(dc,a.x,a.y,b.x+1,b.y+1);else Ellipse(dc,a.x,a.y,b.x+1,b.y+1);
        SelectObject(dc,oldb);SelectObject(dc,oldp);DeleteObject(p);
    }
    EndPaint(hwnd,&ps);
}
static LRESULT CALLBACK EditorProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    Editor *e=&g_ed; POINT p,ip;
    switch(msg) {
    case WM_NCCREATE:e->hwnd=hwnd;return DefWindowProcW(hwnd,msg,wp,lp);
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND h;
        /* The toolbar sits above the canvas and must remain visible when the
           parent repaints during pen strokes. WS_CLIPCHILDREN is also set on
           the top-level window so canvas painting cannot cover these tools. */
        EditorCreateButton(hwnd,L"Pen",ED_PEN,8,54);
        EditorCreateButton(hwnd,L"Rectangle",ED_RECT,66,82);
        EditorCreateButton(hwnd,L"Circle",ED_CIRCLE,152,60);
        EditorCreateButton(hwnd,L"Text",ED_TEXT,216,54);
        EditorCreateButton(hwnd,L"Heal eraser",ED_ERASER,274,92);
        EditorCreateButton(hwnd,L"Color…",ED_COLOR,370,70);
        EditorCreateButton(hwnd,L"Pipette",ED_PICK,444,66);
        e->edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
            514,10,190,26,hwnd,(HMENU)4110,GetModuleHandleW(NULL),NULL);
        EditorCreateButton(hwnd,L"Save",ED_SAVE,712,64);
        EditorCreateButton(hwnd,L"Cancel",ED_CANCEL,780,72);
        /* Apply a consistent, readable font to every child control. */
        for (int id = ED_PEN; id <= ED_CANCEL; ++id) {
            h = GetDlgItem(hwnd, id);
            if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        }
        if (e->edit) SendMessageW(e->edit, WM_SETFONT, (WPARAM)font, TRUE);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_COMMAND:
        switch(LOWORD(wp)){
        case ED_PEN:e->tool=AT_PEN;break;case ED_RECT:e->tool=AT_RECT;break;
        case ED_CIRCLE:e->tool=AT_CIRCLE;break;case ED_TEXT:e->tool=AT_TEXT;break;case ED_ERASER:e->tool=AT_HEAL;break;
        case ED_COLOR:EditorChooseColor(e);return 0;case ED_PICK:EditorPickColor(e);return 0;
        case ED_SAVE:EditorCommit(e);e->commit=TRUE;e->done=TRUE;DestroyWindow(hwnd);return 0;
        case ED_CANCEL:e->done=TRUE;DestroyWindow(hwnd);return 0;
        default:break;
        }
        InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_LBUTTONDOWN:
        p.x=GET_X_LPARAM(lp);p.y=GET_Y_LPARAM(lp);
        if(ClientToImage(e,p,&ip)){e->anchor=e->last=ip;e->dragging=TRUE;SetCapture(hwnd);
            if(e->tool==AT_PEN){EditorDrawLine(e,ip,ip,FALSE);}
            else if(e->tool==AT_HEAL)HealAt(e,ip);
            else if(e->tool==AT_TEXT){EditorText(e,ip);e->dragging=FALSE;ReleaseCapture();}
            InvalidateRect(hwnd,NULL,FALSE);}
        return 0;
    case WM_MOUSEMOVE:
        if(e->dragging){p.x=GET_X_LPARAM(lp);p.y=GET_Y_LPARAM(lp);if(ClientToImage(e,p,&ip)){
            if(e->tool==AT_PEN)EditorDrawLine(e,e->last,ip,FALSE);
            else if(e->tool==AT_HEAL)HealAt(e,ip);
            e->last=ip;InvalidateRect(hwnd,NULL,FALSE);}}
        return 0;
    case WM_LBUTTONUP:
        if(e->dragging){p.x=GET_X_LPARAM(lp);p.y=GET_Y_LPARAM(lp);if(ClientToImage(e,p,&ip)){e->last=ip;
            if(e->tool==AT_RECT||e->tool==AT_CIRCLE){HPEN pen=CreatePen(PS_SOLID,e->brushWidth,e->color);HGDIOBJ op=SelectObject(e->dc,pen),ob=SelectObject(e->dc,GetStockObject(NULL_BRUSH));
                if(e->tool==AT_RECT)Rectangle(e->dc,e->anchor.x,e->anchor.y,ip.x,ip.y);else Ellipse(e->dc,e->anchor.x,e->anchor.y,ip.x,ip.y);
                SelectObject(e->dc,ob);SelectObject(e->dc,op);DeleteObject(pen);}}
            e->dragging=FALSE;ReleaseCapture();InvalidateRect(hwnd,NULL,FALSE);}
        return 0;
    case WM_KEYDOWN: if(wp==VK_ESCAPE){e->done=TRUE;DestroyWindow(hwnd);return 0;} if(wp==VK_RETURN){EditorCommit(e);e->commit=TRUE;e->done=TRUE;DestroyWindow(hwnd);return 0;} break;
    case WM_PAINT: EditorPaint(e,hwnd);return 0;
    case WM_SETCURSOR: if(LOWORD(lp)==HTCLIENT){SetCursor(LoadCursorW(NULL,e->tool==AT_HEAL?IDC_HAND:IDC_CROSS));return TRUE;}break;
    case WM_CLOSE:e->done=TRUE;DestroyWindow(hwnd);return 0;
    case WM_DESTROY: return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL Annotate_Edit(HWND owner,SnipImage *img)
{
    WNDCLASSEXW wc; HINSTANCE inst=GetModuleHandleW(NULL); HMONITOR mon; MONITORINFO mi;
    RECT work; HWND w; MSG msg; BOOL registered; int ww,hh;
    if(!img||!img->pixels||!img->width||!img->height)return TRUE;
    ZeroMemory(&g_ed,sizeof(g_ed));g_ed.img=img;g_ed.color=RGB(255,0,0);g_ed.tool=AT_PEN;
    if(!EditorMakeDib(&g_ed)){Log_Printf(L"annotate: could not create editor bitmap for %ux%u",img->width,img->height);EditorCleanup(&g_ed);return TRUE;}
    ZeroMemory(&wc,sizeof(wc));wc.cbSize=sizeof(wc);wc.hInstance=inst;wc.lpfnWndProc=EditorProc;
    wc.lpszClassName=ED_CLASS;wc.hCursor=LoadCursorW(NULL,IDC_CROSS);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    registered=RegisterClassExW(&wc)!=0||GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
    if(!registered){EditorCleanup(&g_ed);return TRUE;}
    {POINT p;GetCursorPos(&p);mon=MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST);}
    ZeroMemory(&mi,sizeof(mi));mi.cbSize=sizeof(mi);if(!GetMonitorInfoW(mon,&mi))SystemParametersInfoW(SPI_GETWORKAREA,0,&mi.rcWork,0);
    work=mi.rcWork;ww=work.right-work.left;hh=work.bottom-work.top;
    UNREFERENCED_PARAMETER(owner);
    /* Do not make this editor an owned window of the hidden tray host: on
       some Windows z-order paths that can keep the editor behind other apps. */
    w=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,ED_CLASS,L"ScreenshotService — annotate screenshot",
        WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,work.left,work.top,ww,hh,NULL,NULL,inst,NULL);
    if(!w){Log_Printf(L"annotate: CreateWindow failed (%lu)",(unsigned long)GetLastError());EditorCleanup(&g_ed);return TRUE;}g_ed.hwnd=w;
    ZeroMemory(&msg,sizeof(msg));
    ShowWindow(w,SW_SHOWNORMAL);
    SetWindowPos(w,HWND_TOPMOST,work.left,work.top,ww,hh,SWP_SHOWWINDOW|SWP_FRAMECHANGED);
    UpdateWindow(w);
    SetForegroundWindow(w);
    SetActiveWindow(w);
    SetFocus(w);
    Log_Printf(L"annotate: editor shown, canvas=%ux%u, toolbar=visible",img->width,img->height);
    while(!g_ed.done&&GetMessageW(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    if(msg.message==WM_QUIT)PostQuitMessage((int)msg.wParam);
    /* The DIB's bytes are the editable preview; for HDR only modified pixels
       are translated back to scRGB so unedited highlights remain untouched. */
    EditorCleanup(&g_ed);return TRUE;
}
