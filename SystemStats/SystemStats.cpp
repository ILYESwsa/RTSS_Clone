// ============================================================
// SystemStats/SystemStats.cpp
// Intel HD iGPU compatible via PDH GPU Engine counters.
// CPU via PDH, RAM via GlobalMemoryStatusEx,
// GPU temp via OpenHardwareMonitor WMI (optional).
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>
#include <comdef.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>

#include "../Shared/SharedMemory.h"

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// ----------------------------------------------------------------
// CPU via PDH
// ----------------------------------------------------------------
struct CpuMonitor {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool ready = false;
    bool Init() {
        if (::PdhOpenQueryW(nullptr,0,&query)!=ERROR_SUCCESS) return false;
        if (::PdhAddEnglishCounterW(query,
            L"\\Processor(_Total)\\% Processor Time",0,&counter)!=ERROR_SUCCESS) return false;
        ::PdhCollectQueryData(query);
        ready=true; return true;
    }
    float Query() {
        if(!ready) return -1.f;
        if(::PdhCollectQueryData(query)!=ERROR_SUCCESS) return -1.f;
        PDH_FMT_COUNTERVALUE v{};
        if(::PdhGetFormattedCounterValue(counter,PDH_FMT_DOUBLE,nullptr,&v)!=ERROR_SUCCESS) return -1.f;
        return (float)v.doubleValue;
    }
    ~CpuMonitor(){if(query)::PdhCloseQuery(query);}
};

// ----------------------------------------------------------------
// Intel iGPU via PDH GPU Engine counters (Win10 1803+)
// ----------------------------------------------------------------
struct GpuMonitor {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool ready = false;
    bool Init() {
        if(::PdhOpenQueryW(nullptr,0,&query)!=ERROR_SUCCESS) return false;
        LONG r=::PdhAddEnglishCounterW(query,
            L"\\GPU Engine(*)\\Utilization Percentage",0,&counter);
        if(r!=ERROR_SUCCESS){::PdhCloseQuery(query);query=nullptr;return false;}
        ::PdhCollectQueryData(query);
        ready=true; return true;
    }
    float Query() {
        if(!ready) return -1.f;
        if(::PdhCollectQueryData(query)!=ERROR_SUCCESS) return -1.f;
        DWORD bufSize=0,itemCount=0;
        ::PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE,&bufSize,&itemCount,nullptr);
        if(!bufSize||!itemCount) return -1.f;
        auto* items=(PDH_FMT_COUNTERVALUE_ITEM_W*)new BYTE[bufSize];
        float total=0.f; DWORD cnt=0;
        if(::PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE,&bufSize,&itemCount,items)==ERROR_SUCCESS) {
            for(DWORD i=0;i<itemCount;i++){
                std::wstring n(items[i].szName);
                if(n.find(L"engtype_3D")!=std::wstring::npos||
                   n.find(L"engtype_Graphics")!=std::wstring::npos){
                    total+=(float)items[i].FmtValue.doubleValue; cnt++;
                }
            }
        }
        delete[](BYTE*)items;
        return cnt==0?-1.f:std::min(100.f,total/(float)cnt);
    }
    ~GpuMonitor(){if(query)::PdhCloseQuery(query);}
};

// ----------------------------------------------------------------
// RAM
// ----------------------------------------------------------------
static void QueryRam(float& usedMB,float& totalMB){
    MEMORYSTATUSEX ms{sizeof(ms)};
    if(!::GlobalMemoryStatusEx(&ms)) return;
    totalMB=(float)ms.ullTotalPhys/(1024.f*1024.f);
    usedMB=totalMB-(float)ms.ullAvailPhys/(1024.f*1024.f);
}

// ----------------------------------------------------------------
// GPU temp via OpenHardwareMonitor WMI (optional)
// ----------------------------------------------------------------
static float QueryGpuTempWMI(){
    float temp=-1.f;
    IWbemLocator* pLoc=nullptr; IWbemServices* pSvc=nullptr;
    HRESULT hr=::CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    bool ci=SUCCEEDED(hr);
    if(FAILED(::CoCreateInstance(CLSID_WbemLocator,nullptr,
        CLSCTX_INPROC_SERVER,IID_IWbemLocator,(void**)&pLoc))) goto done;
    if(FAILED(pLoc->ConnectServer(_bstr_t(L"root\\OpenHardwareMonitor"),
        nullptr,nullptr,nullptr,0,nullptr,nullptr,&pSvc))) goto done;
    {
        IEnumWbemClassObject* pEnum=nullptr;
        if(SUCCEEDED(pSvc->ExecQuery(_bstr_t(L"WQL"),
            _bstr_t(L"SELECT Value FROM Sensor WHERE SensorType='Temperature' AND Name='GPU Core'"),
            WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&pEnum))&&pEnum){
            IWbemClassObject* pObj=nullptr; ULONG ret=0;
            if(pEnum->Next(WBEM_INFINITE,1,&pObj,&ret)==S_OK&&ret>0){
                VARIANT v; VariantInit(&v);
                if(SUCCEEDED(pObj->Get(L"Value",0,&v,nullptr,nullptr)))
                    {temp=(float)v.fltVal; ::VariantClear(&v);}
                pObj->Release();
            }
            pEnum->Release();
        }
    }
done:
    if(pSvc) pSvc->Release();
    if(pLoc) pLoc->Release();
    if(ci) ::CoUninitialize();
    return temp;
}

// ----------------------------------------------------------------
// NVML fallback for NVIDIA
// ----------------------------------------------------------------
typedef int(WINAPI*PFN_nvmlInit_v2)();
typedef int(WINAPI*PFN_nvmlShutdown)();
typedef int(WINAPI*PFN_nvmlDeviceGetHandleByIndex_v2)(unsigned,void**);
typedef int(WINAPI*PFN_nvmlDeviceGetUtilizationRates)(void*,void*);
typedef int(WINAPI*PFN_nvmlDeviceGetTemperature)(void*,unsigned,unsigned*);
typedef int(WINAPI*PFN_nvmlDeviceGetMemoryInfo)(void*,void*);
struct NvmlUtil{unsigned gpu,memory;};
struct NvmlMem{unsigned long long total,free,used;};
static HMODULE g_hNvml=nullptr; static void* g_nvDev=nullptr;
static PFN_nvmlDeviceGetUtilizationRates g_nvUtil=nullptr;
static PFN_nvmlDeviceGetTemperature g_nvTemp=nullptr;
static PFN_nvmlDeviceGetMemoryInfo g_nvMem=nullptr;
static bool InitNvml(){
    g_hNvml=::LoadLibraryW(L"nvml.dll");
    if(!g_hNvml) g_hNvml=::LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if(!g_hNvml) return false;
    auto fi=(PFN_nvmlInit_v2)::GetProcAddress(g_hNvml,"nvmlInit_v2");
    if(!fi||fi()!=0){::FreeLibrary(g_hNvml);g_hNvml=nullptr;return false;}
    auto fg=(PFN_nvmlDeviceGetHandleByIndex_v2)::GetProcAddress(g_hNvml,"nvmlDeviceGetHandleByIndex_v2");
    if(!fg||fg(0,&g_nvDev)!=0){::FreeLibrary(g_hNvml);g_hNvml=nullptr;return false;}
    g_nvUtil=(PFN_nvmlDeviceGetUtilizationRates)::GetProcAddress(g_hNvml,"nvmlDeviceGetUtilizationRates");
    g_nvTemp=(PFN_nvmlDeviceGetTemperature)::GetProcAddress(g_hNvml,"nvmlDeviceGetTemperature");
    g_nvMem=(PFN_nvmlDeviceGetMemoryInfo)::GetProcAddress(g_hNvml,"nvmlDeviceGetMemoryInfo");
    return true;
}
static bool QueryNvml(float&p,float&t,float&m){
    if(!g_nvDev) return false;
    NvmlUtil u{}; NvmlMem mm{}; unsigned tmp=0;
    bool ok=true;
    if(g_nvUtil) ok&=(g_nvUtil(g_nvDev,&u)==0);
    if(g_nvTemp) ok&=(g_nvTemp(g_nvDev,0,&tmp)==0);
    if(g_nvMem)  ok&=(g_nvMem(g_nvDev,&mm)==0);
    if(!ok) return false;
    p=(float)u.gpu; t=(float)tmp; m=(float)mm.used/(1024.f*1024.f);
    return true;
}

// ================================================================
// main
// ================================================================
int wmain(){
    wprintf(L"\n[SystemStats] Intel iGPU + PDH mode\n");
    SharedMemHandle shm;
    for(int i=0;i<30;i++){
        if(shm.OpenMapping()) break;
        ::Sleep(1000);
        wprintf(L"  waiting (%d/30)...\r",i+1);
    }
    if(!shm.Valid()){wprintf(L"\n[!] No shared memory. Inject HookDLL first.\n");return 1;}
    wprintf(L"[+] Connected to shared memory.\n");

    CpuMonitor cpu; cpu.Init();
    GpuMonitor gpu; bool gpuOk=gpu.Init();
    bool nvmlOk=InitNvml();
    wprintf(L"[+] GPU PDH: %s | NVML: %s\n",
        gpuOk?L"OK":L"FAIL", nvmlOk?L"OK":L"N/A");

    DWORD lastTempTick=0; float lastTemp=-1.f;

    while(true){
        RtssStats* s=shm.Data();
        if(!s||s->version!=RTSS_SHARED_VERSION) break;

        float cpuPct=cpu.Query();
        if(cpuPct>=0.f) s->cpuUsagePercent=cpuPct;

        float ramUsed=0.f,ramTotal=0.f;
        QueryRam(ramUsed,ramTotal);
        s->ramUsedMB=ramUsed; s->ramTotalMB=ramTotal;

        float gp=-1.f,gt=-1.f,gm=-1.f;
        if(nvmlOk&&QueryNvml(gp,gt,gm)){
            s->gpuUsagePercent=gp; s->gpuTempC=gt; s->gpuMemUsedMB=gm;
        } else if(gpuOk){
            s->gpuUsagePercent=gpu.Query();
            s->gpuMemUsedMB=ramUsed; // iGPU shares system RAM
            DWORD now=::GetTickCount();
            if(now-lastTempTick>2000){lastTemp=QueryGpuTempWMI();lastTempTick=now;}
            s->gpuTempC=lastTemp;
        } else {
            s->gpuUsagePercent=-1.f; s->gpuTempC=-1.f; s->gpuMemUsedMB=-1.f;
        }
        ::Sleep(500);
    }
    if(g_hNvml){
        auto fn=(PFN_nvmlShutdown)::GetProcAddress(g_hNvml,"nvmlShutdown");
        if(fn) fn(); ::FreeLibrary(g_hNvml);
    }
    return 0;
}
