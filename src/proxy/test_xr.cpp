// Offline harness for xr.cpp: imitates the game (instance with VK_KHR_surface, device, "swapchain" image in PRESENT_SRC,
// render-done semaphore per frame) and drives the exact same xr_* entry points the proxy uses.
// Exit: 0 = XR frames rendered, 3 = flat-mode fallback (no headset/runtime) handled cleanly, 1/2 = error.
// Usage: test_xr.exe [seconds]
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include "xr.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <vector>

static LARGE_INTEGER qpf, qpc0;
double now_s() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return double(t.QuadPart - qpc0.QuadPart) / qpf.QuadPart; }
void vlog(const char* f, ...) { printf("[%8.3f] ", now_s()); va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); fflush(stdout); }
bool camhook_install() { return true; } void camhook_set(float, float, float) {} void camhook_stats(uint64_t* h, uint64_t* o) { *h = 0; *o = 0; }
static uint64_t g_eh = 0; bool camhook_eye_ok() { return true; } void camhook_set_eye(float) { ++g_eh; } uint64_t camhook_eye_hits() { return g_eh; }
struct CamFields; const CamFields* camhook_fields() { return nullptr; }
void camhook_set_write(int, uint32_t, float) {}
static uint64_t g_seq = 0; bool camhook_view_ok() { return true; } void camhook_set_view(bool, const float[3][3], float, const float[3], uint64_t s) { g_seq = s; }
uint64_t camhook_view_used_seq() { return g_seq; }
void camhook_set_pull(float) {}
void cap_arm() {}
uint64_t camhook_fov_rec() { return 0; } uint64_t camhook_proj_hits() { return 0; } uint64_t camhook_proj_dst() { return 0; } uint64_t camhook_src_hits() { return 0; }
bool camhook_fov_ok() { return true; } void camhook_set_render_fov(float) {} uint64_t camhook_fov_hits() { return 0; }
void camhook_view_stats(uint64_t* a, uint64_t* b) { *a = *b = 0; }
#define CK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { printf("FAIL %s -> %d\n", #x, _r); return 2; } } while (0)

int main(int argc, char** argv) {
    QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&qpc0);
    double secs = argc > 1 ? atof(argv[1]) : 20.0;
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH); *strrchr(exe, '\\') = 0;
    HMODULE vk = LoadLibraryA("vulkan-1.dll"); if (!vk) { printf("no vulkan-1.dll\n"); return 1; }
    auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    auto createInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");

    bool xr = xr_init(exe);
    vlog("xr_init -> %d", xr);
    const char* iext[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_win32_surface", "VK_KHR_surface"};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "Enshrouded"; app.pEngineName = "holistic"; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app; ici.enabledExtensionCount = 3; ici.ppEnabledExtensionNames = iext;
    XrInstCI xci; if (xr) xr_build_instance_ci(&ici, xci);
    VkInstance inst; CK(createInstance(xr ? &xci.ci : &ici, nullptr, &inst));
    if (xr) xr_on_instance(inst, gipa);
#define IF(n) auto n = (PFN_##n)gipa(inst, #n)
    IF(vkEnumeratePhysicalDevices); IF(vkCreateDevice); IF(vkGetDeviceProcAddr); IF(vkGetPhysicalDeviceMemoryProperties);
    VkPhysicalDevice pd = xr_physical_device();
    if (!pd) { uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd); }
    float prio = 1.f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* dext[] = {"VK_KHR_swapchain", "VK_KHR_dynamic_rendering"};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext = &f12;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = dext;
    XrDevCI xdc; bool dx = xr && xr_build_device_ci(pd, &dci, xdc);
    VkDevice dev; CK(vkCreateDevice(pd, dx ? &xdc.ci : &dci, nullptr, &dev));
    if (dx) xr_on_device(dev, pd);
#define DF(n) auto n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    DF(vkGetDeviceQueue); DF(vkCreateImage); DF(vkGetImageMemoryRequirements); DF(vkAllocateMemory); DF(vkBindImageMemory);
    DF(vkCreateBuffer); DF(vkGetBufferMemoryRequirements); DF(vkBindBufferMemory); DF(vkMapMemory); DF(vkCreateCommandPool);
    DF(vkAllocateCommandBuffers); DF(vkBeginCommandBuffer); DF(vkEndCommandBuffer); DF(vkCmdPipelineBarrier); DF(vkCmdCopyBufferToImage);
    DF(vkQueueSubmit); DF(vkCreateSemaphore); DF(vkCreateFence); DF(vkWaitForFences); DF(vkResetFences); DF(vkDeviceWaitIdle);
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q); xr_note_queue(q, 0, 0);

    const uint32_t W = 1920, H = 1080;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {W, H, 1}; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage img; CK(vkCreateImage(dev, &ii, nullptr, &img));
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    auto memtype = [&](uint32_t bits, VkMemoryPropertyFlags fl) { for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & fl) == fl) return i; return 0u; };
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory im; CK(vkAllocateMemory(dev, &mai, nullptr, &im)); CK(vkBindImageMemory(dev, img, im, 0));
    // staging: pattern (gradient + 120px grid) + one white 48px column for a moving bar
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = (VkDeviceSize)W * H * 4 + 48 * H * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf; CK(vkCreateBuffer(dev, &bci, nullptr, &buf)); vkGetBufferMemoryRequirements(dev, buf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bm; CK(vkAllocateMemory(dev, &mai, nullptr, &bm)); CK(vkBindBufferMemory(dev, buf, bm, 0));
    uint8_t* px; CK(vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, (void**)&px));
    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
        uint8_t* p = px + ((size_t)y * W + x) * 4; bool grid = (x % 120) < 3 || (y % 120) < 3;
        p[0] = grid ? 255 : (uint8_t)(x * 255 / W); p[1] = grid ? 255 : (uint8_t)(y * 255 / H); p[2] = grid ? 255 : 128; p[3] = 255;
    }
    memset(px + (size_t)W * H * 4, 255, 48 * H * 4);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool; CK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    const int N = 3; VkCommandBuffer cbs[N]; VkSemaphore done[N], drain[N]; VkFence fences[N];
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.commandBufferCount = N;
    CK(vkAllocateCommandBuffers(dev, &cai, cbs));
    for (int i = 0; i < N; ++i) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; CK(vkCreateSemaphore(dev, &sci, nullptr, &done[i])); CK(vkCreateSemaphore(dev, &sci, nullptr, &drain[i]));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; CK(vkCreateFence(dev, &fci, nullptr, &fences[i]));
    }
    VkSwapchainKHR fake_sc = (VkSwapchainKHR)(uintptr_t)0x5c;
    xr_on_swapchain(fake_sc, W, H, VK_FORMAT_R8G8B8A8_UNORM, 0, nullptr);
    xr_on_swapchain(fake_sc, 0, 0, VK_FORMAT_UNDEFINED, 1, &img);
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t frames = 0, xr_frames = 0; double t0 = now_s();
    while (now_s() - t0 < secs) {
        int i = frames % N;
        CK(vkWaitForFences(dev, 1, &fences[i], VK_TRUE, 2000000000ull)); CK(vkResetFences(dev, 1, &fences[i]));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cbs[i], &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout = layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cbs[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy c{}; c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c.imageExtent = {W, H, 1};
        vkCmdCopyBufferToImage(cbs[i], buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        VkBufferImageCopy bar{}; bar.bufferOffset = (VkDeviceSize)W * H * 4; bar.imageSubresource = c.imageSubresource;
        bar.imageOffset = {(int32_t)((frames * 6) % (W - 48)), 0, 0}; bar.imageExtent = {48, H, 1};
        vkCmdCopyBufferToImage(cbs[i], buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bar);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cbs[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cbs[i]); layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cbs[i]; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &done[i];
        CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        // "present"
        uint32_t idx = 0; VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &done[i];
        pi.swapchainCount = 1; pi.pSwapchains = &fake_sc; pi.pImageIndices = &idx;
        VkSemaphore ws = xr_on_present(q, &pi);
        if (ws) xr_frames++;
        VkSemaphore w = ws ? ws : done[i]; VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;   // stand-in for the present engine wait
        VkSubmitInfo si2{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si2.waitSemaphoreCount = 1; si2.pWaitSemaphores = &w; si2.pWaitDstStageMask = &st;
        CK(vkQueueSubmit(q, 1, &si2, fences[i]));
        frames++;
        if (!ws) Sleep(5);
    }
    vkDeviceWaitIdle(dev);
    double dt = now_s() - t0;
    vlog("frames=%llu (%.1f/s), frames shown in XR=%llu", (unsigned long long)frames, frames / dt, (unsigned long long)xr_frames);
    if (!xr) { printf("RESULT: FLAT-FALLBACK OK (no XR system) \n"); return 3; }
    printf(xr_frames > 0 ? "RESULT: XR PASS\n" : "RESULT: XR initialised but no frames shown\n");
    return xr_frames > 0 ? 0 : 1;
}
