// EnshroudedVR — S4 OpenXR pipe. See xr.h. All XR frame calls happen on the game's present thread.
// Frame copy: game swapchain image (R8G8B8A8_UNORM, sRGB-encoded bytes) --vkCmdCopyImage (bitwise)--> XR swapchain
// R8G8B8A8_SRGB => runtime decodes correctly. Shown as a world-locked quad ("virtual screen") in LOCAL space.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include "xr.h"
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

void vlog(const char* fmt, ...);
double now_s();
bool camhook_install(); void camhook_set(float, float, float); void camhook_stats(uint64_t*, uint64_t*);
bool camhook_eye_ok(); void camhook_set_eye(float); uint64_t camhook_eye_hits(); void camhook_set_write(int, uint32_t, float);
bool camhook_view_ok(); void camhook_set_view(bool, const float[3][3], float, const float[3], uint64_t); void camhook_view_stats(uint64_t*, uint64_t*);
uint64_t camhook_view_used_seq(); void camhook_set_pull(float);
bool camhook_fov_ok(); void camhook_set_render_fov(float); uint64_t camhook_fov_hits(); uint64_t camhook_fov_rec(); uint64_t camhook_proj_hits(); uint64_t camhook_proj_dst(); uint64_t camhook_src_hits();
void hwbp_trace(const uintptr_t* addr, const int* len, const char* const* names, int n, double secs);
struct CamFields { uint32_t yaw, pitch, roll, pivot, eyepos, basis, offset; const char* build; uint32_t aspect, fov, fov_deg_setting; uint32_t fov_copies[3]; };
const CamFields* camhook_fields();
void cap_arm();
bool osd_render(const wchar_t* text, int w, int h, uint8_t* out);
std::atomic<int> g_hud_redirect{0}; std::atomic<uint64_t> g_hud_passes{0};

namespace {
#define XR_FUNCS(X) X(xrDestroyInstance) X(xrGetSystem) X(xrGetSystemProperties) X(xrCreateSession) X(xrDestroySession) \
    X(xrBeginSession) X(xrEndSession) X(xrPollEvent) X(xrCreateReferenceSpace) X(xrEnumerateSwapchainFormats) \
    X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) \
    X(xrGetVulkanInstanceExtensionsKHR) X(xrGetVulkanDeviceExtensionsKHR) X(xrGetVulkanGraphicsDeviceKHR) \
    X(xrGetVulkanGraphicsRequirementsKHR) X(xrResultToString) X(xrLocateViews)
#define DECL(n) PFN_##n n = nullptr;
XR_FUNCS(DECL)
PFN_xrGetInstanceProcAddr xrGIPA = nullptr;

#define VK_FUNCS(X) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCreateFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateSemaphore) X(vkQueueSubmit) \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCmdCopyBufferToImage) \
    X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView)
#define VDECL(n) PFN_##n p_##n = nullptr;
VK_FUNCS(VDECL)

constexpr int kSlots = 6;
constexpr int kNumActs = 24, kOW = 768, kOH = 96;   // hotkey actions; OSD panel texture
struct Slot { VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; VkSemaphore sem = VK_NULL_HANDLE; bool used = false; };
struct GameSc { uint32_t w = 0, h = 0; VkFormat fmt = VK_FORMAT_UNDEFINED; std::vector<VkImage> imgs; };

struct State {
    bool active = false, session_failed = false, running = false, disabled_runtime = false;
    XrInstance inst = XR_NULL_HANDLE; XrSystemId sys = XR_NULL_SYSTEM_ID; XrSession sess = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE; XrSessionState sstate = XR_SESSION_STATE_UNKNOWN;
    XrSwapchain xsc = XR_NULL_HANDLE; uint32_t xw = 0, xh = 0; int64_t xfmt = 0; bool xcopy = true;
    std::vector<VkImage> ximgs;
    XrSwapchain xsc1 = XR_NULL_HANDLE; std::vector<VkImage> ximgs1;
    // S8a pair-lock: second left-eye swapchain (L of pair p goes to xscL[p&1]) so the previous complete pair stays submittable
    XrSwapchain xsc2 = XR_NULL_HANDLE; std::vector<VkImage> ximgs2;
    int pair_lock = 1; XrView pair_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint64_t hpair[16] = {};
    uint64_t Lpid = ~0ull; XrSwapchain Lsc = XR_NULL_HANDLE; XrPosef Lpose = {}; bool cvalid = false; XrSwapchain cLsc = XR_NULL_HANDLE; XrPosef cLpose = {}, cRpose = {};
    uint64_t pairs_ok = 0, pairs_broken = 0, pair_holds = 0;
    // S8a-2: one XR frame per PAIR -> the L present skips xrWaitFrame/Begin/End, game free-runs at 2x HMD rate
    int layer_eyefov = 1; bool efov_ok = false; XrFovf efov[2] = {};
    int mat_proj = 1; float mat_tu = 0.f, mat_tr = 0.f; int cam_dist_pct = 100; float cam_dist = 0.f; int pair_rate = 1; bool pairing_prev = false, cur_last = true; XrTime last_pdt = 0; XrDuration last_period = 13888888; uint64_t xr_skips = 0, xr_ends = 0;   // S6: right-eye swapchain (xsc = left / mono)
    VkInstance vki = VK_NULL_HANDLE; PFN_vkGetInstanceProcAddr gipa = nullptr; VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE; VkCommandPool pool = VK_NULL_HANDLE; Slot slots[kSlots]; int slot = 0;
    std::map<VkQueue, std::pair<uint32_t, uint32_t>> queues; VkQueue q = VK_NULL_HANDLE; uint32_t qfam = 0, qidx = 0;
    std::map<VkSwapchainKHR, GameSc> scs; std::mutex sc_mtx;
    // cursor layer (system/hardware cursor is composited by Windows, not in the swapchain image)
    XrSwapchain csc = XR_NULL_HANDLE; std::vector<VkImage> cimgs; bool cfailed = false, cready = false, cdirty = false;
    VkBuffer cbuf = VK_NULL_HANDLE; uint8_t* cmap = nullptr; HCURSOR ch = nullptr; int chx = 0, chy = 0;
    std::vector<uint8_t> cpx = std::vector<uint8_t>(64 * 64 * 4);
    bool cursor_anywhere = getenv("EVR_CURSOR_ANYWHERE") != nullptr;   // test_xr: console app has no game window
    // S5 head tracking
    std::string dir;
    bool tracking = true, hook_tried = false, hook_ok = false, centered = false;
    int ys = 1, ps = 1, rsg = 0, lag = 1; float yaw0 = 0.f;
    XrView hist[16][2] = {}; bool hvalid[16] = {}; uint64_t hn = 0;
    // S6 alternate-eye rendering
    int heye[16] = {}; int stereo = 1, ipd_pct = 100; XrPosef epose[2] = {}; bool evalid[2] = {};
    uint64_t stereo_frames = 0, eye_hits_prev = 0, eye_anom = 0, eye_calls = 0;
    // S7 FOV: fov_off = CamObj float field with the game's projection FOV (rad), found by PageDown dump-diff; override = HMD FOV
    uint32_t fov_off = 0, fov_off_ini = 0; int fov_mode = 0 /*0 = fov field only, 1 = + setting + copies*/; int fov_axis = 0 /*0 vertical, 1 horizontal*/, fov_ovr = 0, fov_pct = 100;
    float hfov[16] = {}; bool fov_written = false; float fov_game = 1.1345f, hmd_tu = 1.f, hmd_tr = 1.f; bool hmd_fov_ok = false;
    std::vector<uint8_t> dump_prev; int dump_n = 0;
    // S8 head mode: 1 = view (head rotates the view about the camera, position from mouse angles), 0 = orbit (legacy: head -> orbit angles)
    int head_mode = 1, head_pos = 0, head_look = 0; bool pos_centered = false;   // head_look 0 = mouse-only stereo screen (world-locked), 1 = head rotates the view
    // S7a deterministic frame tagging (view mode): seq of the pose the builder consumed, sampled at every present
    uint64_t hseq[16] = {}, pres_seq[16] = {}, pn = 0, tag_ok = 0, tag_fb = 0, seq_age_sum = 0, seq_age_n = 0; XrVector3f pos0 = {}; float fov_saved = 0.f; uint64_t va_prev = 0, vb_prev = 0; float ipd = 0.063f;
    float cur_yaw = 0, cur_pitch = 0, cur_roll = 0, fovy = 1.1345f, faspect = 16.f / 9.f; uint64_t proj_frames = 0;
    // S7b HUD: game's UI pass redirected into himg (transparent), shown as a head-locked quad layer
    int hud_mode = 0; int hud_dist_cm = 150; int hud_sync = 1; uint64_t hud_skips = 0; XrSpace view = XR_NULL_HANDLE; XrSwapchain hsc = XR_NULL_HANDLE; std::vector<VkImage> himgs;
    VkImage himg = VK_NULL_HANDLE; VkImageView hview = VK_NULL_HANDLE; uint32_t hw = 0, hh = 0; bool hfailed = false, hready = false;
    uint64_t hud_copies = 0, hud_seen = 0;
    // public build: rebindable keys, in-headset OSD, live ini reload, safe flat fallback
    int keys[kNumActs] = {}; uint64_t ini_mtime = 0; bool engaged = false; volatile bool crashed = false;
    XrSwapchain osc = XR_NULL_HANDLE; std::vector<VkImage> oimgs; VkBuffer obuf = VK_NULL_HANDLE; uint8_t* omap = nullptr;
    bool ofailed = false, oready = false, odirty = false; double ountil = 0; std::vector<uint8_t> opx = std::vector<uint8_t>(kOW * kOH * 4);
    bool key_prev[32] = {}; bool pose_log = false; double t_pose = 0;
    // stats
    double t_stat = 0, wait_ms = 0; uint64_t frames = 0, rendered = 0, presents = 0; int errors = 0;
} g;

const char* rs(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (xrResultToString && g.inst && XR_SUCCEEDED(xrResultToString(g.inst, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r); return buf;
}
#define XRCHK(call) ([&]{ XrResult _r = (call); if (XR_FAILED(_r)) { vlog("[xr] %s FAILED: %s", #call, rs(_r)); \
    if (_r == XR_ERROR_SESSION_LOST || _r == XR_ERROR_INSTANCE_LOST || _r == XR_ERROR_RUNTIME_FAILURE) { g.active = false; vlog("[xr] headset/runtime lost -> flat mode"); } \
    if (++g.errors > 50) g.active = false; } return _r; }())

void split_append(const char* s, std::deque<std::string>& store) {
    std::string cur;
    for (const char* p = s;; ++p) {
        if (*p == ' ' || *p == 0) { if (!cur.empty()) store.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
}
void merge_exts(uint32_t n, const char* const* game, std::deque<std::string>& store, std::vector<const char*>& out, const char* tag) {
    std::deque<std::string> add; std::swap(add, store);
    for (uint32_t i = 0; i < n; ++i) store.push_back(game[i]);
    for (auto& e : add) {
        bool dup = false; for (auto& s : store) if (s == e) dup = true;
        if (!dup) { store.push_back(e); vlog("[xr]   +%s %s", tag, e.c_str()); }
    }
    out.clear(); for (auto& s : store) out.push_back(s.c_str());
}

void poll_events();
void save_ini(); void load_keys(const char* f); void save_keys(const char* f); uint64_t ini_mtime(); void osd(const char* fmt, ...);
constexpr float kScreenDist = 2.0f, kScreenW = 2.4f, kScreenH = kScreenW * 9.f / 16.f;   // v1.0.6: screen keeps the 16:9 HEIGHT; width follows the frame aspect
constexpr int kCur = 64;

// Render HCURSOR into straight-alpha RGBA (64x64): draw on black and on white, alpha = 255 - (white - black).
bool capture_cursor(HCURSOR hc, std::vector<uint8_t>& out, int& hx, int& hy) {
    ICONINFO ii{}; if (!GetIconInfo(hc, &ii)) return false;
    hx = (int)ii.xHotspot; hy = (int)ii.yHotspot;
    if (ii.hbmMask) DeleteObject(ii.hbmMask); if (ii.hbmColor) DeleteObject(ii.hbmColor);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = kCur; bi.bmiHeader.biHeight = -kCur;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    uint8_t *pb = nullptr, *pw = nullptr;
    HBITMAP bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&pb, nullptr, 0);
    HBITMAP bw = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&pw, nullptr, 0);
    bool ok = bb && bw;
    if (ok) {
        memset(pb, 0, kCur * kCur * 4); memset(pw, 255, kCur * kCur * 4);
        HGDIOBJ old = SelectObject(dc, bb); DrawIconEx(dc, 0, 0, hc, 0, 0, 0, nullptr, DI_NORMAL);
        SelectObject(dc, bw); DrawIconEx(dc, 0, 0, hc, 0, 0, 0, nullptr, DI_NORMAL); SelectObject(dc, old);
        GdiFlush();
        for (int i = 0; i < kCur * kCur; ++i) {
            const uint8_t* b = pb + i * 4; const uint8_t* w = pw + i * 4; uint8_t* o = out.data() + i * 4;
            int a = 255 - ((w[0] - b[0]) + (w[1] - b[1]) + (w[2] - b[2])) / 3; a = a < 0 ? 0 : a > 255 ? 255 : a;
            for (int c = 0; c < 3; ++c) { int v = a ? b[2 - c] * 255 / a : 0; o[c] = (uint8_t)(v > 255 ? 255 : v); }   // BGRA -> RGB
            o[3] = (uint8_t)a;
        }
    }
    if (bb) DeleteObject(bb); if (bw) DeleteObject(bw); DeleteDC(dc);
    return ok;
}

bool make_cursor_resources() {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB; ci.sampleCount = 1; ci.width = kCur; ci.height = kCur; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(XRCHK(xrCreateSwapchain(g.sess, &ci, &g.csc)))) return false;
    uint32_t n = 0; xrEnumerateSwapchainImages(g.csc, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> im(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g.csc, n, &n, (XrSwapchainImageBaseHeader*)im.data());
    for (auto& i : im) g.cimgs.push_back(i.image);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = (VkDeviceSize)kCur * kCur * 4 * kSlots; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateBuffer(g.dev, &bci, nullptr, &g.cbuf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; p_vkGetBufferMemoryRequirements(g.dev, g.cbuf, &mr);
    auto gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)g.gipa(g.vki, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp; gmp(g.pd, &mp);
    uint32_t mt = UINT32_MAX; const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; ++i) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) mt = i;
    if (mt == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory mem; if (p_vkAllocateMemory(g.dev, &mai, nullptr, &mem) != VK_SUCCESS) return false;
    if (p_vkBindBufferMemory(g.dev, g.cbuf, mem, 0) != VK_SUCCESS || p_vkMapMemory(g.dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&g.cmap) != VK_SUCCESS) return false;
    vlog("[xr] cursor layer resources: %ux%u swapchain, %u images", kCur, kCur, n);
    return true;
}

// where is the cursor on the virtual screen? returns false if hidden / not over the game window
bool cur_to_virtual_i(POINT& p);
bool cursor_state(float& px_x, float& px_y, float& m_per_px) {
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return false;
    RECT rc; POINT p = ci.ptScreenPos;
    if (g.cursor_anywhere) {
        HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY); MONITORINFO mi{sizeof(mi)}; GetMonitorInfo(m, &mi);
        rc = mi.rcMonitor; p.x -= rc.left; p.y -= rc.top; rc.right -= rc.left; rc.bottom -= rc.top; rc.left = rc.top = 0;
    } else {
        HWND fw = GetForegroundWindow(); DWORD pid = 0; GetWindowThreadProcessId(fw, &pid);
        if (pid != GetCurrentProcessId()) return false;
        cur_to_virtual_i(p); ScreenToClient(fw, &p); GetClientRect(fw, &rc);
    }
    if (rc.right <= 0 || rc.bottom <= 0 || p.x < 0 || p.y < 0 || p.x >= rc.right || p.y >= rc.bottom) return false;
    if (ci.hCursor != g.ch) {
        int hx = 0, hy = 0;
        if (capture_cursor(ci.hCursor, g.cpx, hx, hy)) { g.ch = ci.hCursor; g.chx = hx; g.chy = hy; g.cdirty = true; }
    }
    m_per_px = kScreenH / float(rc.bottom);
    px_x = float(p.x - g.chx); px_y = float(p.y - g.chy);     // top-left of the cursor bitmap, window pixels
    (void)rc; return g.ch != nullptr;
}


// ---- v1.0.4 S9c: force the game window's client size (square frame for asymmetric HMDs; no driver needed).
// vr.ini force_window=WxH (0 = off). Subclass the game window: WM_GETMINMAXINFO lifts the screen-size clamp,
// WM_WINDOWPOSCHANGING pins every resize to the target; the game then recreates its swapchain at that size.
static HWND g_gw = nullptr; static WNDPROC g_gw_old = nullptr; static int g_fw = 0, g_fh = 0;
static void gw_outer(HWND h, int& ow, int& oh, int* ox = nullptr, int* oy = nullptr) {
    RECT r{0, 0, g_fw, g_fh}; AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE));
    ow = r.right - r.left; oh = r.bottom - r.top;
    // v1.0.5: client CENTRE = monitor centre. The game warps the hidden cursor to the window centre for mouse-look;
    // an off-screen centre (tall window at y=0) makes the cursor flicker visible -> mod flips to the flat menu screen.
    if (ox && oy) { MONITORINFO mi{sizeof(mi)}; GetMonitorInfo(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        const int mx = (mi.rcMonitor.left + mi.rcMonitor.right) / 2, my = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
        *ox = mx - g_fw / 2 + r.left; *oy = my - g_fh / 2 + r.top; } }
// v1.1.0: only a captioned (Windowed) game window is resized; Fullscreen (WS_POPUP-like, no caption) is left to the game.
static bool gw_windowed(HWND h) { return (GetWindowLongPtrW(h, GWL_STYLE) & WS_CAPTION) == WS_CAPTION; }
static LRESULT CALLBACK gw_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_fw > 0 && g_fh > 0 && gw_windowed(h)) {
        if (m == WM_GETMINMAXINFO) { LRESULT r = CallWindowProcW(g_gw_old, h, m, w, l); auto* mm = (MINMAXINFO*)l;
            mm->ptMaxTrackSize = {16384, 16384}; mm->ptMaxSize = {16384, 16384}; return r; }
        if (m == WM_WINDOWPOSCHANGING) { auto* wp = (WINDOWPOS*)l; if (!IsIconic(h)) { int ow, oh, ox, oy; gw_outer(h, ow, oh, &ox, &oy);
            if (!(wp->flags & SWP_NOSIZE)) { wp->cx = ow; wp->cy = oh; } if (!(wp->flags & SWP_NOMOVE)) { wp->x = ox; wp->y = oy; } } }
    }
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) {   // client coords
        POINT p{(short)LOWORD(l), (short)HIWORD(l)}; ClientToScreen(h, &p);
        if (cur_to_virtual_i(p)) { ScreenToClient(h, &p); l = MAKELPARAM((WORD)(short)p.x, (WORD)(short)p.y); } }
    return CallWindowProcW(g_gw_old, h, m, w, l);
}

// v1.0.7: menu cursor reach. The forced window is bigger than the monitor, the OS cursor cannot leave the monitor ->
// top/bottom/sides of the client were unreachable. While the cursor is VISIBLE (menus), the game sees a virtual cursor:
// monitor area (∩ client) is stretched onto the whole client. Hidden cursor (mouse-look) = untouched.
static bool gw_maprects(RECT& M, RECT& C) {
    if (!g_gw || g_fw <= 0 || !gw_windowed(g_gw)) return false;
    CURSORINFO ci{sizeof(ci)}; if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return false;
    POINT o{0, 0}; if (!ClientToScreen(g_gw, &o)) return false; RECT cr; GetClientRect(g_gw, &cr);
    C = {o.x, o.y, o.x + cr.right, o.y + cr.bottom};
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfo(MonitorFromWindow(g_gw, MONITOR_DEFAULTTOPRIMARY), &mi);
    if (!IntersectRect(&M, &mi.rcMonitor, &C)) return false;
    return !EqualRect(&M, &C) && M.right > M.left + 16 && M.bottom > M.top + 16;
}
static void map_lin(POINT& p, const RECT& a, const RECT& b) {   // a -> b, endpoints map to endpoints
    p.x = b.left + MulDiv(p.x - a.left, b.right - b.left - 1, a.right - a.left - 1);
    p.y = b.top + MulDiv(p.y - a.top, b.bottom - b.top - 1, a.bottom - a.top - 1);
}
bool cur_to_virtual_i(POINT& p) { RECT M, C; if (!gw_maprects(M, C)) return false;
    p.x = p.x < M.left ? M.left : p.x >= M.right ? M.right - 1 : p.x; p.y = p.y < M.top ? M.top : p.y >= M.bottom ? M.bottom - 1 : p.y;
    map_lin(p, M, C); return true; }
bool cur_to_physical_i(POINT& p) { RECT M, C; if (!gw_maprects(M, C)) return false; map_lin(p, C, M); return true; }
static BOOL CALLBACK gw_enum(HWND h, LPARAM p) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetClientRect(h, &r); auto* best = (std::pair<HWND, long>*)p; long a = (r.right - r.left) * (r.bottom - r.top);
    if (a > best->second) *best = {h, a}; return TRUE; }
void xr_window_force_impl() {
    if (g_fw <= 0 || g_fh <= 0 || !g.active) return;
    if (!g_gw) {
        std::pair<HWND, long> best{nullptr, 0}; EnumWindows(gw_enum, (LPARAM)&best); if (!best.first) { vlog("[win] force_window: game window not found"); return; }
        g_gw = best.first; g_gw_old = (WNDPROC)SetWindowLongPtrW(g_gw, GWLP_WNDPROC, (LONG_PTR)gw_proc);
        vlog("[win] force_window %dx%d: hwnd=%p style=0x%llx subclassed=%d", g_fw, g_fh, (void*)g_gw, (unsigned long long)GetWindowLongPtrW(g_gw, GWL_STYLE), g_gw_old != nullptr);
        if (!g_gw_old) { g_gw = nullptr; g_fw = 0; return; }
    }
    if (!gw_windowed(g_gw)) { static bool told = false; if (!told) { told = true;
        vlog("[win] force_window: the game is in Fullscreen mode -> ignored. Set Display Mode = Windowed in the game's graphics settings for the square frame."); } return; }
    RECT rc; GetClientRect(g_gw, &rc); POINT c0{0, 0}; ClientToScreen(g_gw, &c0);
    int ow, oh, ox, oy; gw_outer(g_gw, ow, oh, &ox, &oy); RECT wr; GetWindowRect(g_gw, &wr);
    if (rc.right == g_fw && rc.bottom == g_fh && wr.left == ox && wr.top == oy) return;
    vlog("[win] client %ldx%ld at %ld,%ld -> %dx%d, window pos %d,%d (outer %dx%d)", rc.right, rc.bottom, c0.x, c0.y, g_fw, g_fh, ox, oy, ow, oh);
    SetWindowPos(g_gw, nullptr, ox, oy, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// ---- S5 helpers
DWORD WINAPI beep_thread(LPVOID n) { for (int i = 0; i < (int)(intptr_t)n; ++i) { Beep(1100, 70); Sleep(90); } return 0; }
void beep_async(int n) { HANDLE h = CreateThread(nullptr, 0, beep_thread, (LPVOID)(intptr_t)n, 0, nullptr); if (h) CloseHandle(h); }
std::string ini_path() { return g.dir + "\\vr.ini"; }
void load_ini() {
    std::string f = ini_path();
    g.ys = (int)GetPrivateProfileIntA("vr", "yaw_sign", 1, f.c_str()) < 0 ? -1 : 1;
    g.ps = (int)GetPrivateProfileIntA("vr", "pitch_sign", 1, f.c_str()) < 0 ? -1 : 1;
    { int r = (int)GetPrivateProfileIntA("vr", "roll_sign", 0, f.c_str()); g.rsg = r < 0 ? -1 : r > 0 ? 1 : 0; }
    g.pose_log = GetPrivateProfileIntA("vr", "pose_log", 0, f.c_str()) != 0 || getenv("EVR_POSE_LOG") != nullptr;
    g.lag = GetPrivateProfileIntA("vr", "pose_lag", 2, f.c_str()) & 3;
    g.tracking = GetPrivateProfileIntA("vr", "head_tracking", 1, f.c_str()) != 0;
    { int st = (int)GetPrivateProfileIntA("vr", "stereo", 1, f.c_str()); g.stereo = st < 0 ? -1 : st > 0 ? 1 : 0; }
    g.ipd_pct = (int)GetPrivateProfileIntA("vr", "ipd_pct", 100, f.c_str()); if (g.ipd_pct < 0 || g.ipd_pct > 1000) g.ipd_pct = 100;
    { char b[32] = {}; GetPrivateProfileStringA("vr", "fov_off", "0", b, sizeof(b), f.c_str()); g.fov_off = (uint32_t)strtoul(b, nullptr, 0);
      if (g.fov_off & 3 || g.fov_off >= 0x3000 || g.fov_off == 0x68) g.fov_off = 0; }   // 0x68 = run-20 false positive (camera smoothing)
    g.fov_off_ini = g.fov_off;
    g.fov_axis = GetPrivateProfileIntA("vr", "fov_axis", 0, f.c_str()) ? 1 : 0;
    g.fov_mode = GetPrivateProfileIntA("vr", "fov_mode", 1, f.c_str()) ? 1 : 0;
    g.head_mode = GetPrivateProfileIntA("vr", "head_mode", 1, f.c_str()) ? 1 : 0;
    g.head_pos = GetPrivateProfileIntA("vr", "head_pos", 0, f.c_str()) ? 1 : 0;
    g.head_look = GetPrivateProfileIntA("vr", "head_look", 1, f.c_str()) ? 1 : 0;
    g.pair_lock = GetPrivateProfileIntA("vr", "pair_lock", 1, f.c_str()) ? 1 : 0;
    g.pair_rate = GetPrivateProfileIntA("vr", "pair_rate", 0, f.c_str()) ? 1 : 0;
    g.mat_proj = GetPrivateProfileIntA("vr", "mat_proj", 1, f.c_str()) ? 1 : 0;
    { char fb[32] = {}; GetPrivateProfileStringA("vr", "force_window", "0", fb, sizeof(fb), f.c_str()); int fw = 0, fh = 0;
      if (sscanf(fb, "%dx%d", &fw, &fh) == 2 && fw >= 640 && fh >= 480 && fw <= 4096 && fh <= 4096) { g_fw = fw; g_fh = fh; } else { g_fw = g_fh = 0; } }
    g.layer_eyefov = GetPrivateProfileIntA("vr", "layer_eyefov", 1, f.c_str()) ? 1 : 0;
    g.hud_mode = (int)GetPrivateProfileIntA("vr", "hud_mode", 1, f.c_str()) % 3;
    g.hud_dist_cm = (int)GetPrivateProfileIntA("vr", "hud_dist_cm", 150, f.c_str()); if (g.hud_dist_cm < 50 || g.hud_dist_cm > 500) g.hud_dist_cm = 150;
    g.hud_sync = GetPrivateProfileIntA("vr", "hud_sync", 1, f.c_str()) ? 1 : 0;
    vlog("[vr] hud_sync=%d (1 = HUD refreshed from LEFT-eye frames only: world prompts don't jump between eye projections)", g.hud_sync);
    vlog("[vr] hud_mode=%d (0 = in the game image, 1 = quad 55 deg, 2 = quad 70 deg) hud_dist_cm=%d", g.hud_mode, g.hud_dist_cm);
    g.cam_dist_pct = (int)GetPrivateProfileIntA("vr", "cam_dist_pct", 100, f.c_str()); if (g.cam_dist_pct < 20 || g.cam_dist_pct > 150) g.cam_dist_pct = 100;
    vlog("[vr] cam_dist_pct=%d (3rd-person boom length, %% of the game's)", g.cam_dist_pct);
    vlog("[vr] pair_rate=%d (1 = XR frame once per pair, game runs at 2x HMD rate)", g.pair_rate);
    vlog("[vr] pair_lock=%d (both eyes of a pair share one head pose, submitted together)", g.pair_lock);
    vlog("[vr] head_look=%s", g.head_look ? "head rotates view" : "OFF (mouse-only, world-locked stereo screen)");
    vlog("[vr] head_mode=%s head_pos(6DoF)=%d", g.head_mode ? "view" : "orbit", g.head_pos);
    g.fov_ovr = GetPrivateProfileIntA("vr", "fov_override", 1, f.c_str()) ? 1 : 0;
    g.fov_pct = (int)GetPrivateProfileIntA("vr", "fov_pct", 100, f.c_str()); if (g.fov_pct < 50 || g.fov_pct > 120) g.fov_pct = 100;
    vlog("[vr] fov: fov_off=+0x%x axis=%s override=%d fov_pct=%d", g.fov_off, g.fov_axis ? "H" : "V", g.fov_ovr, g.fov_pct);
    vlog("[vr] settings: yaw_sign=%d pitch_sign=%d roll_sign=%d pose_lag=%d head_tracking=%d stereo=%d ipd_pct=%d (%s)", g.ys, g.ps, g.rsg, g.lag, g.tracking, g.stereo, g.ipd_pct, f.c_str());
    load_keys(f.c_str());
    { char b[8] = {}; GetPrivateProfileStringA("keys", "recenter", "", b, sizeof(b), f.c_str());
      if (GetFileAttributesA(f.c_str()) == INVALID_FILE_ATTRIBUTES) save_ini();   // first run: write a complete, documented-by-keys ini
      else if (!b[0]) save_keys(f.c_str()); }
    g.ini_mtime = ini_mtime();
}
void save_ini() {
    std::string f = ini_path(); char b[16];
    auto w = [&](const char* k, int v) { snprintf(b, sizeof(b), "%d", v); WritePrivateProfileStringA("vr", k, b, f.c_str()); };
    w("yaw_sign", g.ys); w("pitch_sign", g.ps); w("roll_sign", g.rsg); w("pose_lag", g.lag); w("head_tracking", g.tracking);
    w("stereo", g.stereo); w("ipd_pct", g.ipd_pct);
    w("fov_axis", g.fov_axis); w("fov_override", g.fov_ovr); w("fov_pct", g.fov_pct); w("fov_mode", g.fov_mode); w("head_mode", g.head_mode); w("head_pos", g.head_pos); w("head_look", g.head_look); w("pair_lock", g.pair_lock); w("pair_rate", g.pair_rate); w("cam_dist_pct", g.cam_dist_pct); w("mat_proj", g.mat_proj); w("layer_eyefov", g.layer_eyefov); w("hud_mode", g.hud_mode); w("hud_dist_cm", g.hud_dist_cm); w("hud_sync", g.hud_sync);
    { char hb[16]; snprintf(hb, sizeof(hb), "0x%x", g.fov_off_ini); WritePrivateProfileStringA("vr", "fov_off", hb, f.c_str()); }
    save_keys(f.c_str());
    g.ini_mtime = ini_mtime();   // our own write: not a reload trigger
    vlog("[vr] settings saved: yaw_sign=%d pitch_sign=%d roll_sign=%d pose_lag=%d head_tracking=%d stereo=%d ipd_pct=%d fov_off=+0x%x fov_override=%d",
         g.ys, g.ps, g.rsg, g.lag, g.tracking, g.stereo, g.ipd_pct, g.fov_off, g.fov_ovr);
}
bool seh_copy(uint8_t* dst, uint64_t src, size_t n) {
    __try { memcpy(dst, (const void*)src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_readf(uint64_t a, float* v) { __try { *v = *(volatile float*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
void seh_writef(uint64_t a, float v) { __try { *(volatile float*)a = v; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
// PageDown: snapshot CamObj [0, 0x3000) -> camobj_N.bin; diff with previous -> FOV candidates (user moves the FOV slider between dumps)
void camobj_dump() {
    const size_t N = 0x3000; uint64_t h, obj; camhook_stats(&h, &obj);
    std::vector<uint8_t> cur(N);
    if (!obj || !seh_copy(cur.data(), obj, N)) { beep_async(5); vlog("[fovdiag] no CamObj / read failed"); return; }
    char fn[64]; snprintf(fn, sizeof(fn), "\\camobj_%d.bin", ++g.dump_n);
    FILE* fp = fopen((g.dir + fn).c_str(), "wb"); if (fp) { fwrite(cur.data(), 1, N, fp); fclose(fp); }
    vlog("[fovdiag] dump #%d of CamObj %p -> %s", g.dump_n, (void*)obj, fn + 1);
    if (g.dump_prev.size() != N) { g.dump_prev = cur; beep_async(2); return; }
    const CamFields* cf = camhook_fields();
    int cands = 0; uint32_t pick = 0;
    for (size_t o = 0; o + 4 <= N; o += 4) {
        float a, b; memcpy(&a, &g.dump_prev[o], 4); memcpy(&b, &cur[o], 4);
        if (a == b || !(a == a) || !(b == b)) continue;
        bool rad = a > 0.3f && a < 2.8f && b > 0.3f && b < 2.8f, deg = a > 20.f && a < 170.f && b > 20.f && b < 170.f;
        if (!rad && !deg) continue;
        bool cam_state = cf && o >= cf->pivot && o < cf->pivot + 0xa0;    // angles/offset/pos change with motion - not FOV
        vlog("[fovdiag]   +0x%zx: %.5f -> %.5f  (%s)%s", o, a, b, rad ? "rad?" : "deg?", cam_state ? " [camera state, ignored]" : "");
        if (rad) vlog("[fovdiag]          = %.2f -> %.2f deg", a * 57.29578f, b * 57.29578f);
        if (!cam_state && rad) { ++cands; if (!pick) pick = (uint32_t)o; }
    }
    vlog("[fovdiag] diff #%d vs #%d: %d rad-candidates outside camera state", g.dump_n - 1, g.dump_n, cands);
    (void)pick;   // v0.8.1: FOV field comes from the build table; diff is diagnostics only
    g.dump_prev = cur;
    beep_async(cands ? 3 : 5);
}
bool foreground_is_us() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }
// ---- key bindings ([keys] in vr.ini). Names shared with EnshroudedVR-Config.exe.
struct KeyName { const char* n; int vk; };
const KeyName kKeyNames[] = {{"None", 0}, {"Insert", VK_INSERT}, {"Home", VK_HOME}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"End", VK_END},
    {"Delete", VK_DELETE}, {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL}, {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
    {"Num0", VK_NUMPAD0}, {"Num1", VK_NUMPAD1}, {"Num2", VK_NUMPAD2}, {"Num3", VK_NUMPAD3}, {"Num4", VK_NUMPAD4}, {"Num5", VK_NUMPAD5},
    {"Num6", VK_NUMPAD6}, {"Num7", VK_NUMPAD7}, {"Num8", VK_NUMPAD8}, {"Num9", VK_NUMPAD9}, {"Num+", VK_ADD}, {"Num-", VK_SUBTRACT},
    {"Num*", VK_MULTIPLY}, {"Num/", VK_DIVIDE}, {"NumDel", VK_DECIMAL}, {"[", VK_OEM_4}, {"]", VK_OEM_6}, {"'", VK_OEM_7}, {",", VK_OEM_COMMA},
    {".", VK_OEM_PERIOD}, {"/", VK_OEM_2}, {"\\", VK_OEM_5}, {"-", VK_OEM_MINUS}, {"=", VK_OEM_PLUS}, {"`", VK_OEM_3},
    {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9},
    {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12}};
int key_parse(const char* s) {
    while (*s == ' ') ++s;
    if (!*s) return 0;
    for (const KeyName& k : kKeyNames) if (!_stricmp(k.n, s)) return k.vk;
    if (!s[1] && ((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9'))) return *s;
    if (!s[1] && *s >= 'a' && *s <= 'z') return *s - 32;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s, nullptr, 16) & 0xff;
    return -1;
}
const char* key_name(int vk, char* buf) {
    for (const KeyName& k : kKeyNames) if (k.vk == vk) return k.n;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { buf[0] = (char)vk; buf[1] = 0; return buf; }
    snprintf(buf, 8, "0x%02x", vk); return buf;
}
#ifdef EVR_PUBLIC
#define DEVKEY(k) "None"
#else
#define DEVKEY(k) k
#endif
struct Act { const char* id; const char* def; };
const Act kActs[kNumActs] = {
    {"toggle_tracking", "Insert"}, {"recenter", "Home"}, {"yaw_sign", DEVKEY("PageUp")}, {"camobj_dump", DEVKEY("PageDown")},
    {"roll_mode", DEVKEY("End")}, {"pose_lag", DEVKEY("Pause")}, {"stereo_mode", "ScrollLock"}, {"ipd_up", "Num+"}, {"ipd_down", "Num-"},
    {"fov_toggle", "Num*"}, {"ipd_reset", "Num0"}, {"ipd_up_alt", "]"}, {"ipd_down_alt", "["}, {"fov_fill_fit", "Num/"},
    {"head_look", "Num5"}, {"six_dof", "Num8"}, {"pitch_sign", DEVKEY("Num2")}, {"fov_hwbp", DEVKEY("Num9")},
    {"pair_lock", DEVKEY("Num7")}, {"pair_rate", DEVKEY("Num1")}, {"cam_closer", "Num4"}, {"cam_farther", "Num6"}, {"cam_reset", "Num3"},
    {"hud_mode", "NumDel"}};
void load_keys(const char* f) {
    char b[32];
    for (int i = 0; i < kNumActs; ++i) {
        GetPrivateProfileStringA("keys", kActs[i].id, kActs[i].def, b, sizeof(b), f);
        int vk = key_parse(b);
        if (vk < 0) { vlog("[keys] %s = '%s' unknown -> default %s", kActs[i].id, b, kActs[i].def); vk = key_parse(kActs[i].def); }
        g.keys[i] = vk;
    }
}
void save_keys(const char* f) {
    char b[8];
    for (int i = 0; i < kNumActs; ++i) WritePrivateProfileStringA("keys", kActs[i].id, key_name(g.keys[i], b), f);
}
// ---- OSD: short text panel in the headset (2 s) for every hotkey
void osd(const char* fmt, ...) {
    char t[160]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof(t), fmt, ap); va_end(ap);
    wchar_t w[160]; MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 160);
    if (osd_render(w, kOW, kOH, g.opx.data())) { g.odirty = true; g.ountil = now_s() + 2.0; }
    vlog("[osd] %s", t);
}
#define DEG "\xC2\xB0"
uint64_t ini_mtime() {
    WIN32_FILE_ATTRIBUTE_DATA fa{}; if (!GetFileAttributesExA(ini_path().c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
}
void live_reload() {   // vr.ini edited by EnshroudedVR-Config.exe or a text editor -> apply without restarting the game
    static double t_chk = 0; const double tn = now_s();
    if (tn - t_chk < 0.5) return;
    t_chk = tn;
    const uint64_t m = ini_mtime();
    if (!m || m == g.ini_mtime) return;
    const int hl = g.head_look, pl = g.pair_lock, st = g.stereo, hp = g.head_pos; const uint32_t fo = g.fov_off;
    load_ini();
    if (!g.fov_off) g.fov_off = fo;   // runtime-derived FOV field: keep, or the override would flicker off for a frame
    if (hl != g.head_look || hp != g.head_pos) g.centered = false;
    if (pl != g.pair_lock) { g.cvalid = false; g.Lpid = ~0ull; }
    if (st != g.stereo) g.evalid[0] = g.evalid[1] = false;
    vlog("[vr] vr.ini changed on disk -> settings reloaded");
    osd("Settings applied (IPD %d%%, camera %d%%)", g.ipd_pct, g.cam_dist_pct);
}
void hotkeys() {
    live_reload();
    bool fg = foreground_is_us() || g.cursor_anywhere;
    for (int i = 0; i < kNumActs; ++i) {
        const int vk = g.keys[i]; if (!vk) { g.key_prev[i] = false; continue; }
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0, edge = down && !g.key_prev[i]; g.key_prev[i] = down;
        if (!edge) continue;
        if (!fg) { vlog("[key] %s ignored: game window not in foreground", kActs[i].id); continue; }
        vlog("[key] %s (vk 0x%02x)", kActs[i].id, vk);
        int ii = i == 11 ? 7 : i == 12 ? 8 : i;
        switch (ii) {
        case 0: g.tracking = !g.tracking; g.centered = false; beep_async(g.tracking ? 2 : 1); save_ini(); osd("Head tracking %s", g.tracking ? "ON" : "OFF"); break;
        case 1: g.centered = false; beep_async(1); vlog("[vr] recenter"); osd("Recentered"); break;
        case 2: g.ys = -g.ys; beep_async(g.ys > 0 ? 2 : 1); save_ini(); osd("Yaw sign %+d", g.ys); break;
#ifndef EVR_PUBLIC
        case 3: camobj_dump(); break;   // PageDown: S7 FOV field discovery (dump + diff)
#endif
        case 4: g.rsg = g.rsg == 0 ? -1 : g.rsg < 0 ? 1 : 0; beep_async(g.rsg == 0 ? 3 : g.rsg > 0 ? 2 : 1); save_ini(); osd("Roll %s", g.rsg == 0 ? "OFF" : g.rsg > 0 ? "+1" : "-1"); break;
        case 5: g.lag = (g.lag + 1) & 3; beep_async(g.lag + 1); save_ini(); osd("Pose lag %d", g.lag); break;
        case 6: g.stereo = g.stereo == 1 ? -1 : g.stereo == -1 ? 0 : 1; g.evalid[0] = g.evalid[1] = false;   // stereo(1) -> swapped(2) -> mono(3)
                beep_async(g.stereo == 1 ? 1 : g.stereo == -1 ? 2 : 3); save_ini(); osd("%s", g.stereo == 1 ? "Stereo" : g.stereo == -1 ? "Stereo (eyes swapped)" : "Mono"); break;
        case 7: case 8: {   // stereo separation (world scale) in 10% steps
            int n = g.ipd_pct + (ii == 7 ? 10 : -10);
            if (n < 10 || n > 400) { beep_async(5); osd("Stereo (IPD) limit %d%%", g.ipd_pct); break; }
            g.ipd_pct = n; vlog("[vr] ipd_pct=%d", n); beep_async(1); save_ini(); osd("Stereo (IPD) %d%%", n); break; }
        case 10: g.ipd_pct = 100; vlog("[vr] ipd_pct reset 100"); beep_async(2); save_ini(); osd("Stereo (IPD) 100%%"); break;
        case 9:   // FOV override (HMD FOV) on/off
            if (!g.fov_off) { beep_async(5); vlog("[vr] fov override: fov_off unknown"); osd("Headset FOV not available yet"); break; }
            g.fov_ovr = !g.fov_ovr; beep_async(g.fov_ovr ? 2 : 1); save_ini();
            vlog("[vr] fov override %s, %s", g.fov_ovr ? "ON" : "OFF", g.fov_axis ? "FIT" : "FILL");
            osd(g.fov_ovr ? "Headset FOV ON (%s)" : "Game FOV (headset FOV off)", g.fov_axis ? "FIT" : "FILL"); break;
        case 14: if (!camhook_view_ok()) { beep_async(5); vlog("[vr] view hooks not installed -> orbit only"); osd("Head look unavailable"); break; }
                 g.head_mode = 1; g.head_look = !g.head_look; g.centered = false; beep_async(g.head_look ? 2 : 1); save_ini();
                 vlog("[vr] head_look %s", g.head_look ? "ON" : "OFF"); osd(g.head_look ? "Head look ON" : "Head look OFF (fixed stereo screen)"); break;
        case 15: g.head_pos = !g.head_pos; g.centered = false; beep_async(g.head_pos ? 2 : 1); save_ini(); vlog("[vr] head_pos(6DoF) %d", g.head_pos); osd("6DoF head position %s", g.head_pos ? "ON" : "OFF"); break;
        case 16: g.ps = -g.ps; beep_async(g.ps > 0 ? 2 : 1); save_ini(); vlog("[vr] pitch_sign %d", g.ps); osd("Pitch sign %+d", g.ps); break;
#ifndef EVR_PUBLIC
        case 17: {   // Numpad9: HW-BP read/write trace on the FOV candidates (who consumes the projection FOV)
            uint64_t h, obj; camhook_stats(&h, &obj); const CamFields* cf = camhook_fields();
            static volatile LONG busy = 0; static uintptr_t base = 0; static CamFields scf;
            static uintptr_t rec = 0;
            if (!obj || !cf || !cf->fov || InterlockedCompareExchange(&busy, 1, 0) != 0) { beep_async(5); break; }
            base = (uintptr_t)obj; scf = *cf; rec = (uintptr_t)camhook_fov_rec();
            vlog("[diag] Num9: HW-BP on FOV fields of CamObj %p", (void*)obj);
            HANDLE th = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                Beep(900, 120); Sleep(60); Beep(900, 120);
                // S8b-2: who consumes the FOV for the GPU projection? frame record (60 Hz sim copy) vs the source field CamObj-0x18
                uintptr_t a[4] = {base - 0x18, rec ? rec + 0x4c : base + scf.fov, rec ? rec + 0x50 : base + scf.aspect, rec ? rec + 0x38 : base + scf.fov_copies[0]};
                int l[4] = {4, 4, 4, 4};
                const char* nm[4] = {"src@-18", rec ? "rec+4c(fov)" : "fov@20c4", rec ? "rec+50(aspect)" : "aspect@20b8", rec ? "rec+38(quat)" : "fovcopy@4c"};
                vlog("[diag] Num9: CamObj %p, frame record %p", (void*)base, (void*)rec);
                hwbp_trace(a, l, nm, 4, 3.0);
                for (int i = 0; i < 3; ++i) { Beep(1200, 80); Sleep(100); }
                InterlockedExchange(&busy, 0); return 0; }, nullptr, 0, nullptr);
            if (th) CloseHandle(th);
            break; }
#endif
        case 20: case 21: { int n = g.cam_dist_pct + (i == 20 ? -10 : 10);
                 if (n < 20 || n > 150) { beep_async(5); osd("Camera distance limit %d%%", g.cam_dist_pct); break; }
                 g.cam_dist_pct = n; beep_async(1); save_ini(); vlog("[vr] cam_dist_pct=%d (boom %.2f m of %.2f)", n, g.cam_dist * n / 100.f, g.cam_dist);
                 osd("Camera distance %d%%", n); break; }
        case 22: g.cam_dist_pct = 100; beep_async(2); save_ini(); vlog("[vr] cam_dist_pct reset 100"); osd("Camera distance 100%%"); break;
        case 19: g.pair_rate = !g.pair_rate; beep_async(g.pair_rate ? 2 : 1); save_ini(); vlog("[vr] pair_rate %d", g.pair_rate); osd("Pair rate %s", g.pair_rate ? "ON" : "OFF"); break;
        case 18: g.pair_lock = !g.pair_lock; g.cvalid = false; g.Lpid = ~0ull; beep_async(g.pair_lock ? 2 : 1); save_ini(); vlog("[vr] pair_lock %d", g.pair_lock); osd("Pair lock %s", g.pair_lock ? "ON" : "OFF"); break;
        case 23: g.hud_mode = (g.hud_mode + 1) % 3; beep_async(g.hud_mode + 1); save_ini();
#ifndef EVR_PUBLIC
                 cap_arm();
#endif
                 vlog("[vr] hud_mode %d", g.hud_mode);
                 osd("%s", g.hud_mode == 0 ? "HUD in game image" : g.hud_mode == 1 ? "HUD panel 55" DEG : "HUD panel 70" DEG); break;
        case 13: g.fov_axis = !g.fov_axis; beep_async(g.fov_axis ? 2 : 1); save_ini(); vlog("[vr] fov mode %s", g.fov_axis ? "FIT" : "FILL");
                 osd(g.fov_axis ? "FOV mode FIT (full width)" : "FOV mode FILL (full height)"); break;
        }
    }
}
// YXZ (yaw about +Y, then pitch about +X, then roll about +Z), OpenXR right-handed, -Z forward
void q2e(const XrQuaternionf& q, float& yaw, float& pitch, float& roll) {
    float sp = 2.f * (q.w * q.x - q.y * q.z); sp = sp > 1.f ? 1.f : sp < -1.f ? -1.f : sp;
    pitch = asinf(sp);
    yaw = atan2f(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y));
    roll = atan2f(2.f * (q.w * q.z + q.x * q.y), 1.f - 2.f * (q.x * q.x + q.z * q.z));
}
float wrap_pi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

} // namespace

bool xr_active() { return g.active; }
// v1.0.8: clean OpenXR teardown when the game destroys its VkDevice (exit). Without it the session stays alive,
// the process hangs in ExitProcess (runtime threads/DLL detach) and Meta keeps a frozen app in the headset.
// Order matters: the runtime owns VkImages on the game's device -> destroy the session BEFORE vkDestroyDevice.
static std::atomic<bool> g_xr_down{false};
bool xr_is_down() { return g_xr_down.load(); }
void xr_shutdown(const char* why) {
    if (g_xr_down.exchange(true)) return;
    vlog("[xr] shutdown (%s): sess=%p inst=%p running=%d", why, (void*)g.sess, (void*)g.inst, (int)g.running);
    g.active = false; g.session_failed = true; g_hud_redirect.store(0);
    if (g.sess) {
        if (g.running) { XrResult r = xrEndSession(g.sess); vlog("[xr]   xrEndSession -> %d", (int)r); g.running = false; }
        XrResult r = xrDestroySession(g.sess); vlog("[xr]   xrDestroySession -> %d (child swapchains/spaces released)", (int)r);
        g.sess = XR_NULL_HANDLE; g.xsc = g.xsc1 = g.xsc2 = g.hsc = g.csc = g.osc = XR_NULL_HANDLE; g.view = XR_NULL_HANDLE;
    }
    if (g.inst) { XrResult r = xrDestroyInstance(g.inst); vlog("[xr]   xrDestroyInstance -> %d", (int)r); g.inst = XR_NULL_HANDLE; }
    vlog("[xr] shutdown done");
}
void xr_window_force() { xr_window_force_impl(); }
bool cur_to_virtual(POINT& p) { return cur_to_virtual_i(p); }
bool cur_to_physical(POINT& p) { return cur_to_physical_i(p); }
VkPhysicalDevice xr_physical_device() { return g.pd; }

#pragma comment(lib, "advapi32.lib")
// ---- runtime selection (v1.0.1): try installed OpenXR runtimes until one has a headset ----
static std::string rt_tag(const std::string& j) {
    std::string l = j; for (auto& c : l) c = (char)tolower((unsigned char)c);
    if (l.find("oculus") != std::string::npos) return "meta";
    if (l.find("virtualdesktop") != std::string::npos) return "vd";
    if (l.find("steamxr") != std::string::npos || l.find("steamvr") != std::string::npos) return "steamvr";
    if (l.find("pico") != std::string::npos) return "pico";
    if (l.find("wmr") != std::string::npos || l.find("mixedreality") != std::string::npos) return "wmr";
    return "other";
}
static void rt_reg_list(HKEY root, std::vector<std::string>& out) {
    HKEY k; if (RegOpenKeyExA(root, "SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; i++) {
        char name[1024]; DWORD nl = sizeof(name), type = 0, val = 1, vl = sizeof(val);
        if (RegEnumValueA(k, i, name, &nl, nullptr, &type, (BYTE*)&val, &vl) != ERROR_SUCCESS) break;
        if (type == REG_DWORD && val != 0) continue;   // non-zero = disabled
        out.push_back(name);
    }
    RegCloseKey(k);
}
static std::string rt_active(HKEY root) {
    char buf[1024]; DWORD bl = sizeof(buf), type = 0;
    if (RegGetValueA(root, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, &type, buf, &bl) == ERROR_SUCCESS) return buf;
    return "";
}
// candidates: "" = loader default. Order: active, meta, vd, wmr, other; pico/steamvr only if active/explicit (they fake a system).
static std::vector<std::string> rt_candidates(const std::string& pref) {
    std::vector<std::string> all; rt_reg_list(HKEY_LOCAL_MACHINE, all); rt_reg_list(HKEY_CURRENT_USER, all);
    std::string act = rt_active(HKEY_CURRENT_USER); if (act.empty()) act = rt_active(HKEY_LOCAL_MACHINE);
    std::vector<std::string> c;
    auto add = [&](const std::string& j) { if (j.empty() || GetFileAttributesA(j.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        for (auto& x : c) if (_stricmp(x.c_str(), j.c_str()) == 0) return; c.push_back(j); };
    std::string pl = pref; for (auto& ch : pl) ch = (char)tolower((unsigned char)ch);
    if (pl == "active") { c.push_back(""); return c; }
    if (!pl.empty() && pl != "auto") {
        if (pl.find('\\') != std::string::npos || pl.find('/') != std::string::npos || pl.find(".json") != std::string::npos) { add(pref); return c; }
        for (auto& j : all) if (rt_tag(j) == pl) add(j);
        if (rt_tag(act) == pl) add(act);
        return c;   // explicit tag: only that runtime
    }
    add(act);
    for (const char* t : {"meta", "vd", "wmr", "other"})   // pico/steamvr report a system without a headset -> only if active/explicit
        for (auto& j : all) if (rt_tag(j) == t) add(j);
    if (c.empty()) c.push_back("");
    return c;
}

// one attempt with a freshly loaded loader (the loader caches the runtime per process image)
static HMODULE g_loader = nullptr;
static bool xr_try_runtime(const std::string& d, const std::string& json) {
    if (!json.empty()) SetEnvironmentVariableA("XR_RUNTIME_JSON", json.c_str()); else SetEnvironmentVariableA("XR_RUNTIME_JSON", nullptr);
    std::string tg = json.empty() ? "active" : rt_tag(json);
    vlog("[xr] trying runtime [%s] %s", tg.c_str(), json.empty() ? "(registry ActiveRuntime)" : json.c_str());
    HMODULE lib = LoadLibraryA((d + "\\openxr_loader.dll").c_str());
    if (!lib) { vlog("[xr] openxr_loader.dll not found in %s (err %lu) -> flat mode", d.c_str(), GetLastError()); return false; }
    g_loader = lib;
    auto fail = [&]() { xrGIPA = nullptr; FreeLibrary(lib); g_loader = nullptr; return false; };
    xrGIPA = (PFN_xrGetInstanceProcAddr)GetProcAddress(lib, "xrGetInstanceProcAddr");
    if (!xrGIPA) { vlog("[xr] loader has no xrGetInstanceProcAddr"); return fail(); }
    PFN_xrEnumerateInstanceExtensionProperties enumExt = nullptr; PFN_xrCreateInstance createInst = nullptr;
    xrGIPA(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumExt);
    xrGIPA(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&createInst);
    uint32_t n = 0;
    if (!enumExt || XR_FAILED(enumExt(nullptr, 0, &n, nullptr))) { vlog("[xr]   runtime not loadable (enumerate extensions failed)"); return fail(); }
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    enumExt(nullptr, n, &n, props.data());
    bool vk1 = false;
    for (auto& p : props) if (!strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) vk1 = true;
    vlog("[xr] runtime exposes %u extensions, %s=%d", n, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, vk1);
    if (!vk1) return fail();
    const char* exts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "EnshroudedVR");
    strcpy(ci.applicationInfo.engineName, "holistic");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1; ci.enabledExtensionNames = exts;
    XrResult r = createInst(&ci, &g.inst);
    if (XR_FAILED(r)) { vlog("[xr]   xrCreateInstance failed %d", (int)r); g.inst = XR_NULL_HANDLE; return fail(); }
#define LOAD(nm) xrGIPA(g.inst, #nm, (PFN_xrVoidFunction*)&nm); if (!nm) { vlog("[xr] missing %s", #nm); return false; }
    XR_FUNCS(LOAD)
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO}; si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(g.inst, &si, &g.sys);
    if (XR_FAILED(r)) { vlog("[xr]   xrGetSystem: %s (no headset on this runtime)", rs(r)); xrDestroyInstance(g.inst); g.inst = XR_NULL_HANDLE; return fail(); }
    vlog("[xr] using runtime [%s]", tg.c_str());
    return true;
}

bool xr_init(const char* dir) {
    if (g.active || g.disabled_runtime) return g.active;
    g.disabled_runtime = true;   // one attempt per process
    std::string d(dir);
    if (GetFileAttributesA((d + "\\xr_off.txt").c_str()) != INVALID_FILE_ATTRIBUTES) { vlog("[xr] xr_off.txt present -> flat mode"); return false; }
    char pref[1024] = "auto";
    GetPrivateProfileStringA("vr", "xr_runtime", "auto", pref, sizeof(pref), (d + "\\vr.ini").c_str());
    if (getenv("EVR_XR_RUNTIME")) strncpy(pref, getenv("EVR_XR_RUNTIME"), sizeof(pref) - 1);
    auto cands = rt_candidates(pref);
    vlog("[xr] xr_runtime=%s, %zu candidate(s)", pref, cands.size());
    bool ok = false;
    for (auto& j : cands) if ((ok = xr_try_runtime(d, j))) break;
    if (!ok) { vlog("[xr] no runtime with a connected headset -> flat mode"); return false; }
    if (getenv("EVR_XR_PROBE_ONLY")) { vlog("[xr] probe-only: stop after xrGetSystem"); xrDestroyInstance(g.inst); g.inst = XR_NULL_HANDLE; return false; }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES}; xrGetSystemProperties(g.inst, g.sys, &sp);
    vlog("[xr] system '%s' vendor=%u maxSwapchain %ux%u layers=%u orientTracking=%d", sp.systemName, sp.vendorId,
         sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount,
         sp.trackingProperties.orientationTracking);
    g.active = true;
    g.dir = d; load_ini();
    return true;
}

void xr_build_instance_ci(const VkInstanceCreateInfo* in, XrInstCI& o) {
    o.ci = *in;
    if (!g.active) return;
    uint32_t n = 0; xrGetVulkanInstanceExtensionsKHR(g.inst, g.sys, 0, &n, nullptr);
    std::string s(n, '\0'); xrGetVulkanInstanceExtensionsKHR(g.inst, g.sys, n, &n, s.data());
    vlog("[xr] runtime Vulkan instance exts: %s", s.c_str());
    split_append(s.c_str(), o.store);
    merge_exts(in->enabledExtensionCount, in->ppEnabledExtensionNames, o.store, o.exts, "iext");
    o.ci.enabledExtensionCount = (uint32_t)o.exts.size(); o.ci.ppEnabledExtensionNames = o.exts.data();
}

void xr_on_instance(VkInstance inst, PFN_vkGetInstanceProcAddr gipa) {
    if (!g.active) return;
    g.vki = inst; g.gipa = gipa;
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XRCHK(xrGetVulkanGraphicsRequirementsKHR(g.inst, g.sys, &req));
    if (XR_FAILED(XRCHK(xrGetVulkanGraphicsDeviceKHR(g.inst, g.sys, inst, &g.pd)))) { g.active = false; return; }
    auto props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties p{}; if (props) props(g.pd, &p);
    vlog("[xr] HMD GPU = '%s' (%p); runtime Vulkan api %u.%u..%u.%u", p.deviceName, (void*)g.pd,
         XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
         XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));
}

bool xr_build_device_ci(VkPhysicalDevice pd, const VkDeviceCreateInfo* in, XrDevCI& o) {
    o.ci = *in;
    if (!g.active || !g.pd) return false;
    if (pd != g.pd) { vlog("[xr] vkCreateDevice on %p, HMD GPU is %p -> leave untouched", (void*)pd, (void*)g.pd); return false; }
    uint32_t n = 0; xrGetVulkanDeviceExtensionsKHR(g.inst, g.sys, 0, &n, nullptr);
    std::string s(n, '\0'); xrGetVulkanDeviceExtensionsKHR(g.inst, g.sys, n, &n, s.data());
    vlog("[xr] runtime Vulkan device exts: %s", s.c_str());
    split_append(s.c_str(), o.store);
    merge_exts(in->enabledExtensionCount, in->ppEnabledExtensionNames, o.store, o.exts, "dext");
    o.ci.enabledExtensionCount = (uint32_t)o.exts.size(); o.ci.ppEnabledExtensionNames = o.exts.data();
    // timelineSemaphore feature: runtimes sync through imported timeline semaphores
    bool done = false;
    for (auto* b = (VkBaseOutStructure*)in->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto f = (VkPhysicalDeviceVulkan12Features*)b; vlog("[xr]   Vulkan12Features.timelineSemaphore %u -> 1", f->timelineSemaphore);
            f->timelineSemaphore = VK_TRUE; done = true;
        } else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto f = (VkPhysicalDeviceTimelineSemaphoreFeatures*)b; vlog("[xr]   TimelineSemaphoreFeatures %u -> 1", f->timelineSemaphore);
            f->timelineSemaphore = VK_TRUE; done = true;
        }
    }
    if (!done) {
        o.tl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, const_cast<void*>(in->pNext), VK_TRUE};
        o.ci.pNext = &o.tl; vlog("[xr]   +TimelineSemaphoreFeatures (chained)");
    }
    return true;
}

void xr_on_device(VkDevice dev, VkPhysicalDevice pd) {
    if (!g.active || pd != g.pd || g.dev) return;
    g.dev = dev;
    auto gdpa = (PFN_vkGetDeviceProcAddr)g.gipa(g.vki, "vkGetDeviceProcAddr");
#define VLOAD(n) p_##n = (PFN_##n)gdpa(dev, #n); if (!p_##n) { vlog("[xr] missing %s", #n); g.active = false; return; }
    VK_FUNCS(VLOAD)
    vlog("[xr] device %p bound", (void*)dev);
}

void xr_note_queue(VkQueue q, uint32_t fam, uint32_t idx) { g.queues[q] = {fam, idx}; }

void xr_on_swapchain(VkSwapchainKHR sc, uint32_t w, uint32_t h, VkFormat fmt, uint32_t n, const VkImage* imgs) {
    std::lock_guard<std::mutex> lk(g.sc_mtx);
    GameSc& s = g.scs[sc];
    if (w) { s.w = w; s.h = h; s.fmt = fmt; }
    if (imgs) s.imgs.assign(imgs, imgs + n);
}

namespace {
bool create_session(VkQueue q) {
    auto it = g.queues.find(q);
    if (it == g.queues.end()) { vlog("[xr] present queue %p unknown -> assume family 0 index 0", (void*)q); g.qfam = 0; g.qidx = 0; }
    else { g.qfam = it->second.first; g.qidx = it->second.second; }
    g.q = q;
    XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance = g.vki; gb.physicalDevice = g.pd; gb.device = g.dev; gb.queueFamilyIndex = g.qfam; gb.queueIndex = g.qidx;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO}; ci.next = &gb; ci.systemId = g.sys;
    if (XR_FAILED(XRCHK(xrCreateSession(g.inst, &ci, &g.sess)))) return false;
    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rci.poseInReferenceSpace.orientation.w = 1.f;
    if (XR_FAILED(XRCHK(xrCreateReferenceSpace(g.sess, &rci, &g.local)))) return false;
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(XRCHK(xrCreateReferenceSpace(g.sess, &rci, &g.view)))) g.view = XR_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = g.qfam;
    if (p_vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool) != VK_SUCCESS) { vlog("[xr] vkCreateCommandPool failed"); return false; }
    for (auto& s : g.slots) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (p_vkAllocateCommandBuffers(g.dev, &ai, &s.cb) || p_vkCreateFence(g.dev, &fci, nullptr, &s.fence) ||
            p_vkCreateSemaphore(g.dev, &sci, nullptr, &s.sem)) { vlog("[xr] slot alloc failed"); return false; }
        // dispatchable handle allocated outside the loader trampoline: copy the loader dispatch pointer
        *(void**)s.cb = *(void**)g.dev;
    }
    vlog("[xr] session created on queue family %u index %u", g.qfam, g.qidx);
    return true;
}

bool make_xr_swapchain(const GameSc& s) {
    if (g.xsc) { xrDestroySwapchain(g.xsc); g.xsc = XR_NULL_HANDLE; }
    uint32_t n = 0; xrEnumerateSwapchainFormats(g.sess, 0, &n, nullptr);
    std::vector<int64_t> fm(n); xrEnumerateSwapchainFormats(g.sess, n, &n, fm.data());
    std::string list; for (auto f : fm) list += std::to_string(f) + " ";
    vlog("[xr] runtime swapchain formats: %s", list.c_str());
    // bitwise copy into the sRGB twin of the game format (same texel layout) => correct gamma in the headset
    int64_t want = s.fmt == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : s.fmt == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB : s.fmt;
    g.xfmt = 0; g.xcopy = true;
    for (auto f : fm) if (f == want) g.xfmt = f;
    if (!g.xfmt) for (auto f : fm) if (f == s.fmt) g.xfmt = f;
    if (!g.xfmt && !fm.empty()) { g.xfmt = fm[0]; g.xcopy = false; }   // fall back to blit (format conversion)
    if (!g.xfmt) return false;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = g.xfmt; ci.sampleCount = 1; ci.width = s.w; ci.height = s.h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(XRCHK(xrCreateSwapchain(g.sess, &ci, &g.xsc)))) return false;
    xrEnumerateSwapchainImages(g.xsc, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> im(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g.xsc, n, &n, (XrSwapchainImageBaseHeader*)im.data());
    g.ximgs.clear(); for (auto& i : im) g.ximgs.push_back(i.image);
    if (g.xsc1) { xrDestroySwapchain(g.xsc1); g.xsc1 = XR_NULL_HANDLE; }
    if (XR_FAILED(XRCHK(xrCreateSwapchain(g.sess, &ci, &g.xsc1)))) return false;
    std::vector<XrSwapchainImageVulkanKHR> im1(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g.xsc1, n, &n, (XrSwapchainImageBaseHeader*)im1.data());
    g.ximgs1.clear(); for (auto& i : im1) g.ximgs1.push_back(i.image);
    if (g.xsc2) { xrDestroySwapchain(g.xsc2); g.xsc2 = XR_NULL_HANDLE; }
    if (XR_FAILED(XRCHK(xrCreateSwapchain(g.sess, &ci, &g.xsc2)))) return false;
    std::vector<XrSwapchainImageVulkanKHR> im2(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g.xsc2, n, &n, (XrSwapchainImageBaseHeader*)im2.data());
    g.ximgs2.clear(); for (auto& i : im2) g.ximgs2.push_back(i.image);
    g.evalid[0] = g.evalid[1] = false; g.cvalid = false; g.Lpid = ~0ull;
    g.xw = s.w; g.xh = s.h;
    vlog("[xr] XR swapchain %ux%u fmt %lld (%s), %u images", s.w, s.h, (long long)g.xfmt, g.xcopy ? "bitwise copy" : "blit", n);
    return true;
}

// OSD panel: R8G8B8A8_SRGB XR swapchain + host buffer (kSlots regions), uploaded only when the text changes
bool make_osd_resources() {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB; ci.sampleCount = 1; ci.width = kOW; ci.height = kOH; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(XRCHK(xrCreateSwapchain(g.sess, &ci, &g.osc)))) return false;
    uint32_t n = 0; xrEnumerateSwapchainImages(g.osc, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> im(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g.osc, n, &n, (XrSwapchainImageBaseHeader*)im.data());
    for (auto& i : im) g.oimgs.push_back(i.image);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = (VkDeviceSize)kOW * kOH * 4 * kSlots; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateBuffer(g.dev, &bci, nullptr, &g.obuf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; p_vkGetBufferMemoryRequirements(g.dev, g.obuf, &mr);
    auto gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)g.gipa(g.vki, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp; gmp(g.pd, &mp);
    uint32_t mt = UINT32_MAX; const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; ++i) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) mt = i;
    if (mt == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory mem; if (p_vkAllocateMemory(g.dev, &mai, nullptr, &mem) != VK_SUCCESS) return false;
    if (p_vkBindBufferMemory(g.dev, g.obuf, mem, 0) != VK_SUCCESS || p_vkMapMemory(g.dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&g.omap) != VK_SUCCESS) return false;
    vlog("[osd] resources: %ux%u swapchain, %u images", kOW, kOH, n);
    return true;
}
// drop every per-frame override so the flat game looks vanilla (headset gone / session stopped / exception)
void disengage(const char* why) {
    if (!g.engaged) return;
    g.engaged = false;
    g_hud_redirect.store(0);
    camhook_set(0.f, 0.f, 0.f); camhook_set_eye(0.f); camhook_set_render_fov(0.f); camhook_set_pull(0.f);
    static const float I[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; static const float Z[3] = {0, 0, 0};
    camhook_set_view(false, I, 0.f, Z, 0);
    g.fov_written = false;
    vlog("[vr] disengaged (%s): camera/FOV/HUD back to the flat game", why);
}
// S7b: transparent HUD target (game swapchain format, so the UI pipelines stay compatible) + XR quad swapchain
bool make_hud(const GameSc& s) {
    if (g.hfailed) return false;
    if (g.himg && g.hw == s.w && g.hh == s.h) return true;
    g_hud_redirect.store(0); g.hready = false;   // old image/view leaked on resize (may still be referenced by in-flight game command buffers)
    if (g.hsc) { xrDestroySwapchain(g.hsc); g.hsc = XR_NULL_HANDLE; }
    auto fail = [](const char* w) { vlog("[hud] setup failed: %s -> HUD stays in the game image", w); g.hfailed = true; g.himg = VK_NULL_HANDLE; return false; };
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = g.xfmt; ci.sampleCount = 1; ci.width = s.w; ci.height = s.h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (!g.xcopy || XR_FAILED(XRCHK(xrCreateSwapchain(g.sess, &ci, &g.hsc)))) return fail("xr swapchain");
    uint32_t n = 0; xrEnumerateSwapchainImages(g.hsc, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> im(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g.hsc, n, &n, (XrSwapchainImageBaseHeader*)im.data());
    g.himgs.clear(); for (auto& i : im) g.himgs.push_back(i.image);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = s.fmt; ii.extent = {s.w, s.h, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img; if (p_vkCreateImage(g.dev, &ii, nullptr, &img) != VK_SUCCESS) return fail("vkCreateImage");
    VkMemoryRequirements mr; p_vkGetImageMemoryRequirements(g.dev, img, &mr);
    auto gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)g.gipa(g.vki, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp; gmp(g.pd, &mp); uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; ++i) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) mt = i;
    if (mt == UINT32_MAX) return fail("memory type");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory mem; if (p_vkAllocateMemory(g.dev, &mai, nullptr, &mem) != VK_SUCCESS || p_vkBindImageMemory(g.dev, img, mem, 0) != VK_SUCCESS) return fail("memory");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = s.fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v; if (p_vkCreateImageView(g.dev, &vi, nullptr, &v) != VK_SUCCESS) return fail("image view");
    g.himg = img; g.hview = v; g.hw = s.w; g.hh = s.h; g.hud_seen = g_hud_passes.load();
    vlog("[hud] target %ux%u fmt %d + XR quad swapchain fmt %lld, %u images", s.w, s.h, (int)s.fmt, (long long)g.xfmt, n);
    return true;
}
void poll_events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g.inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            g.sstate = e->state; vlog("[xr] session state -> %d", (int)e->state);
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(XRCHK(xrBeginSession(g.sess, &bi)))) { g.running = true; vlog("[xr] session RUNNING");
                    char rk[8], hk[8], b1[8], b2[8]; osd("EnshroudedVR  \xC2\xB7  %s = recenter  \xC2\xB7  %s = HUD", key_name(g.keys[1], b1), key_name(g.keys[23], b2)); g.ountil = now_s() + 6.0; (void)rk; (void)hk; }
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                g.running = false; XRCHK(xrEndSession(g.sess)); vlog("[xr] session stopped");
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                g.running = false; g.active = false; vlog("[xr] session exiting/lost -> flat mode");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g.running = false; g.active = false; vlog("[xr] instance loss pending -> flat mode");
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
             VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}
} // namespace

bool xr_hud_target(uint32_t w, uint32_t h, VkImageView* v) {
    if (!g_hud_redirect.load(std::memory_order_acquire) || !g.hview || w != g.hw || h != g.hh) return false;
    *v = g.hview; return true;
}
void xr_hud_barrier(VkCommandBuffer cb, int after) {   // recorded INTO the game's command buffer around its UI pass
    if (!after) barrier(cb, g.himg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    else { barrier(cb, g.himg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
           g_hud_passes.fetch_add(1, std::memory_order_relaxed); }
}

static VkSemaphore g_out_sem = VK_NULL_HANDLE;
static VkSemaphore xr_on_present_impl(VkQueue q, const VkPresentInfoKHR* pi) {
    if (g.crashed) { g.crashed = false; vlog("[xr] EXCEPTION inside the VR frame -> VR disabled, game continues flat"); }
    if (!g.active || !g.dev) { disengage("XR inactive"); return VK_NULL_HANDLE; }
    g.presents++;
    if (!g.sess) {
        if (g.session_failed) return VK_NULL_HANDLE;
        if (!create_session(q)) { g.session_failed = true; vlog("[xr] session setup failed -> flat mode"); return VK_NULL_HANDLE; }
    }
    poll_events();
    if (!g.running) { disengage("session not running"); return VK_NULL_HANDLE; }
    if (q != g.q || pi->swapchainCount != 1) return VK_NULL_HANDLE;
    GameSc s; { std::lock_guard<std::mutex> lk(g.sc_mtx); auto it = g.scs.find(pi->pSwapchains[0]); if (it == g.scs.end()) return VK_NULL_HANDLE; s = it->second; }
    uint32_t gi = pi->pImageIndices[0];
    if (gi >= s.imgs.size() || !s.w) return VK_NULL_HANDLE;
    if (!g.xsc || g.xw != s.w || g.xh != s.h) if (!make_xr_swapchain(s)) { g.active = false; vlog("[xr] no usable XR swapchain -> flat mode"); return VK_NULL_HANDLE; }

    // S8a-2: is the image presented NOW the left half of a pair? (peek the frame-tag ring exactly as the tagging code below will)
    bool skip = false;
    if (g.pair_rate && g.pairing_prev && g.cvalid && !g.cur_last) {
        uint64_t u = camhook_view_used_seq(), sq = 0;
        if (g.lag == 0) sq = u; else if (g.pn >= (uint64_t)g.lag) sq = g.pres_seq[(g.pn - g.lag) % 16];
        if (sq && g.hn - sq < 14 && g.hseq[(sq - 1) % 16] == sq && g.heye[(sq - 1) % 16] == 0) skip = true;
    }
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!skip) {
        double t0 = now_s();
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(XRCHK(xrWaitFrame(g.sess, &wi, &fs)))) return VK_NULL_HANDLE;
        g.wait_ms += (now_s() - t0) * 1000.0;
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(XRCHK(xrBeginFrame(g.sess, &bi)))) return VK_NULL_HANDLE;
        g.last_pdt = fs.predictedDisplayTime; if (fs.predictedDisplayPeriod > 0) g.last_period = fs.predictedDisplayPeriod;
    } else {   // no XR frame: L image only goes into its swapchain; pose for the next render predicted one period ahead
        fs.predictedDisplayTime = g.last_pdt + g.last_period; fs.predictedDisplayPeriod = g.last_period; fs.shouldRender = XR_TRUE;
        g.xr_skips++;
    }

    // ---- S5: head pose -> game camera angles (applied to the NEXT rendered frame by the camhook trampoline)
    hotkeys();
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO}; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime; li.space = g.local;
    XrViewState vst{XR_TYPE_VIEW_STATE}; uint32_t nv = 0;
    bool located = XR_SUCCEEDED(xrLocateViews(g.sess, &li, &vst, 2, &nv, views)) && nv == 2 &&
                   (vst.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    VkSemaphore out = VK_NULL_HANDLE;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD}, cq{XR_TYPE_COMPOSITION_LAYER_QUAD}, hq{XR_TYPE_COMPOSITION_LAYER_QUAD}, oq{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[4]; uint32_t nl = 0;
    XrCompositionLayerProjection pl{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    float cx = 0, cy = 0, mpp = 0;
    bool cur = cursor_state(cx, cy, mpp);
    { static bool pc = false; static int nflip = 0; if (cur != pc) { pc = cur; if (++nflip <= 40) { POINT sp; GetCursorPos(&sp);
          vlog("[cur] %s #%d screen %ld,%ld", cur ? "SHOWN (flat screen)" : "hidden (VR)", nflip, sp.x, sp.y); } } }
    if (cur && !g.csc && !g.cfailed && !make_cursor_resources()) { g.cfailed = true; vlog("[xr] cursor layer disabled (setup failed)"); }
    if (g.cfailed) cur = false;
    bool cupload = cur && (g.cdirty || !g.cready); uint32_t cxi = 0;
    bool want_track = g.tracking && located && !cur;          // cursor visible = menu/inventory/map -> flat screen, head look paused
    if (want_track && !g.hook_tried) { g.hook_tried = true; g.hook_ok = camhook_install(); vlog("[vr] camhook %s", g.hook_ok ? "ready" : "FAILED -> no head tracking"); }
    bool track = want_track && g.hook_ok;
    if (track) g.engaged = true;
    int hs = (int)(g.hn % 16);
    const bool pairing = track && g.pair_lock && g.head_look && g.head_mode && g.stereo != 0 && camhook_view_ok() && camhook_eye_ok();
    g.pairing_prev = pairing; g.cur_last = cur;
    if (pairing) {   // S8a: frame hn renders eye hn&1; the R frame reuses the head pose sampled for the L frame of the same pair
        if ((g.hn & 1) == 0) { g.pair_views[0] = views[0]; g.pair_views[1] = views[1]; }
        else { views[0] = g.pair_views[0]; views[1] = g.pair_views[1]; }
    }
    g.hpair[g.hn % 16] = g.hn >> 1;
    if (track) {
        float yaw, pitch, roll; q2e(views[0].pose.orientation, yaw, pitch, roll);
        if (!g.centered) { g.yaw0 = yaw; g.centered = true; g.pos_centered = false; vlog("[vr] centered: head yaw %.3f", yaw); }
        g.cur_yaw = wrap_pi(yaw - g.yaw0); g.cur_pitch = pitch; g.cur_roll = roll;
        const bool view = g.head_mode && camhook_view_ok();
        if (view) camhook_set(0.f, 0.f, 0.f);
        else camhook_set(g.ys * g.cur_yaw, g.ps * g.cur_pitch, g.rsg * g.cur_roll);   // orbit (legacy); rsg 0 = roll off
        // S6: the NEXT game frame renders eye (hn&1): camera shifted along its local right axis by -/+ IPD/2
        { const XrVector3f& a = views[0].pose.position; const XrVector3f& b = views[1].pose.position;
          float d3 = sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
          if (d3 > 0.045f && d3 < 0.085f) g.ipd = d3; }
        int eye = (g.stereo != 0 && camhook_eye_ok()) ? (int)(g.hn & 1) : -1;
        g.heye[hs] = eye;
        const float eye_x = eye < 0 ? 0.f : (eye == 0 ? -0.5f : 0.5f) * g.ipd * (g.ipd_pct / 100.f) * (float)g.stereo;
        if (view) {
            // H rows = rotated camera axes in base-camera coords (r,u,f): yaw about up (XR yaw>0 = left), then pitch about new right (>0 = up)
            const float y = g.head_look ? g.ys * g.cur_yaw : 0.f, p = g.head_look ? g.ps * g.cur_pitch : 0.f;
            const float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
            const float H[3][3] = {{cy, 0.f, sy}, {sp * sy, cp, -sp * cy}, {-cp * sy, sp, cp * cy}};
            float hp[3] = {0.f, 0.f, 0.f};
            XrVector3f c{(views[0].pose.position.x + views[1].pose.position.x) * 0.5f, (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                         (views[0].pose.position.z + views[1].pose.position.z) * 0.5f};
            if (!g.pos_centered) { g.pos0 = c; g.pos_centered = true; }
            if (g.head_pos && g.head_look) {
                float dx = c.x - g.pos0.x, dy = c.y - g.pos0.y, dz = c.z - g.pos0.z, c0 = cosf(g.yaw0), s0 = sinf(g.yaw0);
                float lx = c0 * dx - s0 * dz, lz = s0 * dx + c0 * dz;      // into the recentered frame
                hp[0] = fmaxf(-1.f, fminf(1.f, lx)); hp[1] = fmaxf(-1.f, fminf(1.f, dy)); hp[2] = fmaxf(-1.f, fminf(1.f, -lz));
            }
            camhook_set_eye(0.f);
            { uint64_t hh, obj; camhook_stats(&hh, &obj); const CamFields* cf = camhook_fields(); float dv = 0.f;
              const uint32_t doff = (cf && cf->build && strcmp(cf->build, "a07fae72") == 0) ? 0x14a4u : 0u;   // boom length (render builder: pos -= dist*fwd)
              if (doff && obj && seh_readf(obj + doff, &dv) && dv >= 0.f && dv < 50.f) g.cam_dist = dv; else g.cam_dist = 0.f;
              camhook_set_pull(g.cam_dist * (1.f - g.cam_dist_pct / 100.f)); }
            camhook_set_view(true, H, eye_x, hp, g.hn + 1); g.hseq[hs] = g.hn + 1;
        } else {
            static const float I[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; static const float Z[3] = {0, 0, 0};
            camhook_set_view(false, I, 0.f, Z, 0);
            camhook_set_eye(eye_x);
        }
        g.hist[hs][0] = views[0]; g.hist[hs][1] = views[1]; g.hvalid[hs] = true;
        { // S7: HMD FOV (symmetric frustum enclosing both eyes)
          float tu = 0, tr = 0;
          for (int e = 0; e < 2; ++e) {
              const XrFovf& fv = views[e].fov;
              tu = fmaxf(tu, fmaxf(tanf(fv.angleUp), tanf(-fv.angleDown)));
              tr = fmaxf(tr, fmaxf(tanf(fv.angleRight), tanf(-fv.angleLeft)));
          }
          if (tu > 0.3f && tu < 4.f && tr > 0.3f && tr < 4.f) { g.hmd_tu = tu; g.hmd_tr = tr; if (!g.hmd_fov_ok) { g.hmd_fov_ok = true;
              vlog("[vr] HMD fov: up/down tan %.3f (%.1f deg V), left/right tan %.3f (%.1f deg H)", tu, 2 * atanf(tu) * 57.3f, tr, 2 * atanf(tr) * 57.3f); } }
          uint64_t hh, obj; camhook_stats(&hh, &obj); float gf, ga;
          const CamFields* cf = camhook_fields();
          if (!g.fov_off && cf && cf->fov) { g.fov_off = cf->fov; vlog("[vr] fov field +0x%x (build %s), aspect +0x%x", cf->fov, cf->build, cf->aspect); }
          if (cf && cf->aspect && obj && seh_readf(obj + cf->aspect, &ga) && ga > 0.5f && ga < 4.f) g.faspect = ga;
          if (g.fov_off && obj && seh_readf(obj + g.fov_off, &gf) && gf > 0.3f && gf < 2.8f && !g.fov_ovr) {
              if (fabsf(gf - g.fov_game) > 1e-4f) vlog("[vr] game fov %.4f rad (%.1f deg)", gf, gf * 57.29578f);
              g.fov_game = gf; }
          float want = 0.f;
          if (g.fov_ovr && g.fov_off && g.hmd_fov_ok) {
              float k = g.fov_pct / 100.f;   // fill the HMD vertically; H-axis field -> scale by the render aspect
              // game FOV is VERTICAL (run 21+ stable projection). FILL: vertical = HMD; FIT: horizontal = HMD (16:9 -> bands top/bottom)
              const float ra = g.faspect > 0.5f ? g.faspect : 1.7778f;
              want = g.fov_axis ? 2.f * atanf(g.hmd_tr * k / ra) : 2.f * atanf(g.hmd_tu * k);
              if (want > 2.7f) want = 2.7f;
              if (!g.fov_written) g.fov_saved = g.fov_game;
              if (camhook_fov_ok()) camhook_set_render_fov(want);   // S8b: the renderer's copy; CamObj fields untouched
              else { camhook_set_write(0, g.fov_off, want);
              if (obj) {
                  seh_writef(obj + g.fov_off, want);
                  if (g.fov_mode && cf && cf->fov == g.fov_off) {
                      if (cf->fov_deg_setting) { camhook_set_write(1, cf->fov_deg_setting, want * 57.29578f); seh_writef(obj + cf->fov_deg_setting, want * 57.29578f); }
                      for (uint32_t co : cf->fov_copies) if (co) seh_writef(obj + co, want);
                  }
              } }
          } else if (g.fov_written) {   // restore the game's value once after override off
              const float rv = g.fov_saved > 0.3f ? g.fov_saved : 1.3963f; g.fov_game = rv; camhook_set_render_fov(0.f);
              if (obj && g.fov_off) { seh_writef(obj + g.fov_off, rv); if (cf && cf->fov == g.fov_off) {
                  if (cf->fov_deg_setting) seh_writef(obj + cf->fov_deg_setting, rv * 57.29578f);
                  for (uint32_t co : cf->fov_copies) if (co) seh_writef(obj + co, rv); } }
              camhook_set_write(0, 0, 0.f); camhook_set_write(1, 0, 0.f); g.fov_written = false;
          }
          if (want > 0.f) g.fov_written = true;
          g.hfov[hs] = want > 0.f ? want : g.fov_game; }
        if (g.rsg == 0) {   // roll not sent to the game -> image pose without roll; compositor counter-rotates for real head roll
            float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f), cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
            XrQuaternionf q{cy * sp, sy * cp, -sy * sp, cy * cp};   // Ry(yaw) * Rx(pitch)
            g.hist[hs][0].pose.orientation = q; g.hist[hs][1].pose.orientation = q;
        }
        if (view && !g.head_look) {   // mouse-only: image is head-independent -> show it as a world-locked stereo screen at the recenter pose
            const float hy = g.yaw0 * 0.5f; XrQuaternionf q0{0.f, sinf(hy), 0.f, cosf(hy)};
            const float rx = cosf(g.yaw0), rz = -sinf(g.yaw0);
            for (int e = 0; e < 2; ++e) {
                const float s = (e == 0 ? -0.5f : 0.5f) * g.ipd;
                g.hist[hs][e].pose.orientation = q0;
                g.hist[hs][e].pose.position = {g.pos0.x + rx * s, g.pos0.y, g.pos0.z + rz * s};
            }
        }
        if (g.pose_log && now_s() - g.t_pose >= 0.5) {
            g.t_pose = now_s(); const XrQuaternionf& a = views[0].pose.orientation; const XrQuaternionf& b = views[1].pose.orientation;
            vlog("[pose] L q=(%.3f %.3f %.3f %.3f) R q=(%.3f %.3f %.3f %.3f) pos L=(%.3f %.3f %.3f) -> yaw=%.3f pitch=%.3f roll=%.3f (deg %.1f %.1f %.1f)",
                 a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
                 yaw, pitch, roll, yaw * 57.3f, pitch * 57.3f, roll * 57.3f);
        }
    } else {
        camhook_set(0.f, 0.f, 0.f); camhook_set_eye(0.f); g.hvalid[hs] = false; g.heye[hs] = -1;
        { static const float I[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; static const float Z[3] = {0, 0, 0}; camhook_set_view(false, I, 0.f, Z, 0); }
    }
    if (located) { g.efov[0] = views[0].fov; g.efov[1] = views[1].fov; g.efov_ok = true; }
    g.hn++;
    // pose the presented image was rendered with: set lag+1 presents ago (this frame's pose = hn-1)
    int us = (int)((g.hn + 16 - 2 - g.lag) % 16);
    if (track && g.head_mode && camhook_view_ok()) {   // view mode: image = pose the builder consumed, `lag` presents ago
        uint64_t u = camhook_view_used_seq();
        g.pres_seq[g.pn % 16] = u; g.pn++;
        if (u) { g.seq_age_sum += g.hn - (u - 1); g.seq_age_n++; }
        if (g.pn > (uint64_t)g.lag) {
            uint64_t sq = g.pres_seq[(g.pn - 1 - g.lag) % 16];
            if (sq && g.hn - sq < 14 && g.hseq[(sq - 1) % 16] == sq) { us = (int)((sq - 1) % 16); g.tag_ok++; } else g.tag_fb++;
        }
    } else g.pn = 0;
    bool proj = track && g.hn > (uint64_t)(2 + g.lag) && g.hvalid[us];
    int eye_img = proj ? g.heye[us] : -1;                     // which eye the presented image was rendered for (-1 = mono)
    if (eye_img < 0) g.evalid[0] = g.evalid[1] = false;
    const bool pmode = pairing && eye_img >= 0;
    const uint64_t pid = g.hpair[us];
    XrSwapchain tsc = eye_img == 1 ? g.xsc1 : (pmode && (pid & 1)) ? g.xsc2 : g.xsc;
    std::vector<VkImage>& timgs = eye_img == 1 ? g.ximgs1 : (pmode && (pid & 1)) ? g.ximgs2 : g.ximgs;
    { uint64_t eh = camhook_eye_hits(); if (g.hook_ok && camhook_eye_ok() && track && eh == g.eye_hits_prev) g.eye_anom++; g.eye_calls += eh - g.eye_hits_prev; g.eye_hits_prev = eh; }
    g.fovy = g.hfov[us] > 0.f ? g.hfov[us] : g.fov_game;
    { uint64_t pd = camhook_proj_dst(); float m00 = 0.f, m11 = 0.f;   // S8b-3: the frustum the GPU actually used (perspective 0xf4e5e0: m00 @+0, m11 @+0x14)
      if (pd && seh_readf(pd + 0x00, &m00) && seh_readf(pd + 0x14, &m11) && fabsf(m00) > 0.1f && fabsf(m11) > 0.1f && fabsf(m00) < 20.f && fabsf(m11) < 20.f) {
          g.mat_tr = 1.f / fabsf(m00); g.mat_tu = 1.f / fabsf(m11); } }
    // S7b: UI pass -> HUD quad only while the world is a projection layer (menus/inventory with cursor stay in the flat screen)
    const bool hud_on = g.hud_mode && proj && !cur && g.view && make_hud(s);
    g_hud_redirect.store(hud_on ? 1 : 0, std::memory_order_release);
    bool hcopy = false; uint32_t hxi = 0;
    const bool osd_vis = now_s() < g.ountil && g.view;
    if (osd_vis && !g.osc && !g.ofailed && !make_osd_resources()) { g.ofailed = true; vlog("[osd] disabled (setup failed)"); }
    bool oupload = osd_vis && !g.ofailed && g.osc && g.odirty;
    if (fs.shouldRender) {
        uint32_t xi = 0; XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; swi.timeout = 100000000;   // 100 ms
        if (XR_SUCCEEDED(XRCHK(xrAcquireSwapchainImage(tsc, &ai, &xi))) && XRCHK(xrWaitSwapchainImage(tsc, &swi)) == XR_SUCCESS) {
            int slot_i = g.slot; Slot& sl = g.slots[g.slot]; g.slot = (g.slot + 1) % kSlots;
            p_vkWaitForFences(g.dev, 1, &sl.fence, VK_TRUE, 1000000000ull);
            p_vkResetFences(g.dev, 1, &sl.fence);
            p_vkResetCommandBuffer(sl.cb, 0);
            VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            p_vkBeginCommandBuffer(sl.cb, &cbi);
            VkImage src = s.imgs[gi], dst = timgs[xi];
            barrier(sl.cb, src, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            barrier(sl.cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            if (g.xcopy) {
                VkImageCopy c{}; c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.extent = {s.w, s.h, 1};
                p_vkCmdCopyImage(sl.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            } else {
                VkImageBlit b{}; b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                b.srcOffsets[1] = {(int32_t)s.w, (int32_t)s.h, 1}; b.dstOffsets[1] = {(int32_t)s.w, (int32_t)s.h, 1};
                p_vkCmdBlitImage(sl.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
            }
            barrier(sl.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            barrier(sl.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            const bool hud_eye_ok = !g.hud_sync || eye_img != 1;   // S7b-2: AER -> world-anchored prompts are projected per eye; keep one eye's UI
            if (hud_on && !hud_eye_ok) g.hud_skips++;
            if (hud_on && hud_eye_ok && g_hud_passes.load() >= g.hud_seen + 2) {   // the target holds a finished UI frame in TRANSFER_SRC layout
                XrSwapchainImageAcquireInfo hai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (XR_SUCCEEDED(XRCHK(xrAcquireSwapchainImage(g.hsc, &hai, &hxi))) && XRCHK(xrWaitSwapchainImage(g.hsc, &swi)) == XR_SUCCESS) {
                    VkImage hd = g.himgs[hxi];
                    barrier(sl.cb, hd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkImageCopy c{}; c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c.extent = {g.hw, g.hh, 1};
                    p_vkCmdCopyImage(sl.cb, g.himg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                    barrier(sl.cb, hd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    hcopy = true;
                }
            }
            if (oupload) {
                XrSwapchainImageAcquireInfo oai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; uint32_t oxi = 0;
                if (XR_SUCCEEDED(XRCHK(xrAcquireSwapchainImage(g.osc, &oai, &oxi))) && XRCHK(xrWaitSwapchainImage(g.osc, &swi)) == XR_SUCCESS) {
                    VkDeviceSize off = (VkDeviceSize)slot_i * kOW * kOH * 4;
                    memcpy(g.omap + off, g.opx.data(), (size_t)kOW * kOH * 4);
                    VkImage oi = g.oimgs[oxi];
                    barrier(sl.cb, oi, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkBufferImageCopy bc{}; bc.bufferOffset = off; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {kOW, kOH, 1};
                    p_vkCmdCopyBufferToImage(sl.cb, g.obuf, oi, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
                    barrier(sl.cb, oi, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                } else oupload = false;
            }
            if (cupload) {
                XrSwapchainImageAcquireInfo cai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (XR_SUCCEEDED(XRCHK(xrAcquireSwapchainImage(g.csc, &cai, &cxi))) && XRCHK(xrWaitSwapchainImage(g.csc, &swi)) == XR_SUCCESS) {
                    VkDeviceSize off = (VkDeviceSize)slot_i * kCur * kCur * 4;
                    memcpy(g.cmap + off, g.cpx.data(), kCur * kCur * 4);
                    VkImage ci_ = g.cimgs[cxi];
                    barrier(sl.cb, ci_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkBufferImageCopy bc{}; bc.bufferOffset = off; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {kCur, kCur, 1};
                    p_vkCmdCopyBufferToImage(sl.cb, g.cbuf, ci_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
                    barrier(sl.cb, ci_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                } else cupload = false;
            }
            p_vkEndCommandBuffer(sl.cb);
            std::vector<VkPipelineStageFlags> st(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.waitSemaphoreCount = pi->waitSemaphoreCount; si.pWaitSemaphores = pi->pWaitSemaphores; si.pWaitDstStageMask = st.data();
            si.commandBufferCount = 1; si.pCommandBuffers = &sl.cb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sl.sem;
            VkResult vr = p_vkQueueSubmit(q, 1, &si, sl.fence);
            if (vr == VK_SUCCESS) { out = sl.sem; g_out_sem = out; } else vlog("[xr] copy submit failed %d", vr);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XRCHK(xrReleaseSwapchainImage(tsc, &ri));
            if (cupload) { XRCHK(xrReleaseSwapchainImage(g.csc, &ri)); if (out) { g.cready = true; g.cdirty = false; } }
            if (hcopy) { XRCHK(xrReleaseSwapchainImage(g.hsc, &ri)); if (out) { g.hready = true; g.hud_copies++; } }
            if (oupload) { XRCHK(xrReleaseSwapchainImage(g.osc, &ri)); if (out) { g.oready = true; g.odirty = false; } }
            if (out) {
                quad.layerFlags = 0; quad.space = g.local; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = g.xsc; quad.subImage.imageRect = {{0, 0}, {(int32_t)s.w, (int32_t)s.h}};
                const float sh = kScreenH, sw = kScreenH * float(s.w) / float(s.h);
                if (proj) {   // mono: same game image for both eyes, at the head pose it was rendered with, game's frustum
                    const float asp = float(s.w) / float(s.h);   // render aspect (swapchain); game aspect field logged as faspect
                    float tu = tanf(g.fovy * 0.5f), tr = tu * asp;   // fovy is always the vertical render FOV
                    if (g.mat_proj && g.mat_tu > 0.f) { tu = g.mat_tu; tr = g.mat_tr; }   // trust the game's own projection matrix
                    if (pmode) {   // S8a: only complete pairs (same head pose for L and R) reach the compositor
                        XrPosef ep; ep.orientation = g.hist[us][0].pose.orientation; ep.position = g.hist[us][eye_img].pose.position;
                        g.stereo_frames++;
                        if (eye_img == 0) { g.Lpid = pid; g.Lsc = tsc; g.Lpose = ep; }
                        else if (g.Lpid == pid) { g.cLsc = g.Lsc; g.cLpose = g.Lpose; g.cRpose = ep; g.cvalid = true; g.pairs_ok++; }
                        else g.pairs_broken++;
                        if (g.cvalid) {
                            if (eye_img == 0) g.pair_holds++;
                            for (int e = 0; e < 2; ++e) {
                                pv[e].pose = e == 0 ? g.cLpose : g.cRpose;
                                pv[e].fov = {-atanf(tr), atanf(tr), atanf(tu), -atanf(tu)};
                                pv[e].subImage.swapchain = e == 0 ? g.cLsc : g.xsc1;
                                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)s.w, (int32_t)s.h}};
                            }
                            pl.space = g.local; pl.viewCount = 2; pl.views = pv;
                            layers[nl++] = (XrCompositionLayerBaseHeader*)&pl; g.proj_frames++;
                            goto layers_done;
                        }
                        // no complete pair yet: fall through to the per-eye path (left image lives in tsc)
                        g.epose[eye_img] = ep; g.evalid[eye_img] = true;
                        for (int e = 0; e < 2; ++e) {
                            int se = g.evalid[e] ? e : eye_img;
                            pv[e].pose = g.epose[se]; if (se != e) pv[e].pose.position = g.hist[us][e].pose.position;
                            pv[e].fov = {-atanf(tr), atanf(tr), atanf(tu), -atanf(tu)};
                            pv[e].subImage.swapchain = se == 1 ? g.xsc1 : (eye_img == 0 ? tsc : (g.Lsc ? g.Lsc : g.xsc));
                            pv[e].subImage.imageRect = {{0, 0}, {(int32_t)s.w, (int32_t)s.h}};
                        }
                        pl.space = g.local; pl.viewCount = 2; pl.views = pv;
                        layers[nl++] = (XrCompositionLayerBaseHeader*)&pl; g.proj_frames++;
                        goto layers_done;
                    }
                    if (eye_img >= 0) {   // S6: this image is one eye; the other eye keeps its last image + pose
                        g.epose[eye_img].orientation = g.hist[us][0].pose.orientation;
                        g.epose[eye_img].position = g.hist[us][eye_img].pose.position;
                        g.evalid[eye_img] = true; g.stereo_frames++;
                    }
                    for (int e = 0; e < 2; ++e) {
                        int src_e = eye_img < 0 ? -1 : g.evalid[e] ? e : eye_img;
                        if (src_e < 0) { pv[e].pose = g.hist[us][e].pose; pv[e].pose.orientation = g.hist[us][0].pose.orientation; }
                        else { pv[e].pose = g.epose[src_e]; if (src_e != e) pv[e].pose.position = g.hist[us][e].pose.position; }
                        pv[e].fov = {-atanf(tr), atanf(tr), atanf(tu), -atanf(tu)};
                        pv[e].subImage.swapchain = (src_e == 1) ? g.xsc1 : g.xsc;
                        pv[e].subImage.imageRect = {{0, 0}, {(int32_t)s.w, (int32_t)s.h}};
                    }
                    pl.space = g.local; pl.viewCount = 2; pl.views = pv;
                    layers[nl++] = (XrCompositionLayerBaseHeader*)&pl; g.proj_frames++;
                    goto layers_done;
                }
                quad.pose.orientation.w = 1.f; quad.pose.position = {0.f, 0.f, -kScreenDist};   // 2 m in front of the start pose
                quad.size = {sw, sh};                                                     // ~62 deg wide virtual screen
                layers[nl++] = (XrCompositionLayerBaseHeader*)&quad;
                if (cur && g.cready) {
                    const float half = kCur * mpp * 0.5f;
                    cq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    cq.space = g.local; cq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    cq.subImage.swapchain = g.csc; cq.subImage.imageRect = {{0, 0}, {kCur, kCur}};
                    cq.pose.orientation.w = 1.f;
                    cq.pose.position = {-sw * 0.5f + cx * mpp + half, sh * 0.5f - cy * mpp - half, -kScreenDist + 0.002f};
                    cq.size = {kCur * mpp, kCur * mpp};
                    layers[nl++] = (XrCompositionLayerBaseHeader*)&cq;
                }
            layers_done:;
                // v1.0.3: submit each projection view with the RUNTIME's per-eye FOV (asymmetric on Quest 3) and the matching
                // sub-rect of the (symmetric, wider) game image. Meta's compositor does not fuse a symmetric layer on an
                // asymmetric display; VD did. Intersection keeps geometry exact when the game frustum is narrower (FIT / FOV off).
                if (g.layer_eyefov && nl > 0 && layers[0] == (XrCompositionLayerBaseHeader*)&pl && g.efov_ok) {
                    for (int e = 0; e < 2; ++e) {
                        const float gr = tanf(pv[e].fov.angleRight), gu = tanf(pv[e].fov.angleUp);   // symmetric game frustum
                        if (gr <= 0.f || gu <= 0.f) continue;
                        const XrFovf& ef = g.efov[e];
                        float l = fmaxf(tanf(ef.angleLeft), -gr), r = fminf(tanf(ef.angleRight), gr);
                        float u = fminf(tanf(ef.angleUp), gu), dn = fmaxf(tanf(ef.angleDown), -gu);
                        if (r - l < 0.05f || u - dn < 0.05f) continue;
                        const XrRect2Di full = pv[e].subImage.imageRect; const float W = (float)full.extent.width, Hh = (float)full.extent.height;
                        int x0 = (int)lroundf((l + gr) / (2.f * gr) * W), x1 = (int)lroundf((r + gr) / (2.f * gr) * W);
                        int y0 = (int)lroundf((gu - u) / (2.f * gu) * Hh), y1 = (int)lroundf((gu - dn) / (2.f * gu) * Hh);
                        // re-derive the angles from the integer rect so the pixel mapping stays exact
                        l = -gr + 2.f * gr * x0 / W; r = -gr + 2.f * gr * x1 / W; u = gu - 2.f * gu * y0 / Hh; dn = gu - 2.f * gu * y1 / Hh;
                        pv[e].subImage.imageRect = {{full.offset.x + x0, full.offset.y + y0}, {x1 - x0, y1 - y0}};
                        pv[e].fov = {atanf(l), atanf(r), atanf(u), atanf(dn)};
                    }
                    static bool once = false; if (!once) { once = true;
                        vlog("[vr] layer_eyefov: L rect %d,%d %dx%d fov(%.3f %.3f %.3f %.3f) | R rect %d,%d %dx%d fov(%.3f %.3f %.3f %.3f)",
                             pv[0].subImage.imageRect.offset.x, pv[0].subImage.imageRect.offset.y, pv[0].subImage.imageRect.extent.width, pv[0].subImage.imageRect.extent.height,
                             pv[0].fov.angleLeft, pv[0].fov.angleRight, pv[0].fov.angleUp, pv[0].fov.angleDown,
                             pv[1].subImage.imageRect.offset.x, pv[1].subImage.imageRect.offset.y, pv[1].subImage.imageRect.extent.width, pv[1].subImage.imageRect.extent.height,
                             pv[1].fov.angleLeft, pv[1].fov.angleRight, pv[1].fov.angleUp, pv[1].fov.angleDown); }
                }
                {   // DIAG (env EVR_LAYER_CYCLE): 12 s phases, beeps = phase. 1 normal | 2 same eye position | 3 runtime per-eye FOV | 4 raw runtime views (pose+fov)
                    static int diag = getenv("EVR_LAYER_CYCLE") ? 1 : 0; static int lastph = 0; static double t0 = 0;
                    if (diag && nl > 0 && layers[0] == (XrCompositionLayerBaseHeader*)&pl) {
                        if (t0 == 0) t0 = now_s();
                        int ph = 1 + (int)((now_s() - t0) / 12.0) % 4;
                        if (ph != lastph) { lastph = ph; vlog("[diag] layer phase %d", ph); for (int b = 0; b < ph; ++b) Beep(1200, 90); }
                        if (ph == 2) { XrVector3f c{(pv[0].pose.position.x + pv[1].pose.position.x) * .5f, (pv[0].pose.position.y + pv[1].pose.position.y) * .5f, (pv[0].pose.position.z + pv[1].pose.position.z) * .5f};
                                       pv[0].pose.position = c; pv[1].pose.position = c; }
                        if (ph == 3 && located) { pv[0].fov = views[0].fov; pv[1].fov = views[1].fov; }
                        if (ph == 4 && located) { for (int e = 0; e < 2; ++e) { pv[e].pose = views[e].pose; pv[e].fov = views[e].fov; } }
                        static double tl = 0; if (now_s() - tl > 3.0) { tl = now_s();
                            vlog("[diag] ph%d L pos(%.3f %.3f %.3f) fov(%.3f %.3f %.3f %.3f) | R pos(%.3f %.3f %.3f) fov(%.3f %.3f %.3f %.3f) | runtime L fov(%.3f %.3f %.3f %.3f) R fov(%.3f %.3f %.3f %.3f)", ph,
                                 pv[0].pose.position.x, pv[0].pose.position.y, pv[0].pose.position.z, pv[0].fov.angleLeft, pv[0].fov.angleRight, pv[0].fov.angleUp, pv[0].fov.angleDown,
                                 pv[1].pose.position.x, pv[1].pose.position.y, pv[1].pose.position.z, pv[1].fov.angleLeft, pv[1].fov.angleRight, pv[1].fov.angleUp, pv[1].fov.angleDown,
                                 views[0].fov.angleLeft, views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown, views[1].fov.angleLeft, views[1].fov.angleRight, views[1].fov.angleUp, views[1].fov.angleDown); }
                    }
                }
                if (hud_on && g.hready && nl > 0 && layers[0] == (XrCompositionLayerBaseHeader*)&pl) {
                    const float d = g.hud_dist_cm / 100.f, deg = g.hud_mode == 1 ? 55.f : 70.f;
                    const float wq = 2.f * d * tanf(deg * 0.5f / 57.29578f);
                    hq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied (UI drawn over cleared 0,0,0,0)
                    hq.space = g.view; hq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    hq.subImage.swapchain = g.hsc; hq.subImage.imageRect = {{0, 0}, {(int32_t)g.hw, (int32_t)g.hh}};
                    hq.pose.orientation.w = 1.f; hq.pose.position = {0.f, 0.f, -d};
                    // v1.0.6: 55/70 deg = width of a 16:9 HUD; keep that angular HEIGHT for any frame aspect (UI is laid out by height),
                    // so a square frame (Quest) gives the same text size and top/bottom positions, only narrower.
                    const float hq_h = wq * 9.f / 16.f; hq.size = {hq_h * float(g.hw) / float(g.hh), hq_h};
                    layers[nl++] = (XrCompositionLayerBaseHeader*)&hq;
                }
                if (osd_vis && g.oready && nl < 4) {   // OSD: head-locked, below the view centre, ~27 deg wide
                    const float d = 1.3f, wq = 0.62f;
                    oq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied
                    oq.space = g.view; oq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    oq.subImage.swapchain = g.osc; oq.subImage.imageRect = {{0, 0}, {kOW, kOH}};
                    oq.pose.orientation.w = 1.f; oq.pose.position = {0.f, -0.22f, -d};
                    oq.size = {wq, wq * float(kOH) / float(kOW)};
                    layers[nl++] = (XrCompositionLayerBaseHeader*)&oq;
                }
                g.rendered++;
            }
        }
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nl; ei.layers = layers;
    if (!skip) { XRCHK(xrEndFrame(g.sess, &ei)); g.xr_ends++; }
    g.frames++;
    double t = now_s();
    if (t - g.t_stat >= 5.0) {
        if (g.t_stat > 0)
            vlog("[xr] stats: xr frames=%llu rendered=%llu (%.1f/s) avg xrWaitFrame %.2f ms period %.2f ms", (unsigned long long)g.frames,
                 (unsigned long long)g.rendered, g.rendered / (t - g.t_stat), g.frames ? g.wait_ms / g.frames : 0.0,
                 fs.predictedDisplayPeriod / 1e6);
        if (g.t_stat > 0)
            vlog("[vr] proj=%llu/%llu frames, tracking=%d hook=%d cursor=%d head yaw=%.2f pitch=%.2f roll=%.2f, game fovY=%.3f (%.1f deg) aspect=%.3f lag=%d",
                 (unsigned long long)g.proj_frames, (unsigned long long)g.frames, g.tracking, g.hook_ok, cur, g.cur_yaw, g.cur_pitch, g.cur_roll,
                 g.fovy, g.fovy * 57.29578f, g.faspect, g.lag);
        if (g.t_stat > 0)
            vlog("[vr] stereo=%d eyehook=%d stereo_frames=%llu ipd=%.4f m x%d%% eye-builder calls=%llu frames-without-call=%llu", g.stereo, camhook_eye_ok(),
                 (unsigned long long)g.stereo_frames, g.ipd, g.ipd_pct, (unsigned long long)g.eye_calls, (unsigned long long)g.eye_anom);
        g.stereo_frames = 0; g.eye_anom = 0; g.eye_calls = 0;
        { uint64_t a, b; camhook_view_stats(&a, &b);
          if (g.t_stat > 0) vlog("[vr] head_mode=%s view-hook calls A=%llu B=%llu 6DoF=%d", g.head_mode && camhook_view_ok() ? "view" : "orbit",
                                 (unsigned long long)(a - g.va_prev), (unsigned long long)(b - g.vb_prev), g.head_pos);
          g.va_prev = a; g.vb_prev = b;
          if (g.t_stat > 0) vlog("[vr] frame tags: exact=%llu fallback=%llu, builder pose age avg %.2f XR frames, lag=%d presents", (unsigned long long)g.tag_ok,
                                 (unsigned long long)g.tag_fb, g.seq_age_n ? double(g.seq_age_sum) / g.seq_age_n : -1.0, g.lag);
          g.tag_ok = g.tag_fb = g.seq_age_sum = g.seq_age_n = 0;
          if (g.t_stat > 0) vlog("[vr] pair_lock=%d head_look=%d pairs ok=%llu broken=%llu holds=%llu", g.pair_lock, g.head_look,
                                 (unsigned long long)g.pairs_ok, (unsigned long long)g.pairs_broken, (unsigned long long)g.pair_holds);
          if (g.t_stat > 0) vlog("[vr] pair_rate=%d presents=%llu xr frames=%llu skipped=%llu (%.1f XR fps, %.1f game fps)", g.pair_rate,
                                 (unsigned long long)g.frames, (unsigned long long)g.xr_ends, (unsigned long long)g.xr_skips, g.xr_ends / (t - g.t_stat), g.frames / (t - g.t_stat));
          g.xr_ends = g.xr_skips = 0;
          { static uint64_t hp_prev = 0, hc_prev = 0; uint64_t hp = g_hud_passes.load();
            if (g.t_stat > 0 && g.hud_mode) vlog("[hud] mode %d sync %d: UI passes redirected %llu, quad copies %llu (R-eye skips total %llu), ready %d", g.hud_mode, g.hud_sync,
                                                 (unsigned long long)(hp - hp_prev), (unsigned long long)(g.hud_copies - hc_prev), (unsigned long long)g.hud_skips, g.hready);
            hp_prev = hp; hc_prev = g.hud_copies; }
          { static uint64_t ph_prev = 0; uint64_t ph = camhook_proj_hits();
            if (g.t_stat > 0) vlog("[vr] projection hook calls %llu; GPU matrix: tanU %.4f tanR %.4f (V %.1f H %.1f deg, aspect %.4f) vs swapchain aspect %.4f; layer uses %s",
                                   (unsigned long long)(ph - ph_prev), g.mat_tu, g.mat_tr, 2 * atanf(g.mat_tu) * 57.29578f, 2 * atanf(g.mat_tr) * 57.29578f,
                                   g.mat_tu > 0.f ? g.mat_tr / g.mat_tu : 0.f, g.xh ? float(g.xw) / float(g.xh) : 0.f, g.mat_proj ? "MATRIX" : "fov*aspect");
            ph_prev = ph;
            static uint64_t sh_prev = 0; uint64_t sh = camhook_src_hits();
            if (g.t_stat > 0) vlog("[vr] fov-source hook calls %llu", (unsigned long long)(sh - sh_prev)); sh_prev = sh; }
          { static uint64_t fh_prev = 0; uint64_t fh = camhook_fov_hits();
            if (g.t_stat > 0) vlog("[vr] render fov: hook=%d override=%d %s, %.1f deg V (game %.1f), record writes %llu", camhook_fov_ok(), g.fov_ovr,
                                   g.fov_axis ? "FIT" : "FILL", g.fovy * 57.29578f, g.fov_game * 57.29578f, (unsigned long long)(fh - fh_prev));
            fh_prev = fh; }
          if (g.t_stat > 0) vlog("[vr] boom: game %.2f m, cam_dist_pct=%d -> %.2f m", g.cam_dist, g.cam_dist_pct, g.cam_dist * g.cam_dist_pct / 100.f);
          g.pairs_ok = g.pairs_broken = g.pair_holds = 0; }
        g.t_stat = t; g.frames = g.rendered = 0; g.wait_ms = 0; g.proj_frames = 0;
    }
    return out;
}

// SEH guard: a fault inside the VR frame (runtime/driver/headset unplugged) must not take the game down.
// No C++ objects with destructors in this function (C2712). If our copy submit already consumed the game's
// present semaphores, hand back our signal semaphore so the present stays valid.
VkSemaphore xr_on_present(VkQueue q, const VkPresentInfoKHR* pi) {
    g_out_sem = VK_NULL_HANDLE;
    __try { return xr_on_present_impl(q, pi); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g.active = false; g.crashed = true; g_hud_redirect.store(0); return g_out_sem; }
}
