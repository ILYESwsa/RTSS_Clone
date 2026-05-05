// ============================================================
// overlay_editor.cpp  —  RTSS Clone Live Config Editor
// Win32 GUI app. Reads/writes config via shared memory so
// the in-game overlay updates in real time without recompile.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include "../Shared/SharedMemory.h"

using namespace Gdiplus;

// ----------------------------------------------------------------
// Overlay config — stored in shared memory extension block
// We append this after RtssStats in the shared mapping.
// The hook DLL reads these values every frame.
// ----------------------------------------------------------------
#define CFG_MAGIC 0xCF6E0001u

struct OverlayConfig
{
    UINT32  magic;

    // Position  0=TL 1=TC 2=TR 3=BL 4=BC 5=BR
    UINT32  position;

    // Font
    float   fontSize;       // 10-22
    float   lineHeight;     // 10-28
    UINT32  fontBold;       // 0 or 1
    UINT32  fontIndex;      // 0=Courier 1=Consolas 2=Arial 3=Verdana

    // Colors (ABGR packed for easy ImGui use)
    UINT32  colLabel;       // orange
    UINT32  colValue;       // white
    UINT32  colUnit;        // grey
    UINT32  colFpsHi;       // green/white
    UINT32  colFpsMid;      // orange
    UINT32  colFpsLo;       // red

    // Background
    float   bgAlpha;        // 0.0 - 1.0

    // Show flags
    UINT32  showGpu;
    UINT32  showCpu;
    UINT32  showRam;
    UINT32  showFps;
    UINT32  showFrametime;
    UINT32  showGraph;

    // Label column width
    float   labelWidth;     // pixels
    float   padding;        // window padding
};

#define RTSS_CFG_OFFSET   4096   // config starts after RtssStats in mapping

// ----------------------------------------------------------------
// Shared memory helpers
// ----------------------------------------------------------------
static HANDLE      g_hMap    = nullptr;
static BYTE*       g_pBase   = nullptr;
static RtssStats*  g_pStats  = nullptr;
static OverlayConfig* g_pCfg = nullptr;

static bool OpenSharedMem()
{
    if (g_hMap) return true;
    g_hMap = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, RTSS_SHARED_MEM_NAME);
    if (!g_hMap) return false;
    g_pBase  = (BYTE*)::MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!g_pBase) { ::CloseHandle(g_hMap); g_hMap=nullptr; return false; }
    g_pStats = (RtssStats*)g_pBase;
    g_pCfg   = (OverlayConfig*)(g_pBase + RTSS_CFG_OFFSET);
    return true;
}

static void CloseSharedMem()
{
    if (g_pBase) ::UnmapViewOfFile(g_pBase);
    if (g_hMap)  ::CloseHandle(g_hMap);
    g_pBase=nullptr; g_hMap=nullptr; g_pStats=nullptr; g_pCfg=nullptr;
}

// ----------------------------------------------------------------
// Default config
// ----------------------------------------------------------------
static OverlayConfig g_cfg = {
    CFG_MAGIC,
    0,        // TL
    13.f, 18.f, 0, 0,
    0xFF2080E0, // label  (ABGR: orange)
    0xFFFFFFFF, // value  (white)
    0xFF888888, // unit   (grey)
    0xFFFFFFFF, // fps hi
    0xFF20A0E0, // fps mid
    0xFF3030E0, // fps lo
    0.f,        // bg alpha
    1,1,1,1,1,1,
    42.f, 4.f
};

static void PushConfig()
{
    if (!g_pCfg) return;
    memcpy(g_pCfg, &g_cfg, sizeof(OverlayConfig));
}

// ----------------------------------------------------------------
// Window layout
// ----------------------------------------------------------------
#define WW  420
#define WH  620

static ULONG_PTR g_gdipToken = 0;
static HWND      g_hwnd      = nullptr;
static bool      g_connected = false;
static bool      g_dragging  = false;
static POINT     g_dragOff   = {};

// Control IDs
#define ID_TIMER      100
#define ID_POS_TL     201
#define ID_POS_TC     202
#define ID_POS_TR     203
#define ID_POS_BL     204
#define ID_POS_BC     205
#define ID_POS_BR     206
#define ID_BOLD       210
#define ID_SHOW_GPU   220
#define ID_SHOW_CPU   221
#define ID_SHOW_RAM   222
#define ID_SHOW_FPS   223
#define ID_SHOW_FT    224
#define ID_SHOW_GR    225
#define ID_SAVE       230
#define ID_LOAD       231

// Slider IDs
#define ID_SL_FSIZE   300
#define ID_SL_LINEH   301
#define ID_SL_BGALPHA 302
#define ID_SL_LBLW    303
#define ID_SL_PAD     304

// Color button IDs
#define ID_COL_LABEL  400
#define ID_COL_VAL    401
#define ID_COL_UNIT   402
#define ID_COL_FPSHI  403
#define ID_COL_FPSMD  404
#define ID_COL_FPSLO  405

static HWND g_controls[50] = {};
static int  g_nCtrl = 0;

// ----------------------------------------------------------------
// Color utilities
// ----------------------------------------------------------------
static COLORREF AbgrToColorref(UINT32 abgr)
{
    BYTE r=(abgr>>0)&0xFF, g=(abgr>>8)&0xFF, b=(abgr>>16)&0xFF;
    return RGB(r,g,b);
}
static UINT32 ColorrefToAbgr(COLORREF cr, BYTE alpha=255)
{
    BYTE r=GetRValue(cr),g=GetGValue(cr),b=GetBValue(cr);
    return (alpha<<24)|(b<<16)|(g<<8)|r;
}
static void DrawColorSwatch(Graphics& gfx, int x,int y,int w,int h, UINT32 abgr)
{
    BYTE r=(abgr>>0)&0xFF, g=(abgr>>8)&0xFF, b=(abgr>>16)&0xFF;
    SolidBrush br(Color(255,r,g,b));
    gfx.FillRectangle(&br,x,y,w,h);
    Pen p(Color(255,60,60,60),1.f);
    gfx.DrawRectangle(&p,x,y,w-1,h-1);
}

static bool PickColor(HWND parent, UINT32& abgr)
{
    static COLORREF custom[16]={};
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner=parent;
    cc.rgbResult=AbgrToColorref(abgr);
    cc.lpCustColors=custom;
    cc.Flags=CC_FULLOPEN|CC_RGBINIT;
    if (!::ChooseColorW(&cc)) return false;
    abgr=ColorrefToAbgr(cc.rgbResult);
    return true;
}

// ----------------------------------------------------------------
// GDI+ draw helpers
// ----------------------------------------------------------------
static Color C_BG   (255, 14, 15, 20);
static Color C_PANEL(255, 20, 22, 28);
static Color C_BRD  (255, 40, 44, 55);
static Color C_ACC  (255,224,128,  0);
static Color C_TXT  (255,210,210,215);
static Color C_MUTE (255, 95,100,115);
static Color C_GREEN(255, 60,190, 90);
static Color C_RED  (255,210, 55, 55);

static void Label(Graphics& g, const wchar_t* text,
    int x, int y, FontFamily& ff, float sz=9.f, Color col=Color(255,95,100,115))
{
    Font f(&ff,sz,FontStyleRegular,UnitPoint);
    SolidBrush br(col);
    g.DrawString(text,-1,&f,PointF((float)x,(float)y),&br);
}

static void DrawSliderRow(Graphics& g, FontFamily& ff,
    int x, int y, int w, const wchar_t* name, float val, float mn, float mx)
{
    Label(g,name,x,y,ff,9.f,C_MUTE);
    // Track
    int tx=x+90,ty=y+4,tw=w-90-50;
    SolidBrush trBg(Color(255,30,33,42));
    g.FillRectangle(&trBg,tx,ty,tw,5);
    float pct=(val-mn)/(mx-mn);
    SolidBrush trFill(C_ACC);
    g.FillRectangle(&trFill,tx,ty,(int)(tw*pct),5);
    // Thumb
    SolidBrush thumb(Color(255,220,220,225));
    g.FillEllipse(&thumb,tx+(int)(tw*pct)-5,ty-3,10,10);
    // Value
    wchar_t vbuf[16]; swprintf_s(vbuf,L"%.0f",val);
    Label(g,vbuf,x+w-45,y,ff,9.f,C_TXT);
}

// ----------------------------------------------------------------
// Paint
// ----------------------------------------------------------------
static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc=::BeginPaint(hwnd,&ps);
    HDC mem=::CreateCompatibleDC(hdc);
    HBITMAP bmp=::CreateCompatibleBitmap(hdc,WW,WH);
    ::SelectObject(mem,bmp);

    Graphics g(mem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Background
    SolidBrush brBg(C_BG);
    g.FillRectangle(&brBg,0,0,WW,WH);

    // Top accent
    SolidBrush brAcc(C_ACC);
    g.FillRectangle(&brAcc,0,0,WW,3);

    // Header
    SolidBrush brPanel(C_PANEL);
    g.FillRectangle(&brPanel,0,0,WW,56);
    Pen brdrBot(Color(50,224,128,0),1.f);
    g.DrawLine(&brdrBot,0,56,WW,56);

    FontFamily ff(L"Consolas");
    Font fTitle(&ff,15.f,FontStyleBold,UnitPoint);
    SolidBrush brAccB(C_ACC);
    g.DrawString(L"RTSS Clone  Overlay Editor",-1,&fTitle,PointF(18.f,12.f),&brAccB);

    // Connection status
    bool conn=OpenSharedMem();
    SolidBrush stDot(conn?C_GREEN:C_RED);
    g.FillEllipse(&stDot,WW-120,20,8,8);
    Font fSt(&ff,8.f,FontStyleRegular,UnitPoint);
    SolidBrush stTxt(conn?C_GREEN:C_RED);
    g.DrawString(conn?L"Game connected":L"Waiting for game",
        -1,&fSt,PointF(WW-108.f,18.f),&stTxt);

    if (conn && g_pStats) {
        // Show live FPS
        wchar_t fpsBuf[32];
        swprintf_s(fpsBuf,L"%.0f FPS  [%hs]",
            g_pStats->fps, g_pStats->apiName[0]?g_pStats->apiName:"...");
        SolidBrush brFps(Color(255,180,180,100));
        g.DrawString(fpsBuf,-1,&fSt,PointF(WW-108.f,32.f),&brFps);
    }

    int y=66, x=18, W=WW-36;

    // ---- Section: Position ----
    Label(g,L"POSITION",x,y,ff,7.5f,C_MUTE); y+=16;
    // Position buttons are Win32 controls — just draw section bg
    SolidBrush secBg(C_PANEL);
    Pen secBrd(C_BRD,1.f);
    g.FillRectangle(&secBg,x,y,W,32);
    g.DrawRectangle(&secBrd,x,y,W,32);
    y+=40;

    // ---- Section: Font ----
    Label(g,L"FONT",x,y,ff,7.5f,C_MUTE); y+=14;
    DrawSliderRow(g,ff,x,y,W,L"size",g_cfg.fontSize,10,22); y+=18;
    DrawSliderRow(g,ff,x,y,W,L"line height",g_cfg.lineHeight,10,28); y+=18;
    // Bold toggle drawn as Win32 checkbox
    y+=22;

    // ---- Section: Colors ----
    Label(g,L"COLORS",x,y,ff,7.5f,C_MUTE); y+=14;
    struct { const wchar_t* n; UINT32 c; } cols[]={
        {L"Label",g_cfg.colLabel},{L"Value",g_cfg.colValue},
        {L"Unit",g_cfg.colUnit},{L"FPS hi",g_cfg.colFpsHi},
        {L"FPS mid",g_cfg.colFpsMid},{L"FPS lo",g_cfg.colFpsLo}
    };
    for(int i=0;i<6;i++){
        int cx=x+(i%3)*(W/3), cy=y+(i/3)*26;
        Label(g,cols[i].n,cx,cy,ff,8.f,C_MUTE);
        DrawColorSwatch(g,cx,cy+12,W/3-8,12,cols[i].c);
    }
    y+=54;

    DrawSliderRow(g,ff,x,y,W,L"bg opacity",g_cfg.bgAlpha*100,0,100); y+=18;

    // ---- Section: Layout ----
    Label(g,L"LAYOUT",x,y,ff,7.5f,C_MUTE); y+=14;
    DrawSliderRow(g,ff,x,y,W,L"label width",g_cfg.labelWidth,20,80); y+=18;
    DrawSliderRow(g,ff,x,y,W,L"padding",g_cfg.padding,0,20); y+=26;

    // ---- Section: Show rows ----
    Label(g,L"SHOW ROWS",x,y,ff,7.5f,C_MUTE); y+=16;
    // checkboxes are Win32 controls
    y+=28;

    // ---- Live stats preview bar ----
    if(conn && g_pStats){
        SolidBrush prevBg(C_PANEL);
        g.FillRectangle(&prevBg,x,y,W,36);
        Pen prevBrd(C_BRD,1.f);
        g.DrawRectangle(&prevBrd,x,y,W,36);
        Label(g,L"LIVE STATS",x+8,y+4,ff,7.5f,C_MUTE);
        wchar_t sb[128];
        swprintf_s(sb,L"CPU %.0f%%   RAM %.0f MB   GPU %.0f%%   FPS %.0f",
            g_pStats->cpuUsagePercent, g_pStats->ramUsedMB,
            g_pStats->gpuUsagePercent, g_pStats->fps);
        Label(g,sb,x+8,y+18,ff,8.5f,C_TXT);
        y+=44;
    }

    // Bottom accent
    SolidBrush brBot(C_ACC);
    g.FillRectangle(&brBot,0,WH-2,WW,2);

    ::BitBlt(hdc,0,0,WW,WH,mem,0,0,SRCCOPY);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::EndPaint(hwnd,&ps);
}

// ----------------------------------------------------------------
// Create all Win32 child controls
// ----------------------------------------------------------------
static HWND MakeBtn(HWND parent,HINSTANCE hi,
    const wchar_t* text,int x,int y,int w,int h,int id,DWORD style=WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON)
{
    return ::CreateWindowW(L"BUTTON",text,style,x,y,w,h,parent,
        (HMENU)(INT_PTR)id,hi,nullptr);
}
static HWND MakeCheck(HWND parent,HINSTANCE hi,
    const wchar_t* text,int x,int y,int w,int h,int id,bool checked)
{
    HWND hw=::CreateWindowW(L"BUTTON",text,
        WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        x,y,w,h,parent,(HMENU)(INT_PTR)id,hi,nullptr);
    ::SendMessageW(hw,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);
    return hw;
}
static HWND MakeSlider(HWND parent,HINSTANCE hi,
    int x,int y,int w,int id,int mn,int mx,int val)
{
    HWND hw=::CreateWindowW(TRACKBAR_CLASSW,L"",
        WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,
        x,y,w,16,parent,(HMENU)(INT_PTR)id,hi,nullptr);
    ::SendMessageW(hw,TBM_SETRANGE,TRUE,MAKELPARAM(mn,mx));
    ::SendMessageW(hw,TBM_SETPOS,TRUE,val);
    return hw;
}

static void CreateControls(HWND hwnd, HINSTANCE hi)
{
    // Position buttons
    const wchar_t* posLabels[]=
        {L"TL",L"TC",L"TR",L"BL",L"BC",L"BR"};
    for(int i=0;i<6;i++){
        int bx=18+(i*63), by=84;
        MakeBtn(hwnd,hi,posLabels[i],bx,by,58,26,ID_POS_TL+i,
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON);
    }

    // Font sliders
    MakeSlider(hwnd,hi,108,148,200,ID_SL_FSIZE,10,22,(int)g_cfg.fontSize);
    MakeSlider(hwnd,hi,108,166,200,ID_SL_LINEH,10,28,(int)g_cfg.lineHeight);

    // Bold
    MakeCheck(hwnd,hi,L"Bold",18,190,80,20,ID_BOLD,g_cfg.fontBold!=0);

    // Color buttons (click to open color picker)
    const wchar_t* cNames[]={L"Label",L"Value",L"Unit",L"FPS hi",L"FPS mid",L"FPS lo"};
    for(int i=0;i<6;i++){
        int cx=18+(i%3)*128, cy=234+(i/3)*26;
        MakeBtn(hwnd,hi,cNames[i],cx,cy+12,118,13,ID_COL_LABEL+i,
            WS_CHILD|WS_VISIBLE|BS_OWNERDRAW);
    }

    // BG opacity
    MakeSlider(hwnd,hi,108,294,200,ID_SL_BGALPHA,0,100,(int)(g_cfg.bgAlpha*100));

    // Layout sliders
    MakeSlider(hwnd,hi,108,328,200,ID_SL_LBLW,20,80,(int)g_cfg.labelWidth);
    MakeSlider(hwnd,hi,108,346,200,ID_SL_PAD,0,20,(int)g_cfg.padding);

    // Show checkboxes
    struct{const wchar_t*n;int id;bool v;}cks[]={
        {L"GPU",ID_SHOW_GPU,!!g_cfg.showGpu},
        {L"CPU",ID_SHOW_CPU,!!g_cfg.showCpu},
        {L"RAM",ID_SHOW_RAM,!!g_cfg.showRam},
        {L"FPS",ID_SHOW_FPS,!!g_cfg.showFps},
        {L"Frametime",ID_SHOW_FT,!!g_cfg.showFrametime},
        {L"Graph",ID_SHOW_GR,!!g_cfg.showGraph},
    };
    for(int i=0;i<6;i++)
        MakeCheck(hwnd,hi,cks[i].n,18+(i%3)*128,392+(i/3)*18,118,18,
            cks[i].id,cks[i].v);

    // Style all controls dark
    // (done via WM_CTLCOLOR* in WndProc)
}

// ----------------------------------------------------------------
// Sync controls → config → shared memory
// ----------------------------------------------------------------
static void SyncFromControls(HWND hwnd)
{
    g_cfg.fontSize   =(float)::SendDlgItemMessageW(hwnd,ID_SL_FSIZE,TBM_GETPOS,0,0);
    g_cfg.lineHeight =(float)::SendDlgItemMessageW(hwnd,ID_SL_LINEH,TBM_GETPOS,0,0);
    g_cfg.bgAlpha    =(float)::SendDlgItemMessageW(hwnd,ID_SL_BGALPHA,TBM_GETPOS,0,0)/100.f;
    g_cfg.labelWidth =(float)::SendDlgItemMessageW(hwnd,ID_SL_LBLW,TBM_GETPOS,0,0);
    g_cfg.padding    =(float)::SendDlgItemMessageW(hwnd,ID_SL_PAD,TBM_GETPOS,0,0);
    g_cfg.fontBold   =::SendDlgItemMessageW(hwnd,ID_BOLD,BM_GETCHECK,0,0)==BST_CHECKED?1:0;
    g_cfg.showGpu    =::SendDlgItemMessageW(hwnd,ID_SHOW_GPU,BM_GETCHECK,0,0)==BST_CHECKED?1:0;
    g_cfg.showCpu    =::SendDlgItemMessageW(hwnd,ID_SHOW_CPU,BM_GETCHECK,0,0)==BST_CHECKED?1:0;
    g_cfg.showRam    =::SendDlgItemMessageW(hwnd,ID_SHOW_RAM,BM_GETCHECK,0,0)==BST_CHECKED?1:0;
    g_cfg.showFps    =::SendDlgItemMessageW(hwnd,ID_SHOW_FPS,BM_GETCHECK,0,0)==BST_CHECKED?1:0;
    g_cfg.showFrametime=::SendDlgItemMessageW(hwnd,ID_SHOW_FT,BM_GETCHECK,0,0)==BST_CHECKED?1:0;
    g_cfg.showGraph  =::SendDlgItemMessageW(hwnd,ID_SHOW_GR,BM_GETCHECK,0,0)==BST_CHECKED?1:0;
    PushConfig();
    ::InvalidateRect(hwnd,nullptr,FALSE);
}

// Save/load config to file
static void SaveConfig()
{
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr,path,MAX_PATH);
    std::wstring p(path);
    auto sl=p.find_last_of(L"\\/");
    p=p.substr(0,sl+1)+L"rtss_overlay.cfg";
    FILE* f=nullptr;
    _wfopen_s(&f,p.c_str(),L"wb");
    if(f){fwrite(&g_cfg,sizeof(g_cfg),1,f);fclose(f);}
}
static void LoadConfig(HWND hwnd)
{
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr,path,MAX_PATH);
    std::wstring p(path);
    auto sl=p.find_last_of(L"\\/");
    p=p.substr(0,sl+1)+L"rtss_overlay.cfg";
    FILE* f=nullptr;
    _wfopen_s(&f,p.c_str(),L"rb");
    if(!f) return;
    OverlayConfig tmp{};
    if(fread(&tmp,sizeof(tmp),1,f)==1 && tmp.magic==CFG_MAGIC)
        g_cfg=tmp;
    fclose(f);
    // Update slider positions
    ::SendDlgItemMessageW(hwnd,ID_SL_FSIZE,TBM_SETPOS,TRUE,(int)g_cfg.fontSize);
    ::SendDlgItemMessageW(hwnd,ID_SL_LINEH,TBM_SETPOS,TRUE,(int)g_cfg.lineHeight);
    ::SendDlgItemMessageW(hwnd,ID_SL_BGALPHA,TBM_SETPOS,TRUE,(int)(g_cfg.bgAlpha*100));
    ::SendDlgItemMessageW(hwnd,ID_SL_LBLW,TBM_SETPOS,TRUE,(int)g_cfg.labelWidth);
    ::SendDlgItemMessageW(hwnd,ID_SL_PAD,TBM_SETPOS,TRUE,(int)g_cfg.padding);
    PushConfig();
    ::InvalidateRect(hwnd,nullptr,FALSE);
}

// ----------------------------------------------------------------
// Dark theme brush
// ----------------------------------------------------------------
static HBRUSH g_brDark  = nullptr;
static HBRUSH g_brPanel = nullptr;

// ----------------------------------------------------------------
// WndProc
// ----------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg)
    {
    case WM_CREATE:
        g_brDark  = ::CreateSolidBrush(RGB(14,15,20));
        g_brPanel = ::CreateSolidBrush(RGB(20,22,28));
        CreateControls(hwnd,(HINSTANCE)::GetWindowLongPtrW(hwnd,GWLP_HINSTANCE));
        ::SetTimer(hwnd,ID_TIMER,200,nullptr);
        return 0;

    case WM_TIMER:
        OpenSharedMem();
        ::InvalidateRect(hwnd,nullptr,FALSE);
        return 0;

    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    // Dark theme for child controls
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc=(HDC)wp;
        ::SetTextColor(hdc,RGB(160,162,170));
        ::SetBkColor(hdc,RGB(20,22,28));
        return (LRESULT)g_brPanel;
    }
    case WM_CTLCOLORDLG:
        return (LRESULT)g_brDark;

    // Drag
    case WM_LBUTTONDOWN:
        if(HIWORD(lp)<58){g_dragging=true;g_dragOff={LOWORD(lp),HIWORD(lp)};::SetCapture(hwnd);}
        return 0;
    case WM_MOUSEMOVE:
        if(g_dragging){RECT r;::GetWindowRect(hwnd,&r);
            ::SetWindowPos(hwnd,nullptr,
                r.left+(short)LOWORD(lp)-g_dragOff.x,
                r.top+(short)HIWORD(lp)-g_dragOff.y,
                0,0,SWP_NOSIZE|SWP_NOZORDER);}
        return 0;
    case WM_LBUTTONUP:
        g_dragging=false;::ReleaseCapture();
        return 0;

    // Sliders
    case WM_HSCROLL:
        SyncFromControls(hwnd);
        return 0;

    // Buttons / checkboxes
    case WM_COMMAND: {
        int id=LOWORD(wp);

        // Position buttons
        if(id>=ID_POS_TL && id<=ID_POS_BR){
            g_cfg.position=id-ID_POS_TL;
            PushConfig(); ::InvalidateRect(hwnd,nullptr,FALSE);
            return 0;
        }

        // Color pickers
        if(id>=ID_COL_LABEL && id<=ID_COL_FPSLO){
            UINT32* cols[]={&g_cfg.colLabel,&g_cfg.colValue,&g_cfg.colUnit,
                &g_cfg.colFpsHi,&g_cfg.colFpsMid,&g_cfg.colFpsLo};
            int ci=id-ID_COL_LABEL;
            if(PickColor(hwnd,*cols[ci])){PushConfig();::InvalidateRect(hwnd,nullptr,FALSE);}
            return 0;
        }

        // Checkboxes
        if(id==ID_BOLD||id==ID_SHOW_GPU||id==ID_SHOW_CPU||id==ID_SHOW_RAM||
           id==ID_SHOW_FPS||id==ID_SHOW_FT||id==ID_SHOW_GR){
            SyncFromControls(hwnd); return 0;
        }

        return 0;
    }

    case WM_KEYDOWN:
        if(wp=='S'&&(::GetKeyState(VK_CONTROL)&0x8000)) SaveConfig();
        if(wp=='O'&&(::GetKeyState(VK_CONTROL)&0x8000)) LoadConfig(hwnd);
        return 0;

    case WM_DESTROY:
        ::KillTimer(hwnd,ID_TIMER);
        SaveConfig();
        CloseSharedMem();
        if(g_brDark)  ::DeleteObject(g_brDark);
        if(g_brPanel) ::DeleteObject(g_brPanel);
        GdiplusShutdown(g_gdipToken);
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd,msg,wp,lp);
}

// ----------------------------------------------------------------
// WinMain
// ----------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int)
{
    INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_BAR_CLASSES};
    ::InitCommonControlsEx(&icc);

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdipToken,&gsi,nullptr);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=WndProc;
    wc.hInstance=hInst;
    wc.lpszClassName=L"RTSSEditor";
    wc.hCursor=::LoadCursorW(nullptr,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)::GetStockObject(BLACK_BRUSH);
    ::RegisterClassExW(&wc);

    g_hwnd=::CreateWindowExW(
        WS_EX_APPWINDOW|WS_EX_LAYERED,
        wc.lpszClassName,L"RTSS Clone — Overlay Editor",
        WS_POPUP|WS_VISIBLE,
        100,100,WW,WH,
        nullptr,nullptr,hInst,nullptr);

    ::SetLayeredWindowAttributes(g_hwnd,0,242,LWA_ALPHA);
    ::ShowWindow(g_hwnd,SW_SHOW);
    ::UpdateWindow(g_hwnd);

    MSG msg{};
    while(::GetMessageW(&msg,nullptr,0,0)){
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}
