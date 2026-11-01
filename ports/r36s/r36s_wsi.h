#pragma once
#include <cstdint>
#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#else
typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
typedef int VkResult;
typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance instance, const char* pName);
#endif
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#endif

// On non-Android platforms, ANativeWindow is defined as a host window container.
#ifndef __ANDROID__
struct ANativeWindow {
    void* display;
    uintptr_t window;
    uint32_t width;
    uint32_t height;
};
#endif

// Hook into DXVK native WSI
extern "C" {
    typedef VkResult (*DxvkCustomSurfaceCreatorFn)(VkInstance instance, VkSurfaceKHR* surface);
    void dxvkSetCustomSurfaceCreator(DxvkCustomSurfaceCreatorFn fn);
    void dxvkAndroidSetWindow(ANativeWindow* window);
    void dxvkAndroidSetRenderSize(uint32_t width, uint32_t height);
    void dxvkAndroidSetInstanceProcAddr(PFN_vkGetInstanceProcAddr entry);
}
