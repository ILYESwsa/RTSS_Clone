// ============================================================
// Overlay/overlay_viewer.cpp
// An OPTIONAL external window that reads shared memory and
// displays stats — useful for debugging without injection.
// In normal use, the overlay renders inside the game via ImGui.
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>

#include "../Shared/SharedMemory.h"

// Simple console-based viewer — no D3D/ImGui required
int wmain()
{
    wprintf(L"╔═══════════════════════════════════════╗\n");
    wprintf(L"║  RTSS Clone — External Stats Viewer  ║\n");
    wprintf(L"╚═══════════════════════════════════════╝\n\n");
    wprintf(L"Waiting for HookDLL shared memory...\n");

    SharedMemHandle shm;
    for (int i = 0; i < 60; ++i)
    {
        if (shm.OpenMapping()) break;
        ::Sleep(1000);
        wprintf(L"  retrying (%d/60)...\r", i + 1);
    }

    if (!shm.Valid())
    {
        wprintf(L"\n[!] Could not open shared memory.\n");
        wprintf(L"    Ensure Injector.exe + HookDLL.dll are running.\n");
        return 1;
    }

    wprintf(L"[+] Connected!\n\n");

    // Poll and display loop
    HANDLE hConsole = ::GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi{};

    while (true)
    {
        const RtssStats* s = shm.Data();
        if (!s || s->version != RTSS_SHARED_VERSION)
        {
            wprintf(L"[!] Shared memory invalidated, exiting.\n");
            break;
        }

        // Move cursor to top-left for in-place update
        ::GetConsoleScreenBufferInfo(hConsole, &csbi);
        COORD pos{ 0, 3 };
        ::SetConsoleCursorPosition(hConsole, pos);

        wprintf(L"  API:       %-10hs\n",  s->apiName[0] ? s->apiName : "waiting...");
        wprintf(L"  FPS:       %6.1f fps\n",       s->fps);
        wprintf(L"  Frame:     %6.2f ms\n",         s->frameTimeMs);
        wprintf(L"  Frames:    %llu total\n",        s->totalFrames);
        wprintf(L"  ─────────────────────────\n");
        wprintf(L"  CPU:       %5.1f %%\n",          s->cpuUsagePercent);
        wprintf(L"  RAM:       %5.0f / %.0f MB\n",  s->ramUsedMB, s->ramTotalMB);

        if (s->gpuUsagePercent >= 0.f)
        {
            wprintf(L"  GPU:       %5.1f %%\n",         s->gpuUsagePercent);
            wprintf(L"  GPU Temp:  %5.0f °C\n",         s->gpuTempC);
            wprintf(L"  GPU Mem:   %5.0f MB\n",         s->gpuMemUsedMB);
        }
        else
        {
            wprintf(L"  GPU:       N/A (no NVML/ADL)\n");
            wprintf(L"  GPU Temp:  N/A              \n");
            wprintf(L"  GPU Mem:   N/A              \n");
        }

        ::Sleep(100);  // ~10 refreshes/sec in the console viewer
    }

    return 0;
}
