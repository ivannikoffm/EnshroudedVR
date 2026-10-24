// Offline GPU test of the S12 MV fix (mvfix.cpp + shaders/mvfix.comp). Synthetic scene at game-like world coordinates,
// three render cameras (prevSame = same eye 2 frames ago, prevOther = other eye last frame, cur). The "game" MVs
// (cur -> prevOther) are written with a SECRET convention; mvfix must find it by calibration and then output
// cur -> prevSame MVs that match the analytic ground truth.
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include "mvfix.h"
#pragma comment(lib, "vulkan-1.lib")
void vlog(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }

static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkCommandPool pool; static VkCommandBuffer cb; static VkFence fence;
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(2); } } while (0)
uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) { VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i; exit(3); }
struct Img { VkImage img; VkImageView view; };
Img mkimg(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags u) {
    Img r; VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f; ii.extent = {w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = u; CK(vkCreateImage(dev, &ii, nullptr, &r.img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, r.img, &mr); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, nullptr, &m)); CK(vkBindImageMemory(dev, r.img, m, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = r.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CK(vkCreateImageView(dev, &vi, nullptr, &r.view)); return r; }
struct Buf { VkBuffer b; uint8_t* p; };
Buf mkbuf(VkDeviceSize n) { Buf r; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = n; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CK(vkCreateBuffer(dev, &bi, nullptr, &r.b)); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, nullptr, &m)); CK(vkBindBufferMemory(dev, r.b, m, 0)); CK(vkMapMemory(dev, m, 0, n, 0, (void**)&r.p)); return r; }
void bar(VkImage i, VkImageLayout a, VkImageLayout b) { VkImageMemoryBarrier m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; m.oldLayout = a; m.newLayout = b; m.image = i;
    m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; m.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &m); }
void begin() { CK(vkResetCommandBuffer(cb, 0)); VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CK(vkBeginCommandBuffer(cb, &bi)); }
void submit() { CK(vkEndCommandBuffer(cb)); VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CK(vkResetFences(dev, 1, &fence)); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)); }

// ---- synthetic cameras (row-vector, like the game struct r14+0x1f10)
struct CamD { double R[3][3]; double p[3]; };
CamD mkcam(double yaw, double pitch, double px, double py, double pz) {
    // camera axes in world: right, up, forward; R rows = world axes expressed... v = X*R + t -> column j of R = camera axis j in world
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    double fwd[3] = {sy * cp, sp, cy * cp}, right[3] = {cy, 0, -sy}, up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0]};
    if (up[1] < 0) for (double& u : up) u = -u;
    CamD c; for (int i = 0; i < 3; ++i) { c.R[i][0] = right[i]; c.R[i][1] = up[i]; c.R[i][2] = fwd[i]; }
    c.p[0] = px; c.p[1] = py; c.p[2] = pz; return c; }
const double P00 = 0.70021, P11 = 0.70021, kNear = 0.01;
void to_struct(const CamD& c, float* E) {
    memset(E, 0, 32 * 4);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) E[i * 4 + j] = (float)c.R[i][j];
    for (int j = 0; j < 3; ++j) { double t = 0; for (int i = 0; i < 3; ++i) t -= c.p[i] * c.R[i][j]; E[12 + j] = (float)t; }
    E[15] = 1; E[16] = (float)P00; E[21] = (float)P11; E[26] = 1; E[27] = 1; E[30] = (float)-kNear; }
// conventions (same bit layout as the shader)
const int SECRET = 1 | 4 | 8 | 32;   // reverse-infinite depth, y flipped, MV x negated, NDC units
void view_of(const CamD& c, const double X[3], double v[3]) { for (int j = 0; j < 3; ++j) { v[j] = 0; for (int i = 0; i < 3; ++i) v[j] += (X[i] - c.p[i]) * c.R[i][j]; } }
bool proj_uv(const CamD& c, const double X[3], int conv, double uv[2]) { double v[3]; view_of(c, X, v); if (v[2] <= 1e-4) return false;
    double nx = P00 * v[0] / v[2], ny = P11 * v[1] / v[2]; uv[0] = nx * 0.5 + 0.5; uv[1] = (conv & 4) ? 0.5 - 0.5 * ny : 0.5 + 0.5 * ny; return true; }
void mv_of(const double duv[2], int conv, double mv[2]) { double u = (conv & 32) ? 2 : 1; mv[0] = ((conv & 8) ? -1 : 1) * duv[0] * u; mv[1] = ((conv & 16) ? -1 : 1) * duv[1] * u; }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1; VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance inst; CK(vkCreateInstance(&ici, nullptr, &inst)); uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
    pd = pds[0]; for (auto p : pds) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(p, &pp); if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pd = p; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("GPU %s\n", pp.deviceName);
    float pr = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &pr;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; CK(vkCreateDevice(pd, &dci, nullptr, &dev)); vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; CK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.commandBufferCount = 1; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; CK(vkCreateFence(dev, &fci, nullptr, &fence));
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    if (!mvfix_init(dev, vkGetDeviceProcAddr, mp, pp.limits.minStorageBufferOffsetAlignment)) { printf("init failed\n"); return 2; }

    const uint32_t W = 1080, H = 1080; const double ipd = 0.0744;
    // world like the game (~3748 m); prevSame -> prevOther -> cur: eyes alternate, mouse yaw/pitch drift, walking forward
    CamD cs = mkcam(0.300, -0.050, 3748.000, 844.60, 1430.40);
    CamD co = mkcam(0.304, -0.046, 3748.000 + ipd * cos(0.304), 844.60, 1430.40 + 0.03 - ipd * sin(0.304));
    CamD cc = mkcam(0.308, -0.042, 3748.000, 844.60, 1430.40 + 0.06);
    // scene: ground y = 843.0, sphere at 4 m ahead, sky
    double C[3] = {cc.p[0] + 4.0 * sin(0.308), 844.2, cc.p[2] + 4.0 * cos(0.308)}, Rs = 0.8;
    std::vector<float> depth(W * H); std::vector<int16_t> mv(W * H * 2); std::vector<double> gt(W * H * 2); std::vector<uint8_t> valid(W * H);
    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
        double u = (x + 0.5) / W, v = (y + 0.5) / H, nx = 2 * u - 1, ny = (SECRET & 4) ? 1 - 2 * v : 2 * v - 1;
        double dv[3] = {nx / P00, ny / P11, 1.0}, dw[3]; for (int i = 0; i < 3; ++i) dw[i] = cc.R[i][0] * dv[0] + cc.R[i][1] * dv[1] + cc.R[i][2] * dv[2];
        double z = 1e6;   // view z along the ray (dv has z=1)
        if (dw[1] < 0) { double zt = (843.0 - cc.p[1]) / dw[1]; if (zt > 0) z = std::min(z, zt); }
        double oc[3] = {cc.p[0] - C[0], cc.p[1] - C[1], cc.p[2] - C[2]}; double a = dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2], b = 2 * (oc[0] * dw[0] + oc[1] * dw[1] + oc[2] * dw[2]),
            cq = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - Rs * Rs, disc = b * b - 4 * a * cq;
        if (disc > 0) { double zt = (-b - sqrt(disc)) / (2 * a); if (zt > 0) z = std::min(z, zt); }
        double X[3] = {cc.p[0] + dw[0] * z, cc.p[1] + dw[1] * z, cc.p[2] + dw[2] * z};
        depth[y * W + x] = (float)(kNear / z);   // reverse infinite (SECRET dt = 1)
        double uo[2], us[2], uvc[2] = {u, v}; bool ok = z < 1e5 && proj_uv(co, X, SECRET, uo) && proj_uv(cs, X, SECRET, us); valid[y * W + x] = ok;
        double m1[2] = {0, 0}, m2[2] = {0, 0};
        if (ok) { double d1[2] = {uo[0] - uvc[0], uo[1] - uvc[1]}, d2[2] = {us[0] - uvc[0], us[1] - uvc[1]}; mv_of(d1, SECRET, m1); mv_of(d2, SECRET, m2); }
        for (int k = 0; k < 2; ++k) { mv[(y * W + x) * 2 + k] = (int16_t)lround(std::max(-1.0, std::min(1.0, m1[k])) * 32767); gt[(y * W + x) * 2 + k] = m2[k]; }
    }
    Img dimg = mkimg(W, H, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Img mimg = mkimg(W, H, VK_FORMAT_R16G16_SNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Buf up = mkbuf((VkDeviceSize)W * H * 4 * 2);
    memcpy(up.p, depth.data(), W * H * 4); memcpy(up.p + W * H * 4, mv.data(), W * H * 4);
    begin(); bar(dimg.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); bar(mimg.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{}; c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c.imageExtent = {W, H, 1};
    vkCmdCopyBufferToImage(cb, up.b, dimg.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c); c.bufferOffset = W * H * 4;
    vkCmdCopyBufferToImage(cb, up.b, mimg.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    bar(dimg.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL); bar(mimg.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL); submit();
    // fake NGX resources + eval params
    alignas(16) uint8_t rd[0x38] = {}, rm[0x38] = {}, ev[0x80] = {};
    memcpy(rd, &dimg.view, 8); memcpy(rd + 8, &dimg.img, 8); memcpy(rm, &mimg.view, 8); memcpy(rm + 8, &mimg.img, 8);
    memcpy(rd + 0x28, &W, 4); memcpy(rd + 0x2c, &H, 4); memcpy(rm + 0x28, &W, 4); memcpy(rm + 0x2c, &H, 4);
    void* prd = rd; void* prm = rm; memcpy(ev + 0x18, &prd, 8); memcpy(ev + 0x20, &prm, 8);
    float Es[32], Eo[32], Ec[32]; to_struct(cs, Es); to_struct(co, Eo); to_struct(cc, Ec);
    void* res = nullptr; int fails = 0;
    for (int it = 0; it < 60 && !res; ++it) for (int k = 0; k < 3; ++k) {   // sequence prevSame, prevOther, cur (cur = every 3rd call)
        begin(); void* r = mvfix_pre(cb, ev, k == 0 ? Es : k == 1 ? Eo : Ec, 1); submit(); if (r && k == 2) res = r; else if (r) { printf("apply on a non-cur call\n"); ++fails; }
    }
    MvFixStats st; mvfix_stats(&st);
    printf("calibs=%llu calibrated=%d conv=0x%02x (secret 0x%02x) %s\n", st.calibs, st.calibrated, st.conv, SECRET, st.top);
    if (!st.calibrated || st.conv != SECRET) { printf("FAIL: calibration\n"); ++fails; }
    if (!res) { printf("FAIL: no replacement resource\n"); return 1; }
    VkImage oimg; memcpy(&oimg, (uint8_t*)res + 8, 8); uint32_t ow, oh; memcpy(&ow, (uint8_t*)res + 0x28, 4); memcpy(&oh, (uint8_t*)res + 0x2c, 4);
    Buf dn = mkbuf((VkDeviceSize)W * H * 4);
    begin(); bar(oimg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); c.bufferOffset = 0; vkCmdCopyImageToBuffer(cb, oimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dn.b, 1, &c);
    bar(oimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL); submit();
    auto h2f = [](uint16_t h) { uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float v = e == 0 ? ldexpf((float)m, -24) : ldexpf((float)(m | 1024), (int)e - 25); return s ? -v : v; };
    std::vector<double> err, errGame; const uint16_t* o = (const uint16_t*)dn.p;
    for (uint32_t i = 0; i < W * H; ++i) if (valid[i]) {
        double ex = h2f(o[i * 2]) - gt[i * 2], ey = h2f(o[i * 2 + 1]) - gt[i * 2 + 1]; err.push_back(sqrt(ex * ex + ey * ey) * W);
        double gx = mv[i * 2] / 32767.0 - gt[i * 2], gy = mv[i * 2 + 1] / 32767.0 - gt[i * 2 + 1]; errGame.push_back(sqrt(gx * gx + gy * gy) * W); }
    std::sort(err.begin(), err.end()); std::sort(errGame.begin(), errGame.end());
    auto pct = [](std::vector<double>& v, double p) { return v[(size_t)(p * (v.size() - 1))]; };
    printf("pixels %zu | vs same-eye truth: GAME MV median %.2f px p99 %.2f px | FIXED median %.3f px p99 %.3f px max %.3f px\n", err.size(),
           pct(errGame, 0.5), pct(errGame, 0.99), pct(err, 0.5), pct(err, 0.99), err.back());
    if (pct(err, 0.99) > 0.25 || pct(errGame, 0.5) < 1.0) { printf("FAIL: accuracy\n"); ++fails; }
    printf(fails ? "MVFIX TEST FAIL\n" : "MVFIX TEST PASS\n"); vkDeviceWaitIdle(dev); return fails;
}
