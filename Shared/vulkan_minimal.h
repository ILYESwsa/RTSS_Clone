#pragma once
// ============================================================
// vulkan_minimal.h
// Minimal Vulkan type definitions so HookDLL can compile without
// the full Vulkan SDK. The hook uses GetProcAddress at runtime,
// so we only need the function signature and handle types.
// ============================================================

#ifndef VULKAN_H_
#define VULKAN_H_

#include <stdint.h>

// Dispatchable handle type (pointer-sized opaque)
#define VK_DEFINE_HANDLE(object)  typedef struct object##_T* object;

VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkSwapchainKHR)
VK_DEFINE_HANDLE(VkSemaphore)
VK_DEFINE_HANDLE(VkFence)

typedef uint32_t VkFlags;
typedef uint32_t VkResult;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkStructureType;

#define VK_SUCCESS 0
#define VK_STRUCTURE_TYPE_PRESENT_INFO_KHR 1000001001

// VkPresentInfoKHR — what vkQueuePresentKHR receives
typedef struct VkPresentInfoKHR {
    VkStructureType  sType;
    const void*      pNext;
    uint32_t         waitSemaphoreCount;
    const VkSemaphore* pWaitSemaphores;
    uint32_t         swapchainCount;
    const VkSwapchainKHR* pSwapchains;
    const uint32_t*  pImageIndices;
    VkResult*        pResults;
} VkPresentInfoKHR;

// Calling convention for Vulkan functions
#ifndef VKAPI_CALL
#  ifdef _WIN32
#    define VKAPI_CALL __stdcall
#  else
#    define VKAPI_CALL
#  endif
#endif
#ifndef VKAPI_ATTR
#  define VKAPI_ATTR
#endif

// vkQueuePresentKHR signature
typedef VkResult (VKAPI_ATTR VKAPI_CALL *PFN_vkQueuePresentKHR)(
    VkQueue queue, const VkPresentInfoKHR* pPresentInfo);

#endif // VULKAN_H_
