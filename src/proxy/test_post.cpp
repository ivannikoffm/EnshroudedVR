// Offline GPU test of the v1.1.1 post pass (shaders/post.comp -> post_comp.h). No game, no XR runtime.
// 1) neutral params reproduce the input bit-exactly  2) CAS raises edge contrast, leaves flat areas alone
// 3) contrast / saturation / gamma move values the right way  4) cost of one 2160x2160 dispatch
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <cmath>
#include "post_comp.h"
#pragma comment(lib, "vulkan-1.lib")

static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static uint32_t qf;
static VkCommandPool pool; static VkCommandBuffer cb; static VkFence fence;
static int fails = 0;
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(2); } } while (0)
#define EXPECT(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("no memtype\n"); exit(2);
}
struct Img { VkImage img; VkDeviceMemory mem; VkImageView view; };
Img mkimg(uint32_t w, uint32_t h, VkImageUsageFlags u) {
    Img r; VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = u;
    CK(vkCreateImage(dev, &ii, nullptr, &r.img)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, r.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CK(vkAllocateMemory(dev, &ai, nullptr, &r.mem)); CK(vkBindImageMemory(dev, r.img, r.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = r.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; CK(vkCreateImageView(dev, &vi, nullptr, &r.view)); return r;
}
struct Buf { VkBuffer b; VkDeviceMemory m; uint8_t* p; };
Buf mkbuf(VkDeviceSize n) {
    Buf r; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = n; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CK(vkCreateBuffer(dev, &bi, nullptr, &r.b)); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CK(vkAllocateMemory(dev, &ai, nullptr, &r.m)); CK(vkBindBufferMemory(dev, r.b, r.m, 0)); CK(vkMapMemory(dev, r.m, 0, n, 0, (void**)&r.p)); return r;
}
void bar(VkImage i, VkImageLayout a, VkImageLayout b) {
    VkImageMemoryBarrier m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; m.oldLayout = a; m.newLayout = b; m.image = i;
    m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; m.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &m);
}
void begin() { CK(vkResetCommandBuffer(cb, 0)); VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CK(vkBeginCommandBuffer(cb, &bi)); }
void submit() { CK(vkEndCommandBuffer(cb)); VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CK(vkResetFences(dev, 1, &fence)); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)); }

VkPipeline pipe; VkPipelineLayout pl; VkDescriptorSet ds;
struct PC { float sharp, contrast, sat, gamma; int w, h; };

std::vector<uint8_t> run(const std::vector<uint8_t>& in, uint32_t w, uint32_t h, PC pc, Img& s, Img& d, Buf& up, Buf& dn) {
    memcpy(up.p, in.data(), in.size());
    begin();
    bar(s.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{}; c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, up.b, s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    bar(s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);      // same layout the mod uses on the game image
    bar(d.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
    pc.w = (int)w; pc.h = (int)h; vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    bar(d.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cb, d.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dn.b, 1, &c);
    submit();
    return std::vector<uint8_t>(dn.p, dn.p + in.size());
}

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app; VkInstance inst; CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
    pd = pds[0]; for (auto p : pds) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(p, &pp); if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pd = p; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("GPU: %s\n", pp.deviceName);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr); std::vector<VkQueueFamilyProperties> qp(n); vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qp.data());
    for (qf = 0; qf < n && !(qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); ++qf) {}
    float pr = 1.f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &pr;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; CK(vkCreateDevice(pd, &dci, nullptr, &dev));
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    printf("R8G8B8A8_UNORM optimal storage image: %s\n", (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? "yes" : "NO");
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = qf; CK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.commandBufferCount = 1; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; CK(vkCreateFence(dev, &fci, nullptr, &fence));

    // pipeline: identical layout to xr.cpp make_post()
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = sizeof(kPostComp); smi.pCode = kPostComp; VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, nullptr, &sm));
    VkDescriptorSetLayoutBinding bd[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.bindingCount = 2; dli.pBindings = bd; VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr; CK(vkCreatePipelineLayout(dev, &pli, nullptr, &pl));
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr}; cpi.layout = pl;
    CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}; VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl; CK(vkAllocateDescriptorSets(dev, &dai, &ds));

    const uint32_t W = 2160, H = 2160; const size_t N = (size_t)W * H * 4;
    Img s = mkimg(W, H, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT), d = mkimg(W, H, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Buf up = mkbuf(N), dn = mkbuf(N);
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, s.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, d.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2]; for (int i = 0; i < 2; ++i) { wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr[i].dstSet = ds; wr[i].dstBinding = i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[i].pImageInfo = &ii[i]; }
    vkUpdateDescriptorSets(dev, 2, wr, 0, nullptr);

    // test image: random noise top half (every byte value), vertical step edge 64|192 bottom half, flat 128 band
    std::vector<uint8_t> img(N); uint32_t rs = 12345;
    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) { uint8_t* p = &img[((size_t)y * W + x) * 4];
        if (y < H / 2) { for (int k = 0; k < 3; ++k) { rs = rs * 1664525u + 1013904223u; p[k] = (uint8_t)(rs >> 24); } }
        else if (y < H * 3 / 4) { uint8_t v = x < W / 2 ? 64 : 192; p[0] = p[1] = p[2] = v; }
        else { p[0] = 200; p[1] = 120; p[2] = 40; }
        p[3] = 255; }
    auto px = [&](const std::vector<uint8_t>& v, uint32_t x, uint32_t y, int k) { return (int)v[((size_t)y * W + x) * 4 + k]; };

    printf("[1] neutral (sharp 0, contrast 1, sat 1, gamma 1): bit-exact\n");
    auto o = run(img, W, H, {0.f, 1.f, 1.f, 1.f}, s, d, up, dn); size_t diff = 0;
    for (size_t i = 0; i < N; ++i) if ((i & 3) != 3 && o[i] != img[i]) ++diff;
    EXPECT(diff == 0, "%zu channel values differ", diff); printf("    differing values: %zu\n", diff);

    printf("[2] CAS sharp 0.5: edge contrast up, flat unchanged\n");
    o = run(img, W, H, {0.5f, 1.f, 1.f, 1.f}, s, d, up, dn); const uint32_t ey = H * 5 / 8, ex = W / 2;
    printf("    edge row: x-2..x+1 in %d %d | %d %d  out %d %d | %d %d\n", px(img, ex - 2, ey, 0), px(img, ex - 1, ey, 0), px(img, ex, ey, 0), px(img, ex + 1, ey, 0),
           px(o, ex - 2, ey, 0), px(o, ex - 1, ey, 0), px(o, ex, ey, 0), px(o, ex + 1, ey, 0));
    EXPECT(px(o, ex - 1, ey, 0) < 64 && px(o, ex, ey, 0) > 192, "edge not sharpened");
    EXPECT(px(o, ex - 10, ey, 0) == 64 && px(o, ex + 10, ey, 0) == 192 && px(o, 100, H * 7 / 8, 1) == 120, "flat area changed");
    double nin = 0, nout = 0; for (uint32_t x = 1; x < 2000; ++x) { nin += abs(px(img, x, 10, 1) - px(img, x - 1, 10, 1)); nout += abs(px(o, x, 10, 1) - px(o, x - 1, 10, 1)); }
    printf("    noise mean |dx|: in %.1f out %.1f\n", nin / 1999, nout / 1999); EXPECT(nout >= nin, "noise detail lost");

    printf("[3] contrast 1.2 / saturation 0 / gamma 0.8\n");
    o = run(img, W, H, {0.f, 1.2f, 1.f, 1.f}, s, d, up, dn);
    printf("    contrast: 64->%d 192->%d\n", px(o, 100, ey, 0), px(o, W - 100, ey, 0)); EXPECT(px(o, 100, ey, 0) < 64 && px(o, W - 100, ey, 0) > 192, "contrast");
    o = run(img, W, H, {0.f, 1.f, 0.f, 1.f}, s, d, up, dn);
    printf("    sat 0: (200,120,40)->(%d,%d,%d)\n", px(o, 100, H * 7 / 8, 0), px(o, 100, H * 7 / 8, 1), px(o, 100, H * 7 / 8, 2));
    EXPECT(px(o, 100, H * 7 / 8, 0) == px(o, 100, H * 7 / 8, 2), "saturation 0 not grey");
    o = run(img, W, H, {0.f, 1.f, 1.f, 0.8f}, s, d, up, dn);
    printf("    gamma exp 0.8 (brighter): 64->%d 192->%d\n", px(o, 100, ey, 0), px(o, W - 100, ey, 0)); EXPECT(px(o, 100, ey, 0) > 64, "gamma");

    printf("[4] cost of one %ux%u dispatch (CAS on), 200 runs\n", W, H);
    begin(); for (int i = 0; i < 200; ++i) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
        PC pc{0.5f, 1.1f, 1.1f, 0.95f, (int)W, (int)H}; vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc); vkCmdDispatch(cb, (W + 7) / 8, (H + 7) / 8, 1); }
    auto t0 = std::chrono::high_resolution_clock::now(); submit(); auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / 200; printf("    %.3f ms per frame\n", ms); EXPECT(ms < 0.5, "too slow");

    printf(fails ? "POST TEST FAIL (%d)\n" : "POST TEST PASS\n", fails);
    vkDeviceWaitIdle(dev); return fails ? 1 : 0;
}
