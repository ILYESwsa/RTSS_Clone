// ============================================================
// HookDLL/dllmain.cpp
// Safe entry point — all work done in worker threads, never
// in DllMain itself to avoid loader lock deadlocks.
//
// Thread order:
//   1. HookWorker  — sleeps 2s, then installs hooks only
//   2. StatsWorker — started by HookWorker AFTER hooks are up,
//                    polls CPU/RAM/GPU every 500ms
//
// PDH is intentionally kept OUT of DllMain and any hook path.
// It runs only in StatsWorker which is a plain background thread.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <cstdio>
#include <string>
#include "../Shared/SharedMemory.h"

#pragma comment(lib, "pdh.lib")

// Declared in hooks.cpp
void InstallHooks();
void RemoveHooks();

// Shared memory is owned here, hooks.cpp opens it
SharedMemHandle g_sharedMem;
static volatile bool g_running = true;

// ----------------------------------------------------------------
// CPU via PDH — only used in StatsWorker thread
// ----------------------------------------------------------------
struct CpuMon {
    PDH_HQUERY q=nullptr; PDH_HCOUNTER c=nullptr; bool ok=false;
    bool Init() {
        if (::PdhOpenQueryW(nullptr,0,&q) != ERROR_SUCCESS) return false;
        if (::PdhAddEnglishCounterW(q,
            L"\\Processor(_Total)\\% Processor Time",0,&c) != ERROR_SUCCESS)
            return false;
        ::PdhCollectQueryData(q);
        ok = true; return true;
    }
    float Get() {
        if (!ok) return 0.f;
        ::PdhCollectQueryData(q);
        PDH_FMT_COUNTERVALUE v{};
        ::PdhGetFormattedCounterValue(c,PDH_FMT_DOUBLE,nullptr,&v);
        return (float)v.doubleValue;
    }
    ~CpuMon() { if (q) ::PdhCloseQuery(q); }
};

// ----------------------------------------------------------------
// GPU via PDH GPU Engine — Intel iGPU compatible, Win10 1803+
// ----------------------------------------------------------------
struct GpuMon {
    PDH_HQUERY q=nullptr; PDH_HCOUNTER c=nullptr; bool ok=false;
    bool Init() {
        if (::PdhOpenQueryW(nullptr,0,&q) != ERROR_SUCCESS) return false;
        if (::PdhAddEnglishCounterW(q,
            L"\\GPU Engine(*)\\Utilization Percentage",0,&c) != ERROR_SUCCESS) {
            ::PdhCloseQuery(q); q=nullptr; return false;
        }
        ::PdhCollectQueryData(q);
        ok = true; return true;
    }
    float Get() {
        if (!ok) return -1.f;
        ::PdhCollectQueryData(q);
        DWORD sz=0, n=0;
        ::PdhGetFormattedCounterArrayW(c,PDH_FMT_DOUBLE,&sz,&n,nullptr);
        if (!sz || !n) return -1.f;
        auto* arr = (PDH_FMT_COUNTERVALUE_ITEM_W*)new BYTE[sz];
        float sum=0.f; DWORD cnt=0;
        if (::PdhGetFormattedCounterArrayW(c,PDH_FMT_DOUBLE,&sz,&n,arr)==ERROR_SUCCESS) {
            for (DWORD i=0;i<n;i++) {
                std::wstring nm(arr[i].szName);
                if (nm.find(L"engtype_3D")!=std::wstring::npos ||
                    nm.find(L"engtype_Graphics")!=std::wstring::npos) {
                    sum += (float)arr[i].FmtValue.doubleValue; cnt++;
                }
            }
        }
        delete[](BYTE*)arr;
        if (!cnt) return -1.f;
        float avg = sum/(float)cnt;
        return avg>100.f ? 100.f : avg;
    }
    ~GpuMon() { if (q) ::PdhCloseQuery(q); }
};

// ----------------------------------------------------------------
// RAM
// ----------------------------------------------------------------
static void GetRam(float& used, float& total) {
    MEMORYSTATUSEX ms{sizeof(ms)};
    ::GlobalMemoryStatusEx(&ms);
    total = (float)ms.ullTotalPhys / 1048576.f;
    used  = total - (float)ms.ullAvailPhys / 1048576.f;
}

// ----------------------------------------------------------------
// StatsWorker — plain background thread, no hook involvement
// ----------------------------------------------------------------
static DWORD WINAPI StatsWorker(LPVOID)
{
    // Small delay to let hooks settle first
    ::Sleep(500);

    CpuMon cpu; cpu.Init();
    GpuMon gpu; bool gpuOk = gpu.Init();

    while (g_running) {
        if (g_sharedMem.Valid()) {
            RtssStats* s = g_sharedMem.Data();
            if (s && s->version == RTSS_SHARED_VERSION) {
                // CPU
                s->cpuUsagePercent = cpu.Get();

                // RAM
                float ru=0, rt=0; GetRam(ru, rt);
                s->ramUsedMB  = ru;
                s->ramTotalMB = rt;

                // GPU
                if (gpuOk) {
                    s->gpuUsagePercent = gpu.Get();
                    s->gpuMemUsedMB    = ru; // iGPU shares RAM
                } else {
                    s->gpuUsagePercent = -1.f;
                    s->gpuMemUsedMB    = -1.f;
                }
                s->gpuTempC = -1.f; // not available on Intel HD
            }
        }
        ::Sleep(500);
    }
    return 0;
}

// ----------------------------------------------------------------
// HookWorker — installs hooks, then starts stats thread
// ----------------------------------------------------------------
static DWORD WINAPI HookWorker(LPVOID)
{
    // Wait for game to fully initialize its graphics context
    ::Sleep(2000);

    // Create shared memory
    if (g_sharedMem.CreateMapping()) {
        RtssStats* s = g_sharedMem.Data();
        s->overlayVisible  = TRUE;
        s->gpuUsagePercent = -1.f;
        s->gpuTempC        = -1.f;
        s->gpuMemUsedMB    = -1.f;
        strncpy_s(s->apiName, sizeof(s->apiName), "...", _TRUNCATE);
    }

    // Install graphics hooks (defined in hooks.cpp)
    InstallHooks();

    // Start stats polling AFTER hooks are installed
    ::CreateThread(nullptr, 0, StatsWorker, nullptr, 0, nullptr);

    return 0;
}

// ----------------------------------------------------------------
// DllMain — minimal, no heavy work here
// ----------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        ::DisableThreadLibraryCalls(hInst);
        ::CreateThread(nullptr, 0, HookWorker, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        g_running = false;
        RemoveHooks();
        g_sharedMem.Close();
        break;
    }
    return TRUE;
}
