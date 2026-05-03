// ============================================================
// HookDLL/dllmain.cpp
// Entry point. Worker thread installs hooks after a short delay
// so the game's D3D device has time to fully initialize.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

void InstallHooks();
void RemoveHooks();

static DWORD WINAPI HookWorker(LPVOID)
{
    // Wait for game to finish its own D3D init
    ::Sleep(1500);
    InstallHooks();
    return 0;
}

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
