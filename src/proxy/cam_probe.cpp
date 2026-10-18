// EnshroudedVR — PORT-02a camera probe v2: find keen::ecs::ClientCamera copies, classify live ones, test causality safely.
// Layout (reflection, keen::ecs::ClientCamera, 64 B):
//   +0x00 WorldPosition (sint64 x,y,z; fixed point 2^-32 m)  +0x18 Quaternion (x,y,z,w; Y-up)
//   +0x28 distance f32  +0x2c fovY f32 (rad)  +0x30 aspectRatio  +0x34 nearPlane  +0x38 farPlane
// Run-3 finding: ~10 live copies (always valid, quat changes with mouse, distance follows zoom);
// stack copies (intermittently zero) and reused-heap matches exist -> writes are validated per write
// and allowed only into copies classified LIVE during the watch window.
// Hotkeys (game window foreground):
//   Ctrl+F9  scan + 20 s watch (rotate camera with the mouse meanwhile) -> LIVE classification
//   Ctrl+F10 step: 1 = pin fov*0.6 on ALL live, 2..N+1 = only live[k], then OFF. Beep per step.
//   Ctrl+F11 toggle pin distance=0 on all live
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

void vlog(const char* fmt, ...);
double now_s();
void hwbp_trace(const uintptr_t* addr, const int* len, const char* const* names, int n, double secs);
void camobj_stage(uintptr_t clientcam_addr, float aspect);
bool camhook_install(); void camhook_set(float, float, float); void camhook_stats(uint64_t*, uint64_t*);
extern std::atomic<float> g_backbuffer_aspect;

namespace {

struct Cand {
    uintptr_t addr;
    float fov0 = 0, dist0 = 0;
    int samples = 0, valid = 0, q_changes = 0;
    float lastq[4] = {};
    bool live = false;
    uint64_t rev_fov = 0, rev_dist = 0, skipped = 0;
};

std::mutex g_mtx;
std::vector<Cand> g_cands;
std::vector<size_t> g_live;          // indices into g_cands
std::atomic<int> g_fov_step{0};      // 0 off, 1 all live, 2.. single live[step-2]
std::atomic<int> g_pin_dist{0};
std::atomic<int> g_quat_mode{0};     // auto sweep pins a 90-deg-yawed quaternion instead of fov
int g_upstream = -1, g_upstream_score = -1;   // tierA index whose fov pin propagates to most copies
float g_qpin[4] = {0, 0, 0, 1};
double g_watch_until = 0.0, g_next_watch = 0.0;
bool g_classified = false;

constexpr size_t kMaxCands = 512;
constexpr double kWatchSec = 20.0, kWatchStep = 0.5;

static inline bool in(float v, float lo, float hi) { return v >= lo && v <= hi; }

static bool check_base(const uint8_t* b, float aspect) {
    const float* f = (const float*)b;
    float asp = f[0x30 / 4], nearp = f[0x34 / 4], farp = f[0x38 / 4], fov = f[0x2c / 4], dist = f[0x28 / 4];
    if (fabsf(asp - aspect) > 0.01f) return false;
    if (!in(nearp, 0.001f, 5.0f) || !in(farp, 10.0f, 1.0e7f)) return false;
    if (!(in(fov, 0.2f, 3.0f) || in(fov, 10.0f, 170.0f))) return false;
    if (!in(dist, -0.001f, 100.0f)) return false;
    float qx = f[0x18 / 4], qy = f[0x1c / 4], qz = f[0x20 / 4], qw = f[0x24 / 4];
    float n = qx * qx + qy * qy + qz * qz + qw * qw;
    return in(n, 0.98f, 1.02f);
}

static size_t scan_region(const uint8_t* base, size_t size, float aspect, uintptr_t* out, size_t out_cap) {
    size_t found = 0;
    __try {
        for (size_t o = 0x30; o + (0x40 - 0x30) <= size; o += 4) {
            float v = *(const float*)(base + o);
            if (fabsf(v - aspect) > 0.01f) continue;
            const uint8_t* b = base + o - 0x30;
            if (((uintptr_t)b & 7) != 0) continue;
            if (check_base(b, aspect) && found < out_cap) out[found++] = (uintptr_t)b;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

static bool safe_read(uintptr_t a, void* dst, size_t n) {
    __try { memcpy(dst, (const void*)a, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool safe_valid(uintptr_t a, float aspect) {
    __try { return check_base((const uint8_t*)a, aspect); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Validation for writes: only fields we never write (aspect, near, far, |q|).
static bool check_frame(const uint8_t* b, float aspect) {
    const float* f = (const float*)b;
    if (fabsf(f[0x30 / 4] - aspect) > 0.01f) return false;
    if (!in(f[0x34 / 4], 0.001f, 5.0f) || !in(f[0x38 / 4], 10.0f, 1.0e7f)) return false;
    float qx = f[6], qy = f[7], qz = f[8], qw = f[9];
    return in(qx * qx + qy * qy + qz * qz + qw * qw, 0.98f, 1.02f);
}
static bool safe_frame_valid(uintptr_t a, float aspect) {
    __try { return check_frame((const uint8_t*)a, aspect); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool safe_write_f(uintptr_t a, float v) {
    __try { *(volatile float*)a = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Thread stacks: MEM_PRIVATE allocation that contains a PAGE_GUARD sub-region.
static bool is_stack_addr(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) return true;
    uintptr_t alloc = (uintptr_t)mbi.AllocationBase;
    for (uintptr_t p = alloc; p < a; ) {
        MEMORY_BASIC_INFORMATION m2;
        if (!VirtualQuery((void*)p, &m2, sizeof(m2))) break;
        if (m2.AllocationBase != (void*)alloc) break;
        if (m2.Protect & PAGE_GUARD) return true;
        p = (uintptr_t)m2.BaseAddress + m2.RegionSize;
    }
    return false;
}

static void do_scan() {
    float aspect = g_backbuffer_aspect.load();
    double t0 = now_s();
    std::vector<uintptr_t> hits;
    uintptr_t tmp[kMaxCands];
    uint64_t bytes = 0, regions = 0, stack_skipped = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull; a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) continue;
        if (mbi.Protect != PAGE_READWRITE) continue;
        regions++; bytes += mbi.RegionSize;
        size_t n = scan_region((const uint8_t*)mbi.BaseAddress, mbi.RegionSize, aspect, tmp, kMaxCands);
        for (size_t i = 0; i < n && hits.size() < kMaxCands; ++i) {
            if (is_stack_addr(tmp[i])) { stack_skipped++; continue; }
            hits.push_back(tmp[i]);
        }
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_fov_step = 0; g_pin_dist = 0;
    g_cands.clear(); g_live.clear(); g_classified = false;
    for (uintptr_t h : hits) {
        Cand c; c.addr = h;
        float f[16]; safe_read(h, f, sizeof(f));
        c.fov0 = f[0x2c / 4]; c.dist0 = f[0x28 / 4]; memcpy(c.lastq, &f[6], 16);
        g_cands.push_back(c);
    }
    vlog("[cam] scan aspect=%.4f: %llu regions, %.1f MB, %.2f s -> %zu candidates (stack-skipped %llu). ROTATE CAMERA for %.0f s",
         aspect, (unsigned long long)regions, bytes / 1048576.0, now_s() - t0, g_cands.size(),
         (unsigned long long)stack_skipped, kWatchSec);
    g_watch_until = now_s() + kWatchSec; g_next_watch = 0.0;
}

static void watch_sample() {
    float aspect = g_backbuffer_aspect.load();
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& c : g_cands) {
        c.samples++;
        if (!safe_valid(c.addr, aspect)) continue;
        c.valid++;
        float q[4]; safe_read(c.addr + 0x18, q, 16);
        if (memcmp(q, c.lastq, 16) != 0) { c.q_changes++; memcpy(c.lastq, q, 16); }
    }
}

static bool step_targets(int step, size_t live_idx) {   // is g_live[live_idx] pinned at this step?
    if (step == 1) return true;
    return step >= 2 && (size_t)(step - 2) == live_idx;
}

static void log_one(const char* tag, size_t i, const Cand& c) {
    uint8_t b[0x40];
    if (!safe_read(c.addr, b, sizeof(b))) { vlog("[cam] %s #%zu %p unreadable", tag, i, (void*)c.addr); return; }
    const int64_t* p = (const int64_t*)b; const float* f = (const float*)b;
    const double S = 1.0 / 4294967296.0;
    vlog("[cam] %s #%zu %p live=%d valid=%d/%d qchg=%d pos_m=(%.3f,%.3f,%.3f) q=(%.4f,%.4f,%.4f,%.4f) dist=%.3f fov=%.4f asp=%.4f rev_fov=%llu rev_dist=%llu skip=%llu",
         tag, i, (void*)c.addr, (int)c.live, c.valid, c.samples, c.q_changes, p[0] * S, p[1] * S, p[2] * S,
         f[6], f[7], f[8], f[9], f[10], f[11], f[12],
         (unsigned long long)c.rev_fov, (unsigned long long)c.rev_dist, (unsigned long long)c.skipped);
}

static void classify() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_live.clear();
    int maxq = 0;
    for (auto& c : g_cands) if (c.valid == c.samples && c.q_changes > maxq) maxq = c.q_changes;
    // Run 4: 87 copies are a 1328-B-stride history ring updated every 2nd sample (qchg~13);
    // per-frame copies have qchg~27. Tier A = valid always and qchg >= 75% of max.
    int thr = maxq * 3 / 4; if (thr < 5) thr = 5;
    size_t tierB = 0, dead = 0;
    for (size_t i = 0; i < g_cands.size(); ++i) {
        Cand& c = g_cands[i];
        c.live = c.samples > 0 && c.valid == c.samples && c.q_changes >= thr;
        if (c.live) { g_live.push_back(i); log_one("tierA", i, c); }
        else if (c.valid == c.samples && c.q_changes >= 5) tierB++;
        else dead++;
    }
    vlog("[cam] maxq=%d thr=%d tierA=%zu tierB(history)=%zu static/dead=%zu", maxq, thr, g_live.size(), tierB, dead);
    g_classified = true;
    vlog("[cam] classification done: %zu live of %zu. Ctrl+F10 steps: 1=all live, 2..%zu=single, then off",
         g_live.size(), g_cands.size(), g_live.size() + 1);
}

static void set_step(int s) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        float aspect = g_backbuffer_aspect.load();
        for (size_t k = 0; k < g_live.size(); ++k) {
            Cand& c = g_cands[g_live[k]];
            float cur = 0; safe_read(c.addr + 0x2c, &cur, 4);
            if (!step_targets(s, k) && fabsf(cur - c.fov0 * 0.6f) < 1e-6f && safe_frame_valid(c.addr, aspect))
                safe_write_f(c.addr + 0x2c, c.fov0);
        }
    }
    g_fov_step = s;
}

// fov/fov0 of every tierA copy + overwrite counter of the pinned one -> propagation map
static void log_ratios(const char* tag, int k) {
    char buf[1024]; int n = 0;
    std::lock_guard<std::mutex> lk(g_mtx);
    for (size_t j = 0; j < g_live.size() && n < 960; ++j) {
        Cand& c = g_cands[g_live[j]];
        float f = 0; safe_read(c.addr + 0x2c, &f, 4);
        if (g_quat_mode) { float q[4] = {}; safe_read(c.addr + 0x18, q, 16);
            n += snprintf(buf + n, sizeof(buf) - n, " %zu:%s", j, memcmp(q, g_qpin, 16) == 0 ? "PIN" : "own"); }
        else n += snprintf(buf + n, sizeof(buf) - n, " %zu:%.2f", j, c.fov0 ? f / c.fov0 : 0.f);
    }
    unsigned long long rev = (k >= 0 && (size_t)k < g_live.size()) ? g_cands[g_live[k]].rev_fov : 0;
    if (k >= 0 && !g_quat_mode) {
        int score = 0;
        for (size_t j = 0; j < g_live.size(); ++j) { Cand& c = g_cands[g_live[j]]; float f = 0; safe_read(c.addr + 0x2c, &f, 4);
            if (c.fov0 && fabsf(f / c.fov0 - 0.6f) < 0.02f) score++; }
        if (score > g_upstream_score) { g_upstream_score = score; g_upstream = k; }
    }
    vlog("[cam] %s pin=%d rev=%llu ratios:%s", tag, k, rev, buf);
}

static bool key_edge(int vk, bool& prev) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fire = down && !prev; prev = down; return fire;
}
static bool game_foreground() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static void beep(int n) { for (int i = 0; i < n; ++i) { Beep(900, 70); Sleep(60); } }


static DWORD WINAPI probe_thread(LPVOID) {
    bool p9 = false, p10 = false, p11 = false, p12 = false;
    int auto_k = -1; int auto_phase = 0; double auto_t = 0; int auto_logs = 0;
    bool pq = false, pe = false, pd = false;
    vlog("[cam] probe v3.1 thread started (Insert scan+watch, Home fov sweep, PageUp quat sweep, End hw-breakpoint trace) - no Ctrl (Ctrl = sneak in game)");
    for (;;) {
        bool fg = game_foreground();
        const bool ctrl = true;   // run 5: Ctrl toggles sneak in Enshrouded (camera pulls in) -> plain keys
        bool e9 = key_edge(VK_INSERT, p9), e12 = key_edge(VK_HOME, p12), eq = key_edge(VK_PRIOR, pq), ee = key_edge(VK_END, pe);
        bool ed = key_edge(VK_NEXT, pd);
        if (fg && ed) {   // PageDown: deterministic angle-hook test (no scan needed). yaw, pitch, roll sine sweeps.
            vlog("[key] PageDown foreground=1");
            if (camhook_install()) {
                const char* nm[3] = {"yaw", "pitch", "roll"};
                const float amp[3] = {0.35f, 0.25f, 0.25f};
                for (int ax = 0; ax < 3; ++ax) {
                    Sleep(1500);
                    for (int i = 0; i <= ax; ++i) { Beep(1000, 60); Sleep(80); }
                    uint64_t h0, o0, h1, o1; camhook_stats(&h0, &o0);
                    double t0 = now_s();
                    while (now_s() - t0 < 6.0) {
                        float v = amp[ax] * sinf((float)((now_s() - t0) * 2.0 * 3.14159265 / 3.0));
                        camhook_set(ax == 0 ? v : 0.f, ax == 1 ? v : 0.f, ax == 2 ? v : 0.f);
                        Sleep(2);
                    }
                    camhook_set(0, 0, 0);
                    camhook_stats(&h1, &o1);
                    vlog("[hook] %s sine +-%.2f rad 6 s: hook ran %llu times (%.0f/s), CamObj=%p", nm[ax], amp[ax],
                         (unsigned long long)(h1 - h0), (h1 - h0) / 6.0, (void*)o1);
                }
                Sleep(500); beep(3); Sleep(300); beep(3);
            } else beep(5);
        }
        bool e10 = false, e11 = false; (void)p10; (void)p11;
        if (e9 || e12 || eq || ee) vlog("[key] %s%s%s%s foreground=%d", e9 ? "Insert " : "", e12 ? "Home " : "", eq ? "PageUp " : "", ee ? "End " : "", (int)fg);
        if (fg && eq) {   // PageUp: quaternion sweep (reuses the auto machinery)
            if (!g_classified || g_live.empty() || auto_k >= 0) { vlog("[cam] PageUp ignored"); beep(3); }
            else { g_quat_mode = 1; set_step(0); auto_k = 0; auto_phase = 0; auto_t = now_s(); vlog("[cam] QUAT sweep start over %zu tierA (yaw +90 pin)", g_live.size()); }
        }
        if (fg && ee) {   // End: CamObj stage (capture via hw-bp, dump, trace readers, causal pins with beep groups)
            if (!g_classified || g_live.empty() || auto_k >= 0) { vlog("[cam] End ignored"); beep(3); }
            else {
                int up = g_upstream >= 0 ? g_upstream : (int)(g_live.size() > 4 ? 4 : 0);
                vlog("[cam] END: CamObj stage, upstream=tierA%d", up);
                camobj_stage(g_cands[g_live[up]].addr, g_backbuffer_aspect.load());
            }
        }
        // Home: automatic FOV sweep over tier A: per copy 1 s baseline, 4 s pin fov*0.6 (1 beep), 2 s rest.
        if (fg && e12) {
            if (!g_classified || g_live.empty()) { vlog("[cam] Home ignored: classify first (Insert)"); beep(3); }
            else if (auto_k >= 0) { set_step(0); auto_k = -1; g_quat_mode = 0; vlog("[cam] auto sweep aborted"); beep(3); }
            else { g_quat_mode = 0; set_step(0); auto_k = 0; auto_phase = 0; auto_t = now_s(); vlog("[cam] auto sweep start over %zu tierA", g_live.size()); }
        }
        if (auto_k >= 0) {
            double el = now_s() - auto_t;
            if (auto_phase == 0 && el >= 1.0) { log_ratios("auto-base", -1);
                if (g_quat_mode) {   // capture current quat, pin yaw+90 about world Y
                    float q[4] = {0, 0, 0, 1}; safe_read(g_cands[g_live[auto_k]].addr + 0x18, q, 16);
                    const float sy = 0.70710678f, wy = 0.70710678f;
                    g_qpin[0] = wy * q[0] + sy * q[2]; g_qpin[1] = wy * q[1] + sy * q[3];
                    g_qpin[2] = wy * q[2] - sy * q[0]; g_qpin[3] = wy * q[3] - sy * q[1];
                }
                set_step(auto_k + 2); beep(1);
                vlog("[cam] auto pin #%d = cand %p", auto_k, (void*)g_cands[g_live[auto_k]].addr); auto_phase = 1; auto_t = now_s(); auto_logs = 0; }
            else if (auto_phase == 1) {
                if (el >= 1.0 * (auto_logs + 1) && auto_logs < 4) { log_ratios("auto-pin", auto_k); auto_logs++; }
                if (el >= 4.0) { set_step(0); auto_phase = 2; auto_t = now_s(); }
            } else if (auto_phase == 2 && el >= 2.0) {
                log_ratios("auto-rest", -1);
                if ((size_t)++auto_k >= g_live.size()) { auto_k = -1; g_quat_mode = 0; vlog("[cam] auto sweep done (upstream=tierA%d score=%d)", g_upstream, g_upstream_score); beep(3); }
                else { auto_phase = 0; auto_t = now_s(); }
            }
        }
        if (fg && ctrl && e9) { do_scan(); beep(1); }
        double t = now_s();
        if (g_watch_until > 0 && t < g_watch_until && t >= g_next_watch) { watch_sample(); g_next_watch = t + kWatchStep; }
        if (g_watch_until > 0 && t >= g_watch_until && !g_classified) { classify(); beep(2); }

        if (fg && ctrl && e10) {
            if (!g_classified || g_live.empty()) { vlog("[cam] Ctrl+F10 ignored: no live candidates (classified=%d)", (int)g_classified); beep(3); }
            else {
                int s = g_fov_step.load() + 1;
                if ((size_t)s > g_live.size() + 1) s = 0;
                log_ratios("F10-leave", g_fov_step.load() - 2);
                set_step(s);
                if (s == 0) vlog("[cam] F10 step 0: fov pin OFF");
                else if (s == 1) vlog("[cam] F10 step 1: fov*0.6 on ALL %zu live", g_live.size());
                else vlog("[cam] F10 step %d: fov*0.6 on live[%d] = cand #%zu %p", s, s - 2, g_live[s - 2], (void*)g_cands[g_live[s - 2]].addr);
                beep(s == 0 ? 3 : 1);
                if (s == 0) { std::lock_guard<std::mutex> lk(g_mtx); for (size_t k : g_live) log_one("after-fov", k, g_cands[k]); }
            }
        }
        if (fg && ctrl && e11) {
            if (!g_classified || g_live.empty()) { vlog("[cam] Ctrl+F11 ignored: no live candidates"); beep(3); }
            else {
                int v = !g_pin_dist.load(); g_pin_dist = v;
                vlog("[cam] pin distance=0 on all live %s", v ? "ON" : "OFF"); beep(v ? 1 : 3);
                if (!v) { std::lock_guard<std::mutex> lk(g_mtx); for (size_t k : g_live) log_one("after-dist", k, g_cands[k]); }
            }
        }

        int step = g_fov_step.load();
        if (step || g_pin_dist) {
            double until = now_s() + 0.002;
            float aspect = g_backbuffer_aspect.load();
            while (now_s() < until) {
                std::lock_guard<std::mutex> lk(g_mtx);
                for (size_t k = 0; k < g_live.size(); ++k) {
                    Cand& c = g_cands[g_live[k]];
                    bool want_fov = step && step_targets(step, k);
                    if (!want_fov && !g_pin_dist) continue;
                    float cur[2]; if (!safe_read(c.addr + 0x28, cur, 8)) { c.skipped++; continue; }
                    if (!safe_frame_valid(c.addr, aspect)) { c.skipped++; continue; }
                    if (g_pin_dist && cur[0] != 0.0f) { c.rev_dist++; safe_write_f(c.addr + 0x28, 0.0f); }
                    if (want_fov && g_quat_mode) {
                        float q[4]; if (safe_read(c.addr + 0x18, q, 16) && memcmp(q, g_qpin, 16) != 0) {
                            c.rev_fov++; for (int i = 0; i < 4; ++i) safe_write_f(c.addr + 0x18 + 4 * i, g_qpin[i]); }
                    } else if (want_fov) { float tv = c.fov0 * 0.6f; if (cur[1] != tv) { c.rev_fov++; safe_write_f(c.addr + 0x2c, tv); } }
                }
                YieldProcessor();
            }
        } else {
            Sleep(5);
        }
    }
    return 0;
}

} // namespace

void cam_probe_start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    HANDLE h = CreateThread(nullptr, 0, probe_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

size_t cam_probe_test_scan(const uint8_t* b, size_t n, float a, uintptr_t* o, size_t cap) { return scan_region(b, n, a, o, cap); }
