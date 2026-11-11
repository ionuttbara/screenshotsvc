#define COBJMACROS
#include "colorpicker.h"
#include <strsafe.h>
#include <math.h>
#include <string.h>

#define CP_CLASS L"ScreenshotService.ColorPicker"
#define CP_TIP_CLASS L"ScreenshotService.ColorPickerTip"

typedef struct {
    HWND owner, overlay, tip;
    COLORREF color;
    BOOL haveColor, done, chosen;
    wchar_t name[48];
    wchar_t text[160];
} CPState;
static CPState g_cp;
static HCURSOR g_pipetteCursor;
static const wchar_t *g_names[] = {
    L"Black",L"White",L"Red",L"Green",L"Blue",L"Yellow",L"Orange",L"Purple",
    L"Pink",L"Brown",L"Gray",L"Cyan",L"Magenta",L"Navy",L"Teal",L"Olive",L"Maroon",L"Lime",L"Silver"
};
static const COLORREF g_nameRGB[] = {
    RGB(0,0,0),RGB(255,255,255),RGB(255,0,0),RGB(0,128,0),RGB(0,0,255),RGB(255,255,0),
    RGB(255,165,0),RGB(128,0,128),RGB(255,192,203),RGB(165,42,42),RGB(128,128,128),
    RGB(0,255,255),RGB(255,0,255),RGB(0,0,128),RGB(0,128,128),RGB(128,128,0),
    RGB(128,0,0),RGB(0,255,0),RGB(192,192,192)
};


static HCURSOR MakePipetteCursor(void)
{
    BITMAPINFO bi; HDC dc,maskDC; HBITMAP colorBmp,maskBmp,old; BYTE *bits; ICONINFO ii; HCURSOR cur;
    int x,y; if(g_pipetteCursor)return g_pipetteCursor;
    ZeroMemory(&bi,sizeof(bi));bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=32;
    bi.bmiHeader.biHeight=-32;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    dc=CreateCompatibleDC(NULL);if(!dc)return LoadCursorW(NULL,IDC_CROSS);
    colorBmp=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,(void **)&bits,NULL,0);
    if(!colorBmp||!bits){if(colorBmp)DeleteObject(colorBmp);DeleteDC(dc);return LoadCursorW(NULL,IDC_CROSS);}
    old=(HBITMAP)SelectObject(dc,colorBmp);
    for(y=0;y<32;y++)for(x=0;x<32;x++){BYTE *q=bits+((SIZE_T)y*32+x)*4;q[0]=255;q[1]=0;q[2]=255;q[3]=0;}
    { HPEN black=CreatePen(PS_SOLID,7,RGB(20,20,20));HGDIOBJ op=SelectObject(dc,black);
      MoveToEx(dc,6,28,NULL);LineTo(dc,22,12);MoveToEx(dc,13,11,NULL);LineTo(dc,23,21);
      SelectObject(dc,op);DeleteObject(black); }
    { HPEN white=CreatePen(PS_SOLID,3,RGB(245,245,245));HGDIOBJ op=SelectObject(dc,white);
      MoveToEx(dc,6,28,NULL);LineTo(dc,22,12);MoveToEx(dc,13,11,NULL);LineTo(dc,23,21);
      SelectObject(dc,op);DeleteObject(white); }
    GdiFlush();
    for(y=0;y<32;y++)for(x=0;x<32;x++){BYTE *q=bits+((SIZE_T)y*32+x)*4;if(!(q[0]==255&&q[1]==0&&q[2]==255))q[3]=255;}
    SelectObject(dc,old);DeleteDC(dc);
    maskDC=CreateCompatibleDC(NULL);maskBmp=CreateBitmap(32,32,1,1,NULL);
    if(maskDC&&maskBmp){HGDIOBJ om=SelectObject(maskDC,maskBmp);PatBlt(maskDC,0,0,32,32,BLACKNESS);SelectObject(maskDC,om);}
    ZeroMemory(&ii,sizeof(ii));ii.fIcon=FALSE;ii.xHotspot=5;ii.yHotspot=28;ii.hbmColor=colorBmp;ii.hbmMask=maskBmp;
    cur=CreateIconIndirect(&ii);
    if(maskDC)DeleteDC(maskDC);if(maskBmp)DeleteObject(maskBmp);DeleteObject(colorBmp);
    if(cur)g_pipetteCursor=cur;else g_pipetteCursor=LoadCursorW(NULL,IDC_CROSS);
    return g_pipetteCursor;
}

static unsigned ColorDistance(COLORREF a, COLORREF b)
{
    int dr=(int)GetRValue(a)-(int)GetRValue(b);
    int dg=(int)GetGValue(a)-(int)GetGValue(b);
    int db=(int)GetBValue(a)-(int)GetBValue(b);
    return (unsigned)(dr*dr*3 + dg*dg*4 + db*db*2);
}
static void UpdateTip(POINT p)
{
    HDC dc;
    COLORREF c=RGB(0,0,0);
    unsigned i, best=0xFFFFFFFFu;
    int x, y, sw=GetSystemMetrics(SM_CXVIRTUALSCREEN), sh=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int sl=GetSystemMetrics(SM_XVIRTUALSCREEN), st=GetSystemMetrics(SM_YVIRTUALSCREEN);
    wchar_t msg[160];
    dc=GetDC(NULL);
    if (dc) { c=GetPixel(dc,p.x,p.y); ReleaseDC(NULL,dc); }
    if (c==(COLORREF)CLR_INVALID) c=RGB(0,0,0);
    for (i=0;i<ARRAYSIZE(g_nameRGB);i++) {
        unsigned d=ColorDistance(c,g_nameRGB[i]);
        if (d<best) { best=d; StringCchCopyW(g_cp.name,ARRAYSIZE(g_cp.name),g_names[i]); }
    }
    g_cp.color=c; g_cp.haveColor=TRUE;
    StringCchPrintfW(msg,ARRAYSIZE(msg),L"%s   RGB(%u, %u, %u)   #%02X%02X%02X",
        g_cp.name,(unsigned)GetRValue(c),(unsigned)GetGValue(c),(unsigned)GetBValue(c),
        (unsigned)GetRValue(c),(unsigned)GetGValue(c),(unsigned)GetBValue(c));
    StringCchCopyW(g_cp.text,ARRAYSIZE(g_cp.text),msg);
    if (!g_cp.tip) return;
    SetWindowTextW(g_cp.tip,g_cp.text);
    x=p.x+22; y=p.y+26;
    if (x+310>sl+sw) x=p.x-320;
    if (y+34>st+sh) y=p.y-42;
    SetWindowPos(g_cp.tip,HWND_TOPMOST,x,y,310,34,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    InvalidateRect(g_cp.tip,NULL,TRUE);
}
static LRESULT CALLBACK TipProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    if (msg==WM_PAINT) {
        PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); RECT r; HBRUSH b; wchar_t txt[180];
        GetClientRect(hwnd,&r); b=CreateSolidBrush(RGB(28,28,30)); FillRect(dc,&r,b); DeleteObject(b);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
        StringCchCopyW(txt,ARRAYSIZE(txt),g_cp.text); r.left=7; r.right-=5;
        DrawTextW(dc,txt,-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        EndPaint(hwnd,&ps); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static void CopyColor(void)
{
    wchar_t txt[128]; SIZE_T cb; HGLOBAL h; void *p;
    StringCchPrintfW(txt,ARRAYSIZE(txt),L"RGB(%u, %u, %u)  #%02X%02X%02X",
        (unsigned)GetRValue(g_cp.color),(unsigned)GetGValue(g_cp.color),(unsigned)GetBValue(g_cp.color),
        (unsigned)GetRValue(g_cp.color),(unsigned)GetGValue(g_cp.color),(unsigned)GetBValue(g_cp.color));
    cb=((SIZE_T)lstrlenW(txt)+1)*sizeof(wchar_t);
    if (!OpenClipboard(g_cp.owner)) return;
    EmptyClipboard(); h=GlobalAlloc(GMEM_MOVEABLE,cb);
    if (h) { p=GlobalLock(h); if (p) { memcpy(p,txt,cb); GlobalUnlock(h); if (!SetClipboardData(CF_UNICODETEXT,h)) GlobalFree(h); } else GlobalFree(h); }
    CloseClipboard();
}
static LRESULT CALLBACK PickerProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg) {
    case WM_MOUSEMOVE: { POINT p; GetCursorPos(&p); UpdateTip(p); SetCursor(MakePipetteCursor()); return 0; }
    case WM_LBUTTONDOWN: {
        POINT p; GetCursorPos(&p); UpdateTip(p);
        if (g_cp.haveColor) { g_cp.chosen=TRUE; CopyColor(); }
        g_cp.done=TRUE; ReleaseCapture(); DestroyWindow(hwnd); return 0;
    }
    case WM_RBUTTONDOWN: case WM_KEYDOWN:
        if (msg==WM_RBUTTONDOWN || wp==VK_ESCAPE) { g_cp.done=TRUE; ReleaseCapture(); DestroyWindow(hwnd); return 0; }
        break;
    case WM_SETCURSOR: SetCursor(MakePipetteCursor()); return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd,&ps); EndPaint(hwnd,&ps); return 0; }
    case WM_DESTROY: return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL ColorPicker_Run(HWND owner,COLORREF *outColor,wchar_t *description,size_t cchDescription)
{
    WNDCLASSEXW wc; RECT vr; HWND w; MSG msg; BOOL registered=FALSE; HINSTANCE inst=GetModuleHandleW(NULL);
    if (outColor) *outColor=RGB(255,0,0);
    if (description && cchDescription) description[0]=0;
    ZeroMemory(&g_cp,sizeof(g_cp)); g_cp.owner=owner;
    ZeroMemory(&wc,sizeof(wc)); wc.cbSize=sizeof(wc); wc.hInstance=inst; wc.lpfnWndProc=TipProc;
    wc.lpszClassName=CP_TIP_CLASS; wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if (RegisterClassExW(&wc) || GetLastError()==ERROR_CLASS_ALREADY_EXISTS) registered=TRUE;
    if (!registered) return FALSE;
    ZeroMemory(&wc,sizeof(wc)); wc.cbSize=sizeof(wc); wc.hInstance=inst; wc.lpfnWndProc=PickerProc;
    wc.lpszClassName=CP_CLASS; wc.hCursor=LoadCursorW(NULL,IDC_CROSS); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return FALSE;
    vr.left=GetSystemMetrics(SM_XVIRTUALSCREEN); vr.top=GetSystemMetrics(SM_YVIRTUALSCREEN);
    vr.right=vr.left+GetSystemMetrics(SM_CXVIRTUALSCREEN); vr.bottom=vr.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
    w=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_NOACTIVATE,CP_CLASS,L"ScreenshotService color picker",
        WS_POPUP,vr.left,vr.top,vr.right-vr.left,vr.bottom-vr.top,owner,NULL,inst,NULL);
    if (!w) return FALSE;
    g_cp.overlay=w;
    g_cp.tip=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,CP_TIP_CLASS,L"",WS_POPUP,
        0,0,310,34,owner,NULL,inst,NULL);
    SetLayeredWindowAttributes(w,0,2,LWA_ALPHA);
    ZeroMemory(&msg,sizeof(msg));
    ShowWindow(w,SW_SHOWNOACTIVATE); SetCapture(w); SetCursor(MakePipetteCursor());
    { POINT p; GetCursorPos(&p); UpdateTip(p); }
    while (!g_cp.done && GetMessageW(&msg,NULL,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (msg.message==WM_QUIT) PostQuitMessage((int)msg.wParam);
    if (g_cp.tip) { DestroyWindow(g_cp.tip); g_cp.tip=NULL; }
    if (g_cp.overlay && IsWindow(g_cp.overlay)) DestroyWindow(g_cp.overlay);
    g_cp.overlay=NULL;
    if (g_cp.chosen) {
        if (outColor) *outColor=g_cp.color;
        if (description && cchDescription) StringCchCopyW(description,cchDescription,g_cp.text);
    }
    return g_cp.chosen;
}
