/*
  Vulkan for SDL's dummy video driver on Horizon (Nintendo Switch homebrew), copied into SDL's
  src/video/dummy by aurora's cmake/AuroraSDL3Patches.cmake. zlib license, as SDL.

  Mesa NVK is linked into the program (Horizon has no loader to open), and an application has one
  window, libnx's default NWindow, presented through VK_NN_vi_surface.
*/
#include "SDL_internal.h"

#if defined(__SWITCH__) && defined(SDL_VIDEO_VULKAN)

#define VK_USE_PLATFORM_VI_NN
#include "../SDL_vulkan_internal.h"
#include "../SDL_sysvideo.h"

#include <switch.h>

// NVK's loaderless library exports the instance entry point under its usual name.
extern PFN_vkVoidFunction HORIZON_vkGetInstanceProcAddr(VkInstance instance, const char *name) __asm__("vkGetInstanceProcAddr");

static bool HORIZON_Vulkan_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    (void)path;
    _this->vulkan_config.vkGetInstanceProcAddr = (SDL_FunctionPointer)HORIZON_vkGetInstanceProcAddr;
    _this->vulkan_config.vkEnumerateInstanceExtensionProperties =
        (SDL_FunctionPointer)HORIZON_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!_this->vulkan_config.vkEnumerateInstanceExtensionProperties) {
        return SDL_SetError("The linked Vulkan driver has no vkEnumerateInstanceExtensionProperties");
    }
    _this->vulkan_config.loader_handle = (SDL_SharedObject *)HORIZON_vkGetInstanceProcAddr;  // linked in: nothing to open
    SDL_strlcpy(_this->vulkan_config.loader_path, "(linked)", SDL_arraysize(_this->vulkan_config.loader_path));
    return true;
}

static void HORIZON_Vulkan_UnloadLibrary(SDL_VideoDevice *_this)
{
    _this->vulkan_config.loader_handle = NULL;
}

static char const *const *HORIZON_Vulkan_GetInstanceExtensions(SDL_VideoDevice *_this, Uint32 *count)
{
    static const char *const extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME };
    (void)_this;
    if (count) {
        *count = SDL_arraysize(extensions);
    }
    return extensions;
}

static bool HORIZON_Vulkan_CreateSurface(SDL_VideoDevice *_this, SDL_Window *window, VkInstance instance,
                                         const struct VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    (void)_this;
    (void)window;
    const PFN_vkCreateViSurfaceNN create =
        (PFN_vkCreateViSurfaceNN)HORIZON_vkGetInstanceProcAddr(instance, "vkCreateViSurfaceNN");
    if (!create) {
        return SDL_SetError("vkCreateViSurfaceNN is not available");
    }
    VkViSurfaceCreateInfoNN info;
    SDL_zero(info);
    info.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
    info.window = nwindowGetDefault();
    const VkResult result = create(instance, &info, allocator, surface);
    if (result != VK_SUCCESS) {
        return SDL_SetError("vkCreateViSurfaceNN failed (%d)", (int)result);
    }
    return true;
}

static void HORIZON_Vulkan_DestroySurface(SDL_VideoDevice *_this, VkInstance instance, VkSurfaceKHR surface,
                                          const struct VkAllocationCallbacks *allocator)
{
    SDL_Vulkan_DestroySurface_Internal(_this->vulkan_config.vkGetInstanceProcAddr, instance, surface, allocator);
}

// The dummy driver's display mode: the default VI layer's size (1280x720 docked). Unchanged on failure.
void HORIZON_DisplaySize(int *w, int *h)
{
    u32 width = 0, height = 0;
    if (R_SUCCEEDED(nwindowGetDimensions(nwindowGetDefault(), &width, &height)) && width != 0 && height != 0) {
        *w = (int)width;
        *h = (int)height;
    }
}

void HORIZON_SetupVulkan(SDL_VideoDevice *device)
{
    device->Vulkan_LoadLibrary = HORIZON_Vulkan_LoadLibrary;
    device->Vulkan_UnloadLibrary = HORIZON_Vulkan_UnloadLibrary;
    device->Vulkan_GetInstanceExtensions = HORIZON_Vulkan_GetInstanceExtensions;
    device->Vulkan_CreateSurface = HORIZON_Vulkan_CreateSurface;
    device->Vulkan_DestroySurface = HORIZON_Vulkan_DestroySurface;
}

#endif
