// ============================================================
// SystemStats/SystemStats.cpp
// Standalone monitor process (or can be linked into HookDLL).
// Periodically writes CPU/RAM/GPU data into shared memory so
// the overlay can display it.
//
// CPU  → PDH (Performance Data Helper)
// RAM  → GlobalMemoryStatusEx
// GPU  → NVML (NVIDIA) or ADL (AMD) — graceful fallback
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdio>
#include <cstring>

#include "../Shared/SharedMemory.h"

#pragma comment(lib, "pdh.lib")

// ----------------------------------------------------------------
// NVML — dynamic load so we don't hard-depend on NVIDIA drivers
// ----------------------------------------------------------------
typedef int (WINAPI *PFN_nvmlInit)();
typedef int (WINAPI *PFN_nvmlShutdown)();
typedef int (WINAPI *PFN_nvmlDeviceGetHandleByIndex)(unsigned, void**);
typedef int (WINAPI *PFN_nvmlDeviceGetUtilizationRates)(void*, void*);
typedef int (WINAPI *PFN_nvmlDeviceGetTemperature)(void*, unsigned, unsigned*);
typedef int (WINAPI *PFN_nvmlDeviceGetMemoryInfo)(void*, void*);

struct NvmlUtilization { unsigned gpu, memory; };
struct NvmlMemoryInfo  { unsigned long long total, free, used; };

static HMODULE              g_hNvml        = nullptr;
static void*                g_nvDevice     = nullptr;
static PFN_nvmlDeviceGetUtilizationRates g_nvGetUtil = nullptr;
static PFN_nvmlDeviceGetTemperature      g_nvGetTemp = nullptr;
static PFN_nvmlDeviceGetMemoryInfo       g_nvGetMem  = nullptr;

static bool InitNvml()
{
    g_hNvml = ::LoadLibraryW(L"nvml.dll");
    if (!g_hNvml)
        g_hNvml = ::LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!g_hNvml) return false;

    auto fnInit = (PFN_nvmlInit)::GetProcAddress(g_hNvml, "nvmlInit_v2");
    if (!fnInit) fnInit = (PFN_nvmlInit)::GetProcAddress(g_hNvml, "nvmlInit");
    if (!fnInit || fnInit() != 0) { ::FreeLibrary(g_hNvml); g_hNvml = nullptr; return false; }

    auto fnGet = (PFN_nvmlDeviceGetHandleByIndex)
        ::GetProcAddress(g_hNvml, "nvmlDeviceGetHandleByIndex_v2");
    if (!fnGet) { ::FreeLibrary(g_hNvml); g_hNvml = nullptr; return false; }
    if (fnGet(0, &g_nvDevice) != 0) { ::FreeLibrary(g_hNvml); g_hNvml = nullptr; return false; }

    g_nvGetUtil = (PFN_nvmlDeviceGetUtilizationRates)
        ::GetProcAddress(g_hNvml, "nvmlDeviceGetUtilizationRates");
    g_nvGetTemp = (PFN_nvmlDeviceGetTemperature)
        ::GetProcAddress(g_hNvml, "nvmlDeviceGetTemperature");
    g_nvGetMem  = (PFN_nvmlDeviceGetMemoryInfo)
        ::GetProcAddress(g_hNvml, "nvmlDeviceGetMemoryInfo");

    return true;
}

static bool QueryNvml(float& gpuPct, float& gpuTempC, float& gpuMemMB)
{
    if (!g_nvDevice) return false;
    NvmlUtilization util{};
    NvmlMemoryInfo  mem{};
    unsigned temp = 0;

    bool ok = true;
    if (g_nvGetUtil) ok &= (g_nvGetUtil(g_nvDevice, &util) == 0);
    if (g_nvGetTemp) ok &= (g_nvGetTemp(g_nvDevice, 0 /*temperature_gpu*/, &temp) == 0);
    if (g_nvGetMem)  ok &= (g_nvGetMem (g_nvDevice, &mem) == 0);
    if (!ok) return false;

    gpuPct   = static_cast<float>(util.gpu);
    gpuTempC = static_cast<float>(temp);
    gpuMemMB = static_cast<float>(mem.used) / (1024.f * 1024.f);
    return true;
}

// ----------------------------------------------------------------
// CPU usage via PDH
// ----------------------------------------------------------------
struct CpuMonitor
{
    PDH_HQUERY   query  = nullptr;
    PDH_HCOUNTER counter= nullptr;
    bool         ready  = false;

    bool Init()
    {
        if (::PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return false;
        if (::PdhAddEnglishCounterW(query,
            L"\\Processor(_Total)\\% Processor Time",
            0, &counter) != ERROR_SUCCESS) return false;
        ::PdhCollectQueryData(query); // prime
        ready = true;
        return true;
    }

    float Query()
    {
        if (!ready) return -1.f;
        if (::PdhCollectQueryData(query) != ERROR_SUCCESS) return -1.f;
        PDH_FMT_COUNTERVALUE val{};
        if (::PdhGetFormattedCounterValue(counter,
            PDH_FMT_DOUBLE, nullptr, &val) != ERROR_SUCCESS) return -1.f;
        return static_cast<float>(val.doubleValue);
    }

    ~CpuMonitor()
    {
        if (query) ::PdhCloseQuery(query);
    }
};

// ----------------------------------------------------------------
// RAM usage via GlobalMemoryStatusEx
// ----------------------------------------------------------------
static void QueryRam(float& usedMB, float& totalMB)
{
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (!::GlobalMemoryStatusEx(&ms)) return;
    totalMB = static_cast<float>(ms.ullTotalPhys) / (1024.f * 1024.f);
    usedMB  = totalMB - static_cast<float>(ms.ullAvailPhys) / (1024.f * 1024.f);
}

// ================================================================
// main — runs as a separate process, writes to shared memory
// (or can be compiled as a function called from the DLL thread)
// ================================================================
int wmain()
{
    wprintf(L"[SystemStats] Starting...\n");

    SharedMemHandle shm;
    // Try to open existing shared memory created by HookDLL.
    // If not yet injected, wait and retry.
    for (int i = 0; i < 30; ++i)
    {
        if (shm.OpenMapping()) break;
        ::Sleep(1000);
    }
    if (!shm.Valid())
    {
        wprintf(L"[SystemStats] Shared memory not found. Is HookDLL injected?\n");
        return 1;
    }
    wprintf(L"[SystemStats] Shared memory opened.\n");

    CpuMonitor cpu;
    cpu.Init();

    bool nvmlOk = InitNvml();
    wprintf(L"[SystemStats] NVML: %s\n", nvmlOk ? L"OK" : L"unavailable");

    // Polling loop — update every 500 ms
    while (true)
    {
        RtssStats* s = shm.Data();
        if (!s || s->version != RTSS_SHARED_VERSION) break;

        // CPU
        float cpuPct = cpu.Query();
        if (cpuPct >= 0.f) s->cpuUsagePercent = cpuPct;

        // RAM
        float ramUsed = 0.f, ramTotal = 0.f;
        QueryRam(ramUsed, ramTotal);
        s->ramUsedMB  = ramUsed;
        s->ramTotalMB = ramTotal;

        // GPU
        float gpuPct = -1.f, gpuTemp = -1.f, gpuMem = -1.f;
        if (nvmlOk && QueryNvml(gpuPct, gpuTemp, gpuMem))
        {
            s->gpuUsagePercent = gpuPct;
            s->gpuTempC        = gpuTemp;
            s->gpuMemUsedMB    = gpuMem;
        }
        else
        {
            s->gpuUsagePercent = -1.f;
            s->gpuTempC        = -1.f;
            s->gpuMemUsedMB    = -1.f;
        }

        ::Sleep(500);
    }

    if (g_hNvml)
    {
        auto fnShutdown = (PFN_nvmlShutdown)::GetProcAddress(g_hNvml, "nvmlShutdown");
        if (fnShutdown) fnShutdown();
        ::FreeLibrary(g_hNvml);
    }

    return 0;
}
