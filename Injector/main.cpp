// ============================================================
// Injector/main.cpp
// Scans running processes, lets the user pick a target, then
// injects HookDLL.dll using the classic CreateRemoteThread +
// LoadLibraryW technique.
//
// Build: MSVC / CMake (see root CMakeLists.txt)
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>   // Process32First / Process32Next
#include <psapi.h>      // GetModuleFileNameExW

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

// ----------------------------------------------------------------
// ProcessEntry — simple snapshot of one running process
// ----------------------------------------------------------------
struct ProcessEntry
{
    DWORD  pid;
    std::wstring name;
};

// ----------------------------------------------------------------
// EnumerateProcesses — returns all currently running processes
// ----------------------------------------------------------------
static std::vector<ProcessEntry> EnumerateProcesses()
{
    std::vector<ProcessEntry> result;

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (::Process32FirstW(snap, &pe))
    {
        do {
            ProcessEntry e;
            e.pid  = pe.th32ProcessID;
            e.name = pe.szExeFile;
            result.push_back(e);
        } while (::Process32NextW(snap, &pe));
    }

    ::CloseHandle(snap);
    return result;
}

// ----------------------------------------------------------------
// GetDllPath — resolves absolute path to HookDLL.dll.
// Looks first next to the injector exe, then in cwd.
// ----------------------------------------------------------------
static std::wstring GetDllPath()
{
    wchar_t exePath[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Replace injector filename with DLL filename
    std::wstring path(exePath);
    auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        path = path.substr(0, slash + 1) + L"HookDLL.dll";
    else
        path = L"HookDLL.dll";

    // Verify the file exists at computed path
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        // Fall back to working directory
        wchar_t cwd[MAX_PATH]{};
        ::GetCurrentDirectoryW(MAX_PATH, cwd);
        path = std::wstring(cwd) + L"\\HookDLL.dll";
    }

    return path;
}

// ----------------------------------------------------------------
// InjectDll — classic LoadLibrary injection via CreateRemoteThread
//
// Steps:
//   1. OpenProcess with PROCESS_ALL_ACCESS
//   2. VirtualAllocEx a page in the target for the DLL path string
//   3. WriteProcessMemory the UTF-16 DLL path into that page
//   4. GetProcAddress(kernel32, "LoadLibraryW") — same in every process
//   5. CreateRemoteThread( target, LoadLibraryW, remotePath )
//   6. Wait for thread, clean up
// ----------------------------------------------------------------
static bool InjectDll(DWORD pid, const std::wstring& dllPath)
{
    // --- Step 1: open target process ---
    HANDLE hProc = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc)
    {
        wprintf(L"[!] OpenProcess(%u) failed: %lu\n", pid, ::GetLastError());
        return false;
    }

    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);

    // --- Step 2: allocate memory in the remote process ---
    LPVOID remoteMem = ::VirtualAllocEx(hProc, nullptr, pathBytes,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem)
    {
        wprintf(L"[!] VirtualAllocEx failed: %lu\n", ::GetLastError());
        ::CloseHandle(hProc);
        return false;
    }

    // --- Step 3: write DLL path into remote process memory ---
    SIZE_T written = 0;
    if (!::WriteProcessMemory(hProc, remoteMem, dllPath.c_str(), pathBytes, &written)
        || written != pathBytes)
    {
        wprintf(L"[!] WriteProcessMemory failed: %lu\n", ::GetLastError());
        ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProc);
        return false;
    }

    // --- Step 4: resolve LoadLibraryW address (same in every process on x64) ---
    HMODULE hKernel = ::GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE pfnLoadLib =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            ::GetProcAddress(hKernel, "LoadLibraryW"));

    if (!pfnLoadLib)
    {
        wprintf(L"[!] Could not resolve LoadLibraryW\n");
        ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProc);
        return false;
    }

    // --- Step 5: create remote thread that calls LoadLibraryW(dllPath) ---
    HANDLE hThread = ::CreateRemoteThread(
        hProc, nullptr, 0,
        pfnLoadLib,      // thread function = LoadLibraryW
        remoteMem,       // argument       = remote DLL path
        0, nullptr);

    if (!hThread)
    {
        wprintf(L"[!] CreateRemoteThread failed: %lu\n", ::GetLastError());
        ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProc);
        return false;
    }

    // --- Step 6: wait for DLL to finish loading, then clean up ---
    ::WaitForSingleObject(hThread, 8000);   // 8-second timeout

    DWORD exitCode = 0;
    ::GetExitCodeThread(hThread, &exitCode);

    ::CloseHandle(hThread);
    ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    ::CloseHandle(hProc);

    if (exitCode == 0)
    {
        wprintf(L"[!] LoadLibraryW returned NULL in remote process (DLL load failed)\n");
        return false;
    }

    return true;  // exitCode is the HMODULE of the loaded DLL (non-zero = success)
}

// ----------------------------------------------------------------
// main — console UI: list processes → pick one → inject
// ----------------------------------------------------------------
int wmain(int argc, wchar_t* argv[])
{
    wprintf(L"\n");
    wprintf(L"  ╔══════════════════════════════════════╗\n");
    wprintf(L"  ║   RTSS Clone — DLL Injector v1.0    ║\n");
    wprintf(L"  ╚══════════════════════════════════════╝\n\n");

    // Resolve DLL path early so we can bail if it's missing
    std::wstring dllPath = GetDllPath();
    if (::GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        wprintf(L"[!] HookDLL.dll not found.\n");
        wprintf(L"    Expected at: %ls\n", dllPath.c_str());
        wprintf(L"    Build the project first or place HookDLL.dll next to Injector.exe\n");
        return 1;
    }
    wprintf(L"[+] DLL: %ls\n\n", dllPath.c_str());

    auto procs = EnumerateProcesses();

    // Sort alphabetically for readability
    std::sort(procs.begin(), procs.end(),
        [](const ProcessEntry& a, const ProcessEntry& b)
        { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });

    // Print table
    wprintf(L"  %-6s  %-40s\n", L"PID", L"Process Name");
    wprintf(L"  %s  %s\n", L"------", L"----------------------------------------");
    for (size_t i = 0; i < procs.size(); ++i)
        wprintf(L"  %-6u  %ls\n", procs[i].pid, procs[i].name.c_str());

    wprintf(L"\nEnter target PID (or 0 to cancel): ");
    DWORD targetPid = 0;
    wscanf_s(L"%u", &targetPid);

    if (targetPid == 0)
    {
        wprintf(L"Cancelled.\n");
        return 0;
    }

    // Find PID in the list
    auto it = std::find_if(procs.begin(), procs.end(),
        [&](const ProcessEntry& e){ return e.pid == targetPid; });

    if (it == procs.end())
    {
        wprintf(L"[!] PID %u not found in process list.\n", targetPid);
        return 1;
    }

    wprintf(L"[*] Injecting into PID %u (%ls)...\n", it->pid, it->name.c_str());

    if (InjectDll(it->pid, dllPath))
        wprintf(L"[+] Injection successful! Overlay should appear in-game.\n");
    else
        wprintf(L"[-] Injection failed. Try running as Administrator.\n");

    return 0;
}
