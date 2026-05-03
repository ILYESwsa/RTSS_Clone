// ============================================================
// HookDLL/hooks.cpp
// Hooks wglSwapBuffers (OpenGL) for Psych Engine / OpenFL games.
// Also hooks IDXGISwapChain::Present as fallback for DX11 games.
//
// Psych Engine (OpenFL/Lime) uses OpenGL via wglSwapBuffers.
// We hook it in opengl32.dll which is always loaded by OpenFL.
// ImGui is rendered using the OpenGL3 backend.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <dxgi.h>
#include <d3d11.h>

// ImGui core
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_dx11.h"
#include "MinHook.h"

#include "../Shared/SharedMemory.h"
#include "../Shared/FpsCounter.h"
#include "../Shared/vulkan_minimal.h"

// ----------------------------------------------------------------
// Globals
// ----------------------------------------------------------------
static SharedMemHandle  g_sharedMem;
static FpsCounter       g_fps;

// OpenGL
typedef BOOL (WINAPI* PFN_wglSwapBuffers)(HDC);
static PFN_wglSwapBuffers g_origSwapBuffers = nullptr;
static bool               g_glInit          = false;
static HWND               g_glHwnd          = nullptr;
static HGLRC              g_glCtx           = nullptr; // ImGui's own GL context
static HGLRC              g_gameCtx         = nullptr; // game's GL context

// DX11 fallback
typedef HRESULT (WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present              g_origPresent  = nullptr;
static bool                     g_dx11Init     = false;
static ID3D11Device*            g_d3d11Dev     = nullptr;
static ID3D11DeviceContext*     g_d3d11Ctx     = nullptr;
static ID3D11RenderTargetView*  g_d3d11RTV     = nullptr;
static HWND                     g_dx11Hwnd     = nullptr;

// DX9 fallback
typedef HRESULT (WINAPI* PFN_DX9EndScene)(IDirect3DDevice9*);
static PFN_DX9EndScene g_origDX9 = nullptr;

// Vulkan fallback
typedef VkResult(VKAPI_ATTR VKAPI_CALL* PFN_VkQueuePresent)(VkQueue, const VkPresentInfoKHR*);
static PFN_VkQueuePresent g_origVkPresent   = nullptr;

// Forward declaration
static VkResult VKAPI_ATTR VKAPI_CALL HookedVkPresent(VkQueue, const VkPresentInfoKHR*);

// ----------------------------------------------------------------
// Shared helpers
// ----------------------------------------------------------------
static HWND FindGameWindow()
{
    struct F { HWND h; DWORD pid; };
    F f{ nullptr, ::GetCurrentProcessId() };
    ::EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid = 0; ::GetWindowThreadProcessId(w, &pid);
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

static void SetupImGuiStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding   = 4.f;
    st.WindowBorderSize = 1.f;
    st.Colors[ImGuiCol_WindowBg]      = ImVec4(0.04f,0.04f,0.06f,0.88f);
    st.Colors[ImGuiCol_Border]        = ImVec4(1.f,0.55f,0.f,0.7f);
    st.Colors[ImGuiCol_Separator]     = ImVec4(0.8f,0.4f,0.f,0.5f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.7f,0.35f,0.f,1.f);
}

static void RenderOverlay()
{
    if (!g_sharedMem.Valid()) return;
    const RtssStats* s = g_sharedMem.Data();
    if (!s || !s->overlayVisible) return;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar      |
        ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize|
        ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f,0.55f,0.f,0.9f));
    if (ImGui::Begin("##rtss_overlay", nullptr, flags))
    {
        ImGui::TextColored(ImVec4(1.f,0.6f,0.f,1.f),
            "RTSS Clone [%s]", s->apiName[0] ? s->apiName : "GL");
        ImGui::Separator();

        // FPS coloured green/yellow/red
        ImVec4 fc = s->fps >= 60.f ? ImVec4(0.2f,1.f,0.4f,1.f)
                  : s->fps >= 30.f ? ImVec4(1.f,0.85f,0.f,1.f)
                                   : ImVec4(1.f,0.3f,0.3f,1.f);
        ImGui::TextColored(fc, "FPS    %6.1f", s->fps);
        ImGui::Text(          "Frame  %6.2f ms", s->frameTimeMs);

        if (s->cpuUsagePercent > 0.f) {
            ImGui::Separator();
            ImGui::Text("CPU    %6.1f %%", s->cpuUsagePercent);
            ImGui::Text("RAM    %6.0f MB", s->ramUsedMB);
        }
        if (s->gpuUsagePercent >= 0.f) {
            ImGui::Separator();
            ImGui::Text("GPU    %6.1f %%", s->gpuUsagePercent);
            if (s->gpuTempC >= 0.f)
                ImGui::Text("Temp   %6.0f C", s->gpuTempC);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// ================================================================
// OpenGL Hook — wglSwapBuffers
// Called by Psych Engine / OpenFL every frame after rendering.
// We share the game's GL context so we can call GL draw commands.
// ================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static BOOL WINAPI HookedWglSwapBuffers(HDC hDC)
{
    // Count real frame
    g_fps.OnFrame();
    UpdateShared("OpenGL");

    if (!g_glInit)
    {
        g_glHwnd  = ::WindowFromDC(hDC);
        g_gameCtx = ::wglGetCurrentContext(); // save game's context

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SetupImGuiStyle();

        ImGui_ImplWin32_Init(g_glHwnd);
        // Use OpenGL3 backend — works with any GL version >= 3.0
        // Psych Engine uses OpenGL 3.3 core profile
        ImGui_ImplOpenGL3_Init("#version 130");

        g_glInit = true;
    }

    // Render ImGui using the game's active GL context
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderOverlay();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Call real wglSwapBuffers to present the frame
    return g_origSwapBuffers(hDC);
}

// ================================================================
// DX11 Hook — IDXGISwapChain::Present (fallback)
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
    g_dx11Hwnd = FindGameWindow();
    if (!g_dx11Hwnd) {
        DXGI_SWAP_CHAIN_DESC d{}; pSwap->GetDesc(&d);
        g_dx11Hwnd = d.OutputWindow;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    SetupImGuiStyle();
    ImGui_ImplWin32_Init(g_dx11Hwnd);
    ImGui_ImplDX11_Init(g_d3d11Dev, g_d3d11Ctx);
    g_dx11Init = true;
}

static HRESULT WINAPI HookedDXGIPresent(IDXGISwapChain* pSwap, UINT sync, UINT flags)
{
    g_fps.OnFrame();
    UpdateShared("DX11");
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
    return g_origPresent(pSwap, sync, flags);
}

// ================================================================
// InstallHooks
// ================================================================
void InstallHooks()
{
    g_sharedMem.CreateMapping();
    if (g_sharedMem.Valid()) {
        RtssStats* s = g_sharedMem.Data();
        s->overlayVisible = TRUE;
        strncpy_s(s->apiName,sizeof(s->apiName),"...",_TRUNCATE);
    }

    if (MH_Initialize() != MH_OK) return;

    // ---- OpenGL (Psych Engine / OpenFL) — PRIMARY ----
    // Hook wglSwapBuffers directly from opengl32.dll
    {
        HMODULE hGL = ::GetModuleHandleW(L"opengl32.dll");
        if (!hGL) hGL = ::LoadLibraryW(L"opengl32.dll"); // ensure loaded
        if (hGL) {
            void* pfn = ::GetProcAddress(hGL, "wglSwapBuffers");
            if (pfn) {
                if (MH_CreateHook(pfn,(void*)&HookedWglSwapBuffers,
                        (void**)&g_origSwapBuffers)==MH_OK)
                    MH_EnableHook(pfn);
            }
        }
    }

    // ---- DX11 / DXGI fallback ----
    if (::GetModuleHandleW(L"dxgi.dll") && ::GetModuleHandleW(L"d3d11.dll"))
    {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=::DefWindowProcW;
        wc.lpszClassName=L"_rtss_dx11";
        wc.hInstance=::GetModuleHandleW(nullptr);
        ::RegisterClassExW(&wc);
        HWND hw=::CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,0,0,2,2,
            nullptr,nullptr,wc.hInstance,nullptr);

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
        if (SUCCEEDED(hr) && swap) {
            void* pfn=VTbl(swap,8);
            if (MH_CreateHook(pfn,(void*)&HookedDXGIPresent,
                    (void**)&g_origPresent)==MH_OK)
                MH_EnableHook(pfn);
            swap->Release(); dev->Release();
        }
        ::DestroyWindow(hw);
        ::UnregisterClassW(wc.lpszClassName,wc.hInstance);
    }

    // ---- Vulkan fallback ----
    {
        HMODULE hVk=::GetModuleHandleW(L"vulkan-1.dll");
        if (hVk) {
            void* pfn=::GetProcAddress(hVk,"vkQueuePresentKHR");
            if (pfn && MH_CreateHook(pfn,(void*)HookedVkPresent,
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
