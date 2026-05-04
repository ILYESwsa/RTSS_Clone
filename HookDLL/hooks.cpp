// ============================================================
// HookDLL/hooks.cpp
// Graphics hooks — OpenGL (primary), DX11, Vulkan (fallbacks).
// g_sharedMem is declared in dllmain.cpp, opened here lazily.
// All ImGui init is guarded against null HWND.
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

// g_sharedMem is owned by dllmain.cpp
extern SharedMemHandle g_sharedMem;

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

static HWND                    g_hwnd     = nullptr;
static ID3D11Device*           g_dx11Dev  = nullptr;
static ID3D11DeviceContext*    g_dx11Ctx  = nullptr;
static ID3D11RenderTargetView* g_dx11RTV  = nullptr;

static float g_ftHistory[26] = {};
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
        if (pid==f->pid && ::IsWindowVisible(w)) { f->h=w; return FALSE; }
        return TRUE;
    }, (LPARAM)&f);
    return f.h;
}

static void* VTbl(void* obj, int slot)
{
    return (*reinterpret_cast<void***>(obj))[slot];
}

static void PushFt()
{
    g_ftHistory[g_ftHead] = g_fps.GetFrameTimeMs();
    g_ftHead = (g_ftHead+1) % 26;
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
// ImGui style — ghost transparent
// ----------------------------------------------------------------
static void ApplyStyle()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding    = ImVec2(0,0);
    st.ItemSpacing      = ImVec2(4,0);
    st.FramePadding     = ImVec2(4,1);
    st.WindowRounding   = 0.f;
    st.WindowBorderSize = 0.f;
    // Wipe all colours to transparent — we draw everything manually
    for (int i=0;i<ImGuiCol_COUNT;i++)
        ImGui::GetStyle().Colors[i] = ImVec4(0,0,0,0);
    ImGui::GetStyle().Colors[ImGuiCol_Text] = ImVec4(1,1,1,1);
}

// ----------------------------------------------------------------
// DrawStatRow
// ----------------------------------------------------------------
static int g_row = 0;
static void DrawStatRow(const char* lbl, float pct,
    const char* val, ImVec4 barCol)
{
    const float W=185,lW=78,vW=40,bW=W-lW-vW-8,rH=15,bH=4;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x+W,p.y+rH),
        (g_row++%2==0)?IM_COL32(0,0,0,110):IM_COL32(0,0,0,75));
    dl->AddText(ImVec2(p.x+6,p.y+2), IM_COL32(180,180,180,255), lbl);
    float bx=p.x+lW, by=p.y+(rH-bH)*0.5f;
    dl->AddRectFilled(ImVec2(bx,by),ImVec2(bx+bW,by+bH),IM_COL32(255,255,255,22));
    float fp=pct<0?0:pct>100?100:pct;
    if(fp>0) dl->AddRectFilled(ImVec2(bx,by),
        ImVec2(bx+bW*fp/100.f,by+bH),
        ImGui::ColorConvertFloat4ToU32(barCol));
    ImVec2 vs=ImGui::CalcTextSize(val);
    dl->AddText(ImVec2(p.x+W-vW+(vW-vs.x)-4,p.y+2),IM_COL32(255,255,255,255),val);
    ImGui::SetCursorScreenPos(ImVec2(p.x,p.y+rH));
    ImGui::Dummy(ImVec2(W,0));
}

static void SectionHdr(const char* name)
{
    ImVec2 p=ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p,ImVec2(p.x+185,p.y+12),IM_COL32(0,0,0,130));
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x+6,p.y+1),IM_COL32(120,120,120,255),name);
    ImGui::SetCursorScreenPos(ImVec2(p.x,p.y+12));
    ImGui::Dummy(ImVec2(185,0));
}

// ----------------------------------------------------------------
// RenderOverlay
// ----------------------------------------------------------------
static void RenderOverlay()
{
    if (!g_sharedMem.Valid()) return;
    const RtssStats* s = g_sharedMem.Data();
    if (!s || !s->overlayVisible) return;
    g_row = 0;

    ImGui::SetNextWindowPos(ImVec2(5,5), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(190,0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);

    ImGuiWindowFlags wf =
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
        ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_AlwaysAutoResize|
        ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin("##o",nullptr,wf)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    char buf[32];

    // FPS row
    {
        ImVec2 p=ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p,ImVec2(p.x+185,p.y+20),IM_COL32(0,0,0,120));
        ImVec4 fc = s->fps>=60?ImVec4(0.91f,0.88f,0.25f,1):
                    s->fps>=30?ImVec4(0.88f,0.55f,0.15f,1):
                               ImVec4(0.88f,0.25f,0.25f,1);
        snprintf(buf,sizeof(buf),"%.0f FPS",s->fps);
        dl->AddText(ImVec2(p.x+7,p.y+4),IM_COL32(0,0,0,180),buf);
        dl->AddText(ImVec2(p.x+6,p.y+3),ImGui::ColorConvertFloat4ToU32(fc),buf);
        snprintf(buf,sizeof(buf),"%.1fms",s->frameTimeMs);
        ImVec2 fsz=ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(p.x+185-fsz.x-6,p.y+5),IM_COL32(150,150,150,220),buf);
        ImGui::SetCursorScreenPos(ImVec2(p.x,p.y+20));
        ImGui::Dummy(ImVec2(185,0));
    }
    ImGui::Dummy(ImVec2(185,1));

    SectionHdr("PROCESSOR");
    snprintf(buf,sizeof(buf),"%.0f%%",s->cpuUsagePercent);
    DrawStatRow("CPU usage",s->cpuUsagePercent,buf,ImVec4(0.25f,0.85f,0.55f,0.9f));
    float rp=s->ramTotalMB>0?s->ramUsedMB/s->ramTotalMB*100.f:0.f;
    snprintf(buf,sizeof(buf),"%.1fG",s->ramUsedMB/1024.f);
    DrawStatRow("RAM usage",rp,buf,ImVec4(0.25f,0.60f,0.95f,0.9f));
    ImGui::Dummy(ImVec2(185,1));

    SectionHdr("GPU - INTEL HD");
    if(s->gpuUsagePercent>=0){
        snprintf(buf,sizeof(buf),"%.0f%%",s->gpuUsagePercent);
        DrawStatRow("GPU usage",s->gpuUsagePercent,buf,ImVec4(0.75f,0.30f,0.95f,0.9f));
        float mp=s->ramTotalMB>0?s->gpuMemUsedMB/s->ramTotalMB*100.f:0.f;
        snprintf(buf,sizeof(buf),"%.0fM",s->gpuMemUsedMB);
        DrawStatRow("GPU mem",mp,buf,ImVec4(0.75f,0.30f,0.95f,0.55f));
    } else {
        DrawStatRow("GPU usage",0,"N/A",ImVec4(0.4f,0.4f,0.4f,0.5f));
    }
    ImGui::Dummy(ImVec2(185,1));

    // Frametime graph
    {
        ImVec2 p=ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p,ImVec2(p.x+185,p.y+12),IM_COL32(0,0,0,130));
        dl->AddText(ImVec2(p.x+6,p.y+1),IM_COL32(120,120,120,255),"FRAMETIME");
        ImGui::SetCursorScreenPos(ImVec2(p.x,p.y+12));
        ImGui::Dummy(ImVec2(185,0));
        ImVec2 gp=ImGui::GetCursorScreenPos();
        const float gh=22,bw=(185.f-10)/26;
        dl->AddRectFilled(gp,ImVec2(gp.x+185,gp.y+gh),IM_COL32(0,0,0,100));
        float mx=1;
        for(int i=0;i<26;i++) if(g_ftHistory[i]>mx) mx=g_ftHistory[i];
        for(int i=0;i<26;i++){
            float h=g_ftHistory[i]/mx*(gh-2),x=gp.x+5+i*bw,y=gp.y+gh-h;
            dl->AddRectFilled(ImVec2(x,y),ImVec2(x+bw-1,gp.y+gh),
                g_ftHistory[i]>16.7f?IM_COL32(220,70,70,200):IM_COL32(232,220,64,180));
        }
        float ly=gp.y+gh-(16.7f/mx*(gh-2));
        if(ly>gp.y&&ly<gp.y+gh)
            dl->AddLine(ImVec2(gp.x+5,ly),ImVec2(gp.x+180,ly),IM_COL32(255,255,255,35));
        ImGui::SetCursorScreenPos(ImVec2(gp.x,gp.y+gh+2));
        ImGui::Dummy(ImVec2(185,0));
    }
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

    if (!g_glInit)
    {
        // Try multiple ways to get a valid HWND
        g_hwnd = ::WindowFromDC(hDC);
        if (!g_hwnd) g_hwnd = FindGameWindow();
        if (!g_hwnd) {
            // No window yet — skip, try again next frame
            return g_origSwap(hDC);
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ApplyStyle();
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
    ApplyStyle();
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
// InstallHooks — called from HookWorker after 2s delay
// ================================================================
void InstallHooks()
{
    if (MH_Initialize() != MH_OK) return;

    // OpenGL — Psych Engine
    {
        HMODULE hGL = ::GetModuleHandleW(L"opengl32.dll");
        if (!hGL) hGL = ::LoadLibraryW(L"opengl32.dll");
        if (hGL) {
            void* pfn = ::GetProcAddress(hGL,"wglSwapBuffers");
            if (pfn && MH_CreateHook(pfn,(void*)&HookedWglSwapBuffers,
                    (void**)&g_origSwap)==MH_OK)
                MH_EnableHook(pfn);
        }
    }

    // DX11 fallback
    if (::GetModuleHandleW(L"dxgi.dll") && ::GetModuleHandleW(L"d3d11.dll")) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=::DefWindowProcW;
        wc.lpszClassName=L"_rtss_d11";
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
            if (pfn && MH_CreateHook(pfn,(void*)&HookedVkPresent,
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
    if (g_glInit)   { ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
    else if (g_dx11Init) { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
    if (g_dx11RTV) { g_dx11RTV->Release(); g_dx11RTV=nullptr; }
    if (g_dx11Ctx) { g_dx11Ctx->Release(); g_dx11Ctx=nullptr; }
    if (g_dx11Dev) { g_dx11Dev->Release(); g_dx11Dev=nullptr; }
}
