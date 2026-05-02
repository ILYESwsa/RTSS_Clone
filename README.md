# RTSS Clone — In-Game Overlay System

A Windows overlay system similar to MSI Afterburner + RivaTuner Statistics Server, featuring **real FPS capture**, DirectX/Vulkan hooking, and in-game ImGui rendering.

---

## Architecture

```
RTSS_Clone/
├── Injector/           → Injector.exe  (process scanner + DLL injector)
├── HookDLL/            → HookDLL.dll   (D3D9/11/12 + Vulkan hooks + ImGui overlay)
├── Overlay/            → OverlayViewer.exe (debug console viewer)
├── Shared/             → SharedMemory.h + FpsCounter.h (header-only)
├── SystemStats/        → SystemStats.exe (CPU/RAM/GPU monitor)
├── .github/workflows/  → GitHub Actions CI/CD
└── CMakeLists.txt
```

## How It Works

### Injection Flow
1. `Injector.exe` enumerates running processes
2. User selects a game by PID
3. Injector uses `CreateRemoteThread + LoadLibraryW` to load `HookDLL.dll` into the game process

### Hooking Flow
4. `HookDLL.dll` loads in the game's address space
5. MinHook patches the vtable of `IDXGISwapChain::Present` (DX11/12) and `IDirect3DDevice9::EndScene` (DX9)
6. Every frame, the hook fires → `FpsCounter` counts via `QueryPerformanceCounter`
7. Dear ImGui renders the overlay inside the game's render pipeline
8. Stats written to Windows named shared memory (`Local\RTSS_Clone_SharedMem`)

### System Monitoring
9. `SystemStats.exe` runs separately, reads shared memory, and writes CPU/RAM/GPU data at 2 Hz

---

## Building

### Prerequisites
- Windows 10/11
- Visual Studio 2019+ (MSVC) or Build Tools
- CMake 3.20+
- Git

### Build Commands
```cmd
git clone https://github.com/YOUR_USERNAME/RTSS_Clone.git
cd RTSS_Clone

:: Configure (downloads MinHook + ImGui automatically via FetchContent)
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release

:: Build
cmake --build build --parallel
```

Output binaries appear in `build/`:
- `Injector.exe`
- `HookDLL.dll`
- `SystemStats.exe`
- `OverlayViewer.exe`

---

## Usage

### Step 1 — Run as Administrator
Injection requires elevated privileges.

### Step 2 — Launch the game you want to overlay

### Step 3 — Inject
```cmd
cd build
Injector.exe
```
Select the game's PID from the list. The overlay will appear in the top-left of the game window.

### Step 4 — (Optional) Start system monitoring
```cmd
SystemStats.exe
```
This adds CPU/RAM/GPU stats to the overlay.

### Step 5 — (Debug) External viewer
```cmd
OverlayViewer.exe
```
Shows stats in a console window without injection.

---

## Download Pre-built Binaries (GitHub Actions)

1. Go to the **Actions** tab in this repository
2. Click the latest **Build RTSS Clone** workflow run
3. Scroll to the **Artifacts** section
4. Download **RTSS-Clone-Binaries-x64.zip**
5. Extract and run as Administrator

---

## Supported APIs

| API | Status | Method |
|-----|--------|--------|
| DirectX 9 | ✅ Full | `EndScene` vtable hook |
| DirectX 11 | ✅ Full | `IDXGISwapChain::Present` vtable hook |
| DirectX 12 | ✅ Partial | `IDXGISwapChain::Present` + ECL hook |
| Vulkan | ✅ FPS only | `vkQueuePresentKHR` import hook |

---

## Technical Notes

- **No fake FPS** — uses `QueryPerformanceCounter` on every real frame
- **No external overlay window** — rendered inside the game via ImGui
- **Shared memory IPC** — `Local\RTSS_Clone_SharedMem` (named file mapping)
- **Hooking library** — [MinHook](https://github.com/TsudaKageyu/minhook)
- **Rendering** — [Dear ImGui](https://github.com/ocornut/imgui) v1.90+

---

## Anti-Cheat Warning

DLL injection is detected by anti-cheat systems (BattlEye, EAC, Vanguard, etc.).  
**Do not use this with online games that have anti-cheat.** For educational/offline use only.

---

## License

MIT
