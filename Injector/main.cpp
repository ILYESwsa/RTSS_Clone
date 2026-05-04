// ============================================================
// Injector/main.cpp  —  RTSS Clone Injector
// Win32 GUI with GDI+ rendering. Dark game-tool aesthetic.
// Lists processes, lets user select, injects HookDLL.dll.
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
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")

using namespace Gdiplus;

// ----------------------------------------------------------------
// Layout / colours
// ----------------------------------------------------------------
#define WW  480
#define WH  580

// Colours
static const Color C_BG       (255,  12,  13,  16);
static const Color C_PANEL    (255,  18,  20,  25);
static const Color C_BORDER   (255,  35,  38,  46);
static const Color C_ACCENT   (255, 240, 160,   0);  // amber
static const Color C_ACCENT2  (255, 255, 200,  80);
static const Color C_TEXT     (255, 220, 220, 220);
static const Color C_MUTED    (255, 100, 105, 115);
static const Color C_SEL      (255,  30,  32,  40);
static const Color C_SELBRD   (255, 240, 160,   0);
static const Color C_GREEN    (255,  60, 200, 100);
static const Color C_RED      (255, 220,  60,  60);

// ----------------------------------------------------------------
// Process entry
// ----------------------------------------------------------------
struct ProcEntry {
    DWORD        pid;
    std::wstring name;
    std::wstring exePath;
};

// ----------------------------------------------------------------
// Globals
// ----------------------------------------------------------------
static HWND            g_hwnd        = nullptr;
static ULONG_PTR       g_gdipToken   = 0;
static std::vector<ProcEntry> g_procs;
static int             g_selected    = -1;
static bool            g_dragging    = false;
static POINT           g_dragOff     = {};
static wchar_t         g_status[256] = L"Select a process and click INJECT";
static bool            g_statusOk    = true;
static DWORD           g_flashTick   = 0;
static bool            g_injected    = false;

// List scroll
static int  g_scrollTop  = 0;
static int  g_visibleRows = 12;
static const int ROW_H   = 34;

// ----------------------------------------------------------------
// Enumerate processes
// ----------------------------------------------------------------
static void RefreshProcesses()
{
    g_procs.clear();
    g_selected  = -1;
    g_scrollTop = 0;

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{ sizeof(pe) };
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;
            ProcEntry e;
            e.pid  = pe.th32ProcessID;
            e.name = pe.szExeFile;

            // Get full path
            HANDLE hp = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.pid);
            if (hp) {
                wchar_t path[MAX_PATH]{};
                DWORD sz = MAX_PATH;
                if (::QueryFullProcessImageNameW(hp, 0, path, &sz))
                    e.exePath = path;
                ::CloseHandle(hp);
            }
            g_procs.push_back(e);
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);

    std::sort(g_procs.begin(), g_procs.end(),
        [](const ProcEntry& a, const ProcEntry& b){
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
}

// ----------------------------------------------------------------
// Get DLL path (next to injector exe)
// ----------------------------------------------------------------
static std::wstring GetDllPath()
{
    wchar_t exePath[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring p(exePath);
    auto sl = p.find_last_of(L"\\/");
    if (sl != std::wstring::npos)
        p = p.substr(0, sl+1) + L"HookDLL.dll";
    else
        p = L"HookDLL.dll";
    return p;
}

// ----------------------------------------------------------------
// Inject
// ----------------------------------------------------------------
static bool DoInject(DWORD pid, const std::wstring& dll)
{
    HANDLE hp = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hp) return false;

    const size_t nb = (dll.size()+1)*sizeof(wchar_t);
    LPVOID rm = ::VirtualAllocEx(hp, nullptr, nb, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!rm) { ::CloseHandle(hp); return false; }

    SIZE_T written=0;
    if (!::WriteProcessMemory(hp, rm, dll.c_str(), nb, &written) || written!=nb) {
        ::VirtualFreeEx(hp, rm, 0, MEM_RELEASE);
        ::CloseHandle(hp); return false;
    }

    HMODULE hk = ::GetModuleHandleW(L"kernel32.dll");
    auto pfn = (LPTHREAD_START_ROUTINE)::GetProcAddress(hk, "LoadLibraryW");
    HANDLE ht = ::CreateRemoteThread(hp, nullptr, 0, pfn, rm, 0, nullptr);
    if (!ht) {
        ::VirtualFreeEx(hp, rm, 0, MEM_RELEASE);
        ::CloseHandle(hp); return false;
    }

    ::WaitForSingleObject(ht, 8000);
    DWORD ec=0; ::GetExitCodeThread(ht, &ec);
    ::CloseHandle(ht);
    ::VirtualFreeEx(hp, rm, 0, MEM_RELEASE);
    ::CloseHandle(hp);
    return ec != 0;
}

// ----------------------------------------------------------------
// Draw helpers
// ----------------------------------------------------------------
static void DrawRoundRect(Graphics& g, Pen* pen, SolidBrush* fill,
    int x,int y,int w,int h, int r)
{
    GraphicsPath path;
    path.AddArc(x,y,r*2,r*2,180,90);
    path.AddArc(x+w-r*2,y,r*2,r*2,270,90);
    path.AddArc(x+w-r*2,y+h-r*2,r*2,r*2,0,90);
    path.AddArc(x,y+h-r*2,r*2,r*2,90,90);
    path.CloseFigure();
    if (fill) g.FillPath(fill, &path);
    if (pen)  g.DrawPath(pen,  &path);
}

// ----------------------------------------------------------------
// Paint
// ----------------------------------------------------------------
static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = ::BeginPaint(hwnd, &ps);
    HDC mem = ::CreateCompatibleDC(hdc);
    HBITMAP bmp = ::CreateCompatibleBitmap(hdc, WW, WH);
    ::SelectObject(mem, bmp);

    Graphics g(mem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // ---- Background ----
    SolidBrush brBg(C_BG);
    g.FillRectangle(&brBg, 0,0,WW,WH);

    // Subtle scanline texture
    for (int y=0; y<WH; y+=4) {
        Pen scanPen(Color(6,255,255,255), 1.f);
        g.DrawLine(&scanPen, 0,y,WW,y);
    }

    // ---- Top accent bar ----
    LinearGradientBrush topBar(Point(0,0),Point(WW,0),
        Color(255,240,140,0), Color(255,180,80,0));
    g.FillRectangle(&topBar, 0,0,WW,3);

    // ---- Header panel ----
    SolidBrush brPanel(C_PANEL);
    g.FillRectangle(&brPanel, 0,0,WW,64);
    Pen borderBot(Color(50,240,160,0),1.f);
    g.DrawLine(&borderBot, 0,64,WW,64);

    // Title
    FontFamily ffTitle(L"Consolas");
    Font fTitle(&ffTitle, 18.f, FontStyleBold, UnitPoint);
    SolidBrush brAccent(C_ACCENT);
    g.DrawString(L"RTSS Clone", -1, &fTitle, PointF(20.f,12.f), &brAccent);

    Font fSub(&ffTitle, 8.f, FontStyleRegular, UnitPoint);
    SolidBrush brMuted(C_MUTED);
    g.DrawString(L"DLL INJECTOR  v1.0", -1, &fSub, PointF(22.f,40.f), &brMuted);

    // DLL status indicator (top-right)
    std::wstring dllPath = GetDllPath();
    bool dllFound = ::GetFileAttributesW(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    SolidBrush brDllDot(dllFound ? C_GREEN : C_RED);
    g.FillEllipse(&brDllDot, WW-110, 22, 8, 8);
    Font fDll(&ffTitle, 8.f, FontStyleRegular, UnitPoint);
    SolidBrush brDllTxt(dllFound ? C_GREEN : C_RED);
    g.DrawString(dllFound ? L"HookDLL found" : L"HookDLL missing",
        -1, &fDll, PointF((float)(WW-98), 20.f), &brDllTxt);

    // ---- Section label ----
    Font fSec(&ffTitle, 7.f, FontStyleRegular, UnitPoint);
    g.DrawString(L"RUNNING PROCESSES", -1, &fSec, PointF(20.f,76.f), &brMuted);

    // Refresh hint
    g.DrawString(L"[F5] REFRESH", -1, &fSec,
        PointF(WW-105.f, 76.f), &brMuted);

    // ---- Process list panel ----
    int listX=16, listY=92, listW=WW-32, listH=g_visibleRows*ROW_H;

    SolidBrush brListBg(C_PANEL);
    Pen brListBorder(C_BORDER, 1.f);
    DrawRoundRect(g, &brListBorder, &brListBg, listX, listY, listW, listH, 4);

    // Clip to list area
    g.SetClip(Rect(listX+1, listY+1, listW-2, listH-2));

    FontFamily ffMono(L"Consolas");
    Font fPid(&ffMono, 8.f, FontStyleRegular, UnitPoint);
    Font fName(&ffMono, 9.5f, FontStyleBold, UnitPoint);
    Font fPath(&ffMono, 7.f, FontStyleRegular, UnitPoint);

    int total = (int)g_procs.size();
    for (int i=0; i<g_visibleRows; i++) {
        int idx = g_scrollTop + i;
        if (idx >= total) break;

        int ry = listY + i*ROW_H;
        bool sel = (idx == g_selected);

        // Row background
        if (sel) {
            SolidBrush brSel(C_SEL);
            g.FillRectangle(&brSel, listX+1, ry, listW-2, ROW_H);
            // Left accent bar
            SolidBrush brSelBar(C_ACCENT);
            g.FillRectangle(&brSelBar, listX+1, ry, 3, ROW_H);
        } else if (i%2==0) {
            SolidBrush brStripe(Color(15,255,255,255));
            g.FillRectangle(&brStripe, listX+1, ry, listW-2, ROW_H);
        }

        // PID
        wchar_t pidBuf[16];
        swprintf_s(pidBuf, L"%u", g_procs[idx].pid);
        SolidBrush brPid(C_MUTED);
        g.DrawString(pidBuf, -1, &fPid,
            PointF((float)(listX+10), (float)(ry+4)), &brPid);

        // Name
        SolidBrush brName(sel ? C_ACCENT2 : C_TEXT);
        g.DrawString(g_procs[idx].name.c_str(), -1, &fName,
            PointF((float)(listX+60), (float)(ry+3)), &brName);

        // Path (truncated)
        if (!g_procs[idx].exePath.empty()) {
            std::wstring ep = g_procs[idx].exePath;
            if (ep.size() > 48) ep = L"..." + ep.substr(ep.size()-45);
            SolidBrush brPath(Color(80,160,160,170));
            g.DrawString(ep.c_str(), -1, &fPath,
                PointF((float)(listX+60), (float)(ry+18)), &brPath);
        }

        // Row separator
        Pen rowSep(Color(20,255,255,255), 1.f);
        g.DrawLine(&rowSep, listX+1, ry+ROW_H-1, listX+listW-2, ry+ROW_H-1);
    }

    g.ResetClip();

    // Scrollbar
    if (total > g_visibleRows) {
        int sbX = listX+listW-6, sbY=listY+1, sbH=listH-2;
        SolidBrush brSbBg(Color(30,255,255,255));
        g.FillRectangle(&brSbBg, sbX, sbY, 4, sbH);
        float thumbH = (float)sbH * g_visibleRows / total;
        float thumbY = sbY + (float)sbH * g_scrollTop / total;
        SolidBrush brThumb(C_MUTED);
        g.FillRectangle(&brThumb, sbX, (int)thumbY, 4, (int)thumbH);
    }

    // Count label
    wchar_t cntBuf[64];
    swprintf_s(cntBuf, L"%d processes", total);
    g.DrawString(cntBuf, -1, &fSec,
        PointF(20.f, (float)(listY+listH+6)), &brMuted);

    // ---- Selected info box ----
    int infoY = listY+listH+22;
    SolidBrush brInfoBg(C_PANEL);
    Pen brInfoBorder(C_BORDER, 1.f);
    DrawRoundRect(g, &brInfoBorder, &brInfoBg, listX, infoY, listW, 48, 4);

    if (g_selected >= 0 && g_selected < total) {
        const ProcEntry& sel = g_procs[g_selected];
        Font fInfoName(&ffMono, 10.f, FontStyleBold, UnitPoint);
        SolidBrush brInfoName(C_ACCENT);
        g.DrawString(sel.name.c_str(), -1, &fInfoName,
            PointF((float)(listX+12), (float)(infoY+6)), &brInfoName);

        wchar_t infoBuf[64];
        swprintf_s(infoBuf, L"PID: %u", sel.pid);
        g.DrawString(infoBuf, -1, &fSec,
            PointF((float)(listX+12), (float)(infoY+28)), &brMuted);

        if (!sel.exePath.empty()) {
            std::wstring ep = sel.exePath;
            if (ep.size()>55) ep = L"..."+ep.substr(ep.size()-52);
            g.DrawString(ep.c_str(), -1, &fPath,
                PointF((float)(listX+100), (float)(infoY+28)),
                &brMuted);
        }
    } else {
        g.DrawString(L"No process selected", -1, &fSec,
            PointF((float)(listX+12), (float)(infoY+18)), &brMuted);
    }

    // ---- INJECT button ----
    int btnY = infoY+60;
    bool canInject = (g_selected>=0 && dllFound && !g_injected);
    Color btnBg  = canInject ? C_ACCENT  : Color(255,40,42,50);
    Color btnBrd = canInject ? C_ACCENT2 : C_BORDER;
    Color btnTxt = canInject ? Color(255,20,20,20) : C_MUTED;

    SolidBrush brBtn(btnBg);
    Pen penBtn(btnBrd, 1.5f);
    DrawRoundRect(g, &penBtn, &brBtn, listX, btnY, listW, 40, 5);

    Font fBtn(&ffMono, 12.f, FontStyleBold, UnitPoint);
    SolidBrush brBtnTxt(btnTxt);
    StringFormat sfCenter;
    sfCenter.SetAlignment(StringAlignmentCenter);
    sfCenter.SetLineAlignment(StringAlignmentCenter);
    const wchar_t* btnLabel = g_injected ? L"✓  INJECTED" : L"INJECT";
    g.DrawString(btnLabel, -1, &fBtn,
        RectF((float)listX,(float)btnY,(float)listW,40.f),
        &sfCenter, &brBtnTxt);

    // ---- Status bar ----
    int stY = btnY+50;
    SolidBrush brStBg(C_PANEL);
    g.FillRectangle(&brStBg, 0, stY, WW, WH-stY);
    Pen stTop(C_BORDER,1.f);
    g.DrawLine(&stTop, 0,stY,WW,stY);

    // Flash effect on status change
    DWORD now = ::GetTickCount();
    float flash = 1.f;
    if (now - g_flashTick < 600)
        flash = 0.5f + 0.5f*sinf((float)(now-g_flashTick)*0.01f);

    Color stCol = g_statusOk
        ? Color((BYTE)(flash*200),60,200,100)
        : Color((BYTE)(flash*220),220,60,60);
    SolidBrush brSt(stCol);
    g.DrawString(g_status, -1, &fSec,
        PointF(20.f,(float)(stY+10)), &brSt);

    // Bottom accent
    LinearGradientBrush botBar(Point(0,WH-2),Point(WW,WH-2),
        Color(180,240,140,0), Color(0,180,80,0));
    g.FillRectangle(&botBar, 0,WH-2,WW,2);

    ::BitBlt(hdc, 0,0,WW,WH, mem,0,0,SRCCOPY);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::EndPaint(hwnd, &ps);
}

// ----------------------------------------------------------------
// Hit test — which row did user click?
// ----------------------------------------------------------------
static int HitTestRow(int mx, int my)
{
    int listX=16, listY=92, listW=WW-32, listH=g_visibleRows*ROW_H;
    if (mx<listX||mx>listX+listW||my<listY||my>listY+listH) return -1;
    int row = (my-listY)/ROW_H;
    int idx = g_scrollTop+row;
    if (idx<0||idx>=(int)g_procs.size()) return -1;
    return idx;
}

static bool HitInjectBtn(int mx, int my)
{
    int listX=16,listY=92,listH=g_visibleRows*ROW_H,listW=WW-32;
    int infoY=listY+listH+22;
    int btnY=infoY+60;
    return mx>=listX&&mx<=listX+listW&&my>=btnY&&my<=btnY+40;
}

// ----------------------------------------------------------------
// WndProc
// ----------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (g_flashTick && ::GetTickCount()-g_flashTick < 800)
            ::InvalidateRect(hwnd,nullptr,FALSE);
        return 0;

    case WM_KEYDOWN:
        if (wp==VK_F5) {
            RefreshProcesses();
            wcscpy_s(g_status,L"Process list refreshed");
            g_statusOk=true; g_flashTick=::GetTickCount();
            ::InvalidateRect(hwnd,nullptr,FALSE);
        }
        if (wp==VK_UP && g_selected>0) {
            g_selected--;
            if (g_selected<g_scrollTop) g_scrollTop=g_selected;
            ::InvalidateRect(hwnd,nullptr,FALSE);
        }
        if (wp==VK_DOWN && g_selected<(int)g_procs.size()-1) {
            g_selected++;
            if (g_selected>=g_scrollTop+g_visibleRows)
                g_scrollTop=g_selected-g_visibleRows+1;
            ::InvalidateRect(hwnd,nullptr,FALSE);
        }
        if (wp==VK_RETURN) ::SendMessageW(hwnd,WM_LBUTTONUP,0,
            MAKELPARAM(WW/2, 92+g_visibleRows*ROW_H+22+60+20));
        return 0;

    case WM_LBUTTONDOWN: {
        int mx=LOWORD(lp), my=HIWORD(lp);
        // Drag titlebar
        if (my<64) {
            g_dragging=true; g_dragOff={mx,my};
            ::SetCapture(hwnd);
        }
        // Row select
        int row=HitTestRow(mx,my);
        if (row>=0) {
            g_selected=row;
            g_injected=false;
            ::InvalidateRect(hwnd,nullptr,FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_dragging) {
            RECT r; ::GetWindowRect(hwnd,&r);
            ::SetWindowPos(hwnd,nullptr,
                r.left+(short)LOWORD(lp)-g_dragOff.x,
                r.top +(short)HIWORD(lp)-g_dragOff.y,
                0,0,SWP_NOSIZE|SWP_NOZORDER);
        }
        return 0;

    case WM_LBUTTONUP: {
        g_dragging=false; ::ReleaseCapture();
        int mx=LOWORD(lp), my=HIWORD(lp);
        if (HitInjectBtn(mx,my) && g_selected>=0 &&
            g_selected<(int)g_procs.size())
        {
            std::wstring dll=GetDllPath();
            if (::GetFileAttributesW(dll.c_str())==INVALID_FILE_ATTRIBUTES){
                wcscpy_s(g_status,L"HookDLL.dll not found next to Injector.exe");
                g_statusOk=false;
            } else {
                DWORD pid=g_procs[g_selected].pid;
                if (DoInject(pid,dll)){
                    swprintf_s(g_status,L"Injected into %s (PID %u)",
                        g_procs[g_selected].name.c_str(), pid);
                    g_statusOk=true;
                    g_injected=true;
                } else {
                    wcscpy_s(g_status,L"Injection failed — run as Administrator");
                    g_statusOk=false;
                }
            }
            g_flashTick=::GetTickCount();
            ::InvalidateRect(hwnd,nullptr,FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta=GET_WHEEL_DELTA_WPARAM(wp);
        g_scrollTop -= delta/WHEEL_DELTA;
        int maxScroll=(int)g_procs.size()-g_visibleRows;
        if(g_scrollTop<0) g_scrollTop=0;
        if(g_scrollTop>maxScroll) g_scrollTop=maxScroll<0?0:maxScroll;
        ::InvalidateRect(hwnd,nullptr,FALSE);
        return 0;
    }

    case WM_RBUTTONUP:
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd,msg,wp,lp);
}

// ----------------------------------------------------------------
// WinMain
// ----------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdipToken,&gsi,nullptr);

    RefreshProcesses();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"RTSSInjector";
    wc.hCursor       = ::LoadCursorW(nullptr,IDC_ARROW);
    ::RegisterClassExW(&wc);

    g_hwnd = ::CreateWindowExW(
        WS_EX_LAYERED|WS_EX_APPWINDOW,
        wc.lpszClassName, L"RTSS Clone Injector",
        WS_POPUP|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,WW,WH,
        nullptr,nullptr,hInst,nullptr);

    ::SetLayeredWindowAttributes(g_hwnd,0,245,LWA_ALPHA);
    ::SetTimer(g_hwnd,1,16,nullptr); // 60fps repaint for flash

    ::ShowWindow(g_hwnd,SW_SHOW);
    ::UpdateWindow(g_hwnd);

    MSG msg{};
    while(::GetMessageW(&msg,nullptr,0,0)){
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdipToken);
    return 0;
}
