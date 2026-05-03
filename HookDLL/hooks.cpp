// ============================================================
// HookDLL/hooks.cpp  —  RTSS Clone
// Primary:  wglSwapBuffers  (OpenGL — Psych Engine / OpenFL)
// Fallback: IDXGISwapChain::Present (DX11)
// Fallback: vkQueuePresentKHR (Vulkan)
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <dxgi.h>
#include <d3d11.h>
#include <d3d12.h>
#include <psapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_dx11.h"
#include "MinHook.h"

#include "../Shared/SharedMemory.h"
#include "../Shared/FpsCounter.h"
#include "../Shared/vulkan_minimal.h"

#pragma comment(lib, "psapi.lib")

// ----------------------------------------------------------------
// State
// ----------------------------------------------------------------
static SharedMemHandle  g_sharedMem;
static FpsCounter       g_fps;

typedef BOOL   (WINAPI* PFN_wglSwapBuffers)(HDC);
typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef VkResult(VKAPI_ATTR VKAPI_CALL* PFN_VkQueuePresent)(VkQueue, const VkPresentInfoKHR*);

static PFN_wglSwapBuffers      g_origSwapBuffers = nullptr;
static PFN_Present             g_origPresent     = nullptr;
static PFN_VkQueuePresent      g_origVkPresent   = nullptr;

static bool                    g_glInit   = false;
static bool                    g_dx11Init = false;
static HWND                    g_glHwnd   = nullptr;
static HWND                    g_dx11Hwnd = nullptr;

static ID3D11Device*           g_d3d11Dev = nullptr;
static ID3D11DeviceContext*    g_d3d11Ctx = nullptr;
static ID3D11RenderTargetView* g_d3d11RTV = nullptr;

// Frametime ring buffer
static float g_ftHistory[26] = {};
static int   g_ftHead        = 0;

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------
static HWND FindGameWindow()
{
    struct F { HWND h; DWORD pid; };
    F f{ nullptr, ::GetCurrentProcessId() };
    ::EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(w, &pid);
        auto* f = (F*)lp;
        if (pid == f->pid && ::IsWindowVisible(w)) { f->h = w; return FALSE; }
        return TRUE;
    }, (LPARAM)&f);
    return f.h;
}

static void* VTbl(void* obj, int slot)
{
    return (*reinterpret_cast<void***>(obj))[slot];
}

static void UpdateShared(const char* api)
{
    if (!g_sharedMem.Valid()) return;
    RtssStats* s = g_sharedMem.Data();
    s->fps         = g_fps.GetFps();
    s->frameTimeMs = g_fps.GetFrameTimeMs();
    s->totalFrames = g_fps.GetTotalFrames();
    strncpy_s(s->apiName, sizeof(s->apiName), api, _TRUNCATE);
}

// ----------------------------------------------------------------
// Ghost-transparent RTSS style
// Semi-transparent background, bright readable text
// ----------------------------------------------------------------
static void ApplyRTSSStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding     = ImVec2(0, 0);
    s.ItemSpacing       = ImVec2(4, 0);
    s.FramePadding      = ImVec2(4, 1);
    s.WindowRounding    = 0.f;
    s.FrameRounding     = 0.f;
    s.WindowBorderSize  = 0.f;   // no border — pure ghost
    s.ChildBorderSize   = 0.f;

    ImVec4* c = s.Colors;
    // Fully transparent window background — ghost effect
    c[ImGuiCol_WindowBg]          = ImVec4(0.f,  0.f,  0.f,  0.f);
    c[ImGuiCol_Border]            = ImVec4(0.f,  0.f,  0.f,  0.f);
    c[ImGuiCol_Text]              = ImVec4(1.f,  1.f,  1.f,  1.f);
    c[ImGuiCol_Separator]         = ImVec4(0.f,  0.f,  0.f,  0.f);
    c[ImGuiCol_Header]            = ImVec4(0.f,  0.f,  0.f,  0.f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(0.f,  0.f,  0.f,  0.f);
    c[ImGuiCol_HeaderActive]      = ImVec4(0.f,  0.f,  0.f,  0.f);
    c[ImGuiCol_PlotHistogram]     = ImVec4(0.91f,0.88f,0.25f,1.f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.f,0.3f,0.3f,1.f);
}

// ----------------------------------------------------------------
// DrawStatRow — label + bar + value
// Draws its own semi-transparent bg pill so it's readable on any game
// ----------------------------------------------------------------
static int g_rowIdx = 0;

static void DrawStatRow(
    const char* label,
    float       pct,       // 0-100 for bar width
    const char* valStr,
    ImVec4      barColor)
{
    const float totalW = 185.f;
    const float labelW = 78.f;
    const float valW   = 40.f;
    const float barW   = totalW - labelW - valW - 8.f;
    const float rowH   = 15.f;
    const float barH   = 4.f;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Semi-transparent row background — alternating shades for readability
    ImU32 rowBg = (g_rowIdx % 2 == 0)
        ? IM_COL32(0, 0, 0, 110)   // slightly darker
        : IM_COL32(0, 0, 0, 80);   // slightly lighter
    dl->AddRectFilled(pos, ImVec2(pos.x + totalW, pos.y + rowH), rowBg);
    g_rowIdx++;

    // Label — muted white
    dl->AddText(ImVec2(pos.x + 6.f, pos.y + 2.f),
        IM_COL32(180, 180, 180, 255), label);

    // Bar background
    float bx = pos.x + labelW;
    float by = pos.y + (rowH - barH) * 0.5f;
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + barW, by + barH),
        IM_COL32(255, 255, 255, 25));

    // Bar fill
    float fillPct = pct < 0.f ? 0.f : pct > 100.f ? 100.f : pct;
    if (fillPct > 0.f) {
        ImU32 col = ImGui::ColorConvertFloat4ToU32(barColor);
        dl->AddRectFilled(ImVec2(bx, by),
            ImVec2(bx + barW * fillPct / 100.f, by + barH), col);
    }

    // Value — bright white, right-aligned in its column
    ImVec2 valSz = ImGui::CalcTextSize(valStr);
    dl->AddText(
        ImVec2(pos.x + totalW - valW + (valW - valSz.x) - 4.f, pos.y + 2.f),
        IM_COL32(255, 255, 255, 255), valStr);

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + rowH));
    ImGui::Dummy(ImVec2(totalW, 0));
}

static void SectionHdr(const char* name, ImDrawList* dl)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + 185.f, p.y + 12.f), IM_COL32(0,0,0,130));
    dl->AddText(ImVec2(p.x + 6.f, p.y + 1.f), IM_COL32(120,120,120,255), name);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 12.f));
    ImGui::Dummy(ImVec2(185.f, 0));
}

// ----------------------------------------------------------------
// Main overlay render
// ----------------------------------------------------------------
static void RenderOverlay()
{
    if (!g_sharedMem.Valid()) return;
    const RtssStats* s = g_sharedMem.Data();
    if (!s || !s->overlayVisible) return;

    g_rowIdx = 0;

    ImGui::SetNextWindowPos(ImVec2(5, 5), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(190, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);  // fully transparent window

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar       |
        ImGuiWindowFlags_NoResize         |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoScrollbar      |
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoInputs         |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin("##rtss", nullptr, flags)) { ImGui::End(); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    char buf[32];

    // ---- FPS row — big, coloured, shadow for readability ----
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + 185.f, p.y + 20.f), IM_COL32(0,0,0,120));

        ImVec4 fpsCol = s->fps >= 60.f
            ? ImVec4(0.91f,0.88f,0.25f,1.f)   // yellow
            : s->fps >= 30.f
              ? ImVec4(0.88f,0.55f,0.15f,1.f)  // orange
              : ImVec4(0.88f,0.25f,0.25f,1.f); // red
        ImU32 fpsColU = ImGui::ColorConvertFloat4ToU32(fpsCol);

        snprintf(buf, sizeof(buf), "%.0f FPS", s->fps);
        // Text shadow for readability over any game background
        dl->AddText(ImVec2(p.x+7.f, p.y+4.f), IM_COL32(0,0,0,200), buf);
        dl->AddText(ImVec2(p.x+6.f, p.y+3.f), fpsColU, buf);

        snprintf(buf, sizeof(buf), "%.1fms", s->frameTimeMs);
        ImVec2 ftSz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(p.x + 185.f - ftSz.x - 6.f, p.y + 5.f),
            IM_COL32(150,150,150,220), buf);

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 20.f));
        ImGui::Dummy(ImVec2(185.f, 0));
    }

    // 1px gap
    ImGui::Dummy(ImVec2(185.f, 1.f));

    // ---- CPU ----
    SectionHdr("PROCESSOR", dl);
    snprintf(buf, sizeof(buf), "%.0f%%", s->cpuUsagePercent);
    DrawStatRow("CPU usage", s->cpuUsagePercent, buf,
        ImVec4(0.25f,0.85f,0.55f,0.9f));   // green

    // ---- RAM ----
    float ramPct = s->ramTotalMB > 0.f
        ? (s->ramUsedMB / s->ramTotalMB * 100.f) : 0.f;
    snprintf(buf, sizeof(buf), "%.1fG", s->ramUsedMB / 1024.f);
    DrawStatRow("RAM usage", ramPct, buf,
        ImVec4(0.25f,0.60f,0.95f,0.9f));   // blue

    ImGui::Dummy(ImVec2(185.f, 1.f));

    // ---- GPU ----
    SectionHdr("GPU - INTEL HD", dl);
    if (s->gpuUsagePercent >= 0.f) {
        snprintf(buf, sizeof(buf), "%.0f%%", s->gpuUsagePercent);
        DrawStatRow("GPU usage", s->gpuUsagePercent, buf,
            ImVec4(0.75f,0.30f,0.95f,0.9f)); // purple

        float memPct = s->ramTotalMB > 0.f
            ? (s->gpuMemUsedMB / s->ramTotalMB * 100.f) : 0.f;
        snprintf(buf, sizeof(buf), "%.0fM", s->gpuMemUsedMB);
        DrawStatRow("GPU mem", memPct, buf,
            ImVec4(0.75f,0.30f,0.95f,0.55f));
        // No temp row — Intel HD doesn't expose it without external tools
    } else {
        DrawStatRow("GPU usage", 0.f, "N/A",
            ImVec4(0.5f,0.5f,0.5f,0.5f));
    }

    ImGui::Dummy(ImVec2(185.f, 1.f));

    // ---- Frametime graph ----
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x+185.f, p.y+12.f), IM_COL32(0,0,0,130));
        dl->AddText(ImVec2(p.x+6.f, p.y+1.f), IM_COL32(120,120,120,255), "FRAMETIME");
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y+12.f));
        ImGui::Dummy(ImVec2(185.f,0));

        ImVec2 gp = ImGui::GetCursorScreenPos();
        const float gh  = 22.f;
        const float bw  = (185.f - 10.f) / 26.f;
        dl->AddRectFilled(gp, ImVec2(gp.x+185.f, gp.y+gh), IM_COL32(0,0,0,100));

        float mx = 1.f;
        for (int i=0;i<26;i++) if(g_ftHistory[i]>mx) mx=g_ftHistory[i];

        for (int i=0;i<26;i++) {
            float h = g_ftHistory[i]/mx*(gh-2.f);
            float x = gp.x+5.f+i*bw;
            float y = gp.y+gh-h;
            ImU32 col = g_ftHistory[i]>16.7f
                ? IM_COL32(220,70,70,200)
                : IM_COL32(232,220,64,180);
            dl->AddRectFilled(ImVec2(x,y), ImVec2(x+bw-1.f, gp.y+gh), col);
        }
        // 60fps reference line
        float ly = gp.y+gh-(16.7f/mx*(gh-2.f));
        if(ly>gp.y&&ly<gp.y+gh)
            dl->AddLine(ImVec2(gp.x+5.f,ly), ImVec2(gp.x+180.f,ly),
                IM_COL32(255,255,255,40));

        ImGui::SetCursorScreenPos(ImVec2(gp.x, gp.y+gh+2.f));
        ImGui::Dummy(ImVec2(185.f,0));
    }

    ImGui::End();
}

// ================================================================
// Vulkan hook
// ================================================================
static VkResult VKAPI_ATTR VKAPI_CALL HookedVkPresent(
    VkQueue q, const VkPresentInfoKHR* p)
{
    g_fps.OnFrame();
    UpdateShared("Vulkan");
    g_ftHistory[g_ftHead]=g_fps.GetFrameTimeMs();
    g_ftHead=(g_ftHead+1)%26;
    return g_origVkPresent(q,p);
}

// ================================================================
// OpenGL hook — wglSwapBuffers (Psych Engine primary)
// ================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static BOOL WINAPI HookedWglSwapBuffers(HDC hDC)
{
    g_fps.OnFrame();
    UpdateShared("OpenGL");
    g_ftHistory[g_ftHead]=g_fps.GetFrameTimeMs();
    g_ftHead=(g_ftHead+1)%26;

    if (!g_glInit) {
        g_glHwnd = ::WindowFromDC(hDC);
        if (!g_glHwnd) g_glHwnd = FindGameWindow();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ApplyRTSSStyle();
        ImGui_ImplWin32_Init(g_glHwnd);
        ImGui_ImplOpenGL3_Init("#version 130");
        g_glInit = true;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderOverlay();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return g_origSwapBuffers(hDC);
}

// ================================================================
// DX11 hook — IDXGISwapChain::Present (fallback)
// ================================================================
static void SetupDX11ImGui(IDXGISwapChain* pSwap)
{
    if (FAILED(pSwap->GetDevice(__uuidof(ID3D11Device),(void**)&g_d3d11Dev))) return;
    g_d3d11Dev->GetImmediateContext(&g_d3d11Ctx);
    ID3D11Texture2D* pBack=nullptr;
    if (SUCCEEDED(pSwap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&pBack))) {
        g_d3d11Dev->CreateRenderTargetView(pBack,nullptr,&g_d3d11RTV);
        pBack->Release();
    }
    g_dx11Hwnd=FindGameWindow();
    if (!g_dx11Hwnd) {
        DXGI_SWAP_CHAIN_DESC d{}; pSwap->GetDesc(&d);
        g_dx11Hwnd=d.OutputWindow;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename=nullptr;
    ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
    ApplyRTSSStyle();
    ImGui_ImplWin32_Init(g_dx11Hwnd);
    ImGui_ImplDX11_Init(g_d3d11Dev,g_d3d11Ctx);
    g_dx11Init=true;
}

static HRESULT WINAPI HookedDXGIPresent(IDXGISwapChain* pSwap, UINT sync, UINT flags)
{
    g_fps.OnFrame();
    UpdateShared("DX11");
    g_ftHistory[g_ftHead]=g_fps.GetFrameTimeMs();
    g_ftHead=(g_ftHead+1)%26;

    if (!g_dx11Init) SetupDX11ImGui(pSwap);
    if (g_dx11Init && g_d3d11Ctx && g_d3d11RTV) {
        g_d3d11Ctx->OMSetRenderTargets(1,&g_d3d11RTV,nullptr);
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
    g_sharedMem.OpenMapping(); // created by dllmain HookWorker
    if (MH_Initialize()!=MH_OK) return;

    // OpenGL primary
    {
        HMODULE hGL=::GetModuleHandleW(L"opengl32.dll");
        if (!hGL) hGL=::LoadLibraryW(L"opengl32.dll");
        if (hGL) {
            void* pfn=::GetProcAddress(hGL,"wglSwapBuffers");
            if (pfn&&MH_CreateHook(pfn,(void*)&HookedWglSwapBuffers,
                    (void**)&g_origSwapBuffers)==MH_OK)
                MH_EnableHook(pfn);
        }
    }

    // DX11 fallback
    if (::GetModuleHandleW(L"dxgi.dll")&&::GetModuleHandleW(L"d3d11.dll")) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=::DefWindowProcW;
        wc.lpszClassName=L"_rtss_dx11";
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
                    (void**)&g_origVkPresent)==MH_OK)
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

    if (g_glInit) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    } else if (g_dx11Init) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (g_d3d11RTV) { g_d3d11RTV->Release(); g_d3d11RTV=nullptr; }
    if (g_d3d11Ctx) { g_d3d11Ctx->Release(); g_d3d11Ctx=nullptr; }
    if (g_d3d11Dev) { g_d3d11Dev->Release(); g_d3d11Dev=nullptr; }

    g_sharedMem.Close();
}
