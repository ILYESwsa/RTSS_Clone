// ============================================================
// HookDLL/dllmain.cpp
// Entry point. Spawns two worker threads:
//   1. HookWorker  — installs graphics hooks (after delay)
//   2. StatsWorker — polls CPU/RAM/GPU every 500ms in-process
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

// ----------------------------------------------------------------
// CPU monitor (PDH) — runs inside the game process
// ----------------------------------------------------------------
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

// ----------------------------------------------------------------
// GPU monitor (PDH GPU Engine) — Intel iGPU compatible
// ----------------------------------------------------------------
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

// ----------------------------------------------------------------
// RAM via GlobalMemoryStatusEx
// ----------------------------------------------------------------
static void GetRam(float& used, float& total){
    MEMORYSTATUSEX ms{sizeof(ms)};
    ::GlobalMemoryStatusEx(&ms);
    total=(float)ms.ullTotalPhys/1048576.f;
    used =total-(float)ms.ullAvailPhys/1048576.f;
}

// ----------------------------------------------------------------
// StatsWorker — runs inside the game process, writes shared mem
// ----------------------------------------------------------------
static volatile bool g_running = true;

static DWORD WINAPI StatsWorker(LPVOID pShm)
{
    SharedMemHandle* shm = reinterpret_cast<SharedMemHandle*>(pShm);

    CpuMon cpu; cpu.Init();
    GpuMon gpu; bool gpuOk = gpu.Init();

    while (g_running)
    {
        RtssStats* s = shm->Data();
        if (!s || s->version != RTSS_SHARED_VERSION) break;

        // CPU
        s->cpuUsagePercent = cpu.Get();

        // RAM
        float ru=0,rt=0; GetRam(ru,rt);
        s->ramUsedMB  = ru;
        s->ramTotalMB = rt;

        // GPU (Intel iGPU via PDH)
        if (gpuOk) {
            s->gpuUsagePercent = gpu.Get();
            s->gpuMemUsedMB    = ru;  // iGPU shares system RAM
        } else {
            s->gpuUsagePercent = -1.f;
            s->gpuMemUsedMB    = -1.f;
        }
        s->gpuTempC = -1.f; // not available on Intel HD without external tools

        ::Sleep(500);
    }
    return 0;
}

// ----------------------------------------------------------------
// HookWorker — installs graphics hooks after game D3D is ready
// ----------------------------------------------------------------
static SharedMemHandle g_sharedMem;

static DWORD WINAPI HookWorker(LPVOID)
{
    ::Sleep(1500); // wait for game D3D init

    // Create shared memory — hooks.cpp will use g_sharedMem externally
    // but we also pass it to StatsWorker so stats are written in-process
    g_sharedMem.CreateMapping();
    if (g_sharedMem.Valid()) {
        RtssStats* s = g_sharedMem.Data();
        s->overlayVisible     = TRUE;
        s->cpuUsagePercent    = 0.f;
        s->ramUsedMB          = 0.f;
        s->ramTotalMB         = 0.f;
        s->gpuUsagePercent    = -1.f;
        s->gpuTempC           = -1.f;
        s->gpuMemUsedMB       = -1.f;

        // Start stats polling thread immediately
        ::CreateThread(nullptr, 0, StatsWorker, &g_sharedMem, 0, nullptr);
    }

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
