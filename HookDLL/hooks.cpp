// ============================================================
// HookDLL/hooks.cpp
// Auto-detects DX9 / DX11 / DX12 / Vulkan by checking which
// D3D modules are loaded in the game process, then hooks only
// the relevant Present/EndScene function.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <dxgi.h>
#include <d3d11.h>
#include <d3d12.h>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "MinHook.h"

#include "../Shared/SharedMemory.h"
#include "../Shared/FpsCounter.h"
#include "../Shared/vulkan_minimal.h"

// ----------------------------------------------------------------
// Globals
// ----------------------------------------------------------------
static SharedMemHandle g_sharedMem;
static FpsCounter      g_fps;

static bool g_dx9Init  = false;
static bool g_dx11Init = false;
static bool g_dx12Init = false;

typedef HRESULT(WINAPI* PFN_DX9EndScene)(IDirect3DDevice9*);
typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef VkResult(VKAPI_ATTR VKAPI_CALL* PFN_VkQueuePresent)(VkQueue, const VkPresentInfoKHR*);

static PFN_DX9EndScene    g_origDX9EndScene  = nullptr;
static PFN_Present        g_origDXGIPresent  = nullptr;
static PFN_VkQueuePresent g_origVkPresent    = nullptr;

static HWND               g_hwnd             = nullptr;
static ID3D11DeviceContext* g_d3d11Ctx        = nullptr;
static ID3D11RenderTargetView* g_d3d11RTV     = nullptr;
static ID3D12DescriptorHeap*  g_d3d12SrvHeap  = nullptr;
static ID3D12CommandQueue*    g_d3d12Queue     = nullptr;

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
        if (pid == f->pid && ::IsWindowVisible(w) && ::GetWindowTextLengthW(w) > 0) {
            f->h = w; return FALSE;
        }
        return TRUE;
    }, (LPARAM)&f);
    return f.h;
}

static void* VTableEntry(void* obj, int slot)
{
    return (*((void***)obj))[slot];
}

static void EnsureImGui()
{
    if (!ImGui::GetCurrentContext()) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        // Make overlay look clean
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 4.f;
        s.WindowBorderSize = 1.f;
    }
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
// Overlay render (called after game draws, before present)
// ----------------------------------------------------------------
static void RenderOverlay()
{
    if (!g_sharedMem.Valid()) return;
    const RtssStats* s = g_sharedMem.Data();
    if (!s || !s->overlayVisible) return;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(215, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##rtss", nullptr, flags))
    {
        // Header
        ImGui::TextColored(ImVec4(1.f, 0.55f, 0.f, 1.f),
            "RTSS Clone  [%s]", s->apiName[0] ? s->apiName : "...");
        ImGui::Separator();

        // FPS — large and green/yellow/red depending on value
        ImVec4 fpsCol = s->fps >= 60.f
            ? ImVec4(0.2f,1.f,0.4f,1.f)
            : s->fps >= 30.f
              ? ImVec4(1.f,0.9f,0.f,1.f)
              : ImVec4(1.f,0.3f,0.3f,1.f);
        ImGui::TextColored(fpsCol, "FPS    %6.1f", s->fps);
        ImGui::Text(        "Frame  %6.2f ms", s->frameTimeMs);
        ImGui::Separator();
        ImGui::Text(        "CPU    %6.1f %%", s->cpuUsagePercent);
        ImGui::Text(        "RAM    %6.0f MB", s->ramUsedMB);
        ImGui::Separator();

        if (s->gpuUsagePercent >= 0.f) {
            ImGui::Text("GPU    %6.1f %%", s->gpuUsagePercent);
            ImGui::Text("VRAM   %6.0f MB", s->gpuMemUsedMB);
            if (s->gpuTempC >= 0.f)
                ImGui::Text("Temp   %6.0f C",  s->gpuTempC);
        } else {
            ImGui::TextDisabled("GPU    N/A");
        }
    }
    ImGui::End();
}

// ================================================================
// DX9 Hook — IDirect3DDevice9::EndScene (vtable slot 42)
// ================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static HRESULT WINAPI HookedDX9EndScene(IDirect3DDevice9* pDev)
{
    g_fps.OnFrame();
    UpdateShared("DX9");

    if (!g_dx9Init) {
        g_hwnd = FindGameWindow();
        EnsureImGui();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX9_Init(pDev);
        g_dx9Init = true;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderOverlay();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    return g_origDX9EndScene(pDev);
}

// ================================================================
// DXGI Hook — IDXGISwapChain::Present (vtable slot 8)
// Handles both DX11 and DX12 — detect by querying device type.
// ================================================================
static void InitDX11(IDXGISwapChain* pSwap)
{
    ID3D11Device* dev = nullptr;
    if (FAILED(pSwap->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) return;

    dev->GetImmediateContext(&g_d3d11Ctx);

    ID3D11Texture2D* pBack = nullptr;
    if (SUCCEEDED(pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBack))) {
        dev->CreateRenderTargetView(pBack, nullptr, &g_d3d11RTV);
        pBack->Release();
    }

    g_hwnd = FindGameWindow();
    EnsureImGui();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(dev, g_d3d11Ctx);
    dev->Release();
    g_dx11Init = true;
}

static HRESULT WINAPI HookedDXGIPresent(IDXGISwapChain* pSwap, UINT sync, UINT flags)
{
    g_fps.OnFrame();

    // Detect DX11 vs DX12 on first call
    if (!g_dx11Init && !g_dx12Init) {
        ID3D11Device* dev11 = nullptr;
        if (SUCCEEDED(pSwap->GetDevice(__uuidof(ID3D11Device), (void**)&dev11)) && dev11) {
            dev11->Release();
            InitDX11(pSwap);
            UpdateShared("DX11");
        } else {
            // DX12 path — just count FPS for now, full DX12 ImGui needs cmd list
            UpdateShared("DX12");
            g_dx12Init = true;
        }
    }

    if (g_dx11Init) {
        UpdateShared("DX11");
        if (g_d3d11Ctx && g_d3d11RTV) {
            g_d3d11Ctx->OMSetRenderTargets(1, &g_d3d11RTV, nullptr);
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            RenderOverlay();
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    } else if (g_dx12Init) {
        UpdateShared("DX12");
        // DX12: FPS counter works, ImGui render requires per-frame cmd list
        // (game-specific setup needed for full DX12 overlay)
    }

    return g_origDXGIPresent(pSwap, sync, flags);
}

// ================================================================
// Vulkan Hook
// ================================================================
static VkResult VKAPI_ATTR VKAPI_CALL HookedVkPresent(
    VkQueue q, const VkPresentInfoKHR* p)
{
    g_fps.OnFrame();
    UpdateShared("Vulkan");
    return g_origVkPresent(q, p);
}

// ================================================================
// Dummy device creation to steal vtable pointers
// ================================================================
static bool GetDX9PresentPtr(void** ppEndScene)
{
    HMODULE hD3D9 = ::GetModuleHandleW(L"d3d9.dll");
    if (!hD3D9) return false; // DX9 not loaded by this game

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return false;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.lpszClassName = L"_rtss_dx9";
    ::RegisterClassExW(&wc);
    HWND hw = ::CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,0,0,2,2,nullptr,nullptr,nullptr,nullptr);

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = hw;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hw,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (SUCCEEDED(hr)) {
        *ppEndScene = VTableEntry(dev, 42);
        dev->Release();
    }
    d3d->Release();
    ::DestroyWindow(hw); ::UnregisterClassW(wc.lpszClassName,nullptr);
    return SUCCEEDED(hr);
}

static bool GetDXGIPresentPtr(void** ppPresent)
{
    HMODULE hDXGI = ::GetModuleHandleW(L"dxgi.dll");
    if (!hDXGI) return false; // neither DX11 nor DX12 loaded

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.lpszClassName = L"_rtss_dxgi";
    ::RegisterClassExW(&wc);
    HWND hw = ::CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,0,0,2,2,nullptr,nullptr,nullptr,nullptr);

    IDXGIFactory* fac = nullptr;
    ::CreateDXGIFactory(__uuidof(IDXGIFactory),(void**)&fac);
    if (!fac) { ::DestroyWindow(hw); ::UnregisterClassW(wc.lpszClassName,nullptr); return false; }

    IDXGIAdapter* ada = nullptr; fac->EnumAdapters(0,&ada);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount=1; scd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow=hw; scd.SampleDesc.Count=1; scd.Windowed=TRUE;

    ID3D11Device* dev=nullptr; IDXGISwapChain* swap=nullptr;
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(ada,D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,0,nullptr,0,D3D11_SDK_VERSION,&scd,&swap,&dev,nullptr,nullptr);

    if (SUCCEEDED(hr)) {
        *ppPresent = VTableEntry(swap, 8); // Present = slot 8
        swap->Release(); dev->Release();
    }
    if (ada) ada->Release();
    fac->Release();
    ::DestroyWindow(hw); ::UnregisterClassW(wc.lpszClassName,nullptr);
    return SUCCEEDED(hr);
}

// ================================================================
// InstallHooks / RemoveHooks
// ================================================================
void InstallHooks()
{
    // Create shared memory first so SystemStats can connect
    g_sharedMem.CreateMapping();

    if (MH_Initialize() != MH_OK) return;

    bool hooked = false;

    // --- Try DX9 ---
    {
        void* pfn = nullptr;
        if (GetDX9PresentPtr(&pfn) && pfn) {
            if (MH_CreateHook(pfn,(void*)&HookedDX9EndScene,(void**)&g_origDX9EndScene)==MH_OK)
                if (MH_EnableHook(pfn)==MH_OK) hooked = true;
        }
    }

    // --- Try DXGI (DX11/DX12) ---
    {
        void* pfn = nullptr;
        if (GetDXGIPresentPtr(&pfn) && pfn) {
            if (MH_CreateHook(pfn,(void*)&HookedDXGIPresent,(void**)&g_origDXGIPresent)==MH_OK)
                if (MH_EnableHook(pfn)==MH_OK) hooked = true;
        }
    }

    // --- Try Vulkan ---
    {
        HMODULE hVk = ::GetModuleHandleW(L"vulkan-1.dll");
        if (hVk) {
            void* pfn = ::GetProcAddress(hVk,"vkQueuePresentKHR");
            if (pfn)
                if (MH_CreateHook(pfn,(void*)&HookedVkPresent,(void**)&g_origVkPresent)==MH_OK)
                    MH_EnableHook(pfn);
        }
    }

    // Write a status message into shared mem so OverlayViewer shows something
    if (g_sharedMem.Valid()) {
        RtssStats* s = g_sharedMem.Data();
        s->overlayVisible = TRUE;
        if (!hooked)
            strncpy_s(s->apiName, sizeof(s->apiName), "HOOKED", _TRUNCATE);
    }
}

void RemoveHooks()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_dx9Init)  { ImGui_ImplDX9_Shutdown(); }
    if (g_dx11Init) { ImGui_ImplDX11_Shutdown(); }
    if (g_dx9Init || g_dx11Init) ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();

    if (g_d3d11RTV) { g_d3d11RTV->Release(); g_d3d11RTV = nullptr; }
    if (g_d3d11Ctx) { g_d3d11Ctx->Release(); g_d3d11Ctx = nullptr; }
    if (g_d3d12SrvHeap) { g_d3d12SrvHeap->Release(); g_d3d12SrvHeap = nullptr; }

    g_sharedMem.Close();
}
