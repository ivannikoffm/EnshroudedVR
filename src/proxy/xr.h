// EnshroudedVR — S4 OpenXR pipe (XR_KHR_vulkan_enable v1: the GAME creates VkInstance/VkDevice, we append runtime extensions).
#pragma once
#include <vulkan/vulkan.h>
#include <deque>
#include <string>
#include <vector>

struct XrInstCI {                       // game's VkInstanceCreateInfo + runtime extensions
    std::deque<std::string> store;
    std::vector<const char*> exts;
    VkInstanceCreateInfo ci{};
};
struct XrDevCI {                        // game's VkDeviceCreateInfo + runtime extensions + timelineSemaphore feature
    std::deque<std::string> store;
    std::vector<const char*> exts;
    VkPhysicalDeviceTimelineSemaphoreFeatures tl{};
    VkDeviceCreateInfo ci{};
};

bool xr_init(const char* loader_dir);   // load loader, XrInstance + HMD system. false => flat mode (no headset / no runtime / xr_off.txt)
bool xr_active();
void xr_build_instance_ci(const VkInstanceCreateInfo* in, XrInstCI& o);
void xr_on_instance(VkInstance inst, PFN_vkGetInstanceProcAddr real_gipa);
VkPhysicalDevice xr_physical_device();
bool xr_build_device_ci(VkPhysicalDevice pd, const VkDeviceCreateInfo* in, XrDevCI& o);   // false = pd is not the HMD GPU, use original ci
void xr_on_device(VkDevice dev, VkPhysicalDevice pd);
void xr_note_queue(VkQueue q, uint32_t family, uint32_t index);
void xr_on_swapchain(VkSwapchainKHR sc, uint32_t w, uint32_t h, VkFormat fmt, uint32_t n, const VkImage* imgs);
// Call right before the real vkQueuePresentKHR. Returns a semaphore present must wait on INSTEAD of pi->pWaitSemaphores
// (our copy submit consumed them), or VK_NULL_HANDLE = present unchanged.
VkSemaphore xr_on_present(VkQueue q, const VkPresentInfoKHR* pi);
