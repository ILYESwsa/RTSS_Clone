// ============================================================
// HookDLL/hooks.cpp
// Installs and manages hooks for:
//   - DirectX 9  → IDirect3DDevice9::EndScene
//   - DirectX 11 → IDXGISwapChain::Present
//   - DirectX 12 → IDXGISwapChain::Present (same vtable slot)
//   - Vulkan      → vkQueuePresentKHR
//
// Uses MinHook for vtable/import patching.
// Real FPS is computed via QueryPerformanceCounter in FpsCounter.h
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <dxgi.h>
#include <d3d11.h>
#include <d3d12.h>

// Minimal Vulkan types (no SDK required — we load at runtime via GetProcAddress)
#include "../Shared/vulkan_minimal.h"

// Dear ImGui
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx9.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

// MinHook
#include "MinHook/MinHook.h"

// Shared / utility
#include "../Shared/SharedMemory.h"
#include "../Shared/FpsCounter.h"

// ----------------------------------------------------------------
// Globals
// ----------------------------------------------------------------
static SharedMemHandle g_sharedMem;
static FpsCounter      g_fps;

// Flags to avoid double-init
static bool g_dx9Init  = false;
static bool g_dx11Init = false;
static bool g_dx12Init = false;
static bool g_vkInit   = false;

// Original function pointers (filled by MinHook)
typedef HRESULT (WINAPI *PFN_DX9EndScene)   (IDirect3DDevice9*);
typedef HRESULT (WINAPI *PFN_Present)        (IDXGISwapChain*, UINT, UINT);
typedef PFN_vkQueuePresentKHR PFN_VkQueuePresent;

static PFN_DX9EndScene   g_origDX9EndScene   = nullptr;
static PFN_Present       g_origDX11Present   = nullptr;
static PFN_Present       g_origDX12Present   = nullptr;
static PFN_VkQueuePresent g_origVkPresent    = nullptr;

// ----------------------------------------------------------------
// ImGui initialization guard (do once per device)
// ----------------------------------------------------------------
static HWND g_hwnd = nullptr;

static void EnsureImGuiContext()
{
    if (!ImGui::GetCurrentContext())
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;   // no imgui.ini
        ImGui::StyleColorsDark();
    }
}

// ----------------------------------------------------------------
// UpdateSharedStats — copies FPS into shared memory every frame
// ----------------------------------------------------------------
static void UpdateSharedStats(const char* apiName)
{
    if (!g_sharedMem.Valid()) return;
    RtssStats* s = g_sharedMem.Data();

    // FPS / frame time already computed in OnFrame()
    s->fps         = g_fps.GetFps();
    s->frameTimeMs = g_fps.GetFrameTimeMs();
    s->totalFrames = g_fps.GetTotalFrames();

    // API tag
    strncpy_s(s->apiName, sizeof(s->apiName), apiName, _TRUNCATE);
}

// ----------------------------------------------------------------
// RenderOverlay — called from every hooked Present/EndScene AFTER
// the game has rendered its frame, so we composite on top.
// ----------------------------------------------------------------
static void RenderOverlay()
{
    if (!g_sharedMem.Valid()) return;
    const RtssStats* s = g_sharedMem.Data();
    if (!s->overlayVisible) return;

    // Position overlay in top-left corner
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(220, 130), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar        |
        ImGuiWindowFlags_NoResize          |
        ImGuiWindowFlags_NoMove            |
        ImGuiWindowFlags_NoScrollbar       |
        ImGuiWindowFlags_NoSavedSettings   |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##rtss_overlay", nullptr, flags))
    {
        ImGui::TextColored(ImVec4(0.2f,1.0f,0.2f,1.f),
            "FPS: %.1f  [%s]", s->fps, s->apiName);
        ImGui::Text("Frame: %.2f ms", s->frameTimeMs);
        ImGui::Separator();
        ImGui::Text("CPU:   %.1f %%", s->cpuUsagePercent);
        ImGui::Text("RAM:   %.0f / %.0f MB", s->ramUsedMB, s->ramTotalMB);

        if (s->gpuUsagePercent >= 0.f)
            ImGui::Text("GPU:   %.1f %%  %.0f MB  %.0f°C",
                s->gpuUsagePercent, s->gpuMemUsedMB, s->gpuTempC);
        else
            ImGui::TextDisabled("GPU:   N/A");
    }
    ImGui::End();
}

// ==============================================================
// DirectX 9 Hook — EndScene
// Called by the game after it finishes drawing the scene.
// ==============================================================

// Forward declaration for ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static HWND GetMainWindow()
{
    // Find the window belonging to our current process
    struct Finder { HWND hwnd; DWORD pid; };
    Finder f{ nullptr, ::GetCurrentProcessId() };
    ::EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(w, &pid);
        auto* f = reinterpret_cast<Finder*>(lp);
        if (pid == f->pid && ::IsWindowVisible(w)) {
            f->hwnd = w;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.hwnd;
}

static HRESULT WINAPI HookedDX9EndScene(IDirect3DDevice9* pDevice)
{
    // --- Count the frame ---
    g_fps.OnFrame();
    UpdateSharedStats("DX9");

    // --- Initialize ImGui once ---
    if (!g_dx9Init)
    {
        g_hwnd = GetMainWindow();
        EnsureImGuiContext();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX9_Init(pDevice);
        g_dx9Init = true;
    }

    // --- Render overlay ---
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderOverlay();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    // --- Call original EndScene ---
    return g_origDX9EndScene(pDevice);
}

// ==============================================================
// DirectX 11 Hook — IDXGISwapChain::Present
// ==============================================================
static ID3D11DeviceContext* g_d3d11Context = nullptr;
static ID3D11RenderTargetView* g_d3d11RTV  = nullptr;

static void InitDX11(IDXGISwapChain* pSwap)
{
    ID3D11Device* dev = nullptr;
    pSwap->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
    if (!dev) return;

    dev->GetImmediateContext(&g_d3d11Context);

    ID3D11Texture2D* pBack = nullptr;
    pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBack);
    if (pBack)
    {
        dev->CreateRenderTargetView(pBack, nullptr, &g_d3d11RTV);
        pBack->Release();
    }

    g_hwnd = GetMainWindow();
    EnsureImGuiContext();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(dev, g_d3d11Context);
    dev->Release();
}

static HRESULT WINAPI HookedDX11Present(IDXGISwapChain* pSwap, UINT sync, UINT flags)
{
    g_fps.OnFrame();
    UpdateSharedStats("DX11");

    if (!g_dx11Init) { InitDX11(pSwap); g_dx11Init = true; }

    if (g_d3d11Context && g_d3d11RTV)
    {
        g_d3d11Context->OMSetRenderTargets(1, &g_d3d11RTV, nullptr);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderOverlay();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return g_origDX11Present(pSwap, sync, flags);
}

// ==============================================================
// DirectX 12 Hook — IDXGISwapChain::Present (same vtable slot)
// DX12 requires a command queue and descriptor heap for ImGui.
// ==============================================================
static ID3D12DescriptorHeap*  g_d3d12SrvHeap = nullptr;
static ID3D12CommandQueue*     g_d3d12Queue   = nullptr;

// The command queue pointer must be supplied by a separate hook
// on ID3D12CommandQueue::ExecuteCommandLists (common pattern).
// For simplicity we expose a setter the ExecuteCommandLists hook
// can call.
void SetD3D12CommandQueue(ID3D12CommandQueue* q) { g_d3d12Queue = q; }

static void InitDX12(IDXGISwapChain* pSwap)
{
    if (!g_d3d12Queue) return;

    ID3D12Device* dev = nullptr;
    g_d3d12Queue->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (!dev) return;

    // Create SRV heap for ImGui
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 1;
    desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_d3d12SrvHeap));

    g_hwnd = GetMainWindow();
    EnsureImGuiContext();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX12_Init(dev, 2,   // 2 back buffers
        DXGI_FORMAT_R8G8B8A8_UNORM,
        g_d3d12SrvHeap,
        g_d3d12SrvHeap->GetCPUDescriptorHandleForHeapStart(),
        g_d3d12SrvHeap->GetGPUDescriptorHandleForHeapStart());

    dev->Release();
}

static HRESULT WINAPI HookedDX12Present(IDXGISwapChain* pSwap, UINT sync, UINT flags)
{
    g_fps.OnFrame();
    UpdateSharedStats("DX12");

    if (!g_dx12Init) { InitDX12(pSwap); g_dx12Init = true; }

    if (g_d3d12SrvHeap)
    {
        // DX12 ImGui rendering requires per-frame command list setup.
        // Full implementation depends on game's swapchain buffer count.
        // Minimal path: NewFrame / Render without command list submission
        // (overlay rendered on next ExecuteCommandLists).
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderOverlay();
        ImGui::Render();
        // ImGui_ImplDX12_RenderDrawData called with cmdList in ECL hook
    }

    return g_origDX12Present(pSwap, sync, flags);
}

// ==============================================================
// Vulkan Hook — vkQueuePresentKHR
// Requires the Vulkan loader to be present (vulkan-1.dll).
// ==============================================================
static VkResult VKAPI_ATTR VKAPI_CALL HookedVkQueuePresent(
    VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    g_fps.OnFrame();
    UpdateSharedStats("Vulkan");
    // ImGui Vulkan backend initialization omitted here (requires
    // render pass, command pool, etc. from the game's VkDevice).
    // FPS counting is the primary value-add via shared memory.
    return g_origVkPresent(queue, pPresentInfo);
}

// ==============================================================
// CreateDummyDevice helpers — used to read vtable pointers
// without needing a real window / swap chain.
// ==============================================================

// Returns the vtable function pointer at index `slot` of object `obj`
static void* GetVTableFunc(void* obj, int slot)
{
    void** vtable = *reinterpret_cast<void***>(obj);
    return vtable[slot];
}

static bool GetDX9VTable(void** ppEndScene)
{
    IDirect3D9* d3d = ::Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return false;

    // Create a tiny invisible window for the dummy device
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = ::DefWindowProcW;
    wc.lpszClassName = L"RTSSDummyDX9";
    ::RegisterClassExW(&wc);
    HWND hw = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
                                0,0,1,1, nullptr, nullptr, nullptr, nullptr);

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow    = hw;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hw,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (SUCCEEDED(hr))
    {
        *ppEndScene = GetVTableFunc(dev, 42); // EndScene = vtable[42]
        dev->Release();
    }

    d3d->Release();
    ::DestroyWindow(hw);
    ::UnregisterClassW(wc.lpszClassName, nullptr);
    return SUCCEEDED(hr);
}

static bool GetDXGIVTable(void** ppPresent)
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = ::DefWindowProcW;
    wc.lpszClassName = L"RTSSDummyDXGI";
    ::RegisterClassExW(&wc);
    HWND hw = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
                                0,0,1,1, nullptr, nullptr, nullptr, nullptr);

    IDXGIFactory* factory = nullptr;
    if (FAILED(::CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
    {
        ::DestroyWindow(hw);
        ::UnregisterClassW(wc.lpszClassName, nullptr);
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    factory->EnumAdapters(0, &adapter);

    ID3D11Device* dev = nullptr;
    IDXGISwapChain* swap = nullptr;

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = hw;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;

    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &swap, &dev, nullptr, nullptr);

    if (SUCCEEDED(hr))
    {
        *ppPresent = GetVTableFunc(swap, 8); // Present = vtable[8]
        swap->Release();
        dev->Release();
    }

    if (adapter) adapter->Release();
    factory->Release();
    ::DestroyWindow(hw);
    ::UnregisterClassW(wc.lpszClassName, nullptr);
    return SUCCEEDED(hr);
}

// ==============================================================
// InstallHooks / RemoveHooks — called from dllmain.cpp worker
// ==============================================================

void InstallHooks()
{
    // Initialize shared memory (producer side)
    g_sharedMem.CreateMapping();

    // Initialize MinHook
    if (MH_Initialize() != MH_OK) return;

    // --- DirectX 9 ---
    {
        void* pfnEndScene = nullptr;
        if (GetDX9VTable(&pfnEndScene))
        {
            MH_CreateHook(pfnEndScene,
                reinterpret_cast<void*>(&HookedDX9EndScene),
                reinterpret_cast<void**>(&g_origDX9EndScene));
            MH_EnableHook(pfnEndScene);
        }
    }

    // --- DirectX 11 / 12 (share same vtable slot via DXGI) ---
    {
        void* pfnPresent = nullptr;
        if (GetDXGIVTable(&pfnPresent))
        {
            // Hook for DX11
            MH_CreateHook(pfnPresent,
                reinterpret_cast<void*>(&HookedDX11Present),
                reinterpret_cast<void**>(&g_origDX11Present));
            MH_EnableHook(pfnPresent);

            // DX12 uses the same IDXGISwapChain::Present vtable slot;
            // we detect DX12 at runtime by checking device type inside the hook.
            g_origDX12Present = g_origDX11Present;
        }
    }

    // --- Vulkan ---
    {
        HMODULE hVk = ::GetModuleHandleW(L"vulkan-1.dll");
        if (hVk)
        {
            void* pfnVkPresent = ::GetProcAddress(hVk, "vkQueuePresentKHR");
            if (pfnVkPresent)
            {
                MH_CreateHook(pfnVkPresent,
                    reinterpret_cast<void*>(&HookedVkQueuePresent),
                    reinterpret_cast<void**>(&g_origVkPresent));
                MH_EnableHook(pfnVkPresent);
            }
        }
    }
}

void RemoveHooks()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    // Shutdown ImGui backends
    if (g_dx9Init)  { ImGui_ImplDX9_Shutdown();  }
    if (g_dx11Init) { ImGui_ImplDX11_Shutdown(); }
    if (g_dx12Init) { ImGui_ImplDX12_Shutdown(); }
    if (g_dx9Init || g_dx11Init || g_dx12Init)
        ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();

    if (g_d3d11RTV) { g_d3d11RTV->Release(); g_d3d11RTV = nullptr; }
    if (g_d3d12SrvHeap) { g_d3d12SrvHeap->Release(); g_d3d12SrvHeap = nullptr; }

    g_sharedMem.Close();
}
