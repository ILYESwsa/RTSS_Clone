// ============================================================
// RTSS Clone — All-in-One Launcher
// Single EXE that contains HookDLL.dll embedded as a resource.
//
// Features:
//   - Extracts HookDLL.dll to %TEMP% on startup
//   - Full GUI: process list + inject + overlay editor tabs
//   - Dark theme, amber accents
//   - No separate files needed — just this EXE
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
#include <string>
#include <algorithm>

#include "resource.h"
#include "../Shared/SharedMemory.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

using namespace Gdiplus;

// ----------------------------------------------------------------
// OverlayConfig (mirrors overlay_editor.cpp / hooks.cpp)
// ----------------------------------------------------------------
#define CFG_MAGIC  0xCF6E0001u
#define CFG_OFFSET 4096

struct OverlayConfig {
    UINT32 magic, position;
    float  fontSize, lineHeight;
    UINT32 fontBold, fontIndex;
    UINT32 colLabel, colValue, colUnit, colFpsHi, colFpsMid, colFpsLo;
    float  bgAlpha;
    UINT32 showGpu, showCpu, showRam, showFps, showFrametime, showGraph;
    float  labelWidth, padding;
};

static OverlayConfig g_oc = {
    CFG_MAGIC, 0,
    13.f, 18.f, 0, 0,
    0xFF2080E0, 0xFFFFFFFF, 0xFF888888,
    0xFFFFFFFF, 0xFF20A0E0, 0xFF3030E0,
    0.f, 1,1,1,1,1,1, 42.f, 4.f
};

// ----------------------------------------------------------------
// Window layout
// ----------------------------------------------------------------
#define WW 520
#define WH 660

// Tabs
#define TAB_INJECT  0
#define TAB_EDITOR  1
static int g_tab = TAB_INJECT;

// Colors
static Color C_BG   (255, 12, 13, 17);
static Color C_PANEL(255, 18, 20, 26);
static Color C_BRD  (255, 35, 38, 50);
static Color C_ACC  (255,230,130,  0);
static Color C_TXT  (255,215,215,220);
static Color C_MUTE (255, 95,100,115);
static Color C_GREEN(255, 55,185, 85);
static Color C_RED  (255,205, 55, 55);
static Color C_AMBER(255,230,130,  0);

// ----------------------------------------------------------------
// Process list
// ----------------------------------------------------------------
struct ProcEntry {
    DWORD pid;
    std::wstring name, path;
};
static std::vector<ProcEntry> g_procs;
static int  g_selected   = -1;
static int  g_scrollTop  = 0;
static const int ROWS    = 10;
static const int ROW_H   = 32;
static bool g_injected   = false;
static wchar_t g_status[256] = L"Select a process then click INJECT";
static bool g_statusOk   = true;
static DWORD g_flashTick = 0;

// ----------------------------------------------------------------
// Shared memory
// ----------------------------------------------------------------
static SharedMemHandle g_shm;
static OverlayConfig*  g_pCfg = nullptr;

static void TryConnectShm()
{
    if (!g_shm.Valid()) g_shm.OpenMapping();
    if (g_shm.Valid() && !g_pCfg)
        g_pCfg = (OverlayConfig*)((BYTE*)g_shm.Data() + CFG_OFFSET);
}
static void PushConfig()
{
    if (g_pCfg) memcpy(g_pCfg, &g_oc, sizeof(OverlayConfig));
}

// ----------------------------------------------------------------
// Extract embedded DLL to %TEMP%\rtss_hookdll.dll
// ----------------------------------------------------------------
static std::wstring g_dllPath;

static bool ExtractDll()
{
    // Find resource
    HRSRC hRes = ::FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_HOOKDLL),
        MAKEINTRESOURCEW(256));
    if (!hRes) return false;

    HGLOBAL hGlob = ::LoadResource(nullptr, hRes);
    if (!hGlob) return false;

    DWORD sz  = ::SizeofResource(nullptr, hRes);
    void* ptr = ::LockResource(hGlob);
    if (!ptr || sz == 0) return false;

    // Write to temp
    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    g_dllPath = std::wstring(tmp) + L"rtss_hookdll.dll";

    HANDLE hf = ::CreateFileW(g_dllPath.c_str(),
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;

    DWORD written=0;
    ::WriteFile(hf, ptr, sz, &written, nullptr);
    ::CloseHandle(hf);
    return written == sz;
}

// ----------------------------------------------------------------
// Process utilities
// ----------------------------------------------------------------
static void RefreshProcs()
{
    g_procs.clear(); g_selected=-1; g_scrollTop=0;
    HANDLE snap=::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap==INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{sizeof(pe)};
    if (::Process32FirstW(snap,&pe)) do {
        if (pe.th32ProcessID<5) continue;
        ProcEntry e; e.pid=pe.th32ProcessID; e.name=pe.szExeFile;
        HANDLE hp=::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,e.pid);
        if (hp) {
            wchar_t buf[MAX_PATH]{}; DWORD n=MAX_PATH;
            if (::QueryFullProcessImageNameW(hp,0,buf,&n)) e.path=buf;
            ::CloseHandle(hp);
        }
        g_procs.push_back(e);
    } while(::Process32NextW(snap,&pe));
    ::CloseHandle(snap);
    std::sort(g_procs.begin(),g_procs.end(),
        [](const ProcEntry&a,const ProcEntry&b){
            return _wcsicmp(a.name.c_str(),b.name.c_str())<0;});
}

static bool DoInject(DWORD pid, const std::wstring& dll)
{
    HANDLE hp=::OpenProcess(PROCESS_ALL_ACCESS,FALSE,pid);
    if (!hp) return false;
    size_t nb=(dll.size()+1)*sizeof(wchar_t);
    LPVOID rm=::VirtualAllocEx(hp,nullptr,nb,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if (!rm){::CloseHandle(hp);return false;}
    SIZE_T w=0;
    ::WriteProcessMemory(hp,rm,dll.c_str(),nb,&w);
    HMODULE hk=::GetModuleHandleW(L"kernel32.dll");
    auto pfn=(LPTHREAD_START_ROUTINE)::GetProcAddress(hk,"LoadLibraryW");
    HANDLE ht=::CreateRemoteThread(hp,nullptr,0,pfn,rm,0,nullptr);
    if (!ht){::VirtualFreeEx(hp,rm,0,MEM_RELEASE);::CloseHandle(hp);return false;}
    ::WaitForSingleObject(ht,8000);
    DWORD ec=0; ::GetExitCodeThread(ht,&ec);
    ::CloseHandle(ht);
    ::VirtualFreeEx(hp,rm,0,MEM_RELEASE);
    ::CloseHandle(hp);
    return ec!=0;
}

// ----------------------------------------------------------------
// GDI+ helpers
// ----------------------------------------------------------------
static ULONG_PTR g_gdip=0;
static HWND      g_hwnd=nullptr;
static bool      g_drag=false;
static POINT     g_dragOff={};

static void RoundRect(Graphics&g,Pen*pen,SolidBrush*fill,int x,int y,int w,int h,int r){
    GraphicsPath p;
    p.AddArc(x,y,r*2,r*2,180,90); p.AddArc(x+w-r*2,y,r*2,r*2,270,90);
    p.AddArc(x+w-r*2,y+h-r*2,r*2,r*2,0,90); p.AddArc(x,y+h-r*2,r*2,r*2,90,90);
    p.CloseFigure();
    if(fill)g.FillPath(fill,&p); if(pen)g.DrawPath(pen,&p);
}

static void Lbl(Graphics&g,FontFamily&ff,const wchar_t*t,float x,float y,
    float sz,Color col,bool bold=false){
    Font f(&ff,sz,bold?FontStyleBold:FontStyleRegular,UnitPoint);
    SolidBrush b(col); g.DrawString(t,-1,&f,PointF(x,y),&b);
}

static void ColorSwatch(Graphics&g,int x,int y,int w,int h,UINT32 abgr){
    BYTE r=(abgr>>0)&0xFF,gr=(abgr>>8)&0xFF,b=(abgr>>16)&0xFF;
    SolidBrush br(Color(255,r,gr,b)); g.FillRectangle(&br,x,y,w,h);
    Pen p(Color(80,255,255,255),1.f); g.DrawRectangle(&p,x,y,w-1,h-1);
}

// ----------------------------------------------------------------
// Paint — Inject tab
// ----------------------------------------------------------------
static void PaintInject(Graphics&g, FontFamily&ff, int ox, int oy, int W)
{
    // Section label
    Lbl(g,ff,L"RUNNING PROCESSES",ox,oy,7.f,C_MUTE);
    Lbl(g,ff,L"F5 = refresh  ↑↓ = navigate",ox+W-160.f,oy,7.f,C_MUTE);
    oy+=14;

    // List panel
    int lH=ROWS*ROW_H;
    SolidBrush brP(C_PANEL); Pen brBrd(C_BRD,1.f);
    RoundRect(g,&brBrd,&brP,ox,oy,W,lH,4);

    Font fPid(&ff,8.f,FontStyleRegular,UnitPoint);
    Font fName(&ff,9.5f,FontStyleBold,UnitPoint);
    Font fPath(&ff,7.f,FontStyleRegular,UnitPoint);

    g.SetClip(Rect(ox+1,oy+1,W-2,lH-2));
    int total=(int)g_procs.size();
    for(int i=0;i<ROWS;i++){
        int idx=g_scrollTop+i; if(idx>=total)break;
        int ry=oy+i*ROW_H; bool sel=(idx==g_selected);
        if(sel){
            SolidBrush sb(Color(255,28,30,40)); g.FillRectangle(&sb,ox+1,ry,W-2,ROW_H);
            SolidBrush ab(C_ACC); g.FillRectangle(&ab,ox+1,ry,3,ROW_H);
        } else if(i%2==0){
            SolidBrush sb(Color(12,255,255,255)); g.FillRectangle(&sb,ox+1,ry,W-2,ROW_H);
        }
        wchar_t pid[16]; swprintf_s(pid,L"%u",g_procs[idx].pid);
        SolidBrush bm(C_MUTE); g.DrawString(pid,-1,&fPid,PointF(ox+8.f,ry+5.f),&bm);
        SolidBrush bn(sel?C_AMBER:C_TXT);
        g.DrawString(g_procs[idx].name.c_str(),-1,&fName,PointF(ox+58.f,ry+4.f),&bn);
        if(!g_procs[idx].path.empty()){
            std::wstring ep=g_procs[idx].path;
            if(ep.size()>52)ep=L"..."+ep.substr(ep.size()-49);
            SolidBrush bp(Color(75,160,162,175)); g.DrawString(ep.c_str(),-1,&fPath,PointF(ox+58.f,ry+18.f),&bp);
        }
        Pen rp(Color(18,255,255,255),1.f); g.DrawLine(&rp,ox+1,ry+ROW_H-1,ox+W-2,ry+ROW_H-1);
    }
    g.ResetClip();

    // Scrollbar
    if(total>ROWS){
        int sx=ox+W-6,sy=oy+1,sh=lH-2;
        SolidBrush bsb(Color(25,255,255,255)); g.FillRectangle(&bsb,sx,sy,4,sh);
        float th=(float)sh*ROWS/total,ty=sy+(float)sh*g_scrollTop/total;
        SolidBrush bt(C_MUTE); g.FillRectangle(&bt,sx,(int)ty,4,(int)th);
    }

    oy+=lH+4;
    wchar_t cnt[32]; swprintf_s(cnt,L"%d processes",total);
    Lbl(g,ff,cnt,(float)ox,(float)oy,7.5f,C_MUTE);
    oy+=18;

    // Info box
    SolidBrush bInfo(C_PANEL); Pen pInfo(C_BRD,1.f);
    RoundRect(g,&pInfo,&bInfo,ox,oy,W,44,4);
    if(g_selected>=0&&g_selected<total){
        Lbl(g,ff,g_procs[g_selected].name.c_str(),(float)ox+10,(float)oy+6,10.f,C_AMBER,true);
        wchar_t pb[32]; swprintf_s(pb,L"PID %u",g_procs[g_selected].pid);
        Lbl(g,ff,pb,(float)ox+10,(float)oy+26,8.f,C_MUTE);
    } else {
        Lbl(g,ff,L"No process selected",(float)ox+10,(float)oy+16,8.f,C_MUTE);
    }
    oy+=52;

    // Inject button
    bool canInj=(g_selected>=0&&!g_dllPath.empty());
    SolidBrush bBtn(canInj?(g_injected?Color(255,40,120,50):C_ACC):Color(255,35,37,48));
    Pen pBtn(canInj?Color(255,255,200,100):C_BRD,1.5f);
    RoundRect(g,&pBtn,&bBtn,ox,oy,W,38,5);
    Font fBtn(&ff,12.f,FontStyleBold,UnitPoint);
    SolidBrush bBtnT(canInj?Color(255,20,20,20):C_MUTE);
    StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
    const wchar_t* bl=g_injected?L"✓  INJECTED — Open Editor tab":L"INJECT";
    g.DrawString(bl,-1,&fBtn,RectF((float)ox,(float)oy,(float)W,38.f),&sf,&bBtnT);
    oy+=46;

    // Status
    DWORD now=::GetTickCount();
    float fl=1.f; if(now-g_flashTick<700)fl=0.5f+0.5f*sinf((now-g_flashTick)*0.012f);
    Color sc(g_statusOk?Color((BYTE)(fl*200),55,190,85):Color((BYTE)(fl*220),205,55,55));
    Lbl(g,ff,g_status,(float)ox,(float)oy,8.f,sc);
}

// ----------------------------------------------------------------
// Paint — Editor tab
// ----------------------------------------------------------------
static void PaintEditor(Graphics&g,FontFamily&ff,int ox,int oy,int W)
{
    bool conn=g_shm.Valid();
    SolidBrush cdot(conn?C_GREEN:C_RED);
    g.FillEllipse(&cdot,ox,oy+4,8,8);
    Lbl(g,ff,conn?L"Live — changes apply instantly":L"Not connected — inject first",
        (float)ox+14,(float)oy,8.f,conn?C_GREEN:C_RED);
    oy+=20;

    // Live stats bar
    if(conn&&g_shm.Data()){
        RtssStats*s=g_shm.Data();
        SolidBrush sb(C_PANEL); Pen pb(C_BRD,1.f);
        RoundRect(g,&pb,&sb,ox,oy,W,28,3);
        wchar_t ls[128]; swprintf_s(ls,
            L"  FPS %.0f   CPU %.0f%%   RAM %.0f MB   GPU %.0f%%   [%hs]",
            s->fps,s->cpuUsagePercent,s->ramUsedMB,s->gpuUsagePercent,
            s->apiName[0]?s->apiName:"...");
        Lbl(g,ff,ls,(float)ox+6,(float)oy+8,8.5f,C_TXT);
        oy+=34;
    }

    // Position
    Lbl(g,ff,L"POSITION",(float)ox,(float)oy,7.f,C_MUTE); oy+=14;
    const wchar_t*posL[]={L"Top Left",L"Top Center",L"Top Right",
                          L"Bot Left",L"Bot Center",L"Bot Right"};
    for(int i=0;i<6;i++){
        int bx=ox+(i%3)*(W/3),by=oy+(i/3)*24;
        bool sel=((int)g_oc.position==i);
        SolidBrush bb(sel?C_ACC:C_PANEL);
        Pen bp(sel?Color(255,255,200,80):C_BRD,1.f);
        RoundRect(g,&bp,&bb,bx,by,W/3-4,20,3);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        Font fb(&ff,7.5f,FontStyleRegular,UnitPoint);
        SolidBrush bt(sel?Color(255,20,20,20):C_MUTE);
        g.DrawString(posL[i],-1,&fb,RectF((float)bx,(float)by,(float)(W/3-4),20.f),&sf,&bt);
    }
    oy+=54;

    // Sliders helper lambda
    auto Slider=[&](const wchar_t*name,float val,float mn,float mx,int id)->float{
        Lbl(g,ff,name,(float)ox,(float)oy,8.f,C_MUTE);
        int tx=ox+100,tw=W-140;
        SolidBrush tb(Color(255,28,30,40)); g.FillRectangle(&tb,tx,oy+5,tw,5);
        float pct=(val-mn)/(mx-mn);
        SolidBrush tf(C_ACC); g.FillRectangle(&tf,tx,oy+5,(int)(tw*pct),5);
        SolidBrush tt(Color(255,210,210,220)); g.FillEllipse(&tt,tx+(int)(tw*pct)-5,oy+1,10,11);
        wchar_t vb[16]; swprintf_s(vb,L"%.0f",val);
        Lbl(g,ff,vb,(float)(ox+W-36),(float)oy,8.f,C_TXT);
        oy+=18; return pct;
    };

    // Font
    Lbl(g,ff,L"FONT",(float)ox,(float)oy,7.f,C_MUTE); oy+=12;
    Slider(L"size",g_oc.fontSize,10,22,0);
    Slider(L"line height",g_oc.lineHeight,10,28,0);

    // Show flags
    Lbl(g,ff,L"SHOW ROWS",(float)ox,(float)oy,7.f,C_MUTE); oy+=12;
    struct{const wchar_t*n;UINT32*v;}rows[]={
        {L"GPU",&g_oc.showGpu},{L"CPU",&g_oc.showCpu},{L"RAM",&g_oc.showRam},
        {L"FPS",&g_oc.showFps},{L"Frametime",&g_oc.showFrametime},{L"Graph",&g_oc.showGraph}
    };
    for(int i=0;i<6;i++){
        int bx=ox+(i%3)*(W/3),by=oy+(i/3)*22;
        bool on=(*rows[i].v!=0);
        SolidBrush bb(on?Color(255,30,80,40):C_PANEL);
        Pen bp(on?C_GREEN:C_BRD,1.f);
        RoundRect(g,&bp,&bb,bx,by,W/3-4,18,3);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        Font fb(&ff,7.5f,FontStyleRegular,UnitPoint);
        SolidBrush bt(on?C_GREEN:C_MUTE);
        g.DrawString(rows[i].n,-1,&fb,RectF((float)bx,(float)by,(float)(W/3-4),18.f),&sf,&bt);
    }
    oy+=50;

    // Colors
    Lbl(g,ff,L"COLORS",(float)ox,(float)oy,7.f,C_MUTE); oy+=12;
    struct{const wchar_t*n;UINT32 c;}cols[]={
        {L"Label",g_oc.colLabel},{L"Value",g_oc.colValue},{L"Unit",g_oc.colUnit},
        {L"FPS hi",g_oc.colFpsHi},{L"FPS mid",g_oc.colFpsMid},{L"FPS lo",g_oc.colFpsLo}
    };
    for(int i=0;i<6;i++){
        int cx=ox+(i%3)*(W/3),cy=oy+(i/3)*30;
        Lbl(g,ff,cols[i].n,(float)cx,(float)cy,7.5f,C_MUTE);
        ColorSwatch(g,cx,cy+12,W/3-8,14,cols[i].c);
    }
    oy+=66;

    // BG opacity
    Lbl(g,ff,L"COLORS",(float)ox,(float)oy,7.f,C_MUTE); // reuse area
    Slider(L"bg opacity",g_oc.bgAlpha*100,0,100,0);
    Slider(L"label width",g_oc.labelWidth,20,80,0);
    Slider(L"padding",g_oc.padding,0,20,0);

    Lbl(g,ff,L"Click a color swatch to change it  •  Ctrl+S save  •  Ctrl+O load",
        (float)ox,(float)oy,7.f,C_MUTE);
}

// ----------------------------------------------------------------
// Full Paint
// ----------------------------------------------------------------
static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps; HDC hdc=::BeginPaint(hwnd,&ps);
    HDC mem=::CreateCompatibleDC(hdc);
    HBITMAP bmp=::CreateCompatibleBitmap(hdc,WW,WH);
    ::SelectObject(mem,bmp);

    Graphics g(mem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    SolidBrush brBg(C_BG); g.FillRectangle(&brBg,0,0,WW,WH);
    // Scanlines
    for(int y=0;y<WH;y+=4){Pen sp(Color(5,255,255,255),1.f);g.DrawLine(&sp,0,y,WW,y);}
    // Top bar
    SolidBrush brAcc(C_ACC); g.FillRectangle(&brAcc,0,0,WW,3);

    FontFamily ff(L"Consolas");

    // Header
    SolidBrush brH(C_PANEL); g.FillRectangle(&brH,0,0,WW,52);
    Pen hBrd(Color(45,230,130,0),1.f); g.DrawLine(&hBrd,0,52,WW,52);
    Lbl(g,ff,L"RTSS Clone",18,12,16.f,C_ACC,true);
    Lbl(g,ff,L"All-in-One  v1.0",18,36,7.5f,C_MUTE);

    // DLL status
    bool dllOk=!g_dllPath.empty();
    SolidBrush dd(dllOk?C_GREEN:C_RED);
    g.FillEllipse(&dd,WW-130,18,8,8);
    Lbl(g,ff,dllOk?L"HookDLL ready":L"DLL extract failed",(float)(WW-118),16,7.5f,dllOk?C_GREEN:C_RED);

    // Connected
    TryConnectShm();
    bool conn=g_shm.Valid();
    SolidBrush dc(conn?C_GREEN:C_MUTE);
    g.FillEllipse(&dc,WW-130,32,8,8);
    Lbl(g,ff,conn?L"Game connected":L"Not injected",(float)(WW-118),30,7.5f,conn?C_GREEN:C_MUTE);

    // Tabs
    const wchar_t* tabs[]={L"INJECT",L"EDITOR"};
    for(int i=0;i<2;i++){
        int tx=18+i*120,ty=58;
        bool sel=(g_tab==i);
        SolidBrush tb(sel?C_ACC:C_PANEL);
        Pen tp(sel?Color(255,255,200,100):C_BRD,1.f);
        RoundRect(g,&tp,&tb,tx,ty,112,24,4);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        Font tf(&ff,9.f,FontStyleBold,UnitPoint);
        SolidBrush tt(sel?Color(255,20,20,20):C_MUTE);
        g.DrawString(tabs[i],-1,&tf,RectF((float)tx,(float)ty,112.f,24.f),&sf,&tt);
    }

    // Tab content
    int contentX=14, contentY=90, contentW=WW-28;
    if(g_tab==TAB_INJECT)
        PaintInject(g,ff,contentX,contentY,contentW);
    else
        PaintEditor(g,ff,contentX,contentY,contentW);

    // Bottom bar
    g.FillRectangle(&brAcc,0,WH-2,WW,2);

    ::BitBlt(hdc,0,0,WW,WH,mem,0,0,SRCCOPY);
    ::DeleteObject(bmp); ::DeleteDC(mem);
    ::EndPaint(hwnd,&ps);
}

// ----------------------------------------------------------------
// Hit testing
// ----------------------------------------------------------------
static int HitRow(int mx,int my){
    int lx=14,ly=104,lw=WW-28,lh=ROWS*ROW_H;
    if(mx<lx||mx>lx+lw||my<ly||my>ly+lh) return -1;
    int idx=g_scrollTop+(my-ly)/ROW_H;
    return (idx>=0&&idx<(int)g_procs.size())?idx:-1;
}
static bool HitInject(int mx,int my){
    // Approx inject button position
    int by=104+ROWS*ROW_H+4+18+44+8;
    return mx>=14&&mx<=WW-14&&my>=by&&my<=by+38;
}
static int HitTab(int mx,int my){
    if(my<58||my>82) return -1;
    if(mx>=18&&mx<=130) return 0;
    if(mx>=138&&mx<=250) return 1;
    return -1;
}
static int HitPos(int mx,int my){
    // Position buttons in editor tab
    int oy=90+20+34+20;
    int W=WW-28, ox=14;
    for(int i=0;i<6;i++){
        int bx=ox+(i%3)*(W/3), by=oy+(i/3)*24;
        if(mx>=bx&&mx<=bx+W/3-4&&my>=by&&my<=by+20) return i;
    }
    return -1;
}
static int HitShowRow(int mx,int my){
    int oy=90+20+34+20+54+12+36; // approx
    int W=WW-28,ox=14;
    for(int i=0;i<6;i++){
        int bx=ox+(i%3)*(W/3), by=oy+(i/3)*22;
        if(mx>=bx&&mx<=bx+W/3-4&&my>=by&&my<=by+18) return i;
    }
    return -1;
}
static int HitColor(int mx,int my){
    int oy=90+20+34+20+54+12+36+50+12; // approx
    int W=WW-28,ox=14;
    for(int i=0;i<6;i++){
        int cx=ox+(i%3)*(W/3), cy=oy+(i/3)*30+12;
        if(mx>=cx&&mx<=cx+W/3-8&&my>=cy&&my<=cy+14) return i;
    }
    return -1;
}

static bool PickCol(UINT32& abgr){
    static COLORREF cust[16]={};
    BYTE r=(abgr>>0)&0xFF,gr=(abgr>>8)&0xFF,b=(abgr>>16)&0xFF;
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner=g_hwnd; cc.rgbResult=RGB(r,gr,b);
    cc.lpCustColors=cust; cc.Flags=CC_FULLOPEN|CC_RGBINIT;
    if(!::ChooseColorW(&cc))return false;
    r=GetRValue(cc.rgbResult);gr=GetGValue(cc.rgbResult);b=GetBValue(cc.rgbResult);
    abgr=(255u<<24)|(b<<16)|(gr<<8)|r;
    return true;
}

static void SaveCfg(){
    wchar_t p[MAX_PATH]{}; ::GetModuleFileNameW(nullptr,p,MAX_PATH);
    std::wstring ps(p); auto sl=ps.find_last_of(L"\\/");
    ps=ps.substr(0,sl+1)+L"rtss_overlay.cfg";
    FILE*f=nullptr; _wfopen_s(&f,ps.c_str(),L"wb");
    if(f){fwrite(&g_oc,sizeof(g_oc),1,f);fclose(f);}
}
static void LoadCfg(){
    wchar_t p[MAX_PATH]{}; ::GetModuleFileNameW(nullptr,p,MAX_PATH);
    std::wstring ps(p); auto sl=ps.find_last_of(L"\\/");
    ps=ps.substr(0,sl+1)+L"rtss_overlay.cfg";
    FILE*f=nullptr; _wfopen_s(&f,ps.c_str(),L"rb");
    if(!f)return;
    OverlayConfig tmp{}; if(fread(&tmp,sizeof(tmp),1,f)==1&&tmp.magic==CFG_MAGIC) g_oc=tmp;
    fclose(f); PushConfig();
}

// ----------------------------------------------------------------
// WndProc
// ----------------------------------------------------------------
static HBRUSH g_brDark=nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg)
    {
    case WM_CREATE:
        g_brDark=::CreateSolidBrush(RGB(12,13,17));
        ::SetTimer(hwnd,1,200,nullptr);
        return 0;

    case WM_TIMER:
        TryConnectShm();
        ::InvalidateRect(hwnd,nullptr,FALSE);
        return 0;

    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        ::SetTextColor((HDC)wp,RGB(160,162,170));
        ::SetBkColor((HDC)wp,RGB(18,20,26));
        return (LRESULT)g_brDark;

    case WM_LBUTTONDOWN:{
        int mx=LOWORD(lp),my=HIWORD(lp);
        if(my<52){g_drag=true;g_dragOff={mx,my};::SetCapture(hwnd);return 0;}
        int tab=HitTab(mx,my);
        if(tab>=0){g_tab=tab;::InvalidateRect(hwnd,nullptr,FALSE);return 0;}
        if(g_tab==TAB_INJECT){
            int row=HitRow(mx,my);
            if(row>=0){g_selected=row;g_injected=false;::InvalidateRect(hwnd,nullptr,FALSE);}
        }
        if(g_tab==TAB_EDITOR){
            int pos=HitPos(mx,my);
            if(pos>=0){g_oc.position=(UINT32)pos;PushConfig();::InvalidateRect(hwnd,nullptr,FALSE);}
            int sr=HitShowRow(mx,my);
            if(sr>=0){
                UINT32*v[]={&g_oc.showGpu,&g_oc.showCpu,&g_oc.showRam,
                    &g_oc.showFps,&g_oc.showFrametime,&g_oc.showGraph};
                *v[sr]=(*v[sr])?0:1; PushConfig(); ::InvalidateRect(hwnd,nullptr,FALSE);
            }
            int ci=HitColor(mx,my);
            if(ci>=0){
                UINT32*cv[]={&g_oc.colLabel,&g_oc.colValue,&g_oc.colUnit,
                    &g_oc.colFpsHi,&g_oc.colFpsMid,&g_oc.colFpsLo};
                if(PickCol(*cv[ci])){PushConfig();::InvalidateRect(hwnd,nullptr,FALSE);}
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if(g_drag){RECT r;::GetWindowRect(hwnd,&r);
            ::SetWindowPos(hwnd,nullptr,
                r.left+(short)LOWORD(lp)-g_dragOff.x,
                r.top+(short)HIWORD(lp)-g_dragOff.y,
                0,0,SWP_NOSIZE|SWP_NOZORDER);}
        return 0;
    case WM_LBUTTONUP:
        if(g_tab==TAB_INJECT&&HitInject(LOWORD(lp),HIWORD(lp))&&g_selected>=0){
            if(DoInject(g_procs[g_selected].pid,g_dllPath)){
                swprintf_s(g_status,L"Injected into %ls (PID %u)",
                    g_procs[g_selected].name.c_str(),g_procs[g_selected].pid);
                g_statusOk=true; g_injected=true;
                // Auto-switch to editor
                ::Sleep(500); g_tab=TAB_EDITOR;
            } else {
                wcscpy_s(g_status,L"Failed — run as Administrator");
                g_statusOk=false;
            }
            g_flashTick=::GetTickCount();
            ::InvalidateRect(hwnd,nullptr,FALSE);
        }
        g_drag=false; ::ReleaseCapture(); return 0;

    case WM_MOUSEWHEEL:
        if(g_tab==TAB_INJECT){
            int d=GET_WHEEL_DELTA_WPARAM(wp);
            g_scrollTop-=d/WHEEL_DELTA;
            int mx2=(int)g_procs.size()-ROWS;
            if(g_scrollTop<0)g_scrollTop=0;
            if(g_scrollTop>mx2)g_scrollTop=mx2<0?0:mx2;
            ::InvalidateRect(hwnd,nullptr,FALSE);
        }
        return 0;

    case WM_KEYDOWN:
        if(wp==VK_F5){RefreshProcs();::InvalidateRect(hwnd,nullptr,FALSE);}
        if(wp==VK_UP&&g_selected>0){g_selected--;
            if(g_selected<g_scrollTop)g_scrollTop=g_selected;
            ::InvalidateRect(hwnd,nullptr,FALSE);}
        if(wp==VK_DOWN&&g_selected<(int)g_procs.size()-1){g_selected++;
            if(g_selected>=g_scrollTop+ROWS)g_scrollTop=g_selected-ROWS+1;
            ::InvalidateRect(hwnd,nullptr,FALSE);}
        if(wp=='S'&&(::GetKeyState(VK_CONTROL)&0x8000))SaveCfg();
        if(wp=='O'&&(::GetKeyState(VK_CONTROL)&0x8000))LoadCfg();
        return 0;

    case WM_RBUTTONUP:
    case WM_DESTROY:
        ::KillTimer(hwnd,1);
        SaveCfg();
        if(g_brDark)::DeleteObject(g_brDark);
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
    GdiplusStartup(&g_gdip,&gsi,nullptr);

    // Extract DLL from resource
    if(!ExtractDll())
        ::MessageBoxW(nullptr,
            L"Failed to extract HookDLL — make sure the build included the DLL resource.",
            L"RTSS Clone",MB_ICONERROR);

    RefreshProcs();
    LoadCfg();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.lpszClassName=L"RTSSAll"; wc.hCursor=::LoadCursorW(nullptr,IDC_ARROW);
    ::RegisterClassExW(&wc);

    g_hwnd=::CreateWindowExW(WS_EX_APPWINDOW|WS_EX_LAYERED,
        wc.lpszClassName,L"RTSS Clone",
        WS_POPUP|WS_VISIBLE,150,80,WW,WH,nullptr,nullptr,hInst,nullptr);

    ::SetLayeredWindowAttributes(g_hwnd,0,245,LWA_ALPHA);
    ::ShowWindow(g_hwnd,SW_SHOW);
    ::UpdateWindow(g_hwnd);

    MSG msg{};
    while(::GetMessageW(&msg,nullptr,0,0)){
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    GdiplusShutdown(g_gdip);
    return 0;
}
