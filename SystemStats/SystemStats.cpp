// ============================================================
// SystemStats/SystemStats.cpp
// Polls CPU/RAM/GPU and writes to shared memory.
// Waits indefinitely for HookDLL to create the mapping.
// Automatically reconnects if the game is restarted.
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
#include <string>
#include "../Shared/SharedMemory.h"

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// ----------------------------------------------------------------
// CPU
// ----------------------------------------------------------------
struct CpuMonitor {
    PDH_HQUERY q=nullptr; PDH_HCOUNTER c=nullptr; bool ok=false;
    bool Init(){
        if(::PdhOpenQueryW(nullptr,0,&q)!=ERROR_SUCCESS) return false;
        if(::PdhAddEnglishCounterW(q,L"\\Processor(_Total)\\% Processor Time",0,&c)!=ERROR_SUCCESS) return false;
        ::PdhCollectQueryData(q); ok=true; return true;
    }
    float Get(){
        if(!ok) return 0.f;
        ::PdhCollectQueryData(q);
        PDH_FMT_COUNTERVALUE v{};
        ::PdhGetFormattedCounterValue(c,PDH_FMT_DOUBLE,nullptr,&v);
        return (float)v.doubleValue;
    }
    ~CpuMonitor(){if(q)::PdhCloseQuery(q);}
};

// ----------------------------------------------------------------
// Intel iGPU via PDH GPU Engine (Win10 1803+)
// ----------------------------------------------------------------
struct GpuMonitor {
    PDH_HQUERY q=nullptr; PDH_HCOUNTER c=nullptr; bool ok=false;
    bool Init(){
        if(::PdhOpenQueryW(nullptr,0,&q)!=ERROR_SUCCESS) return false;
        if(::PdhAddEnglishCounterW(q,L"\\GPU Engine(*)\\Utilization Percentage",0,&c)!=ERROR_SUCCESS){
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
        float avg=sum/(float)cnt; return cnt?(avg>100.f?100.f:avg):-1.f;
    }
    ~GpuMonitor(){if(q)::PdhCloseQuery(q);}
};

// ----------------------------------------------------------------
// RAM
// ----------------------------------------------------------------
static void GetRam(float& used, float& total){
    MEMORYSTATUSEX ms{sizeof(ms)};
    ::GlobalMemoryStatusEx(&ms);
    total=(float)ms.ullTotalPhys/1048576.f;
    used=total-(float)ms.ullAvailPhys/1048576.f;
}

// ----------------------------------------------------------------
// GPU temp via OpenHardwareMonitor WMI (optional)
// ----------------------------------------------------------------
static float GetGpuTempWMI(){
    float t=-1.f;
    IWbemLocator* pL=nullptr; IWbemServices* pS=nullptr;
    bool ci=SUCCEEDED(::CoInitializeEx(nullptr,COINIT_MULTITHREADED));
    if(FAILED(::CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_IWbemLocator,(void**)&pL))) goto done;
    if(FAILED(pL->ConnectServer(_bstr_t(L"root\\OpenHardwareMonitor"),nullptr,nullptr,nullptr,0,nullptr,nullptr,&pS))) goto done;
    {
        IEnumWbemClassObject* pE=nullptr;
        if(SUCCEEDED(pS->ExecQuery(_bstr_t(L"WQL"),
            _bstr_t(L"SELECT Value FROM Sensor WHERE SensorType='Temperature' AND Name='GPU Core'"),
            WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&pE))&&pE){
            IWbemClassObject* pO=nullptr; ULONG r=0;
            if(pE->Next(WBEM_INFINITE,1,&pO,&r)==S_OK&&r){
                VARIANT v; VariantInit(&v);
                if(SUCCEEDED(pO->Get(L"Value",0,&v,nullptr,nullptr))){t=(float)v.fltVal;::VariantClear(&v);}
                pO->Release();
            }
            pE->Release();
        }
    }
done:
    if(pS)pS->Release(); if(pL)pL->Release();
    if(ci)::CoUninitialize();
    return t;
}

// ================================================================
// main — waits forever for shared memory, never exits on its own
// ================================================================
int wmain(){
    // Set console title so user can see it's running
    ::SetConsoleTitleW(L"RTSS Clone - SystemStats (keep this open)");

    wprintf(L"\n  RTSS Clone SystemStats\n");
    wprintf(L"  Keep this window open while gaming.\n");
    wprintf(L"  It will auto-connect when you inject HookDLL.\n\n");

    CpuMonitor cpu; cpu.Init();
    GpuMonitor gpu; bool gpuOk=gpu.Init();
    wprintf(L"  CPU PDH: OK\n");
    wprintf(L"  GPU PDH: %s\n\n", gpuOk?L"OK (Intel iGPU compatible)":L"FAILED");

    SharedMemHandle shm;
    DWORD lastTempTick=0; float lastTemp=-1.f;

    while(true) // never exit — reconnect automatically
    {
        // --- Wait for HookDLL to create shared memory ---
        if(!shm.Valid()){
            wprintf(L"\r  Waiting for game injection...    ");
            while(!shm.OpenMapping()) ::Sleep(500);
            wprintf(L"\r  Connected! Monitoring...         \n");
        }

        RtssStats* s=shm.Data();
        if(!s||s->version!=RTSS_SHARED_VERSION){
            // Mapping gone (game closed) — reset and wait again
            shm.Close();
            wprintf(L"\r  Game closed, waiting...          \n");
            ::Sleep(1000);
            continue;
        }

        // CPU
        s->cpuUsagePercent = cpu.Get();

        // RAM
        float ru=0,rt=0; GetRam(ru,rt);
        s->ramUsedMB=ru; s->ramTotalMB=rt;

        // GPU
        float gp=gpu.Get();
        if(gp>=0.f){
            s->gpuUsagePercent=gp;
            s->gpuMemUsedMB=ru; // iGPU shares system RAM
            // Temp: poll WMI every 2s (slow)
            DWORD now=::GetTickCount();
            if(now-lastTempTick>2000){lastTemp=GetGpuTempWMI();lastTempTick=now;}
            s->gpuTempC=lastTemp;
        } else {
            s->gpuUsagePercent=-1.f;
            s->gpuTempC=-1.f;
            s->gpuMemUsedMB=-1.f;
        }

        wprintf(L"\r  CPU %4.0f%%  RAM %5.0fMB  GPU %4.0f%%  Temp %3.0fC   ",
            s->cpuUsagePercent, s->ramUsedMB,
            s->gpuUsagePercent, s->gpuTempC);

        ::Sleep(500);
    }
}
