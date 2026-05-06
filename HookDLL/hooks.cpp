// ============================================================
// HookDLL/hooks.cpp  —  RTSS Clone
// Overlay styled exactly like RivaTuner Statistics Server:
// plain colored text, no boxes/bars, transparent background.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_dx11.h"
#include "MinHook.h"

#include "../Shared/SharedMemory.h"
#include "../Shared/FpsCounter.h"
#include "../Shared/vulkan_minimal.h"

#pragma comment(lib, "psapi.lib")

extern SharedMemHandle g_sharedMem;

// ----------------------------------------------------------------
// OverlayConfig — read from shared memory offset 4096
// Written by OverlayEditor.exe, read every frame here
// ----------------------------------------------------------------
#define CFG_MAGIC      0xCF6E0001u
#define CFG_OFFSET     4096

struct OverlayConfig {
    UINT32 magic, position;
    float  fontSize, lineHeight;
    UINT32 fontBold, fontIndex;
    UINT32 colLabel, colValue, colUnit, colFpsHi, colFpsMid, colFpsLo;
    float  bgAlpha;
    UINT32 showGpu, showCpu, showRam, showFps, showFrametime, showGraph;
    float  labelWidth, padding;
    wchar_t fontPath[260];  // custom TTF/OTF path
};

// Custom font path tracking — ImGui will load it
static wchar_t g_loadedFontPath[260] = {};

static void LoadCustomFont(const wchar_t* path) {
    if (path) wcsncpy_s(g_loadedFontPath, path, 259);
}

static OverlayConfig g_oc = {
    CFG_MAGIC, 0,
    13.f, 18.f, 0, 0,
    0xFF2080E0, 0xFFFFFFFF, 0xFF888888,
    0xFFFFFFFF, 0xFF20A0E0, 0xFF3030E0,
    0.f,
    1,1,1,1,1,1,
    42.f, 4.f
};

static void ReadConfig() {
    if (!g_sharedMem.Valid()) return;
    BYTE* base = (BYTE*)g_sharedMem.Data();
    OverlayConfig* pc = (OverlayConfig*)(base + CFG_OFFSET);
    if (pc->magic == CFG_MAGIC) {
        memcpy(&g_oc, pc, sizeof(OverlayConfig));
    }
}

static ImVec4 AbgrToImVec4(UINT32 abgr) {
    float r=((abgr>> 0)&0xFF)/255.f;
    float g2=((abgr>> 8)&0xFF)/255.f;
    float b=((abgr>>16)&0xFF)/255.f;
    float a=((abgr>>24)&0xFF)/255.f;
    return ImVec4(r,g2,b,a);
}



// ----------------------------------------------------------------
// State
// ----------------------------------------------------------------
static FpsCounter g_fps;

typedef BOOL    (WINAPI* PFN_wglSwapBuffers)(HDC);
typedef HRESULT (WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef VkResult(VKAPI_ATTR VKAPI_CALL* PFN_VkQueuePresent)(VkQueue, const VkPresentInfoKHR*);

static PFN_wglSwapBuffers g_origSwap    = nullptr;
static PFN_Present        g_origPresent = nullptr;
static PFN_VkQueuePresent g_origVk      = nullptr;

static bool g_glInit   = false;
static bool g_dx11Init = false;
static HWND g_hwnd     = nullptr;

static ID3D11Device*           g_dx11Dev = nullptr;
static ID3D11DeviceContext*    g_dx11Ctx = nullptr;
static ID3D11RenderTargetView* g_dx11RTV = nullptr;

// Frametime graph ring buffer (60 samples like real RTSS)
static const int FT_SAMPLES = 60;
static float g_ftHistory[FT_SAMPLES] = {};
static int   g_ftHead = 0;

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------
static HWND FindGameWindow()
{
    struct F { HWND h; DWORD pid; };
    F f{ nullptr, ::GetCurrentProcessId() };
    ::EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid=0; ::GetWindowThreadProcessId(w,&pid);
        auto* f=(F*)lp;
        if(pid==f->pid && ::IsWindowVisible(w)){f->h=w;return FALSE;}
        return TRUE;
    },(LPARAM)&f);
    return f.h;
}

static void* VTbl(void* obj, int slot)
{
    return (*reinterpret_cast<void***>(obj))[slot];
}

static void PushFt()
{
    g_ftHistory[g_ftHead] = g_fps.GetFrameTimeMs();
    g_ftHead = (g_ftHead+1) % FT_SAMPLES;
}

static void UpdateShared(const char* api)
{
    if (!g_sharedMem.Valid()) return;
    RtssStats* s = g_sharedMem.Data();
    if (!s) return;
    s->fps         = g_fps.GetFps();
    s->frameTimeMs = g_fps.GetFrameTimeMs();
    s->totalFrames = g_fps.GetTotalFrames();
    strncpy_s(s->apiName, sizeof(s->apiName), api, _TRUNCATE);
}

// ----------------------------------------------------------------
// RTSS style — completely transparent, text only
// ----------------------------------------------------------------
static void ApplyRTSSStyle()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding     = ImVec2(6, 4);
    st.ItemSpacing       = ImVec2(0, 1);
    st.FramePadding      = ImVec2(0, 0);
    st.WindowRounding    = 0.f;
    st.WindowBorderSize  = 0.f;
    st.ChildBorderSize   = 0.f;
    // All transparent
    for (int i=0;i<ImGuiCol_COUNT;i++)
        st.Colors[i] = ImVec4(0,0,0,0);
    st.Colors[ImGuiCol_Text] = ImVec4(1,1,1,1);
}

// ----------------------------------------------------------------
// DrawRTSSLine — renders one stat line exactly like RTSS:
//   [LABEL]  [VALUE][UNIT]  [VALUE2][UNIT2]  ...
//
// label      = orange label text e.g. "GPU"
// val        = main value e.g. "99"
// unit       = unit suffix e.g. "%" (smaller, dimmer)
// val2/unit2 = optional second column
// ----------------------------------------------------------------
ImVec4 COL_LABEL=AbgrToImVec4(g_oc.colLabel);
ImVec4 COL_VAL=AbgrToImVec4(g_oc.colValue);
ImVec4 COL_UNIT=AbgrToImVec4(g_oc.colUnit);
ImVec4 COL_DIM=AbgrToImVec4(g_oc.colUnit);

// Draw label + value + unit on current line, returns x after drawing
static float DrawLV(float x, float y, ImDrawList* dl,
    const char* val, ImVec4 valCol,
    const char* unit, float scale=1.f)
{
    // Value
    ImVec2 vsz = ImGui::CalcTextSize(val);
    dl->AddText(ImVec2(x,y), ImGui::ColorConvertFloat4ToU32(valCol), val);
    x += vsz.x + 2.f;

    // Unit (slightly smaller via opacity trick since we can't resize font inline)
    if (unit && unit[0]) {
        dl->AddText(ImVec2(x, y+2.f), ImGui::ColorConvertFloat4ToU32(COL_UNIT), unit);
        x += ImGui::CalcTextSize(unit).x + 8.f;
    }
    return x;
}

// ----------------------------------------------------------------
// RenderOverlay — pure RTSS text style
// ----------------------------------------------------------------
static void RenderOverlay()
{
    if (!g_sharedMem.Valid()) return;
    const RtssStats* s = g_sharedMem.Data();
    if (!s || !s->overlayVisible) return;
    bool shGpu=g_oc.showGpu!=0,shCpu=g_oc.showCpu!=0,shRam=g_oc.showRam!=0,shFps=g_oc.showFps!=0,shFt=g_oc.showFrametime!=0,shGr=g_oc.showGraph!=0;

    // Fully transparent, no decorations
    ReadConfig();
    const float posX[] = {4,0,0,4,0,0};   // L/C/R x computed below
    const float posY[] = {4,4,4,9999,9999,9999}; // T/B
    ImVec2 wpos(4.f,4.f);
    {
        ImVec2 disp=ImGui::GetIO().DisplaySize;
        float ox=g_oc.padding, px[]={ox, disp.x/2-95, disp.x-196-ox, ox, disp.x/2-95, disp.x-196-ox};
        float py[]={ox,ox,ox,disp.y-180,disp.y-180,disp.y-180};
        int p=(int)g_oc.position; if(p<0||p>5)p=0;
        wpos=ImVec2(px[p],py[p]);
    }
    ImGui::SetNextWindowPos(wpos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(g_oc.bgAlpha);

    ImGuiWindowFlags wf =
        ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove          | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoInputs        | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin("##rtss", nullptr, wf)) { ImGui::End(); return; }

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const float lh  = ImGui::GetTextLineHeight(); // line height
    const float lbl = 40.f; // label column width
    float y = ImGui::GetCursorScreenPos().y;
    float ox = ImGui::GetCursorScreenPos().x; // origin x

    char v1[32], v2[32], v3[32];

    // ---- GPU ----
    if (shGpu && s->gpuUsagePercent >= 0.f) {
        // Label
        dl->AddText(ImVec2(ox, y), ImGui::ColorConvertFloat4ToU32(COL_LABEL), "GPU");
        float x = ox + lbl;

        // GPU usage %
        snprintf(v1,sizeof(v1),"%.0f", s->gpuUsagePercent);
        x = DrawLV(x,y,dl,v1,COL_VAL,"%");

        // GPU mem (shared RAM for iGPU)
        snprintf(v2,sizeof(v2),"%.0f", s->gpuMemUsedMB);
        x = DrawLV(x,y,dl,v2,COL_VAL,"MB");

        y += lh + 1.f;
    }

    // ---- CPU ----
    if (shCpu) {
        dl->AddText(ImVec2(ox,y), ImGui::ColorConvertFloat4ToU32(COL_LABEL), "CPU");
        float x = ox + lbl;
        snprintf(v1,sizeof(v1),"%.0f", s->cpuUsagePercent);
        x = DrawLV(x,y,dl,v1,COL_VAL,"%");
        y += lh + 1.f;
    }

    // ---- RAM ----
    if (shRam) {
        dl->AddText(ImVec2(ox,y), ImGui::ColorConvertFloat4ToU32(COL_LABEL), "RAM");
        float x = ox + lbl;
        snprintf(v1,sizeof(v1),"%.0f", s->ramUsedMB);
        x = DrawLV(x,y,dl,v1,COL_VAL,"MB");
        y += lh + 1.f;
    }

    // ---- API + FPS + Frametime ----
    if (shFps) {
        // API label (D3D12 / OpenGL / DX11 etc)
        dl->AddText(ImVec2(ox,y), ImGui::ColorConvertFloat4ToU32(COL_LABEL),
            s->apiName[0] ? s->apiName : "...");
        float x = ox + lbl;

        // FPS — bigger feel via bright white
        snprintf(v1,sizeof(v1),"%.0f", s->fps);
        ImVec4 fpsCol = s->fps>=60.f?AbgrToImVec4(g_oc.colFpsHi):s->fps>=30.f?AbgrToImVec4(g_oc.colFpsMid):AbgrToImVec4(g_oc.colFpsLo);
        x = DrawLV(x,y,dl,v1,fpsCol,"FPS");

        // Frametime
        snprintf(v2,sizeof(v2),"%.1f", s->frameTimeMs);
        x = DrawLV(x,y,dl,v2,COL_DIM,"ms");
        y += lh + 1.f;
    }

    // ---- Frametime label ----
    if (shFt) {
        dl->AddText(ImVec2(ox,y), ImGui::ColorConvertFloat4ToU32(COL_DIM), "Frametime");
        y += lh + 2.f;
    }

    // ---- Frametime graph — thin line like real RTSS ----
    if (shGr) {
        const float gw = 252.f;
        const float gh = 28.f;

        // Dark strip behind graph
        dl->AddRectFilled(ImVec2(ox,y), ImVec2(ox+gw,y+gh),
            IM_COL32(0,0,0,90));

        // Find max frametime for scaling
        float mx = 0.f;
        for (int i=0;i<FT_SAMPLES;i++) if(g_ftHistory[i]>mx) mx=g_ftHistory[i];
        if (mx < 16.7f) mx = 16.7f; // floor at 60fps line

        // 60fps reference line
        float line60y = y + gh - (16.7f/mx)*(gh-4.f) - 2.f;
        dl->AddLine(ImVec2(ox,line60y), ImVec2(ox+gw,line60y),
            IM_COL32(80,80,80,120));

        // Draw frametime as a connected line graph (like RTSS)
        float bw = gw / (float)FT_SAMPLES;
        for (int i=1;i<FT_SAMPLES;i++) {
            int ia = (g_ftHead + i - 1) % FT_SAMPLES;
            int ib = (g_ftHead + i)     % FT_SAMPLES;
            float fa = g_ftHistory[ia];
            float fb = g_ftHistory[ib];
            float xa = ox + (i-1)*bw;
            float xb = ox + i*bw;
            float ya2 = y + gh - 2.f - (fa/mx)*(gh-4.f);
            float yb2 = y + gh - 2.f - (fb/mx)*(gh-4.f);
            // Clamp
            if(ya2<y)ya2=y; if(yb2<y)yb2=y;
            if(ya2>y+gh)ya2=y+gh; if(yb2>y+gh)yb2=y+gh;
            ImU32 col = (fb>33.3f)?IM_COL32(220,60,60,220):
                        (fb>16.7f)?IM_COL32(220,180,40,220):
                                   IM_COL32(200,200,200,200);
            dl->AddLine(ImVec2(xa,ya2), ImVec2(xb,yb2), col, 1.2f);
        }

        // Current frametime label top-right of graph
        snprintf(v1,sizeof(v1),"%.1f ms", s->frameTimeMs);
        ImVec2 ftsz=ImGui::CalcTextSize(v1);
        dl->AddText(ImVec2(ox+gw-ftsz.x-2, y+2),
            IM_COL32(160,160,160,200), v1);

        y += gh + 2.f;
    }

    // Advance ImGui cursor to match what we drew
    ImGui::SetCursorScreenPos(ImVec2(ox, y));
    ImGui::Dummy(ImVec2(260, 0));

    ImGui::End();
}

// ================================================================
// Vulkan hook
// ================================================================
static VkResult VKAPI_ATTR VKAPI_CALL HookedVkPresent(
    VkQueue q, const VkPresentInfoKHR* p)
{
    g_fps.OnFrame(); PushFt(); UpdateShared("Vulkan");
    return g_origVk(q,p);
}

// ================================================================
// OpenGL hook — wglSwapBuffers (Psych Engine primary)
// ================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static BOOL WINAPI HookedWglSwapBuffers(HDC hDC)
{
    g_fps.OnFrame(); PushFt(); UpdateShared("OpenGL");

    if (!g_glInit) {
        g_hwnd = ::WindowFromDC(hDC);
        if (!g_hwnd) g_hwnd = FindGameWindow();
        if (!g_hwnd) return g_origSwap(hDC); // retry next frame

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io2=ImGui::GetIO();
        io2.IniFilename=nullptr;
        io2.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
        // Load custom TTF if specified
        ReadConfig();
        if(g_oc.fontPath[0]){
    char fontBuf[260] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_oc.fontPath, -1, fontBuf, 260, nullptr, nullptr);

    float sz = g_oc.fontSize > 0 ? g_oc.fontSize : 13.f;

    if (!io2.Fonts->AddFontFromFileTTF(fontBuf, sz)) {
        io2.Fonts->AddFontDefault(); // fallback if failed
    }
} else {
    io2.Fonts->AddFontDefault();
}
        ApplyRTSSStyle();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplOpenGL3_Init("#version 130");
        g_glInit = true;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderOverlay();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return g_origSwap(hDC);
}

// ================================================================
// DX11 hook — IDXGISwapChain::Present (fallback)
// ================================================================
static void SetupDX11(IDXGISwapChain* pSwap)
{
    if (FAILED(pSwap->GetDevice(__uuidof(ID3D11Device),(void**)&g_dx11Dev))) return;
    g_dx11Dev->GetImmediateContext(&g_dx11Ctx);
    ID3D11Texture2D* pBack=nullptr;
    if (SUCCEEDED(pSwap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&pBack))) {
        g_dx11Dev->CreateRenderTargetView(pBack,nullptr,&g_dx11RTV);
        pBack->Release();
    }
    if (!g_dx11RTV) return;
    g_hwnd = FindGameWindow();
    if (!g_hwnd) { DXGI_SWAP_CHAIN_DESC d{}; pSwap->GetDesc(&d); g_hwnd=d.OutputWindow; }
    if (!g_hwnd) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename=nullptr;
    ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
    ApplyRTSSStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dx11Dev,g_dx11Ctx);
    g_dx11Init=true;
}

static HRESULT WINAPI HookedDXGIPresent(IDXGISwapChain* pSwap,UINT sync,UINT flags)
{
    g_fps.OnFrame(); PushFt(); UpdateShared("DX11");
    if (!g_dx11Init) SetupDX11(pSwap);
    if (g_dx11Init&&g_dx11Ctx&&g_dx11RTV) {
        g_dx11Ctx->OMSetRenderTargets(1,&g_dx11RTV,nullptr);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderOverlay();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    return g_origPresent(pSwap,sync,flags);
}

// ================================================================
// InstallHooks
// ================================================================
void InstallHooks()
{
    if (MH_Initialize()!=MH_OK) return;

    // OpenGL — Psych Engine
    {
        HMODULE hGL=::GetModuleHandleW(L"opengl32.dll");
        if (!hGL) hGL=::LoadLibraryW(L"opengl32.dll");
        if (hGL) {
            void* pfn=::GetProcAddress(hGL,"wglSwapBuffers");
            if (pfn&&MH_CreateHook(pfn,(void*)&HookedWglSwapBuffers,
                    (void**)&g_origSwap)==MH_OK)
                MH_EnableHook(pfn);
        }
    }

    // DX11 fallback
    if (::GetModuleHandleW(L"dxgi.dll")&&::GetModuleHandleW(L"d3d11.dll")) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=::DefWindowProcW; wc.lpszClassName=L"_rtss_d11";
        wc.hInstance=::GetModuleHandleW(nullptr);
        ::RegisterClassExW(&wc);
        HWND hw=::CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,
            0,0,2,2,nullptr,nullptr,wc.hInstance,nullptr);
        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferCount=1; scd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow=hw; scd.SampleDesc.Count=1; scd.Windowed=TRUE;
        ID3D11Device* dev=nullptr; IDXGISwapChain* swap=nullptr;
        HRESULT hr=::D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,
            nullptr,0,nullptr,0,D3D11_SDK_VERSION,&scd,&swap,&dev,nullptr,nullptr);
        if (FAILED(hr))
            hr=::D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,
                nullptr,0,nullptr,0,D3D11_SDK_VERSION,&scd,&swap,&dev,nullptr,nullptr);
        if (SUCCEEDED(hr)&&swap) {
            void* pfn=VTbl(swap,8);
            if (MH_CreateHook(pfn,(void*)&HookedDXGIPresent,(void**)&g_origPresent)==MH_OK)
                MH_EnableHook(pfn);
            swap->Release(); dev->Release();
        }
        ::DestroyWindow(hw);
        ::UnregisterClassW(wc.lpszClassName,wc.hInstance);
    }

    // Vulkan fallback
    {
        HMODULE hVk=::GetModuleHandleW(L"vulkan-1.dll");
        if (hVk) {
            void* pfn=::GetProcAddress(hVk,"vkQueuePresentKHR");
            if (pfn&&MH_CreateHook(pfn,(void*)&HookedVkPresent,
                    (void**)&g_origVk)==MH_OK)
                MH_EnableHook(pfn);
        }
    }
}

// ================================================================
// RemoveHooks
// ================================================================
void RemoveHooks()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    if (g_glInit)        { ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
    else if (g_dx11Init) { ImGui_ImplDX11_Shutdown();   ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
    if (g_dx11RTV) { g_dx11RTV->Release(); g_dx11RTV=nullptr; }
    if (g_dx11Ctx) { g_dx11Ctx->Release(); g_dx11Ctx=nullptr; }
    if (g_dx11Dev) { g_dx11Dev->Release(); g_dx11Dev=nullptr; }
}

// NOTE: OverlayConfig reading is appended here — see overlay_editor.cpp for struct def
