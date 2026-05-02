#pragma once
// ============================================================
// SharedMemory.h — Shared data between HookDLL and Overlay
// Uses Windows named shared memory for fast, lock-free reads.
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

// Name of the shared memory mapping object
#define RTSS_SHARED_MEM_NAME  L"Local\\RTSS_Clone_SharedMem"
#define RTSS_SHARED_MEM_SIZE  4096

// Version tag to detect stale data
#define RTSS_SHARED_VERSION   0x00010001u

// ----------------------------------------------------------------
// RtssStats — Written by HookDLL, read by Overlay/SystemStats
// All values are updated every frame from inside the hooked game.
// ----------------------------------------------------------------
#pragma pack(push, 1)
struct RtssStats
{
    uint32_t  version;          // Must equal RTSS_SHARED_VERSION
    uint32_t  structSize;       // sizeof(RtssStats) for forward compat

    // --- FPS / frame timing (written by hook) ---
    float     fps;              // Frames per second (rolling 1-second window)
    float     frameTimeMs;      // Last frame time in milliseconds
    uint64_t  totalFrames;      // Monotonically increasing frame counter

    // --- System stats (written by SystemStats module) ---
    float     cpuUsagePercent;  // 0–100
    float     ramUsedMB;        // Megabytes in use
    float     ramTotalMB;       // Total physical MB
    float     gpuUsagePercent;  // 0–100 (NVML/ADL), -1 if unavailable
    float     gpuTempC;         // °C, -1 if unavailable
    float     gpuMemUsedMB;     // MB, -1 if unavailable

    // --- Meta ---
    BOOL      overlayVisible;   // Toggle via hotkey
    char      apiName[16];      // "DX9", "DX11", "DX12", "Vulkan"
    uint32_t  reserved[8];
};
#pragma pack(pop)

// ----------------------------------------------------------------
// SharedMemHandle — RAII wrapper used by both producer and consumer
// ----------------------------------------------------------------
class SharedMemHandle
{
public:
    SharedMemHandle() : m_hMap(nullptr), m_pData(nullptr) {}
    ~SharedMemHandle() { Close(); }

    // Producer: create the mapping (called from HookDLL DllMain)
    bool CreateMapping()
    {
        m_hMap = ::CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, RTSS_SHARED_MEM_SIZE,
            RTSS_SHARED_MEM_NAME);
        if (!m_hMap) return false;

        m_pData = reinterpret_cast<RtssStats*>(
            ::MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0, 0, RTSS_SHARED_MEM_SIZE));
        if (!m_pData) { Close(); return false; }

        ZeroMemory(m_pData, RTSS_SHARED_MEM_SIZE);
        m_pData->version    = RTSS_SHARED_VERSION;
        m_pData->structSize = static_cast<uint32_t>(sizeof(RtssStats));
        m_pData->overlayVisible = TRUE;
        return true;
    }

    // Consumer: open existing mapping (called from Overlay / monitor)
    bool OpenMapping()
    {
        m_hMap = ::OpenFileMappingW(FILE_MAP_READ, FALSE, RTSS_SHARED_MEM_NAME);
        if (!m_hMap) return false;

        m_pData = reinterpret_cast<RtssStats*>(
            ::MapViewOfFile(m_hMap, FILE_MAP_READ, 0, 0, RTSS_SHARED_MEM_SIZE));
        if (!m_pData) { Close(); return false; }
        return true;
    }

    RtssStats* Data() { return m_pData; }
    const RtssStats* Data() const { return m_pData; }
    bool Valid() const { return m_pData != nullptr; }

    void Close()
    {
        if (m_pData) { ::UnmapViewOfFile(m_pData); m_pData = nullptr; }
        if (m_hMap)  { ::CloseHandle(m_hMap);       m_hMap  = nullptr; }
    }

private:
    HANDLE     m_hMap;
    RtssStats* m_pData;
};
