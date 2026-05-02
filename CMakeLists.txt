cmake_minimum_required(VERSION 3.20)
project(RTSS_Clone LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only.")
endif()

if(MSVC)
    add_compile_options(/W3 /WX- /MP)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---- FetchContent ----
include(FetchContent)

FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master)
FetchContent_MakeAvailable(minhook)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4)
FetchContent_MakeAvailable(imgui)

# ---- ImGui static lib ----
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC d3d9 d3d11 d3d12 dxgi dxguid)

# ---- Shared headers ----
add_library(shared_headers INTERFACE)
target_include_directories(shared_headers INTERFACE ${CMAKE_SOURCE_DIR}/Shared)

# ---- Injector.exe ----
add_executable(Injector Injector/main.cpp)
target_link_libraries(Injector PRIVATE shared_headers psapi)
set_target_properties(Injector PROPERTIES WIN32_EXECUTABLE FALSE OUTPUT_NAME "Injector")

# ---- HookDLL.dll ----
add_library(HookDLL SHARED HookDLL/dllmain.cpp HookDLL/hooks.cpp)
target_include_directories(HookDLL PRIVATE
    ${CMAKE_SOURCE_DIR}/Shared
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${minhook_SOURCE_DIR}/include)
target_link_libraries(HookDLL PRIVATE shared_headers imgui_lib minhook d3d9 d3d11 d3d12 dxgi dxguid)
set_target_properties(HookDLL PROPERTIES OUTPUT_NAME "HookDLL")

# ---- SystemStats.exe ----
add_executable(SystemStats SystemStats/SystemStats.cpp)
target_link_libraries(SystemStats PRIVATE shared_headers pdh wbemuuid ole32 oleaut32)
set_target_properties(SystemStats PROPERTIES WIN32_EXECUTABLE FALSE OUTPUT_NAME "SystemStats")

# ---- OverlayViewer.exe  (GUI Win32 window, GDI+) ----
add_executable(OverlayViewer WIN32 Overlay/overlay_viewer.cpp)
target_link_libraries(OverlayViewer PRIVATE shared_headers gdiplus msimg32)
set_target_properties(OverlayViewer PROPERTIES OUTPUT_NAME "OverlayViewer")

# ---- Install ----
install(TARGETS Injector HookDLL SystemStats OverlayViewer
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin)

message(STATUS "Configured: Injector | HookDLL | SystemStats | OverlayViewer")
