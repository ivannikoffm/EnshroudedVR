// EnshroudedVR S12 - per-eye DLSS motion-vector fix (host side). See mvfix.h and shaders/mvfix.comp.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "mvfix.h"
#include "mvfix_comp.h"
#include <cstring>
#include <cmath>
#include <map>
#include <vector>
#include <algorithm>
#include <mutex>

void vlog(const char* fmt, ...);

namespace {
#define MV_FUNCS(X) X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateComputePipelines) \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateSampler) X(vkCreateBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkCmdPipelineBarrier) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch)
#define MDECL(n) PFN_##n m_##n = nullptr;
MV_FUNCS(MDECL)

constexpr int kGrid = 32, kConv = 48, kRing = 16;
constexpr uint32_t kCamBytes = 256;   // Cam block in the shader: 9 vec4 + 3 vec4 + vec4 + ivec4 = 224 B
struct Cam { float R[3][3]; double p[3]; float P00, P11, n; bool ok; };
struct Gpu {
    VkDevice dev = VK_NULL_HANDLE; VkPhysicalDeviceMemoryProperties mp{}; VkDeviceSize align = 256;
    VkShaderModule sm = VK_NULL_HANDLE; VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE; VkSampler samp = VK_NULL_HANDLE;
    VkBuffer cbuf = VK_NULL_HANDLE; uint8_t* cmap = nullptr; VkDeviceSize cstride = 256;
    VkBuffer kbuf = VK_NULL_HANDLE; float* kmap = nullptr;
    VkImage oimg = VK_NULL_HANDLE; VkImageView oview = VK_NULL_HANDLE; uint32_t ow = 0, oh = 0; bool olayout = false;
    std::map<std::pair<VkImageView, VkImageView>, VkDescriptorSet> sets;
    Cam ring[3]; int nring = 0;
    uint64_t calls = 0, applied = 0, calibs = 0, calib_at = 0; bool calib_pending = false;
    double acc[kConv] = {}; int accn = 0; int conv = -1; float best = 0, second = 0; char top[160] = {};
    uint8_t res[0x38] = {};
    bool ok = false;
} G;
std::mutex G_mtx;

uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < G.mp.memoryTypeCount; ++i) if ((bits >> i & 1) && (G.mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
bool mkbuf(VkDeviceSize n, VkBuffer* b, void** map) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = n; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (m_vkCreateBuffer(G.dev, &bi, nullptr, b) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; m_vkGetBufferMemoryRequirements(G.dev, *b, &mr);
    uint32_t mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = mt; VkDeviceMemory m;
    if (m_vkAllocateMemory(G.dev, &ai, nullptr, &m) != VK_SUCCESS || m_vkBindBufferMemory(G.dev, *b, m, 0) != VK_SUCCESS) return false;
    return m_vkMapMemory(G.dev, m, 0, n, 0, map) == VK_SUCCESS;
}
bool mkout(uint32_t w, uint32_t h) {
    if (G.oimg && G.ow == w && G.oh == h) return true;   // old image leaks on resize (may still be in flight)
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R16G16_SFLOAT;
    ii.extent = {w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage img; if (m_vkCreateImage(G.dev, &ii, nullptr, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; m_vkGetImageMemoryRequirements(G.dev, img, &mr);
    uint32_t mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (mt == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = mt; VkDeviceMemory m;
    if (m_vkAllocateMemory(G.dev, &ai, nullptr, &m) != VK_SUCCESS || m_vkBindImageMemory(G.dev, img, m, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v; if (m_vkCreateImageView(G.dev, &vi, nullptr, &v) != VK_SUCCESS) return false;
    G.oimg = img; G.oview = v; G.ow = w; G.oh = h; G.olayout = false; G.sets.clear();
    // replacement NVSDK_NGX_Resource_VK: view +0, image +8, subresource range +0x10, format +0x24, w +0x28, h +0x2c, type +0x30 (0 = image), rw +0x34
    memset(G.res, 0, sizeof(G.res));
    memcpy(G.res + 0, &v, 8); memcpy(G.res + 8, &img, 8);
    VkImageSubresourceRange sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; memcpy(G.res + 0x10, &sr, sizeof(sr));
    uint32_t f = (uint32_t)ii.format; memcpy(G.res + 0x24, &f, 4); memcpy(G.res + 0x28, &w, 4); memcpy(G.res + 0x2c, &h, 4);
    vlog("[mvfix] output MV image %ux%u R16G16_SFLOAT", w, h);
    return true;
}
VkDescriptorSet set_for(VkImageView dv, VkImageView mv) {
    auto key = std::make_pair(dv, mv); auto it = G.sets.find(key); if (it != G.sets.end()) return it->second;
    if (G.sets.size() >= 30) return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = G.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &G.dsl;
    VkDescriptorSet ds; if (m_vkAllocateDescriptorSets(G.dev, &ai, &ds) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo di[3] = {{G.samp, dv, VK_IMAGE_LAYOUT_GENERAL}, {G.samp, mv, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, G.oview, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo bc{G.cbuf, 0, kCamBytes}, bk{G.kbuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[5]; for (int i = 0; i < 5; ++i) { w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[i].dstSet = ds; w[i].dstBinding = i; w[i].descriptorCount = 1; }
    w[0].descriptorType = w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &di[0]; w[1].pImageInfo = &di[1];
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[2].pImageInfo = &di[2];
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; w[3].pBufferInfo = &bc;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &bk;
    m_vkUpdateDescriptorSets(G.dev, 5, w, 0, nullptr);
    G.sets[key] = ds; return ds;
}
Cam cam_from(const float* E) {
    Cam c{}; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) c.R[i][j] = E[i * 4 + j];
    for (int i = 0; i < 3; ++i) { double s = 0; for (int j = 0; j < 3; ++j) s += (double)E[12 + j] * c.R[i][j]; c.p[i] = -s; }   // p = -t*R^T
    c.P00 = E[16]; c.P11 = E[21]; c.n = -E[30];
    if (!(c.n > 1e-5f && c.n < 10.f)) c.n = 0.01f;
    c.ok = std::isfinite(c.P00) && std::isfinite(c.P11) && fabsf(c.P00) > 0.05f && fabsf(c.P11) > 0.05f;
    for (int i = 0; i < 3 && c.ok; ++i) { float l = c.R[i][0] * c.R[i][0] + c.R[i][1] * c.R[i][1] + c.R[i][2] * c.R[i][2]; c.ok = fabsf(l - 1.f) < 0.02f; }
    return c;
}
void calib_read() {
    // per sample: 48 errors (px). Median over samples per convention; accumulate (pitch/vertical motion disambiguates over time)
    std::vector<float> col(kGrid * kGrid); float med[kConv];
    for (int c = 0; c < kConv; ++c) {
        int n = 0; for (int s = 0; s < kGrid * kGrid; ++s) { float e = G.kmap[s * kConv + c]; if (std::isfinite(e)) col[n++] = e; }
        if (n < 64) { med[c] = 1e9f; continue; }
        std::nth_element(col.begin(), col.begin() + n / 2, col.begin() + n); med[c] = col[n / 2];
    }
    for (int c = 0; c < kConv; ++c) G.acc[c] += std::min(med[c], 1e4f);
    G.accn++; G.calibs++;
    int idx[kConv]; for (int c = 0; c < kConv; ++c) idx[c] = c;
    std::sort(idx, idx + kConv, [](int a, int b) { return G.acc[a] < G.acc[b]; });
    const int b = idx[0]; G.best = (float)(G.acc[b] / G.accn); G.second = (float)(G.acc[idx[1]] / G.accn);
    auto code = [](int c) { int dt = c % 3, r = c / 3; return dt | ((r & 1) << 2) | ((r & 2) << 2) | ((r & 4) << 2) | ((r & 8) << 2); };
    snprintf(G.top, sizeof(G.top), "conv %02x %.2fpx | %02x %.2f | %02x %.2f | %02x %.2f (this frame best %.2f)", code(idx[0]), G.best, code(idx[1]), G.second,
             code(idx[2]), G.acc[idx[2]] / G.accn, code(idx[3]), G.acc[idx[3]] / G.accn, *std::min_element(med, med + kConv));
    G.conv = (G.accn >= 3 && G.best < 1.5f) ? code(b) : -1;
}
bool read_res(uint8_t* e, VkImageView* dv, VkImageView* mv, uint32_t* w, uint32_t* h) {   // SEH: no C++ objects here
    __try {
        uint8_t* rd = *(uint8_t**)(e + 0x18); uint8_t* rm = *(uint8_t**)(e + 0x20); if (!rd || !rm) return false;
        memcpy(dv, rd, 8); memcpy(mv, rm, 8); memcpy(w, rm + 0x28, 4); memcpy(h, rm + 0x2c, 4); return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool mvfix_init(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, const VkPhysicalDeviceMemoryProperties& mp, VkDeviceSize align) {
    std::lock_guard<std::mutex> lk(G_mtx);
    if (G.ok) return true;
    G.dev = dev; G.mp = mp; G.align = align ? align : 256;
#define MLOAD(n) m_##n = (PFN_##n)gdpa(dev, #n); if (!m_##n) { vlog("[mvfix] missing %s", #n); return false; }
    MV_FUNCS(MLOAD)
    auto fail = [](const char* w) { vlog("[mvfix] init failed: %s", w); return false; };
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = sizeof(kMvFixComp); smi.pCode = kMvFixComp;
    if (m_vkCreateShaderModule(dev, &smi, nullptr, &G.sm) != VK_SUCCESS) return fail("shader");
    VkDescriptorSetLayoutBinding bd[5] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.bindingCount = 5; dli.pBindings = bd;
    if (m_vkCreateDescriptorSetLayout(dev, &dli, nullptr, &G.dsl) != VK_SUCCESS) return fail("set layout");
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &G.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (m_vkCreatePipelineLayout(dev, &pli, nullptr, &G.pl) != VK_SUCCESS) return fail("pipeline layout");
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, G.sm, "main", nullptr}; cpi.layout = G.pl;
    if (m_vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &G.pipe) != VK_SUCCESS) return fail("pipeline");
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 32}};
    VkDescriptorPoolSize ps2{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32};
    VkDescriptorPoolSize all[4] = {ps[0], ps[1], ps[2], ps2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 32; dpi.poolSizeCount = 4; dpi.pPoolSizes = all;
    if (m_vkCreateDescriptorPool(dev, &dpi, nullptr, &G.pool) != VK_SUCCESS) return fail("pool");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (m_vkCreateSampler(dev, &si, nullptr, &G.samp) != VK_SUCCESS) return fail("sampler");
    G.cstride = (kCamBytes + G.align - 1) / G.align * G.align;
    if (!mkbuf(G.cstride * kRing, &G.cbuf, (void**)&G.cmap)) return fail("camera buffer");
    if (!mkbuf((VkDeviceSize)kGrid * kGrid * kConv * 4, &G.kbuf, (void**)&G.kmap)) return fail("calib buffer");
    G.ok = true; vlog("[mvfix] ready (storage align %llu)", (unsigned long long)G.align);
    return true;
}
void mvfix_reset_history() { std::lock_guard<std::mutex> lk(G_mtx); G.nring = 0; }
void mvfix_stats(MvFixStats* s) {
    std::lock_guard<std::mutex> lk(G_mtx);
    s->calls = G.calls; s->applied = G.applied; s->calibs = G.calibs; s->conv = G.conv; s->best_med = G.best; s->second_med = G.second;
    s->calibrated = G.conv >= 0; memcpy(s->top, G.top, sizeof(s->top));
}
void* mvfix_pre(VkCommandBuffer cmd, void* evalp, const float* cam32, int apply) {
    std::lock_guard<std::mutex> lk(G_mtx);
    if (!G.ok) return nullptr;
    G.calls++;
    // results of an earlier calibration dispatch: its frame has long finished after 12 evaluations
    if (G.calib_pending && G.calls >= G.calib_at + 12) { G.calib_pending = false; calib_read(); }
    Cam c = cam_from(cam32);
    if (!c.ok) { G.nring = 0; return nullptr; }
    G.ring[2] = G.ring[1]; G.ring[1] = G.ring[0]; G.ring[0] = c; if (G.nring < 3) G.nring++;
    if (G.nring < 3) return nullptr;
    VkImageView dv = VK_NULL_HANDLE, mvv = VK_NULL_HANDLE; uint32_t w = 0, h = 0;
    if (!read_res((uint8_t*)evalp, &dv, &mvv, &w, &h)) return nullptr;
    if (!dv || !mvv || !w || !h || w > 8192 || h > 8192) return nullptr;
    const bool want_calib = !G.calib_pending && (G.calls % 36 == 0);
    const bool do_apply = apply && G.conv >= 0;
    if (!want_calib && !do_apply) return nullptr;
    if (!mkout(w, h)) return nullptr;
    VkDescriptorSet ds = set_for(dv, mvv); if (!ds) return nullptr;
    // camera block (relative to the current camera)
    const uint32_t slot = (uint32_t)(G.calls % kRing); uint8_t* cb = G.cmap + slot * G.cstride; float blk[56] = {};
    for (int k = 0; k < 3; ++k) {
        const Cam& ck = G.ring[k];
        for (int r = 0; r < 3; ++r) for (int j = 0; j < 3; ++j) blk[(k * 3 + r) * 4 + j] = ck.R[r][j];
        double dp[3] = {c.p[0] - ck.p[0], c.p[1] - ck.p[1], c.p[2] - ck.p[2]};
        for (int j = 0; j < 3; ++j) blk[36 + k * 4 + j] = (float)(dp[0] * ck.R[0][j] + dp[1] * ck.R[1][j] + dp[2] * ck.R[2][j]);
    }
    blk[48] = c.P00; blk[49] = c.P11; blk[50] = c.n;
    int32_t info[4] = {(int32_t)w, (int32_t)h, G.conv < 0 ? 0 : G.conv, 0}; memcpy(&blk[52], info, 16);
    memcpy(cb, blk, sizeof(blk));
    const uint32_t doff = (uint32_t)(slot * G.cstride);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; ib.image = G.oimg; ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; ib.oldLayout = G.olayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    m_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 1, &ib);
    G.olayout = true;
    m_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe);
    m_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pl, 0, 1, &ds, 1, &doff);
    if (want_calib) {
        int32_t pc[2] = {1, kGrid}; m_vkCmdPushConstants(cmd, G.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
        m_vkCmdDispatch(cmd, (kGrid + 7) / 8, (kGrid + 7) / 8, 1);
        G.calib_pending = true; G.calib_at = G.calls;
    }
    if (do_apply) {
        int32_t pc[2] = {0, kGrid}; m_vkCmdPushConstants(cmd, G.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
        m_vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        G.applied++;
    }
    VkMemoryBarrier mb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    m_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb2, 0, nullptr, 0, nullptr);
    return do_apply ? (void*)G.res : nullptr;
}
