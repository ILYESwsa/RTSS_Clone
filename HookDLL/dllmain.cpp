// ============================================================
// HookDLL/dllmain.cpp
// Entry point. Two threads run inside the game:
//   1. HookWorker  — waits 2s then installs graphics hooks
//   2. StatsWorker — polls CPU/RAM/GPU every 500ms
// No separate SystemStats.exe needed.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdio>
#include <string>
#include "../Shared/SharedMemory.h"
#pragma comment(lib, "pdh.lib")

void InstallHooks();
void RemoveHooks();

// ---- CPU via PDH ----
struct CpuMon {
    PDH_HQUERY q=nullptr; PDH_HCOUNTER c=nullptr; bool ok=false;
    bool Init(){
        if(::PdhOpenQueryW(nullptr,0,&q)!=ERROR_SUCCESS) return false;
        if(::PdhAddEnglishCounterW(q,
            L"\\Processor(_Total)\\% Processor Time",0,&c)!=ERROR_SUCCESS) return false;
        ::PdhCollectQueryData(q); ok=true; return true;
    }
    float Get(){
        if(!ok) return 0.f;
        ::PdhCollectQueryData(q);
        PDH_FMT_COUNTERVALUE v{};
        ::PdhGetFormattedCounterValue(c,PDH_FMT_DOUBLE,nullptr,&v);
        return (float)v.doubleValue;
    }
    ~CpuMon(){if(q)::PdhCloseQuery(q);}
};

// ---- Intel iGPU via PDH GPU Engine ----
struct GpuMon {
    PDH_HQUERY q=nullptr; PDH_HCOUNTER c=nullptr; bool ok=false;
    bool Init(){
        if(::PdhOpenQueryW(nullptr,0,&q)!=ERROR_SUCCESS) return false;
        if(::PdhAddEnglishCounterW(q,
            L"\\GPU Engine(*)\\Utilization Percentage",0,&c)!=ERROR_SUCCESS){
            ::PdhCloseQuery(q);q=nullptr;return false;
        }
        ::PdhCollectQueryData(q); ok=true; return true;
    }
    float Get(){
        if(!ok) return -1.f;
        ::PdhCollectQueryData(q);
        DWORD sz=0,n=0;
        ::PdhGetFormattedCounterArrayW(c,PDH_FMT_DOUBLE,&sz,&n,nullptr);
        if(!sz||!n) return -1.f;
        auto* arr=(PDH_FMT_COUNTERVALUE_ITEM_W*)new BYTE[sz];
        float sum=0.f; DWORD cnt=0;
        if(::PdhGetFormattedCounterArrayW(c,PDH_FMT_DOUBLE,&sz,&n,arr)==ERROR_SUCCESS){
            for(DWORD i=0;i<n;i++){
                std::wstring nm(arr[i].szName);
                if(nm.find(L"engtype_3D")!=std::wstring::npos||
                   nm.find(L"engtype_Graphics")!=std::wstring::npos){
                    sum+=(float)arr[i].FmtValue.doubleValue; cnt++;
                }
            }
        }
        delete[](BYTE*)arr;
        if(!cnt) return -1.f;
        float avg=sum/(float)cnt;
        return avg>100.f?100.f:avg;
    }
    ~GpuMon(){if(q)::PdhCloseQuery(q);}
};

// ---- RAM ----
static void GetRam(float& used, float& total){
    MEMORYSTATUSEX ms{sizeof(ms)};
    ::GlobalMemoryStatusEx(&ms);
    total=(float)ms.ullTotalPhys/1048576.f;
    used =total-(float)ms.ullAvailPhys/1048576.f;
}

// ----------------------------------------------------------------
// Globals shared between threads
// ----------------------------------------------------------------
static volatile bool  g_running   = true;
static SharedMemHandle g_sharedMem;

// ----------------------------------------------------------------
// StatsWorker — writes CPU/RAM/GPU into shared memory every 500ms
// ----------------------------------------------------------------
static DWORD WINAPI StatsWorker(LPVOID)
{
    // Wait until shared memory is valid
    while (g_running && !g_sharedMem.Valid())
        ::Sleep(100);

    CpuMon cpu; cpu.Init();
    GpuMon gpu; bool gpuOk = gpu.Init();

    while (g_running)
    {
        if (!g_sharedMem.Valid()) { ::Sleep(500); continue; }
        RtssStats* s = g_sharedMem.Data();
        if (!s || s->version != RTSS_SHARED_VERSION) { ::Sleep(500); continue; }

        s->cpuUsagePercent = cpu.Get();

        float ru=0,rt=0; GetRam(ru,rt);
        s->ramUsedMB  = ru;
        s->ramTotalMB = rt;

        if (gpuOk){
            s->gpuUsagePercent = gpu.Get();
            s->gpuMemUsedMB    = ru;
        } else {
            s->gpuUsagePercent = -1.f;
            s->gpuMemUsedMB    = -1.f;
        }
        s->gpuTempC = -1.f;

        ::Sleep(500);
    }
    return 0;
}

// ----------------------------------------------------------------
// HookWorker — creates shared mem, starts stats, installs hooks
// ----------------------------------------------------------------
static DWORD WINAPI HookWorker(LPVOID)
{
    // Give the game time to fully initialize its D3D/GL context
    ::Sleep(2000);

    // Create shared memory first
    if (g_sharedMem.CreateMapping()) {
        RtssStats* s = g_sharedMem.Data();
        s->overlayVisible  = TRUE;
        s->gpuUsagePercent = -1.f;
        s->gpuTempC        = -1.f;
        s->gpuMemUsedMB    = -1.f;
        strncpy_s(s->apiName, sizeof(s->apiName), "...", _TRUNCATE);
    }

    // Start stats thread
    ::CreateThread(nullptr, 0, StatsWorker, nullptr, 0, nullptr);

    // Install graphics hooks
    InstallHooks();
    return 0;
}

// ----------------------------------------------------------------
// DllMain
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
