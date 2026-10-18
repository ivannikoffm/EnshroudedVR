// EnshroudedVR — PORT-01 stage A: dinput8.dll proxy + Vulkan observation hooks (log only).
// Does NOT change rendering. Entry: game statically imports DINPUT8!DirectInput8Create.
// Vulkan interception: IAT patch of KERNEL32!GetProcAddress in enshrouded.exe ->
//   vkGetInstanceProcAddr / vkGetDeviceProcAddr wrappers -> logged wrappers of key calls.
// Log: <game dir>\enshrouded_vr\vrlog.txt
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <unknwn.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <mutex>
#include <string>
#include <set>
#include <utility>
#include <unordered_map>

#ifdef EVR_PUBLIC
#define EVR_VERSION "1.1.0"
#else
#define EVR_VERSION "1.1.0"
#endif
#include "xr.h"

void cam_probe_start();
std::atomic<float> g_backbuffer_aspect{16.0f / 9.0f};

// ---------------------------------------------------------------- log
static FILE* g_log = nullptr;
static std::string g_vrdir;
static std::mutex g_log_mtx;
static LARGE_INTEGER g_qpf, g_qpc0;

double now_s() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_qpc0.QuadPart) / double(g_qpf.QuadPart);
}

void vlog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    if (!g_log) return;
    fprintf(g_log, "[%9.3f][t%5lu] ", now_s(), GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void open_log() {
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_qpc0);
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe); dir = dir.substr(0, dir.find_last_of("\\/")) + "\\enshrouded_vr";
    CreateDirectoryA(dir.c_str(), nullptr);
    g_vrdir = dir;
    std::string path = dir + "\\vrlog.txt";
    std::string prev = dir + "\\vrlog_prev.txt";
    DeleteFileA(prev.c_str());
    MoveFileA(path.c_str(), prev.c_str());   // keep previous run (crash/hang logs survive one relaunch)
    g_log = fopen(path.c_str(), "w");
    SYSTEMTIME st; GetLocalTime(&st);
    vlog("EnshroudedVR proxy %s  %04d-%02d-%02d %02d:%02d:%02d  exe=%s", EVR_VERSION,
         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, exe);
}

// ---------------------------------------------------------------- real dinput8 forwarding
static HMODULE g_real_dinput8 = nullptr;
static FARPROC real_dinput(const char* name) {
    if (!g_real_dinput8) {
        char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
        std::string p = std::string(sys) + "\\dinput8.dll";
        g_real_dinput8 = LoadLibraryA(p.c_str());
        vlog("real dinput8 loaded from %s -> %p", p.c_str(), (void*)g_real_dinput8);
    }
    return g_real_dinput8 ? GetProcAddress(g_real_dinput8, name) : nullptr;
}

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, IUnknown* u) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    static Fn fn = (Fn)real_dinput("DirectInput8Create");
    vlog("DirectInput8Create(ver=0x%x)", v);
    return fn ? fn(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() { using Fn = HRESULT(WINAPI*)(); static Fn fn = (Fn)real_dinput("DllCanUnloadNow"); return fn ? fn() : S_FALSE; }
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) { using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*); static Fn fn = (Fn)real_dinput("DllGetClassObject"); return fn ? fn(c, r, o) : E_FAIL; }
HRESULT WINAPI Proxy_DllRegisterServer() { using Fn = HRESULT(WINAPI*)(); static Fn fn = (Fn)real_dinput("DllRegisterServer"); return fn ? fn() : E_FAIL; }
HRESULT WINAPI Proxy_DllUnregisterServer() { using Fn = HRESULT(WINAPI*)(); static Fn fn = (Fn)real_dinput("DllUnregisterServer"); return fn ? fn() : E_FAIL; }
}

// ---------------------------------------------------------------- real vulkan pointers
static PFN_vkGetInstanceProcAddr real_GIPA = nullptr;
static PFN_vkGetDeviceProcAddr   real_GDPA = nullptr;
static PFN_vkCreateInstance      real_CreateInstance = nullptr;
static PFN_vkCreateDevice        real_CreateDevice = nullptr;
static PFN_vkDestroyDevice       real_DestroyDevice = nullptr;
void xr_shutdown(const char* why); bool xr_is_down();
static PFN_vkGetPhysicalDeviceProperties real_GetPDProps = nullptr;
static PFN_vkGetDeviceQueue      real_GetDeviceQueue = nullptr;
static PFN_vkGetDeviceQueue2     real_GetDeviceQueue2 = nullptr;
static PFN_vkCreateSwapchainKHR  real_CreateSwapchain = nullptr;
static PFN_vkGetSwapchainImagesKHR real_GetSwapchainImages = nullptr;
static PFN_vkQueuePresentKHR     real_QueuePresent = nullptr;
static PFN_vkQueueSubmit         real_QueueSubmit = nullptr;
static PFN_vkCmdBeginRenderingKHR real_CmdBeginRenderingKHR = nullptr;
static PFN_vkCmdBeginRenderingKHR real_CmdBeginRendering = nullptr;
static PFN_vkCmdBeginRenderPass  real_CmdBeginRenderPass = nullptr;
// ---- S7b HUD capture (Numpad Del/Decimal): log the render-pass sequence of ~3 frames
typedef void (VKAPI_PTR *PFN_EndR)(VkCommandBuffer);
static PFN_EndR real_CmdEndRendering = nullptr, real_CmdEndRenderingKHR = nullptr;
static PFN_vkCreateImageView real_CreateImageView = nullptr;
static PFN_vkCmdDraw real_CmdDraw = nullptr; static PFN_vkCmdDrawIndexed real_CmdDrawIndexed = nullptr;
static PFN_vkCmdDrawIndirect real_CmdDrawIndirect = nullptr; static PFN_vkCmdDrawIndexedIndirect real_CmdDrawIndexedIndirect = nullptr;
static PFN_vkCmdDispatch real_CmdDispatch = nullptr; static PFN_vkCmdDispatchIndirect real_CmdDispatchIndirect = nullptr;
static PFN_vkCmdCopyImage real_CmdCopyImage = nullptr; static PFN_vkCmdBlitImage real_CmdBlitImage = nullptr;
static PFN_vkCmdBindPipeline real_CmdBindPipeline = nullptr;
static PFN_vkCreateGraphicsPipelines real_CreateGraphicsPipelines = nullptr;
// S7b HUD: xr.cpp owns the transparent HUD target; we swap it in for the game's UI pass (1 color att = swapchain image, loadOp LOAD)
static bool is_sc(uint64_t img);
bool xr_hud_target(uint32_t w, uint32_t h, VkImageView* v); void xr_hud_barrier(VkCommandBuffer cb, int after);
static std::mutex g_pb_mtx; static std::unordered_map<uint64_t, uint64_t> g_pblend;   // pipeline -> packed blend state of attachment 0
static std::atomic<int> g_blend_log{0};
static thread_local bool t_hud = false; static thread_local std::set<uint64_t>* t_blends = nullptr;

static std::mutex g_iv_mtx; static std::unordered_map<uint64_t, std::pair<uint64_t, int>> g_views;   // view -> (image, format)
static std::mutex g_sci_mtx; static std::set<uint64_t> g_sc_imgs;
static std::atomic<int> g_cap{0}; static std::atomic<int> g_cap_lines{0};
struct CapTL { bool on = false; uint32_t d = 0, di = 0, ind = 0, pipes = 0; uint32_t disp = 0; char desc[400]; };
static thread_local CapTL t_cap;
void cap_arm() { g_cap_lines.store(0); g_cap.store(4); g_blend_log.store(1); vlog("[cap] ===== armed by hotkey ====="); }
static const VkRenderingInfo* hud_redirect(VkCommandBuffer cb, const VkRenderingInfo* ri, VkRenderingInfo& tmp, VkRenderingAttachmentInfo& att) {
    if (ri->colorAttachmentCount != 1 || !ri->pColorAttachments || ri->pColorAttachments[0].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) return ri;
    uint64_t img = 0; { std::lock_guard<std::mutex> lk(g_iv_mtx); auto it = g_views.find((uint64_t)ri->pColorAttachments[0].imageView); if (it != g_views.end()) img = it->second.first; }
    if (!img || !is_sc(img)) return ri;
    VkImageView v = VK_NULL_HANDLE;
    if (!xr_hud_target(ri->renderArea.extent.width, ri->renderArea.extent.height, &v)) return ri;
    xr_hud_barrier(cb, 0);
    tmp = *ri; att = ri->pColorAttachments[0];
    att.imageView = v; att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color.float32[0] = att.clearValue.color.float32[1] = att.clearValue.color.float32[2] = att.clearValue.color.float32[3] = 0.f;
    att.resolveMode = VK_RESOLVE_MODE_NONE; att.resolveImageView = VK_NULL_HANDLE;
    tmp.pColorAttachments = &att; t_hud = true;
    if (g_blend_log.load()) { if (!t_blends) t_blends = new std::set<uint64_t>(); t_blends->clear(); }
    return &tmp;
}
static void hud_end(VkCommandBuffer cb) {
    xr_hud_barrier(cb, 1); t_hud = false;
    if (t_blends && g_blend_log.exchange(0)) {
        vlog("[hud] UI pass blend states (%zu unique; packed en|srcC|dstC|opC|srcA|dstA|opA|mask):", t_blends->size());
        for (uint64_t b : *t_blends) vlog("[hud]   en=%d srcC=%d dstC=%d opC=%d srcA=%d dstA=%d opA=%d mask=0x%x", (int)(b >> 60), (int)(b >> 52 & 0xff), (int)(b >> 44 & 0xff),
                                          (int)(b >> 36 & 0xff), (int)(b >> 28 & 0xff), (int)(b >> 20 & 0xff), (int)(b >> 12 & 0xff), (int)(b & 0xf));
        t_blends->clear();
    }
}
static bool is_sc(uint64_t img) { std::lock_guard<std::mutex> lk(g_sci_mtx); return g_sc_imgs.count(img) != 0; }
static void cap_begin(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    if (!g_cap.load(std::memory_order_relaxed)) return;
    CapTL& t = t_cap; int n = snprintf(t.desc, sizeof(t.desc), "cb=%p area=%ux%u+%d,%d flags=0x%x layers=%u col=%u", (void*)cb,
        ri->renderArea.extent.width, ri->renderArea.extent.height, ri->renderArea.offset.x, ri->renderArea.offset.y, ri->flags, ri->layerCount, ri->colorAttachmentCount);
    for (uint32_t i = 0; i < ri->colorAttachmentCount && n < 330; ++i) {
        const VkRenderingAttachmentInfo& a = ri->pColorAttachments[i]; uint64_t img = 0; int fmt = 0;
        { std::lock_guard<std::mutex> lk(g_iv_mtx); auto it = g_views.find((uint64_t)a.imageView); if (it != g_views.end()) { img = it->second.first; fmt = it->second.second; } }
        n += snprintf(t.desc + n, sizeof(t.desc) - n, " [img=%llx%s fmt=%d load=%d store=%d]", (unsigned long long)img, is_sc(img) ? " SWAPCHAIN" : "", fmt, a.loadOp, a.storeOp);
    }
    if (n < 380) snprintf(t.desc + n, sizeof(t.desc) - n, " depth=%d", ri->pDepthAttachment && ri->pDepthAttachment->imageView ? 1 : 0);
    if (t.disp && g_cap_lines.fetch_add(1) < 900) vlog("[cap]   (%u dispatches before)", t.disp);
    t.disp = 0; t.on = true; t.d = t.di = t.ind = t.pipes = 0;
}
static void cap_end() {
    CapTL& t = t_cap; if (!t.on) return; t.on = false;
    if (g_cap_lines.fetch_add(1) < 900) vlog("[cap] pass %s draws=%u indexed=%u indirect=%u pipes=%u", t.desc, t.d, t.di, t.ind, t.pipes);
}
static VKAPI_ATTR void VKAPI_CALL Hk_CmdEndRendering(VkCommandBuffer cb) { cap_end(); real_CmdEndRendering(cb); if (t_hud) hud_end(cb); }
static VKAPI_ATTR void VKAPI_CALL Hk_CmdEndRenderingKHR(VkCommandBuffer cb) { cap_end(); real_CmdEndRenderingKHR(cb); if (t_hud) hud_end(cb); }
static VKAPI_ATTR VkResult VKAPI_CALL Hk_CreateImageView(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* v) {
    VkResult r = real_CreateImageView(d, ci, a, v);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> lk(g_iv_mtx); g_views[(uint64_t)*v] = {(uint64_t)ci->image, (int)ci->format}; }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL Hk_CmdDraw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t e) { if (t_cap.on) t_cap.d++; real_CmdDraw(cb, a, b, c, e); }
static VKAPI_ATTR void VKAPI_CALL Hk_CmdDrawIndexed(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t e, uint32_t f) { if (t_cap.on) t_cap.di++; real_CmdDrawIndexed(cb, a, b, c, e, f); }
static VKAPI_ATTR void VKAPI_CALL Hk_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { if (t_cap.on) t_cap.ind += n; real_CmdDrawIndirect(cb, b, o, n, s); }
static VKAPI_ATTR void VKAPI_CALL Hk_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { if (t_cap.on) t_cap.ind += n; real_CmdDrawIndexedIndirect(cb, b, o, n, s); }
static VKAPI_ATTR void VKAPI_CALL Hk_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) { if (g_cap.load(std::memory_order_relaxed)) t_cap.disp++; real_CmdDispatch(cb, x, y, z); }
static VKAPI_ATTR void VKAPI_CALL Hk_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) { if (g_cap.load(std::memory_order_relaxed)) t_cap.disp++; real_CmdDispatchIndirect(cb, b, o); }
static VKAPI_ATTR void VKAPI_CALL Hk_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (t_cap.on) t_cap.pipes++;
    if (t_hud && t_blends && g_blend_log.load(std::memory_order_relaxed)) { std::lock_guard<std::mutex> lk(g_pb_mtx); auto it = g_pblend.find((uint64_t)p); t_blends->insert(it != g_pblend.end() ? it->second : 0xfull << 60); }
    real_CmdBindPipeline(cb, bp, p); }
static VKAPI_ATTR VkResult VKAPI_CALL Hk_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* a, VkPipeline* out) {
    VkResult r = real_CreateGraphicsPipelines(d, c, n, ci, a, out);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> lk(g_pb_mtx);
        for (uint32_t i = 0; i < n; ++i) { const VkPipelineColorBlendStateCreateInfo* bs = ci[i].pColorBlendState;
            if (!out[i] || !bs || !bs->attachmentCount || !bs->pAttachments) continue; const VkPipelineColorBlendAttachmentState& s = bs->pAttachments[0];
            g_pblend[(uint64_t)out[i]] = (uint64_t)(s.blendEnable & 1) << 60 | (uint64_t)(s.srcColorBlendFactor & 0xff) << 52 | (uint64_t)(s.dstColorBlendFactor & 0xff) << 44 |
                (uint64_t)(s.colorBlendOp & 0xff) << 36 | (uint64_t)(s.srcAlphaBlendFactor & 0xff) << 28 | (uint64_t)(s.dstAlphaBlendFactor & 0xff) << 20 | (uint64_t)(s.alphaBlendOp & 0xff) << 12 | (s.colorWriteMask & 0xf); } }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL Hk_CmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dd, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    if (g_cap.load(std::memory_order_relaxed) && (is_sc((uint64_t)dd) || is_sc((uint64_t)s)) && g_cap_lines.fetch_add(1) < 900)
        vlog("[cap] copyImage cb=%p %llx -> %llx%s ext %ux%u", (void*)cb, (unsigned long long)s, (unsigned long long)dd, is_sc((uint64_t)dd) ? " (dst SWAPCHAIN)" : "", n ? r[0].extent.width : 0, n ? r[0].extent.height : 0);
    real_CmdCopyImage(cb, s, sl, dd, dl, n, r);
}
static VKAPI_ATTR void VKAPI_CALL Hk_CmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dd, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) {
    if (g_cap.load(std::memory_order_relaxed) && g_cap_lines.fetch_add(1) < 900)
        vlog("[cap] blit cb=%p %llx -> %llx%s", (void*)cb, (unsigned long long)s, (unsigned long long)dd, is_sc((uint64_t)dd) ? " (dst SWAPCHAIN)" : "");
    real_CmdBlitImage(cb, s, sl, dd, dl, n, r, f);
}

static VkInstance g_instance = VK_NULL_HANDLE;
static VkDevice   g_device = VK_NULL_HANDLE;

// ---------------------------------------------------------------- per-frame stats
static std::atomic<uint64_t> g_frame{0}, g_submits{0}, g_rendering{0}, g_renderpass{0};
static double g_last_stat_t = 0.0; static uint64_t g_last_stat_frame = 0;
static std::mutex g_extent_mtx;
static std::set<std::pair<uint32_t,uint32_t>> g_extents_seen; // distinct render-area sizes

static void note_extent(uint32_t w, uint32_t h, const char* kind) {
    std::lock_guard<std::mutex> lk(g_extent_mtx);
    if (g_extents_seen.size() < 64 && g_extents_seen.insert({w, h}).second)
        vlog("new render area %ux%u via %s (frame %llu)", w, h, kind, (unsigned long long)g_frame.load());
}

// ---------------------------------------------------------------- wrappers
static VKAPI_ATTR VkResult VKAPI_CALL Hk_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    const VkApplicationInfo* ai = ci->pApplicationInfo;
    vlog("vkCreateInstance app='%s' engine='%s' api=%u.%u.%u layers=%u exts=%u",
         ai && ai->pApplicationName ? ai->pApplicationName : "", ai && ai->pEngineName ? ai->pEngineName : "",
         ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 0, ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0,
         ai ? VK_API_VERSION_PATCH(ai->apiVersion) : 0, ci->enabledLayerCount, ci->enabledExtensionCount);
    for (uint32_t i = 0; i < ci->enabledLayerCount; ++i) vlog("  layer %s", ci->ppEnabledLayerNames[i]);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) vlog("  iext  %s", ci->ppEnabledExtensionNames[i]);
    bool surface = false;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) if (!strcmp(ci->ppEnabledExtensionNames[i], "VK_KHR_surface")) surface = true;
    XrInstCI xci; bool use_xr = surface && xr_init(g_vrdir.c_str());   // only the real (presenting) instance, not the 0-ext probe
    if (use_xr) xr_build_instance_ci(ci, xci);
    VkResult r = real_CreateInstance(use_xr ? &xci.ci : ci, a, out);
    if (r != VK_SUCCESS && use_xr) { vlog("[xr] vkCreateInstance with runtime exts failed (%d) -> retry plain", r); r = real_CreateInstance(ci, a, out); use_xr = false; }
    if (r == VK_SUCCESS && use_xr) xr_on_instance(*out, real_GIPA);
    if (r == VK_SUCCESS) {
        g_instance = *out;
        real_GetPDProps = (PFN_vkGetPhysicalDeviceProperties)real_GIPA(g_instance, "vkGetPhysicalDeviceProperties");
    }
    vlog("vkCreateInstance -> %d instance=%p", r, (void*)(out ? *out : nullptr));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL Hk_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    if (real_GetPDProps) {
        VkPhysicalDeviceProperties p{}; real_GetPDProps(pd, &p);
        vlog("vkCreateDevice gpu='%s' api=%u.%u.%u", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    }
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i)
        vlog("  queue family=%u count=%u", ci->pQueueCreateInfos[i].queueFamilyIndex, ci->pQueueCreateInfos[i].queueCount);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) vlog("  dext  %s", ci->ppEnabledExtensionNames[i]);
    XrDevCI xdc; bool use_xr = xr_active() && xr_build_device_ci(pd, ci, xdc);
    VkResult r = real_CreateDevice(pd, use_xr ? &xdc.ci : ci, a, out);
    if (r != VK_SUCCESS && use_xr) { vlog("[xr] vkCreateDevice with runtime exts failed (%d) -> retry plain", r); r = real_CreateDevice(pd, ci, a, out); use_xr = false; }
    if (r == VK_SUCCESS && use_xr) xr_on_device(*out, pd);
    if (r == VK_SUCCESS && g_device == VK_NULL_HANDLE) g_device = *out;
    vlog("vkCreateDevice -> %d device=%p", r, (void*)(out ? *out : nullptr));
    return r;
}


static VKAPI_ATTR void VKAPI_CALL Hk_DestroyDevice(VkDevice d, const VkAllocationCallbacks* a) {
    vlog("vkDestroyDevice %p (game exit / device reset)", (void*)d);
    if (d == g_device) {
        auto wait = real_GDPA ? (PFN_vkDeviceWaitIdle)real_GDPA(d, "vkDeviceWaitIdle") : nullptr;
        if (wait) wait(d);
        xr_shutdown("vkDestroyDevice");
        g_device = VK_NULL_HANDLE;
    }
    real_DestroyDevice(d, a);
}

static VKAPI_ATTR void VKAPI_CALL Hk_GetDeviceQueue(VkDevice d, uint32_t fam, uint32_t idx, VkQueue* q) {
    real_GetDeviceQueue(d, fam, idx, q);
    xr_note_queue(*q, fam, idx);
    vlog("vkGetDeviceQueue family=%u index=%u -> %p", fam, idx, (void*)*q);
}
static VKAPI_ATTR void VKAPI_CALL Hk_GetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2* qi, VkQueue* q) {
    real_GetDeviceQueue2(d, qi, q);
    xr_note_queue(*q, qi->queueFamilyIndex, qi->queueIndex);
    vlog("vkGetDeviceQueue2 family=%u index=%u -> %p", qi->queueFamilyIndex, qi->queueIndex, (void*)*q);
}

void xr_window_force();
static VKAPI_ATTR VkResult VKAPI_CALL Hk_CreateSwapchain(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    xr_window_force();
    VkSwapchainCreateInfoKHR mci = *ci;
    if (xr_active()) mci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;    // needed to copy the frame into the XR swapchain
    VkResult r = real_CreateSwapchain(d, &mci, a, out);
    if (r != VK_SUCCESS && mci.imageUsage != ci->imageUsage) { vlog("[xr] swapchain with TRANSFER_SRC failed (%d) -> retry plain", r); mci = *ci; r = real_CreateSwapchain(d, ci, a, out); }
    if (r == VK_SUCCESS) xr_on_swapchain(*out, ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat, 0, nullptr);
    if (r == VK_SUCCESS && ci->imageExtent.height) g_backbuffer_aspect = float(ci->imageExtent.width) / float(ci->imageExtent.height);
    vlog("vkCreateSwapchainKHR %ux%u fmt=%d cs=%d usage=0x%x minImages=%u present=%d old=%p -> %d sc=%p",
         ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat, ci->imageColorSpace, mci.imageUsage,
         ci->minImageCount, ci->presentMode, (void*)ci->oldSwapchain, r, (void*)(out ? *out : nullptr));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL Hk_GetSwapchainImages(VkDevice d, VkSwapchainKHR sc, uint32_t* n, VkImage* imgs) {
    VkResult r = real_GetSwapchainImages(d, sc, n, imgs);
    if (imgs) vlog("vkGetSwapchainImagesKHR sc=%p count=%u -> %d", (void*)sc, *n, r);
    if (imgs && (r == VK_SUCCESS || r == VK_INCOMPLETE)) { std::lock_guard<std::mutex> lk(g_sci_mtx); for (uint32_t i = 0; i < *n; ++i) g_sc_imgs.insert((uint64_t)imgs[i]); }
    if (imgs && (r == VK_SUCCESS || r == VK_INCOMPLETE)) xr_on_swapchain(sc, 0, 0, VK_FORMAT_UNDEFINED, *n, imgs);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL Hk_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    g_submits.fetch_add(1, std::memory_order_relaxed);
    return real_QueueSubmit(q, n, s, f);
}

static VKAPI_ATTR void VKAPI_CALL Hk_CmdBeginRenderingKHR(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    g_rendering.fetch_add(1, std::memory_order_relaxed);
    note_extent(ri->renderArea.extent.width, ri->renderArea.extent.height, "BeginRendering");
    cap_begin(cb, ri);
    VkRenderingInfo tmp; VkRenderingAttachmentInfo att;
    real_CmdBeginRenderingKHR(cb, hud_redirect(cb, ri, tmp, att));
}
static VKAPI_ATTR void VKAPI_CALL Hk_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    g_rendering.fetch_add(1, std::memory_order_relaxed);
    note_extent(ri->renderArea.extent.width, ri->renderArea.extent.height, "BeginRendering(core)");
    cap_begin(cb, ri);
    VkRenderingInfo tmp; VkRenderingAttachmentInfo att;
    real_CmdBeginRendering(cb, hud_redirect(cb, ri, tmp, att));
}
static VKAPI_ATTR void VKAPI_CALL Hk_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* rp, VkSubpassContents c) {
    g_renderpass.fetch_add(1, std::memory_order_relaxed);
    note_extent(rp->renderArea.extent.width, rp->renderArea.extent.height, "BeginRenderPass");
    real_CmdBeginRenderPass(cb, rp, c);
}

static void log_modules_once() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{ sizeof(me) };
    vlog("--- loaded modules ---");
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        char path[MAX_PATH * 2]; WideCharToMultiByte(CP_UTF8, 0, me.szExePath, -1, path, sizeof(path), 0, 0);
        vlog("  %p %8lu %s", (void*)me.modBaseAddr, me.modBaseSize, path);
    }
    CloseHandle(snap);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hk_QueuePresent(VkQueue q, const VkPresentInfoKHR* pi) {
    uint64_t f = g_frame.fetch_add(1) + 1;
    if (f == 1) {
        vlog("first vkQueuePresentKHR queue=%p swapchains=%u imageIndex=%u", (void*)q, pi->swapchainCount,
             pi->swapchainCount ? pi->pImageIndices[0] : 0);
        log_modules_once();
        // cam_probe_start();   // S3 probe hotkeys disabled: S5 uses Insert/Home/PageUp/PageDown/End/Pause
    }
    double t = now_s();
    if (t - g_last_stat_t >= 5.0) {
        uint64_t df = f - g_last_stat_frame;
        vlog("stats frame=%llu fps=%.1f per-frame: submits=%.1f beginRendering=%.1f beginRenderPass=%.1f queue=%p",
             (unsigned long long)f, df / (t - g_last_stat_t),
             double(g_submits.exchange(0)) / df, double(g_rendering.exchange(0)) / df,
             double(g_renderpass.exchange(0)) / df, (void*)q);
        g_last_stat_t = t; g_last_stat_frame = f;
    }
    {   // S7b capture hotkey: Numpad Del (VK_DECIMAL) -> 3 frames of pass logging
        int c = g_cap.load();
        if (c > 0) { if (--c == 0) vlog("[cap] ===== end (frame %llu) =====", (unsigned long long)f); g_cap.store(c); }
        if (g_cap.load() && pi->swapchainCount) vlog("[cap] --- present imageIndex %u (frame %llu)", pi->pImageIndices[0], (unsigned long long)f);
    }
    VkPresentInfoKHR mpi = *pi;
    VkSemaphore xs = xr_on_present(q, pi);
    if (xs) { mpi.waitSemaphoreCount = 1; mpi.pWaitSemaphores = &xs; }
    VkResult r = real_QueuePresent(q, &mpi);
    if (r != VK_SUCCESS) vlog("vkQueuePresentKHR frame=%llu -> %d", (unsigned long long)f, r);
    return r;
}

// ---------------------------------------------------------------- proc-addr routing
static PFN_vkVoidFunction route(const char* name, PFN_vkVoidFunction real);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hk_GetDeviceProcAddr(VkDevice d, const char* name) {
    return route(name, real_GDPA(d, name));
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hk_GetInstanceProcAddr(VkInstance inst, const char* name) {
    PFN_vkVoidFunction real = real_GIPA(inst, name);
    return route(name, real);
}

#define ROUTE(NAME, REALVAR, HOOK) \
    if (!strcmp(name, NAME)) { if (!real) return nullptr; REALVAR = (decltype(REALVAR))real; \
        static bool once = false; if (!once) { once = true; vlog("hooked %s", NAME); } \
        return (PFN_vkVoidFunction)&HOOK; }

static PFN_vkVoidFunction route(const char* name, PFN_vkVoidFunction real) {
    if (!name) return real;
    if (!strcmp(name, "vkGetInstanceProcAddr")) { return (PFN_vkVoidFunction)&Hk_GetInstanceProcAddr; }
    ROUTE("vkGetDeviceProcAddr",      real_GDPA,               Hk_GetDeviceProcAddr)
    ROUTE("vkCreateInstance",         real_CreateInstance,     Hk_CreateInstance)
    ROUTE("vkCreateDevice",           real_CreateDevice,       Hk_CreateDevice)
    ROUTE("vkDestroyDevice",          real_DestroyDevice,      Hk_DestroyDevice)
    ROUTE("vkGetDeviceQueue",         real_GetDeviceQueue,     Hk_GetDeviceQueue)
    ROUTE("vkGetDeviceQueue2",        real_GetDeviceQueue2,    Hk_GetDeviceQueue2)
    ROUTE("vkCreateSwapchainKHR",     real_CreateSwapchain,    Hk_CreateSwapchain)
    ROUTE("vkGetSwapchainImagesKHR",  real_GetSwapchainImages, Hk_GetSwapchainImages)
    ROUTE("vkQueuePresentKHR",        real_QueuePresent,       Hk_QueuePresent)
    ROUTE("vkQueueSubmit",            real_QueueSubmit,        Hk_QueueSubmit)
    ROUTE("vkCmdBeginRenderingKHR",   real_CmdBeginRenderingKHR, Hk_CmdBeginRenderingKHR)
    ROUTE("vkCmdBeginRendering",      real_CmdBeginRendering,  Hk_CmdBeginRendering)
    ROUTE("vkCmdBeginRenderPass",     real_CmdBeginRenderPass, Hk_CmdBeginRenderPass)
    ROUTE("vkCmdEndRendering",        real_CmdEndRendering,    Hk_CmdEndRendering)
    ROUTE("vkCmdEndRenderingKHR",     real_CmdEndRenderingKHR, Hk_CmdEndRenderingKHR)
    ROUTE("vkCreateImageView",        real_CreateImageView,    Hk_CreateImageView)
    ROUTE("vkCmdDraw",                real_CmdDraw,            Hk_CmdDraw)
    ROUTE("vkCmdDrawIndexed",         real_CmdDrawIndexed,     Hk_CmdDrawIndexed)
    ROUTE("vkCmdDrawIndirect",        real_CmdDrawIndirect,    Hk_CmdDrawIndirect)
    ROUTE("vkCmdDrawIndexedIndirect", real_CmdDrawIndexedIndirect, Hk_CmdDrawIndexedIndirect)
    ROUTE("vkCmdDispatch",            real_CmdDispatch,        Hk_CmdDispatch)
    ROUTE("vkCmdDispatchIndirect",    real_CmdDispatchIndirect, Hk_CmdDispatchIndirect)
    ROUTE("vkCmdCopyImage",           real_CmdCopyImage,       Hk_CmdCopyImage)
    ROUTE("vkCmdBlitImage",           real_CmdBlitImage,       Hk_CmdBlitImage)
    ROUTE("vkCmdBindPipeline",        real_CmdBindPipeline,    Hk_CmdBindPipeline)
    ROUTE("vkCreateGraphicsPipelines", real_CreateGraphicsPipelines, Hk_CreateGraphicsPipelines)
    return real;
}

// ---------------------------------------------------------------- GetProcAddress IAT hook
using PFN_GPA = FARPROC(WINAPI*)(HMODULE, LPCSTR);
static PFN_GPA real_GetProcAddress = nullptr;
static std::atomic<int> g_vk_gpa_logs{0};

static FARPROC WINAPI Hk_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = real_GetProcAddress(m, name);
    if (((ULONG_PTR)name >> 16) == 0) return p;          // ordinal import
    if (name[0] != 'v' || name[1] != 'k') return p;
    char modname[MAX_PATH] = {}; GetModuleFileNameA(m, modname, MAX_PATH);
    const char* base = strrchr(modname, '\\'); base = base ? base + 1 : modname;
    if (g_vk_gpa_logs.fetch_add(1) < 40) vlog("GetProcAddress(%s, %s) -> %p", base, name, (void*)p);
    if (!p) return p;
    if (!strcmp(name, "vkGetInstanceProcAddr")) {
        if (!real_GIPA) { real_GIPA = (PFN_vkGetInstanceProcAddr)p; vlog("captured real vkGetInstanceProcAddr=%p from %s", (void*)p, base); }
        return (FARPROC)&Hk_GetInstanceProcAddr;
    }
    return (FARPROC)route(name, (PFN_vkVoidFunction)p);
}

static bool patch_iat(HMODULE mod, const char* dll, const char* func, void* hook, void** orig) {
    auto base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll)) continue;
        auto oft = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func)) continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *orig = (void*)ft->u1.Function;
            ft->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}


// v1.0.7: virtual menu cursor (see xr.cpp cur_to_virtual)
bool cur_to_virtual(POINT& p); bool cur_to_physical(POINT& p);
typedef BOOL (WINAPI* PFN_GCP)(LPPOINT); typedef BOOL (WINAPI* PFN_SCP)(int, int);
static PFN_GCP real_GetCursorPos = nullptr; static PFN_SCP real_SetCursorPos = nullptr;
static BOOL WINAPI Hk_GetCursorPos(LPPOINT p) { BOOL r = real_GetCursorPos(p); if (r && p) cur_to_virtual(*p); return r; }
static BOOL WINAPI Hk_SetCursorPos(int x, int y) { POINT q{x, y}; cur_to_physical(q); return real_SetCursorPos(q.x, q.y); }

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        open_log();
        bool ok = patch_iat(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetProcAddress",
                            (void*)&Hk_GetProcAddress, (void**)&real_GetProcAddress);
        if (!real_GetProcAddress) real_GetProcAddress = (PFN_GPA)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress");
        vlog("IAT hook KERNEL32!GetProcAddress: %s (orig=%p)", ok ? "OK" : "FAILED", (void*)real_GetProcAddress);
        { bool a = patch_iat(GetModuleHandleA(nullptr), "USER32.dll", "GetCursorPos", (void*)&Hk_GetCursorPos, (void**)&real_GetCursorPos);
          bool b = patch_iat(GetModuleHandleA(nullptr), "USER32.dll", "SetCursorPos", (void*)&Hk_SetCursorPos, (void**)&real_SetCursorPos);
          vlog("IAT hook USER32!GetCursorPos %s, SetCursorPos %s (menu cursor reach)", a ? "OK" : "FAILED", b ? "OK" : "FAILED"); }
        vlog("vulkan-1.dll already loaded: %s", GetModuleHandleA("vulkan-1.dll") ? "yes" : "no");
    } else if (reason == DLL_PROCESS_DETACH) {
        vlog("detach, frames=%llu xr_down=%d process_exit=%d", (unsigned long long)g_frame.load(), (int)xr_is_down(), reserved != nullptr);
        if (reserved && xr_active() && !xr_is_down()) {   // exit without vkDestroyDevice: a live XR session would hang ExitProcess
            vlog("[xr] process exit with a live session -> TerminateProcess (avoid the frozen app in the headset)");
            if (g_log) { fflush(g_log); }
            TerminateProcess(GetCurrentProcess(), 0);
        }
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
