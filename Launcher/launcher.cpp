// ============================================================
// RTSS Clone — All-in-One Launcher
// Single EXE with embedded HookDLL.dll resource.
// Two tabs: INJECT (process list) + EDITOR (live overlay config)
// Slider dragging, TTF font picker, color picker.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

#include "resource.h"
#include "../Shared/SharedMemory.h"

#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"psapi.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"comdlg32.lib")

using namespace Gdiplus;

// ----------------------------------------------------------------
// OverlayConfig — written to shared mem offset 4096
// Read by hooks.cpp every frame
// ----------------------------------------------------------------
#define CFG_MAGIC  0xCF6E0001u
#define CFG_OFFSET 4096

struct OverlayConfig {
    UINT32 magic, position;      // position: 0=TL 1=TC 2=TR 3=BL 4=BC 5=BR
    float  fontSize, lineHeight;
    UINT32 fontBold;
    UINT32 colLabel, colValue, colUnit, colFpsHi, colFpsMid, colFpsLo;
    float  bgAlpha;
    UINT32 showGpu, showCpu, showRam, showFps, showFrametime, showGraph;
    float  labelWidth, padding;
    wchar_t fontPath[MAX_PATH];  // custom TTF path (empty = default Courier New)
};

static OverlayConfig g_oc = {
    CFG_MAGIC, 0,
    13.f, 18.f, 0,
    0xFF2080E0,0xFFFFFFFF,0xFF888888,
    0xFFFFFFFF,0xFF20A0E0,0xFF3030E0,
    0.f, 1,1,1,1,1,1,
    42.f, 4.f, L""
};

// ----------------------------------------------------------------
// Window constants
// ----------------------------------------------------------------
#define WW 520
#define WH 640
#define TAB_INJECT 0
#define TAB_EDITOR 1

// Colors
static Color C_BG   (255,12,13,17);
static Color C_PANEL(255,18,20,26);
static Color C_BRD  (255,35,38,50);
static Color C_ACC  (255,230,130,0);
static Color C_TXT  (255,215,215,220);
static Color C_MUTE (255,95,100,115);
static Color C_GREEN(255,55,185,85);
static Color C_RED  (255,205,55,55);
static Color C_AMBER(255,230,130,0);

// ----------------------------------------------------------------
// Hit region system — built each paint, tested on click
// ----------------------------------------------------------------
struct HitRegion { int x,y,w,h,id; };
static std::vector<HitRegion> g_hits;
static void   ClearHits()                         { g_hits.clear(); }
static void   AddHit(int x,int y,int w,int h,int id){ g_hits.push_back({x,y,w,h,id}); }
static int    TestHit(int mx,int my){
    // Iterate reverse so topmost (last drawn) wins
    for(int i=(int)g_hits.size()-1;i>=0;i--){
        auto&r=g_hits[i];
        if(mx>=r.x&&mx<r.x+r.w&&my>=r.y&&my<r.y+r.h) return r.id;
    }
    return -1;
}
static const HitRegion* GetHit(int id){
    for(auto&r:g_hits) if(r.id==id) return &r;
    return nullptr;
}

// Hit IDs
#define HIT_TAB_INJ    1000
#define HIT_TAB_ED     1001
#define HIT_ROW_BASE   2000
#define HIT_INJ_BTN    3000
#define HIT_POS_BASE   4000   // +0..5
#define HIT_SHOW_BASE  5000   // +0..5
#define HIT_COL_BASE   6000   // +0..5
#define HIT_SL_FSIZE   7000
#define HIT_SL_LINEH   7001
#define HIT_SL_BGALPHA 7002
#define HIT_SL_LBLW    7003
#define HIT_SL_PAD     7004
#define HIT_FONT_PICK  7010

// ----------------------------------------------------------------
// Slider drag state
// ----------------------------------------------------------------
static int   g_dragSlider = -1;   // which slider is being dragged (-1=none)
static int   g_sliderX    = 0;    // x origin of slider track
static int   g_sliderW    = 0;    // width of slider track

// ----------------------------------------------------------------
// Globals
// ----------------------------------------------------------------
static ULONG_PTR g_gdip    = 0;
static HWND      g_hwnd    = nullptr;
static int       g_tab     = TAB_INJECT;
static bool      g_drag    = false;
static POINT     g_dragOff = {};
static wchar_t   g_status[256] = L"Select a process then click INJECT";
static bool      g_statusOk   = true;
static DWORD     g_flashTick  = 0;
static bool      g_injected   = false;
static std::wstring g_dllPath;

// Process list
struct ProcEntry { DWORD pid; std::wstring name,path; };
static std::vector<ProcEntry> g_procs;
static int g_selected  = -1;
static int g_scrollTop = 0;
static const int ROWS  = 10;
static const int ROW_H = 32;

// Shared memory
static SharedMemHandle g_shm;
static OverlayConfig*  g_pCfg = nullptr;

// ----------------------------------------------------------------
// Shared memory helpers
// ----------------------------------------------------------------
static void TryConnect(){
    if(!g_shm.Valid()) g_shm.OpenMapping();
    if(g_shm.Valid()&&!g_pCfg)
        g_pCfg=(OverlayConfig*)((BYTE*)g_shm.Data()+CFG_OFFSET);
}
static void PushConfig(){
    if(g_pCfg) memcpy(g_pCfg,&g_oc,sizeof(OverlayConfig));
}

// ----------------------------------------------------------------
// Extract embedded DLL or fall back to exe directory
// ----------------------------------------------------------------
static bool ExtractDll(){
    HRSRC hRes=::FindResourceW(nullptr,MAKEINTRESOURCEW(IDR_HOOKDLL),MAKEINTRESOURCEW(256));
    if(!hRes){
        wchar_t ep[MAX_PATH]{}; ::GetModuleFileNameW(nullptr,ep,MAX_PATH);
        std::wstring p(ep); auto sl=p.find_last_of(L"\\/");
        g_dllPath=(sl!=std::wstring::npos?p.substr(0,sl+1):L"")+L"HookDLL.dll";
        return ::GetFileAttributesW(g_dllPath.c_str())!=INVALID_FILE_ATTRIBUTES;
    }
    HGLOBAL hG=::LoadResource(nullptr,hRes);
    DWORD sz=::SizeofResource(nullptr,hRes);
    void* ptr=::LockResource(hG);
    if(!ptr||!sz) return false;
    wchar_t tmp[MAX_PATH]{}; ::GetTempPathW(MAX_PATH,tmp);
    g_dllPath=std::wstring(tmp)+L"rtss_hookdll.dll";
    HANDLE hf=::CreateFileW(g_dllPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
    if(hf==INVALID_HANDLE_VALUE) return false;
    DWORD w=0; ::WriteFile(hf,ptr,sz,&w,nullptr); ::CloseHandle(hf);
    return w==sz;
}

// ----------------------------------------------------------------
// Process utilities
// ----------------------------------------------------------------
static void RefreshProcs(){
    g_procs.clear(); g_selected=-1; g_scrollTop=0;
    HANDLE snap=::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE)return;
    PROCESSENTRY32W pe{sizeof(pe)};
    if(::Process32FirstW(snap,&pe))do{
        if(pe.th32ProcessID<5)continue;
        ProcEntry e; e.pid=pe.th32ProcessID; e.name=pe.szExeFile;
        HANDLE hp=::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,e.pid);
        if(hp){wchar_t buf[MAX_PATH]{};DWORD n=MAX_PATH;
            if(::QueryFullProcessImageNameW(hp,0,buf,&n))e.path=buf;
            ::CloseHandle(hp);}
        g_procs.push_back(e);
    }while(::Process32NextW(snap,&pe));
    ::CloseHandle(snap);
    std::sort(g_procs.begin(),g_procs.end(),
        [](const ProcEntry&a,const ProcEntry&b){return _wcsicmp(a.name.c_str(),b.name.c_str())<0;});
}

static bool DoInject(DWORD pid,const std::wstring&dll){
    HANDLE hp=::OpenProcess(PROCESS_ALL_ACCESS,FALSE,pid);
    if(!hp)return false;
    size_t nb=(dll.size()+1)*sizeof(wchar_t);
    LPVOID rm=::VirtualAllocEx(hp,nullptr,nb,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!rm){::CloseHandle(hp);return false;}
    SIZE_T w=0; ::WriteProcessMemory(hp,rm,dll.c_str(),nb,&w);
    HMODULE hk=::GetModuleHandleW(L"kernel32.dll");
    auto pfn=(LPTHREAD_START_ROUTINE)::GetProcAddress(hk,"LoadLibraryW");
    HANDLE ht=::CreateRemoteThread(hp,nullptr,0,pfn,rm,0,nullptr);
    if(!ht){::VirtualFreeEx(hp,rm,0,MEM_RELEASE);::CloseHandle(hp);return false;}
    ::WaitForSingleObject(ht,8000);
    DWORD ec=0; ::GetExitCodeThread(ht,&ec);
    ::CloseHandle(ht); ::VirtualFreeEx(hp,rm,0,MEM_RELEASE); ::CloseHandle(hp);
    return ec!=0;
}

// ----------------------------------------------------------------
// Save / load config
// ----------------------------------------------------------------
static void SaveCfg(){
    wchar_t p[MAX_PATH]{}; ::GetModuleFileNameW(nullptr,p,MAX_PATH);
    std::wstring ps(p); auto sl=ps.find_last_of(L"\\/");
    ps=(sl!=std::wstring::npos?ps.substr(0,sl+1):L"")+L"rtss_overlay.cfg";
    FILE*f=nullptr; _wfopen_s(&f,ps.c_str(),L"wb");
    if(f){fwrite(&g_oc,sizeof(g_oc),1,f);fclose(f);}
}
static void LoadCfg(){
    wchar_t p[MAX_PATH]{}; ::GetModuleFileNameW(nullptr,p,MAX_PATH);
    std::wstring ps(p); auto sl=ps.find_last_of(L"\\/");
    ps=(sl!=std::wstring::npos?ps.substr(0,sl+1):L"")+L"rtss_overlay.cfg";
    FILE*f=nullptr; _wfopen_s(&f,ps.c_str(),L"rb");
    if(!f)return;
    OverlayConfig tmp{}; if(fread(&tmp,sizeof(tmp),1,f)==1&&tmp.magic==CFG_MAGIC)g_oc=tmp;
    fclose(f);
    if(g_oc.position>5)g_oc.position=0;
    PushConfig();
}

// ----------------------------------------------------------------
// Color picker
// ----------------------------------------------------------------
static bool PickCol(UINT32&abgr){
    static COLORREF cust[16]={};
    BYTE r=(abgr>>0)&0xFF,gr=(abgr>>8)&0xFF,b=(abgr>>16)&0xFF;
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner=g_hwnd; cc.rgbResult=RGB(r,gr,b);
    cc.lpCustColors=cust; cc.Flags=CC_FULLOPEN|CC_RGBINIT;
    if(!::ChooseColorW(&cc))return false;
    r=GetRValue(cc.rgbResult); gr=GetGValue(cc.rgbResult); b=GetBValue(cc.rgbResult);
    abgr=(255u<<24)|(b<<16)|(gr<<8)|r;
    return true;
}

// Font file picker
static bool PickFont(){
    wchar_t buf[MAX_PATH]={};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner=g_hwnd;
    ofn.lpstrFilter=L"Font files\0*.ttf;*.otf\0All files\0*.*\0";
    ofn.lpstrFile=buf; ofn.nMaxFile=MAX_PATH;
    ofn.lpstrTitle=L"Select font file (.ttf/.otf)";
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
    if(!::GetOpenFileNameW(&ofn))return false;
    wcsncpy_s(g_oc.fontPath,buf,MAX_PATH-1);
    return true;
}

// ----------------------------------------------------------------
// GDI+ draw helpers
// ----------------------------------------------------------------
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

// ----------------------------------------------------------------
// DrawSlider — draws and registers hit region
// Returns current oy after drawing
// ----------------------------------------------------------------
static int DrawSlider(Graphics&g,FontFamily&ff,int ox,int oy,int W,
    const wchar_t*name,float val,float mn,float mx2,int hitId)
{
    Lbl(g,ff,name,(float)ox,(float)oy,8.f,C_MUTE);
    int tx=ox+105,tw=W-150;
    // Track bg
    SolidBrush tb(Color(255,28,30,40)); g.FillRectangle(&tb,tx,oy+5,tw,6);
    // Fill
    float pct=(val-mn)/(mx2-mn);
    if(pct<0)pct=0; if(pct>1)pct=1;
    SolidBrush tf(C_ACC); g.FillRectangle(&tf,tx,oy+5,(int)(tw*pct),6);
    // Thumb
    int thumbX=tx+(int)(tw*pct);
    SolidBrush tt(Color(255,220,220,225));
    g.FillEllipse(&tt,thumbX-6,oy,12,14);
    Pen tp(Color(255,240,160,0),1.5f); g.DrawEllipse(&tp,thumbX-6,oy,12,14);
    // Value label
    wchar_t vb[16]; swprintf_s(vb,L"%.0f",val);
    Lbl(g,ff,vb,(float)(ox+W-40),(float)oy,8.f,C_TXT);
    // Hit region — wider area for easier grabbing
    AddHit(tx-4,oy-2,tw+8,18,hitId);
    return oy+20;
}

// ----------------------------------------------------------------
// PaintInject tab
// ----------------------------------------------------------------
static void PaintInject(Graphics&g,FontFamily&ff,int ox,int oy,int W)
{
    ClearHits();
    // Re-register tab buttons since they're drawn in Paint not here
    AddHit(18,58,112,24,HIT_TAB_INJ);
    AddHit(138,58,112,24,HIT_TAB_ED);
    Lbl(g,ff,L"RUNNING PROCESSES",(float)ox,(float)oy,7.f,C_MUTE);
    Lbl(g,ff,L"F5 refresh  ↑↓ navigate",(float)(ox+W-148),(float)oy,7.f,C_MUTE);
    oy+=14;

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
        }else if(i%2==0){
            SolidBrush sb(Color(12,255,255,255)); g.FillRectangle(&sb,ox+1,ry,W-2,ROW_H);
        }
        AddHit(ox,ry,W,ROW_H,HIT_ROW_BASE+idx);
        wchar_t pid[16]; swprintf_s(pid,L"%u",g_procs[idx].pid);
        SolidBrush bm(C_MUTE); g.DrawString(pid,-1,&fPid,PointF((float)(ox+8),(float)(ry+5)),&bm);
        SolidBrush bn(sel?C_AMBER:C_TXT);
        g.DrawString(g_procs[idx].name.c_str(),-1,&fName,PointF((float)(ox+58),(float)(ry+4)),&bn);
        if(!g_procs[idx].path.empty()){
            std::wstring ep=g_procs[idx].path;
            if(ep.size()>52)ep=L"..."+ep.substr(ep.size()-49);
            SolidBrush bp(Color(75,160,162,175));
            g.DrawString(ep.c_str(),-1,&fPath,PointF((float)(ox+58),(float)(ry+18)),&bp);
        }
        Pen rp(Color(18,255,255,255),1.f);
        g.DrawLine(&rp,ox+1,ry+ROW_H-1,ox+W-2,ry+ROW_H-1);
    }
    g.ResetClip();

    if(total>ROWS){
        int sx=ox+W-6,sy=oy+1,sh=lH-2;
        SolidBrush bsb(Color(25,255,255,255)); g.FillRectangle(&bsb,sx,sy,4,sh);
        float th=(float)sh*ROWS/total,ty=sy+(float)sh*g_scrollTop/total;
        SolidBrush bt(C_MUTE); g.FillRectangle(&bt,sx,(int)ty,4,(int)th);
    }
    oy+=lH+4;

    wchar_t cnt[32]; swprintf_s(cnt,L"%d processes",total);
    Lbl(g,ff,cnt,(float)ox,(float)oy,7.5f,C_MUTE); oy+=18;

    // Info box
    SolidBrush bI(C_PANEL); Pen pI(C_BRD,1.f);
    RoundRect(g,&pI,&bI,ox,oy,W,44,4);
    if(g_selected>=0&&g_selected<total){
        Lbl(g,ff,g_procs[g_selected].name.c_str(),(float)(ox+10),(float)(oy+6),10.f,C_AMBER,true);
        wchar_t pb[32]; swprintf_s(pb,L"PID %u",g_procs[g_selected].pid);
        Lbl(g,ff,pb,(float)(ox+10),(float)(oy+26),8.f,C_MUTE);
    }else{
        Lbl(g,ff,L"No process selected",(float)(ox+10),(float)(oy+16),8.f,C_MUTE);
    }
    oy+=52;

    bool canInj=(g_selected>=0&&!g_dllPath.empty());
    SolidBrush bB(canInj?(g_injected?Color(255,40,120,50):C_ACC):Color(255,35,37,48));
    Pen pB(canInj?Color(255,255,200,100):C_BRD,1.5f);
    RoundRect(g,&pB,&bB,ox,oy,W,38,5);
    Font fBtn(&ff,12.f,FontStyleBold,UnitPoint);
    SolidBrush bBT(canInj?Color(255,20,20,20):C_MUTE);
    StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(g_injected?L"✓  INJECTED — switch to EDITOR":L"INJECT",
        -1,&fBtn,RectF((float)ox,(float)oy,(float)W,38.f),&sf,&bBT);
    AddHit(ox,oy,W,38,HIT_INJ_BTN);  // always register — errors handled in click
    oy+=46;

    DWORD now=::GetTickCount();
    float fl=1.f; if(now-g_flashTick<700)fl=0.5f+0.5f*sinf((float)(now-g_flashTick)*0.012f);
    Color sc(g_statusOk?Color((BYTE)(fl*200),55,190,85):Color((BYTE)(fl*220),205,55,55));
    Lbl(g,ff,g_status,(float)ox,(float)oy,8.f,sc);
}

// ----------------------------------------------------------------
// PaintEditor tab
// ----------------------------------------------------------------
static void PaintEditor(Graphics&g,FontFamily&ff,int ox,int oy,int W)
{
    ClearHits();
    // Re-register tab buttons
    AddHit(18,58,112,24,HIT_TAB_INJ);
    AddHit(138,58,112,24,HIT_TAB_ED);
    bool conn=g_shm.Valid();
    SolidBrush cd(conn?C_GREEN:C_RED); g.FillEllipse(&cd,ox,oy+4,8,8);
    Lbl(g,ff,conn?L"Live — changes apply instantly in-game":L"Not connected — inject first",
        (float)(ox+14),(float)oy,8.f,conn?C_GREEN:C_RED);
    oy+=20;

    if(conn&&g_shm.Data()){
        RtssStats*s=g_shm.Data();
        SolidBrush sb(C_PANEL); Pen pb(C_BRD,1.f);
        RoundRect(g,&pb,&sb,ox,oy,W,26,3);
        wchar_t ls[128]; swprintf_s(ls,
            L"  FPS %.0f   CPU %.0f%%   RAM %.0f MB   GPU %.0f%%   [%hs]",
            s->fps,s->cpuUsagePercent,s->ramUsedMB,s->gpuUsagePercent,
            s->apiName[0]?s->apiName:"...");
        Lbl(g,ff,ls,(float)(ox+6),(float)(oy+7),8.5f,C_TXT);
        oy+=32;
    }

    // ---- Position ----
    Lbl(g,ff,L"POSITION",(float)ox,(float)oy,7.f,C_MUTE); oy+=13;
    const wchar_t*posL[]={L"Top Left",L"Top Center",L"Top Right",
                          L"Bot Left",L"Bot Center",L"Bot Right"};
    int bw=W/3-4;
    for(int i=0;i<6;i++){
        int bx=ox+(i%3)*(W/3),by=oy+(i/3)*24;
        bool sel=((int)g_oc.position==i);
        SolidBrush bb(sel?C_ACC:C_PANEL); Pen bp(sel?Color(255,255,200,80):C_BRD,1.f);
        RoundRect(g,&bp,&bb,bx,by,bw,20,3);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        Font fb(&ff,7.5f,FontStyleRegular,UnitPoint);
        SolidBrush bt(sel?Color(255,20,20,20):C_MUTE);
        g.DrawString(posL[i],-1,&fb,RectF((float)bx,(float)by,(float)bw,20.f),&sf,&bt);
        AddHit(bx,by,bw,20,HIT_POS_BASE+i);
    }
    oy+=52;

    // ---- Font ----
    Lbl(g,ff,L"FONT",(float)ox,(float)oy,7.f,C_MUTE); oy+=13;
    oy=DrawSlider(g,ff,ox,oy,W,L"size",    g_oc.fontSize,   10,22,HIT_SL_FSIZE);
    oy=DrawSlider(g,ff,ox,oy,W,L"line height",g_oc.lineHeight,10,28,HIT_SL_LINEH);

    // Font file picker
    {
        SolidBrush fpB(C_PANEL); Pen fpP(C_BRD,1.f);
        RoundRect(g,&fpP,&fpB,ox,oy,W,22,3);
        std::wstring fLabel=g_oc.fontPath[0]?g_oc.fontPath:L"Default (Courier New)  — click to browse .ttf/.otf";
        Lbl(g,ff,fLabel.c_str(),(float)(ox+8),(float)(oy+4),7.5f,
            g_oc.fontPath[0]?C_AMBER:C_MUTE);
        AddHit(ox,oy,W,22,HIT_FONT_PICK);
        oy+=28;
    }

    // ---- Show flags ----
    Lbl(g,ff,L"SHOW ROWS",(float)ox,(float)oy,7.f,C_MUTE); oy+=13;
    const wchar_t*rn[]={L"GPU",L"CPU",L"RAM",L"FPS",L"Frametime",L"Graph"};
    UINT32*rp[]={&g_oc.showGpu,&g_oc.showCpu,&g_oc.showRam,
                 &g_oc.showFps,&g_oc.showFrametime,&g_oc.showGraph};
    for(int i=0;i<6;i++){
        int bx=ox+(i%3)*(W/3),by=oy+(i/3)*22;
        bool on=(*rp[i]!=0);
        SolidBrush bb(on?Color(255,30,80,40):C_PANEL); Pen bp(on?C_GREEN:C_BRD,1.f);
        RoundRect(g,&bp,&bb,bx,by,bw,18,3);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        Font fb(&ff,7.5f,FontStyleRegular,UnitPoint);
        SolidBrush bt(on?C_GREEN:C_MUTE);
        g.DrawString(rn[i],-1,&fb,RectF((float)bx,(float)by,(float)bw,18.f),&sf,&bt);
        AddHit(bx,by,bw,18,HIT_SHOW_BASE+i);
    }
    oy+=48;

    // ---- Colors ----
    Lbl(g,ff,L"COLORS  (click to change)",(float)ox,(float)oy,7.f,C_MUTE); oy+=13;
    const wchar_t*cn[]={L"Label",L"Value",L"Unit",L"FPS hi",L"FPS mid",L"FPS lo"};
    UINT32 cv[]={g_oc.colLabel,g_oc.colValue,g_oc.colUnit,
                 g_oc.colFpsHi,g_oc.colFpsMid,g_oc.colFpsLo};
    for(int i=0;i<6;i++){
        int cx=ox+(i%3)*(W/3),cy=oy+(i/3)*30;
        Lbl(g,ff,cn[i],(float)cx,(float)cy,7.5f,C_MUTE);
        BYTE r=(cv[i]>>0)&0xFF,gr=(cv[i]>>8)&0xFF,b=(cv[i]>>16)&0xFF;
        SolidBrush cs(Color(255,r,gr,b)); g.FillRectangle(&cs,cx,cy+12,bw,14);
        Pen cpn(Color(80,255,255,255),1.f); g.DrawRectangle(&cpn,cx,cy+12,bw-1,13);
        AddHit(cx,cy+12,bw,14,HIT_COL_BASE+i);
    }
    oy+=62;

    // ---- Extra sliders ----
    oy=DrawSlider(g,ff,ox,oy,W,L"bg opacity",g_oc.bgAlpha*100,0,100,HIT_SL_BGALPHA);
    oy=DrawSlider(g,ff,ox,oy,W,L"label width",g_oc.labelWidth,20,80,HIT_SL_LBLW);
    oy=DrawSlider(g,ff,ox,oy,W,L"padding",    g_oc.padding,   0,20, HIT_SL_PAD);

    Lbl(g,ff,L"Ctrl+S save   Ctrl+O load   right-click = close",
        (float)ox,(float)oy,7.f,C_MUTE);
}

// ----------------------------------------------------------------
// Full Paint
// ----------------------------------------------------------------
static HBRUSH g_brDark=nullptr;

static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps; HDC hdc=::BeginPaint(hwnd,&ps);
    HDC mem=::CreateCompatibleDC(hdc);
    HBITMAP bmp=::CreateCompatibleBitmap(hdc,WW,WH);
    ::SelectObject(mem,bmp);

    Graphics g(mem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Background
    SolidBrush brBg(C_BG); g.FillRectangle(&brBg,0,0,WW,WH);
    for(int y=0;y<WH;y+=4){Pen sp(Color(5,255,255,255),1.f);g.DrawLine(&sp,0,y,WW,y);}

    // Top accent
    SolidBrush brAcc(C_ACC); g.FillRectangle(&brAcc,0,0,WW,3);

    FontFamily ff(L"Consolas");

    // Header
    SolidBrush brH(C_PANEL); g.FillRectangle(&brH,0,0,WW,52);
    Pen hBrd(Color(45,230,130,0),1.f); g.DrawLine(&hBrd,0,52,WW,52);
    Lbl(g,ff,L"RTSS Clone",18,12,16.f,C_ACC,true);
    Lbl(g,ff,L"All-in-One  v1.0",18,36,7.5f,C_MUTE);

    bool dllOk=!g_dllPath.empty();
    SolidBrush dd(dllOk?C_GREEN:C_RED); g.FillEllipse(&dd,WW-130,18,8,8);
    Lbl(g,ff,dllOk?L"HookDLL ready":L"DLL missing",(float)(WW-118),16,7.5f,dllOk?C_GREEN:C_RED);

    TryConnect();
    bool conn=g_shm.Valid();
    SolidBrush dc(conn?C_GREEN:C_MUTE); g.FillEllipse(&dc,WW-130,32,8,8);
    Lbl(g,ff,conn?L"Game connected":L"Not injected",(float)(WW-118),30,7.5f,conn?C_GREEN:C_MUTE);

    // Tabs — register hit regions (ClearHits called in tab paint functions)
    const wchar_t*tabs[]={L"INJECT",L"EDITOR"};
    int tabIds[]={HIT_TAB_INJ,HIT_TAB_ED};
    for(int i=0;i<2;i++){
        int tx=18+i*120,ty=58;
        bool sel=(g_tab==i);
        SolidBrush tb(sel?C_ACC:C_PANEL); Pen tp(sel?Color(255,255,200,100):C_BRD,1.f);
        RoundRect(g,&tp,&tb,tx,ty,112,24,4);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        Font tf(&ff,9.f,FontStyleBold,UnitPoint);
        SolidBrush tt(sel?Color(255,20,20,20):C_MUTE);
        g.DrawString(tabs[i],-1,&tf,RectF((float)tx,(float)ty,112.f,24.f),&sf,&tt);
        AddHit(tx,ty,112,24,tabIds[i]);
    }

    // Content
    if(g_tab==TAB_INJECT) PaintInject(g,ff,14,90,WW-28);
    else                   PaintEditor(g,ff,14,90,WW-28);

    // Bottom accent
    g.FillRectangle(&brAcc,0,WH-2,WW,2);

    ::BitBlt(hdc,0,0,WW,WH,mem,0,0,SRCCOPY);
    ::DeleteObject(bmp); ::DeleteDC(mem);
    ::EndPaint(hwnd,&ps);
}

// ----------------------------------------------------------------
// Slider value setter
// ----------------------------------------------------------------
static void SetSliderFromX(int hitId, int mx)
{
    const HitRegion* r=GetHit(hitId);
    if(!r)return;
    float pct=(float)(mx-r->x)/(float)r->w;
    if(pct<0)pct=0; if(pct>1)pct=1;
    if(hitId==HIT_SL_FSIZE)   g_oc.fontSize   =10.f+pct*12.f;
    if(hitId==HIT_SL_LINEH)   g_oc.lineHeight =10.f+pct*18.f;
    if(hitId==HIT_SL_BGALPHA) g_oc.bgAlpha    =pct;
    if(hitId==HIT_SL_LBLW)    g_oc.labelWidth =20.f+pct*60.f;
    if(hitId==HIT_SL_PAD)     g_oc.padding    =pct*20.f;
    PushConfig();
    ::InvalidateRect(g_hwnd,nullptr,FALSE);
}

// ----------------------------------------------------------------
// WndProc
// ----------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg)
    {
    case WM_CREATE:
        g_brDark=::CreateSolidBrush(RGB(12,13,17));
        ::SetTimer(hwnd,1,200,nullptr);
        return 0;

    case WM_TIMER:
        TryConnect();
        ::InvalidateRect(hwnd,nullptr,FALSE);
        return 0;

    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        ::SetTextColor((HDC)wp,RGB(150,152,160));
        ::SetBkColor((HDC)wp,RGB(12,13,17));
        return (LRESULT)g_brDark;

    case WM_LBUTTONDOWN: {
        int mx=LOWORD(lp),my=HIWORD(lp);
        // Drag titlebar
        if(my<52){g_drag=true;g_dragOff={mx,my};::SetCapture(hwnd);return 0;}
        int hit=TestHit(mx,my);
        if(hit<0)return 0;

        // Tabs
        if(hit==HIT_TAB_INJ){g_tab=TAB_INJECT;::InvalidateRect(hwnd,nullptr,FALSE);return 0;}
        if(hit==HIT_TAB_ED) {g_tab=TAB_EDITOR;::InvalidateRect(hwnd,nullptr,FALSE);return 0;}

        // Process rows
        if(hit>=HIT_ROW_BASE&&hit<HIT_ROW_BASE+10000){
            g_selected=hit-HIT_ROW_BASE; g_injected=false;
            ::InvalidateRect(hwnd,nullptr,FALSE); return 0;
        }

        // Inject button — check preconditions and show clear errors
        if(hit==HIT_INJ_BTN){
            if(g_selected<0||g_selected>=(int)g_procs.size()){
                wcscpy_s(g_status,L"Select a process from the list first");
                g_statusOk=false;
            } else if(g_dllPath.empty()){
                wcscpy_s(g_status,L"HookDLL not found — place HookDLL.dll next to RTSSClone.exe");
                g_statusOk=false;
            } else {
                if(DoInject(g_procs[g_selected].pid,g_dllPath)){
                    swprintf_s(g_status,L"Injected into %ls (PID %u)",
                        g_procs[g_selected].name.c_str(),g_procs[g_selected].pid);
                    g_statusOk=true; g_injected=true;
                    ::Sleep(400); g_tab=TAB_EDITOR;
                }else{
                    wcscpy_s(g_status,L"Failed — run as Administrator");
                    g_statusOk=false;
                }
            }
            g_flashTick=::GetTickCount();
            ::InvalidateRect(hwnd,nullptr,FALSE); return 0;
        }

        // Position buttons
        if(hit>=HIT_POS_BASE&&hit<HIT_POS_BASE+6){
            g_oc.position=(UINT32)(hit-HIT_POS_BASE);
            PushConfig();::InvalidateRect(hwnd,nullptr,FALSE);return 0;
        }

        // Show toggles
        if(hit>=HIT_SHOW_BASE&&hit<HIT_SHOW_BASE+6){
            UINT32*v[]={&g_oc.showGpu,&g_oc.showCpu,&g_oc.showRam,
                &g_oc.showFps,&g_oc.showFrametime,&g_oc.showGraph};
            int i=hit-HIT_SHOW_BASE; *v[i]=(*v[i])?0:1;
            PushConfig();::InvalidateRect(hwnd,nullptr,FALSE);return 0;
        }

        // Color swatches
        if(hit>=HIT_COL_BASE&&hit<HIT_COL_BASE+6){
            UINT32*cv[]={&g_oc.colLabel,&g_oc.colValue,&g_oc.colUnit,
                &g_oc.colFpsHi,&g_oc.colFpsMid,&g_oc.colFpsLo};
            if(PickCol(*cv[hit-HIT_COL_BASE])){PushConfig();::InvalidateRect(hwnd,nullptr,FALSE);}
            return 0;
        }

        // Font picker
        if(hit==HIT_FONT_PICK){
            if(PickFont()){PushConfig();::InvalidateRect(hwnd,nullptr,FALSE);}
            return 0;
        }

        // Sliders — start drag
        if(hit>=HIT_SL_FSIZE&&hit<=HIT_SL_PAD){
            g_dragSlider=hit;
            ::SetCapture(hwnd);
            SetSliderFromX(hit,mx);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx=LOWORD(lp);
        if(g_drag){
            RECT r;::GetWindowRect(hwnd,&r);
            ::SetWindowPos(hwnd,nullptr,
                r.left+(short)LOWORD(lp)-g_dragOff.x,
                r.top+(short)HIWORD(lp)-g_dragOff.y,
                0,0,SWP_NOSIZE|SWP_NOZORDER);
        }
        // Slider drag
        if(g_dragSlider>=0) SetSliderFromX(g_dragSlider,mx);
        return 0;
    }

    case WM_LBUTTONUP:
        g_drag=false;
        g_dragSlider=-1;
        ::ReleaseCapture();
        return 0;

    case WM_MOUSEWHEEL:
        if(g_tab==TAB_INJECT){
            int d=GET_WHEEL_DELTA_WPARAM(wp);
            g_scrollTop-=d/WHEEL_DELTA;
            int maxS=(int)g_procs.size()-ROWS;
            if(g_scrollTop<0)g_scrollTop=0;
            if(g_scrollTop>maxS)g_scrollTop=maxS<0?0:maxS;
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
        if(wp=='O'&&(::GetKeyState(VK_CONTROL)&0x8000)){LoadCfg();::InvalidateRect(hwnd,nullptr,FALSE);}
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

    ExtractDll();
    RefreshProcs();
    LoadCfg();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.lpszClassName=L"RTSSAll";
    wc.hCursor=::LoadCursorW(nullptr,IDC_ARROW);
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
