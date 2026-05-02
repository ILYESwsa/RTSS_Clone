// ============================================================
// HookDLL/dllmain.cpp
// Entry point for the injected DLL.
// Spawns a worker thread that installs graphics API hooks.
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Forward declarations
void InstallHooks();
void RemoveHooks();

// ----------------------------------------------------------------
// Worker thread — MinHook must be initialized from a thread,
// not from DllMain (to avoid loader-lock deadlocks).
// ----------------------------------------------------------------
static DWORD WINAPI HookWorker(LPVOID)
{
    // Small delay so the game's D3D device has time to initialize
    ::Sleep(500);
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
        RemoveHooks();
        break;
    }
    return TRUE;
}
