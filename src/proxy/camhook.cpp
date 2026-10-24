// EnshroudedVR — camera hooks (S5 angle offset, S6 eye offset) with a per-build table + masked signature scan.
// Angle hook: right after the per-frame camera update copies controller angles into CamObj output fields
// (rewritten every frame -> an added offset never accumulates). Trampoline adds g_off[0..2] to the 3 fields,
// replays the displaced (position-independent) instructions, jumps back.
//
// Build 9514b388 (pre-1.0): fn chunk 0xce9a04, copy [rbx+0x30/34/38] -> ee4/ee8/eec, site 0xce9fd3 (16 B).
// Build a07fae72 (v1.0, exe md5 11804e7a): fn 0xeaa8c0, copy [rbx+0x38/40/48] -> 0x1498/0x149c/0x14a0 at 0xeaad00..0xeaad30,
//   site 0xeaad36 (movq rax,xmm1 / shufps xmm6,xmm7,55 / shufps xmm8,xmm7,aa = 14 B, no rip-rel, 0 jumps in, rax dead).
//   Consumer: 0xe7ebf0 builds rotation from -1498 (yaw), -149c (pitch), -14a0 (roll), pos = pivot i64 0x1468.. +
//   R*offset(0x148c..0x1494) - dist(0x14a4)*fwd. (research/dis_v1_*.txt)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <tlhelp32.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <initializer_list>

void vlog(const char* fmt, ...);
double now_s();

struct CamFields { uint32_t yaw, pitch, roll, pivot, eyepos, basis, offset; const char* build; uint32_t aspect, fov, fov_deg_setting; uint32_t fov_copies[3]; };

namespace {
struct Sig { const char* hex; uint32_t hint_rva; };   // "??" = wildcard
struct EyeHook { Sig sig; uint32_t site_off, len, keep_len; uint8_t xreg; };   // eye site: first keep_len bytes = load xmm<xreg> <- local offset (x in low lane), replayed first
struct Build {
    const char* name;
    Sig sig; uint32_t site_off, site_len;   // angle hook site = sig start + site_off
    uint32_t f[3];                          // yaw/pitch/roll output fields
    CamFields cf;
    bool has_eye; EyeHook eye;
    // S8 view hooks in the render-camera builder: A = call mat->quat (rcx=dst quat, rdx=3x(4f) rows), B = after position stores (rdi=out)
    Sig viewA; uint32_t viewA_off, viewA_len; Sig viewB; uint32_t viewB_off, viewB_len;
};
const Build kBuilds[] = {
    {"9514b388 (pre-1.0)",
     {"89 83 e4 0e 00 00 8b 43 34 f3 0f 59 c8 89 83 e8 0e 00 00 8b 43 38 89 83 ec 0e 00 00 66 48 0f 7e d0 f3 0f 59 cf 66 44 0f 7f 5c 24 50", 0xce9fb7},
     0x1c, 16, {0xee4, 0xee8, 0xeec},
     {0xee4, 0xee8, 0xeec, 0xeb0, 0xec8, 0xf00, 0xed4, "9514b388", 0x1b18, 0x1b24, 0, {0, 0, 0}},
     true, {{"0f 59 f0 f3 0f 10 83 d8 0e 00 00 44 0f 28 65 b0 f3 0f 10 8b d4 0e 00 00 0f 28 5d c0 66 0f 7f 5c 24 50", 0xcea3ec}, 0x10, 18, 8, 1},
     {nullptr, 0}, 0, 0, {nullptr, 0}, 0, 0},
    {"a07fae72 (v1.0)",
     {"8b 43 38 f3 0f 10 2d ?? ?? ?? ?? f3 0f 10 43 68 0f 28 d7 0f 10 4b 50 89 83 98 14 00 00 8b 43 40 0f 28 f7 89 83 9c 14 00 00 "
      "44 0f 28 c7 8b 43 48 89 83 a0 14 00 00 66 48 0f 7e c8 0f c6 f7 55 44 0f c6 c7 aa", 0xeaad00},
     0x36, 14, {0x1498, 0x149c, 0x14a0},
     {0x1498, 0x149c, 0x14a0, 0x1468, 0x1480, 0x14b0, 0x148c, "a07fae72",
      // run 20 dump: aspect 1.7778 @0x20b8, FOV 1.39626 rad (= slider 80) @0x20c4 (old 1b18/1b24 + 0x5a0); 80.0 @0x18; copies @0x4c/0x20c/0x3f4
      0x20b8, 0x20c4, 0x18, {0x4c, 0x20c, 0x3f4}},
     // eye: render-camera builder 0xe7ebf0 (run 18 HW-BP: reads yaw+pivot ~2x/frame, in sync), local offset x,y = movsd xmm4,[rbx+0x148c]
     true, {{"48 8d 55 87 48 8d 4d d7 e8 ?? ?? ?? ?? f2 0f 10 a3 8c 14 00 00 f3 0f 10 9b 94 14 00 00", 0xe7ed50}, 0x0d, 16, 8, 4},
     // view A @0xe7edfc: call mat2quat (5) + movss xmm4,[rbx+0x14a4] (8) + lea r11,[rsp+0xf0] (8); view B @0xe7eecd: 18 B epilogue loads
     {"89 47 30 f3 44 0f 58 c1 e8 ?? ?? ?? ?? f3 0f 10 a3 a4 14 00 00 4c 8d 9c 24 f0 00 00 00", 0xe7edf4}, 0x08, 21,
     {"48 8d 04 41 48 89 47 10 49 8b 5b 10 49 8b 7b 18 41 0f 28 73 f0 41 0f 28 7b e0", 0xe7eec5}, 0x08, 18},
};

int parse_sig(const char* hex, uint8_t* b, bool* m) {
    int n = 0;
    for (const char* p = hex; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { b[n] = 0; m[n++] = false; p += 2; continue; }
        unsigned v; sscanf_s(p, "%2x", &v); b[n] = (uint8_t)v; m[n++] = true; p += 2;
    }
    return n;
}
bool match_at(const uint8_t* a, const uint8_t* b, const bool* m, int n) {
    for (int i = 0; i < n; ++i) if (m[i] && a[i] != b[i]) return false;
    return true;
}
// hint first, then scan the whole .text (tolerates small hotfix shifts). Returns rva or 0; *dupes = >1 match.
uint32_t find_sig(uint8_t* exe, const Sig& s, int* len_out, bool* dupes) {
    uint8_t b[256]; bool m[256]; int n = parse_sig(s.hex, b, m); *len_out = n; *dupes = false;
    auto nt = (IMAGE_NT_HEADERS*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint32_t tlo = sec->VirtualAddress, thi = sec->VirtualAddress + sec->Misc.VirtualSize;
    if (s.hint_rva >= tlo && s.hint_rva + n <= thi && match_at(exe + s.hint_rva, b, m, n)) return s.hint_rva;
    uint32_t found = 0;
    for (uint32_t r = tlo; r + n <= thi; ++r) {
        if (exe[r] != b[0] || !match_at(exe + r, b, m, n)) continue;
        if (found) { *dupes = true; return 0; }
        found = r;
    }
    return found;
}

struct alignas(16) HookData {
    float off[4];        // yaw, pitch, roll offsets (rad), [3] = eye right offset (game units, m)
    float save[4];       // xmm0 spill (16 B)
    uint64_t hits;       // incremented by trampoline (frames hooked)
    uint64_t camobj;     // last rbx seen
    uint64_t eyehits;    // eye trampoline executions
    uint32_t wr_off[2];  // @0x38: extra per-frame float writes [rbx+wr_off[k]] = wr_val[k] (0 = off) - S7 FOV/aspect override
    float wr_val[2];     // @0x40
};
static_assert(offsetof(HookData, wr_off) == 0x38 && offsetof(HookData, wr_val) == 0x40, "HookData layout");
HookData* g_data = nullptr;
uint8_t* g_tramp = nullptr;
uint8_t g_orig[32]; uint32_t g_site_rva = 0, g_site_len = 0;
bool g_installed = false;
uint8_t* g_tramp2 = nullptr; uint8_t g_orig2[32]; bool g_eye = false;
const Build* g_build = nullptr;

struct Emit {
    uint8_t* p;
    void b(std::initializer_list<uint8_t> l) { for (uint8_t x : l) *p++ = x; }
    void q(uint64_t v) { memcpy(p, &v, 8); p += 8; }
    void d(uint32_t v) { memcpy(p, &v, 4); p += 4; }
};

void build_trampoline(uint8_t* t, HookData* hd, const uint32_t* offs, const uint8_t* displaced, uint32_t dlen, uint64_t back) {
    Emit e{t};
    e.b({0x50});                                   // push rax
    e.b({0x48, 0xb8}); e.q((uint64_t)hd);           // mov rax, hd
    e.b({0x0f, 0x11, 0x40, 0x10});                 // movups [rax+0x10], xmm0
    for (int i = 0; i < 3; ++i) {
        e.b({0xf3, 0x0f, 0x10, 0x83}); e.d(offs[i]);        // movss xmm0, [rbx+off]
        e.b({0xf3, 0x0f, 0x58, 0x40, (uint8_t)(i * 4)});    // addss xmm0, [rax+i*4]
        e.b({0xf3, 0x0f, 0x11, 0x83}); e.d(offs[i]);        // movss [rbx+off], xmm0
    }
    e.b({0x51, 0x9c});                             // push rcx; pushfq
    for (int k = 0; k < 2; ++k) {
        e.b({0x8b, 0x48, (uint8_t)(0x38 + 4 * k)});        // mov ecx, [rax+0x38+4k]
        e.b({0x85, 0xc9, 0x74, 0x0a});                     // test ecx,ecx; jz +10
        e.b({0xf3, 0x0f, 0x10, 0x40, (uint8_t)(0x40 + 4 * k)});   // movss xmm0, [rax+0x40+4k]
        e.b({0xf3, 0x0f, 0x11, 0x04, 0x0b});               // movss [rbx+rcx], xmm0
    }
    e.b({0x9d, 0x59});                             // popfq; pop rcx
    e.b({0x0f, 0x10, 0x40, 0x10});                 // movups xmm0, [rax+0x10]
    e.b({0xf0, 0x48, 0xff, 0x40, 0x20});           // lock inc qword [rax+0x20]   (hits)
    e.b({0x48, 0x89, 0x58, 0x28});                 // mov [rax+0x28], rbx          (camobj)
    e.b({0x58});                                   // pop rax
    memcpy(e.p, displaced, dlen); e.p += dlen;     // replay displaced bytes (PIC)
    e.b({0xff, 0x25, 0, 0, 0, 0}); e.q(back);      // jmp [rip+0] -> back
}

void build_eye_trampoline(uint8_t* t, HookData* hd, const uint8_t* displaced, uint32_t keep, uint32_t len, uint8_t xreg, uint64_t back) {
    Emit e{t};
    memcpy(e.p, displaced, keep); e.p += keep;     // movss xmm1, [rbx+offset_x]
    e.b({0x50});                                   // push rax
    e.b({0x48, 0xb8}); e.q((uint64_t)hd);           // mov rax, hd
    e.b({0xf3, 0x0f, 0x58, (uint8_t)(0x40 | (xreg << 3)), 0x0c});   // addss xmm<xreg>, [rax+0x0c]  (low lane = local x only)
    e.b({0xf0, 0x48, 0xff, 0x40, 0x30});           // lock inc qword [rax+0x30]  (eyehits)
    e.b({0x58});                                   // pop rax
    memcpy(e.p, displaced + keep, len - keep); e.p += len - keep;
    e.b({0xff, 0x25, 0, 0, 0, 0}); e.q(back);
}

// suspend all other threads, make sure none is inside [a, a+n), run fn, resume
template <class F> bool with_threads_suspended(uintptr_t a, size_t n, F fn) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        HANDLE hs[1024]; int nh = 0; bool inside = false;
        THREADENTRY32 te{}; te.dwSize = sizeof(te);
        for (BOOL m = Thread32First(snap, &te); m && nh < 1024; m = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
            hs[nh++] = th;
            CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &c) && c.Rip >= a && c.Rip < a + n) inside = true;
        }
        CloseHandle(snap);
        if (!inside) fn();
        for (int i = 0; i < nh; ++i) { ResumeThread(hs[i]); CloseHandle(hs[i]); }
        if (!inside) return true;
        Sleep(2);
    }
    return false;
}
// ---- S8 view-space head rotation (no orbit): Mf = H * Mbase (rows = camera axes in world), quat from Mf; pos += Mf.r*eye + Mb*headpos
struct ViewState { LONG on; float H[3][3]; float eye; float hp[3]; uint64_t seq; float pull; };
ViewState g_view = {0, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, 0.f, {0, 0, 0}, 0, 0.f};
volatile LONG64 g_used_seq = 0;
SRWLOCK g_view_lock = SRWLOCK_INIT;
typedef void(__fastcall* Mat2Quat)(float* dst, const float* m);
Mat2Quat g_mat2quat = nullptr;
thread_local float t_mf[3][4], t_mb[3][4]; thread_local bool t_valid = false;
volatile LONG64 g_view_a = 0, g_view_b = 0;
void* g_dbg[2];
void __fastcall view_mat2quat(float* dst, const float* m) {
    InterlockedIncrement64(&g_view_a); g_dbg[0] = dst; g_dbg[1] = (void*)m;
    ViewState v; AcquireSRWLockShared(&g_view_lock); v = g_view; ReleaseSRWLockShared(&g_view_lock);
    for (int r = 0; r < 3; ++r) for (int k = 0; k < 4; ++k) t_mb[r][k] = m[r * 4 + k];
    if (!v.on) { t_valid = false; g_mat2quat(dst, m); return; }
    InterlockedExchange64(&g_used_seq, (LONG64)v.seq);   // S7a: which head pose (XR frame seq) this render camera was built with
    alignas(16) float mf[3][4];
    for (int r = 0; r < 3; ++r) { for (int k = 0; k < 3; ++k) mf[r][k] = v.H[r][0] * m[0 * 4 + k] + v.H[r][1] * m[1 * 4 + k] + v.H[r][2] * m[2 * 4 + k]; mf[r][3] = m[r * 4 + 3]; }
    memcpy(t_mf, mf, sizeof(mf)); t_valid = true;
    g_mat2quat(dst, &mf[0][0]);
}
struct PosRec { double t; uint64_t seq; int64_t p[3]; float eye; uint32_t tid; };
PosRec g_posring[32]; volatile LONG g_posn = 0;
void __fastcall view_post(int64_t* out) {
    InterlockedIncrement64(&g_view_b);
    if (!t_valid) return;
    t_valid = false;
    ViewState v; AcquireSRWLockShared(&g_view_lock); v = g_view; ReleaseSRWLockShared(&g_view_lock);
    if (!v.on) return;
    for (int k = 0; k < 3; ++k) {
        // pull > 0: camera slides along the game's forward axis (row 2) toward the orbit pivot = shorter 3rd-person boom
        float d = t_mf[0][k] * v.eye + t_mb[0][k] * v.hp[0] + t_mb[1][k] * v.hp[1] + t_mb[2][k] * (v.hp[2] + v.pull);
        out[k] += (int64_t)(d * 4294967296.0);
    }
    { LONG i = InterlockedIncrement(&g_posn) & 31; PosRec& r = g_posring[i]; r.t = now_s(); r.seq = v.seq; r.p[0] = out[0]; r.p[1] = out[1]; r.p[2] = out[2]; r.eye = v.eye; r.tid = GetCurrentThreadId(); }
}
uint8_t* g_trampA = nullptr; uint8_t* g_trampB = nullptr; bool g_viewok = false;
// S8b render-FOV hook (v1.0): 0x3a8a69 copies CamObj+0x20c4 -> frame camera record [r14+0x4c] (consumed by the renderer).
// 0x20c4 itself is rewritten every frame at 0x855021 from CamObj-0x18, so the record is the only reliable place.
const Sig kFovSig = {"8b 88 a4 14 00 00 41 89 4e 48 48 8b 85 b8 05 00 00 8b 88 c4 20 00 00 41 89 4e 4c 48 8b 85 b8 05 00 00", 0x3a8a58};
const uint32_t kFovSiteOff = 0x11, kFovLen = 17, kFovKeep = 10;   // site = mov ecx,[rax+20c4] (6) + mov [r14+4c],ecx (4) + mov rax,[rbp+5b8] (7)
volatile LONG g_fov_bits = 0; volatile LONG64 g_fov_hits = 0; volatile uint64_t g_fov_rec = 0;
// S8b-2 GPU projection (run 28 HW-BP): 0x8558d0 per render frame loads FOV (CamObj-0x18 = big+0x4672e8), near/far, aspect=w/h
// -> call 0xf4e5e0 (perspective: tanf(fov/2), m00=1/tan/aspect, m11=1/tan). Site: movss xmm1,[r14+0x4672e8] (9) + xorps xmm2,xmm2 (3) + mov eax,ebx (2)
const Sig kProjSig = {"f3 41 0f 10 9e e4 72 46 00 48 8d 4e 40 f3 41 0f 10 8e e8 72 46 00 0f 57 d2 8b c3", 0x8558c3};
const uint32_t kProjSiteOff = 0x0d, kProjLen = 14, kProjKeep = 9;
volatile LONG64 g_proj_hits = 0; volatile uint64_t g_proj_dst = 0; bool g_projok = false;
// S8b-4 FOV SOURCE (run 28 WRITE): 0x86f8dd movss [r13+0x4672e8],xmm6 (= CamObj-0x18) every frame; all consumers (GPU proj 0x8558d0,
// 0x854ad6 -> 0x3a40e0, 0x1181a3, 0x855021 -> 0x20c4 -> sim record) read it -> override here keeps them consistent (DLAA/motion vectors).
const Sig kSrcSig = {"f2 0f 10 4d 40 41 0f 11 85 c8 72 46 00 f2 41 0f 11 8d d8 72 46 00 f3 41 0f 11 b5 e8 72 46 00 eb 07", 0x86f8c7};
const uint32_t kSrcSiteOff = 0x0d, kSrcLen = 20, kSrcKeep = 18, kSrcBack = 0x86f8ef - 0x86f8d4;   // site ends with jmp +7 -> replayed as abs jmp
volatile LONG64 g_src_hits = 0; bool g_srcok = false; uint8_t* g_trampS = nullptr; uint8_t* g_trampP = nullptr; bool g_fovok = false; uint8_t* g_trampF = nullptr;

bool write_jmp(uint8_t* site, uint32_t len, uint8_t* target) {
    uint8_t patch[32];
    memset(patch, 0x90, len); patch[0] = 0xff; patch[1] = 0x25; memset(patch + 2, 0, 4);
    uint64_t t = (uint64_t)target; memcpy(patch + 6, &t, 8);
    DWORD old;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = false;
    ok = with_threads_suspended((uintptr_t)site, len, [&] { memcpy(site, patch, len); });
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return ok;
}
// ---- S12 (v1.1.2): DLSS evaluate wrapper (NGX_VULKAN_EVALUATE_DLSS_EXT, found by momotori01's observer; 1 caller 0x10048cf)
// args: rcx VkCommandBuffer, rdx NVSDK_NGX_Handle* (ctx+0xe8), r8 NVSDK_NGX_Parameter* (ctx+0xe0), r9 eval params
// (color +0, output +8, depth +0x18, MV +0x20, jitter +0x28/+0x2c, subrect +0x30, reset +0x38, MV scale +0x3c/+0x40).
// Entry patch: 3 x 5-byte mov [rsp+x],reg (no rip-rel) -> 15 bytes displaced.
const Sig kDlssSig = {"48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 56 48 83 ec 40 49 8b f8 49 8b d9 4d 8b 01 48 8b ea 4c 8b f1", 0xfa97c0};
const uint32_t kDlssLen = 15;
typedef int(__fastcall* DlssEval)(void*, void*, void*, void*);
typedef int(__cdecl* NgxCreate1)(void* dev, void* cmd, int feature, void* params, void** out);
DlssEval g_dlss_orig = nullptr; uint8_t* g_trampD = nullptr; bool g_dlssok = false;
volatile LONG g_dlss_mode = 0;              // 0 game, 1 reset every eval, 2 per-eye + MV x0 (static test), 3 per-eye + corrected MV (S12 fix)
typedef void* (*DlssPre)(void* cmd, void* evalp, const float* cam32, int apply); DlssPre g_dlss_pre = nullptr; int (*g_dlss_ready)() = nullptr; volatile LONG64 g_mv_applied = 0, g_mv_zero = 0;
bool read_cam32(uint64_t pd, float* out) { __try { if (!pd) return false; memcpy(out, (void*)(pd - 0x40), 32 * 4); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
volatile LONG64 g_dlss_calls = 0, g_dlss_resets = 0, g_dlss_swaps = 0, g_dlss_fail = 0;
void* g_dlss_dev = nullptr; void* g_dlss_game_h = nullptr; void* g_dlss_h2 = nullptr; NgxCreate1 g_ngx_create1 = nullptr;
volatile LONG g_dlss_parity = 0, g_dlss_last_slot = -1; LONG g_dlss_prev_mode = 0; bool g_dlss_fresh2 = false;
float g_dlss_last[8] = {};
volatile LONG g_s12_left = 0; int g_s12_k = 0; std::vector<uint8_t> g_s12buf;
void s12_res(std::vector<uint8_t>& b, uint8_t* r) {   // NVSDK_NGX_Resource_VK: view +0 image +8 ... format +0x24 w +0x28 h +0x2c type +0x30
    uint8_t z[0x38] = {}; __try { if (r) memcpy(z, r, 0x38); } __except (EXCEPTION_EXECUTE_HANDLER) {} b.insert(b.end(), z, z + 0x38); }
void s12_capture(void* evalp, LONG slot) {
    // record: magic, t, slot, tid, eval[0x80], color/out/depth/mv resources, camera struct [proj_dst-0x40-0x400, +0x800), posring
    std::vector<uint8_t>& b = g_s12buf; auto put = [&](const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    uint32_t mg = 0x31325331; put(&mg, 4); double t = now_s(); put(&t, 8); put(&slot, 4); DWORD tid = GetCurrentThreadId(); put(&tid, 4);
    uint64_t pd = g_proj_dst; put(&pd, 8);
    uint8_t ev[0x80] = {}; __try { memcpy(ev, evalp, 0x80); } __except (EXCEPTION_EXECUTE_HANDLER) {} put(ev, 0x80);
    uint8_t* e = (uint8_t*)evalp; uint8_t* rr[4] = {}; __try { rr[0] = *(uint8_t**)e; rr[1] = *(uint8_t**)(e + 8); rr[2] = *(uint8_t**)(e + 0x18); rr[3] = *(uint8_t**)(e + 0x20); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    for (auto r : rr) s12_res(b, r);
    uint8_t cs[0x800] = {}; __try { if (pd) memcpy(cs, (void*)(pd - 0x40 - 0x400), 0x800); } __except (EXCEPTION_EXECUTE_HANDLER) {} put(cs, 0x800);
    put(g_posring, sizeof(g_posring)); LONG pn = g_posn; put(&pn, 4);
}                  // jitter x,y, mv scale x,y, reset, out w,h, slot
NgxCreate1 find_create1() {
    HMODULE mods[512]; DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return nullptr;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; ++i) {
        if (mods[i] == GetModuleHandleA(nullptr)) continue;
        if (auto f = (NgxCreate1)GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_CreateFeature1")) {
            char n[MAX_PATH] = {}; K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], n, MAX_PATH); vlog("[dlss] CreateFeature1 from %s", n); return f; }
    }
    return nullptr;
}
int __fastcall dlss_eval_hook(void* cmd, void* handle, void* params, void* evalp) {
    InterlockedIncrement64(&g_dlss_calls);
    uint8_t* e = (uint8_t*)evalp; const LONG mode = g_dlss_mode;
    __try {
        uint8_t* out = *(uint8_t**)(e + 8);
        g_dlss_last[0] = *(float*)(e + 0x28); g_dlss_last[1] = *(float*)(e + 0x2c); g_dlss_last[2] = *(float*)(e + 0x3c); g_dlss_last[3] = *(float*)(e + 0x40);
        g_dlss_last[4] = (float)*(int32_t*)(e + 0x38); if (out) { g_dlss_last[5] = (float)*(uint32_t*)(out + 0x28); g_dlss_last[6] = (float)*(uint32_t*)(out + 0x2c); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement64(&g_dlss_fail); return g_dlss_orig(cmd, handle, params, evalp); }
    const LONG slot = InterlockedIncrement(&g_dlss_parity) & 1;   // AER: evals alternate L,R (alignment with the presented eye is logged by xr.cpp)
    InterlockedExchange(&g_dlss_last_slot, slot); g_dlss_last[7] = (float)slot;
    if (g_s12_left > 0) { s12_capture(evalp, slot); if (InterlockedDecrement(&g_s12_left) == 0) {
        char fn[MAX_PATH]; GetModuleFileNameA(nullptr, fn, MAX_PATH); char* sl = strrchr(fn, '\\'); if (sl) sl[1] = 0;
        char path[MAX_PATH]; snprintf(path, sizeof(path), "%senshrouded_vr\\s12dump_%d.bin", fn, ++g_s12_k);
        FILE* f = nullptr; if (!fopen_s(&f, path, "wb") && f) { fwrite(g_s12buf.data(), 1, g_s12buf.size(), f); fclose(f); }
        vlog("[s12] camera dump %s (%zu bytes)", path, g_s12buf.size()); g_s12buf.clear(); Beep(1200, 80); Sleep(60); Beep(1200, 80); Beep(1200, 80); } }
    void* mvres = nullptr;
    if (g_dlss_pre) { float cam32[32]; if (read_cam32(g_proj_dst, cam32)) mvres = g_dlss_pre(cmd, evalp, cam32, mode == 3 ? 1 : 0); }
    // release mode 3: until the MV fix has calibrated (or if it never does - other AA/upscaler path) DLSS runs exactly as the game set it up
    const LONG eff = (mode == 3 && !(g_dlss_ready && g_dlss_ready())) ? 0 : mode;
    if (eff != g_dlss_prev_mode) { g_dlss_prev_mode = eff; g_dlss_fresh2 = true; }
    if (mode == 1) { *(int32_t*)(e + 0x38) = 1; InterlockedIncrement64(&g_dlss_resets); }
    if (eff == 2 || eff == 3) {
        if (handle != g_dlss_game_h) { g_dlss_game_h = handle; g_dlss_h2 = nullptr; }   // game (re)created its feature -> ours follows
        if (!g_dlss_h2 && g_dlss_dev) {
            if (!g_ngx_create1) g_ngx_create1 = find_create1();
            void* h2 = nullptr; int r = g_ngx_create1 ? g_ngx_create1(g_dlss_dev, cmd, 1 /*SuperSampling*/, params, &h2) : -1;
            vlog("[dlss] second feature (right-eye history): result 0x%x handle %p", r, h2);
            if (r == 1 && h2) { g_dlss_h2 = h2; g_dlss_fresh2 = true; } else { InterlockedExchange(&g_dlss_mode, 0); InterlockedIncrement64(&g_dlss_fail); }
        }
        if (g_dlss_h2) {
            if (eff == 3 && mvres) InterlockedIncrement64(&g_mv_applied);
            else { *(float*)(e + 0x3c) = 0.f; *(float*)(e + 0x40) = 0.f; InterlockedIncrement64(&g_mv_zero); }   // static test / not calibrated yet
            if (g_dlss_fresh2) { *(int32_t*)(e + 0x38) = 1; if (slot) g_dlss_fresh2 = false; }
            if (slot) { handle = g_dlss_h2; InterlockedIncrement64(&g_dlss_swaps); }
        }
    }
    if (eff == 3 && mvres && g_dlss_h2) {   // corrected MVs (same eye, two frames ago) for this call only
        void* orig_mv = *(void**)(e + 0x20); *(void**)(e + 0x20) = mvres;
        int r = g_dlss_orig(cmd, handle, params, evalp);
        *(void**)(e + 0x20) = orig_mv; return r;
    }
    return g_dlss_orig(cmd, handle, params, evalp);
}
} // namespace
void dlss_set_pre(void* (*f)(void*, void*, const float*, int), int (*ready)()) { g_dlss_pre = f; g_dlss_ready = ready; }
void dlss_mv_stats(uint64_t* applied, uint64_t* zeroed) { *applied = (uint64_t)g_mv_applied; *zeroed = (uint64_t)g_mv_zero; }

void dlss_set_device(void* dev) { g_dlss_dev = dev; }
bool s12_dump_start(int n) { if (!g_dlssok || g_s12_left > 0) return false; g_s12buf.reserve((size_t)n * 0xc00); InterlockedExchange(&g_s12_left, n); return true; }
bool dlss_ok() { return g_dlssok; }
void dlss_set_mode(int m) { InterlockedExchange(&g_dlss_mode, m); }
int dlss_mode() { return g_dlss_mode; }
int dlss_last_slot() { return g_dlss_last_slot; }
void dlss_stats(uint64_t* calls, uint64_t* resets, uint64_t* swaps, uint64_t* fails, float* last8) {
    *calls = (uint64_t)g_dlss_calls; *resets = (uint64_t)g_dlss_resets; *swaps = (uint64_t)g_dlss_swaps; *fails = (uint64_t)g_dlss_fail; memcpy(last8, g_dlss_last, sizeof(g_dlss_last)); }
bool dlss_install_at(uint8_t* exe) {
    int ld = 0; bool dd = false; uint32_t rd = find_sig(exe, kDlssSig, &ld, &dd);
    if (!rd || dd) { vlog("[hook] dlss-eval hook: signature %s -> S12 DLSS modes unavailable", dd ? "AMBIGUOUS" : "not found"); return false; }
    uint8_t* site = exe + rd;
    g_trampD = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampD) return false;
    Emit em{g_trampD}; memcpy(em.p, site, kDlssLen); em.p += kDlssLen;
    em.b({0xff, 0x25, 0, 0, 0, 0}); em.q((uint64_t)(site + kDlssLen));
    g_dlss_orig = (DlssEval)g_trampD;
    if (!write_jmp(site, kDlssLen, (uint8_t*)&dlss_eval_hook)) { vlog("[hook] dlss-eval hook: patch failed"); return false; }
    g_dlssok = true; vlog("[hook] dlss-eval hook INSTALLED: site exe+0x%x", rd);
    return true;
}

bool camhook_install_at(uint8_t* exe) {
    if (g_installed) return true;
    const Build* bd = nullptr; uint32_t sig_rva = 0; int siglen = 0;
    for (const Build& b : kBuilds) {
        bool dup = false;
        uint32_t r = find_sig(exe, b.sig, &siglen, &dup);
        if (dup) vlog("[hook] build %s: signature matches more than once -> skipped", b.name);
        if (r) { bd = &b; sig_rva = r; break; }
    }
    if (!bd) {
        vlog("[hook] signature MISMATCH: no known build (%d tried) -> game updated? hook NOT installed", (int)(sizeof(kBuilds) / sizeof(kBuilds[0])));
        return false;
    }
    g_build = bd;
    g_site_rva = sig_rva + bd->site_off; g_site_len = bd->site_len;
    vlog("[hook] build %s: signature at exe+0x%x%s, site exe+0x%x (%u B), fields yaw/pitch/roll +0x%x/+0x%x/+0x%x", bd->name, sig_rva,
         sig_rva == bd->sig.hint_rva ? "" : " (SHIFTED from hint)", g_site_rva, g_site_len, bd->f[0], bd->f[1], bd->f[2]);
    uint8_t* site = exe + g_site_rva;
    memcpy(g_orig, site, g_site_len);
    g_data = (HookData*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_tramp = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_data || !g_tramp) { vlog("[hook] VirtualAlloc failed"); return false; }
    memset(g_data, 0, sizeof(HookData));
    build_trampoline(g_tramp, g_data, bd->f, g_orig, g_site_len, (uint64_t)(site + g_site_len));
    FlushInstructionCache(GetCurrentProcess(), g_tramp, 4096);
    bool ok = write_jmp(site, g_site_len, g_tramp);
    g_installed = ok;
    vlog("[hook] camera-angle hook %s: site exe+0x%x -> tramp %p, data %p", ok ? "INSTALLED" : "FAILED (thread inside site)",
         g_site_rva, (void*)g_tramp, (void*)g_data);
    if (!ok) return false;
    // ---- S6 eye-offset hook (optional: stereo disabled if missing)
    if (!bd->has_eye) { vlog("[hook] eye-offset hook: not mapped for build %s -> stereo unavailable", bd->name); return true; }
    bool dup = false; int l2 = 0;
    uint32_t r2 = find_sig(exe, bd->eye.sig, &l2, &dup);
    if (!r2) { vlog("[hook] eye signature MISMATCH%s -> stereo unavailable", dup ? " (duplicate)" : ""); return true; }
    uint8_t* site2 = exe + r2 + bd->eye.site_off;
    memcpy(g_orig2, site2, bd->eye.len);
    g_tramp2 = g_tramp + 256;
    build_eye_trampoline(g_tramp2, g_data, g_orig2, bd->eye.keep_len, bd->eye.len, bd->eye.xreg, (uint64_t)(site2 + bd->eye.len));
    FlushInstructionCache(GetCurrentProcess(), g_tramp2, 256);
    g_eye = write_jmp(site2, bd->eye.len, g_tramp2);
    vlog("[hook] eye-offset hook %s: site exe+0x%x -> tramp %p", g_eye ? "INSTALLED" : "FAILED", (uint32_t)(site2 - exe), (void*)g_tramp2);
    // ---- S8 view hooks (A + B both or none)
    if (bd->viewA.hex && bd->viewB.hex) {
        int la = 0, lb = 0; bool da = false, db = false;
        uint32_t ra = find_sig(exe, bd->viewA, &la, &da), rb = find_sig(exe, bd->viewB, &lb, &db);
        if (!ra || !rb) { vlog("[hook] view signatures MISMATCH (A %x B %x) -> head = orbit mode only", ra, rb); return true; }
        uint8_t* sa = exe + ra + bd->viewA_off; uint8_t* sb = exe + rb + bd->viewB_off;
        if (sa[0] != 0xe8) { vlog("[hook] view A: no call at site"); return true; }
        int32_t rel; memcpy(&rel, sa + 1, 4); g_mat2quat = (Mat2Quat)(sa + 5 + rel);
        g_trampA = g_tramp + 512; g_trampB = g_tramp + 768;
        { Emit e{g_trampA};
          e.b({0x48, 0xb8}); e.q((uint64_t)&view_mat2quat); e.b({0xff, 0xd0});    // mov rax, fn; call rax  (rcx/rdx = original args)
          memcpy(e.p, sa + 5, bd->viewA_len - 5); e.p += bd->viewA_len - 5;        // replay movss xmm4,[rbx+..]; lea r11,[rsp+..]
          e.b({0xff, 0x25, 0, 0, 0, 0}); e.q((uint64_t)(sa + bd->viewA_len)); }
        { Emit e{g_trampB};
          e.b({0x50, 0x41, 0x53, 0x48, 0x83, 0xec, 0x20});                         // push rax; push r11; sub rsp,0x20 (16-aligned)
          e.b({0x48, 0x89, 0xf9});                                                 // mov rcx, rdi
          e.b({0x48, 0xb8}); e.q((uint64_t)&view_post); e.b({0xff, 0xd0});
          e.b({0x48, 0x83, 0xc4, 0x20, 0x41, 0x5b, 0x58});                         // add rsp,0x20; pop r11; pop rax
          memcpy(e.p, sb, bd->viewB_len); e.p += bd->viewB_len;
          e.b({0xff, 0x25, 0, 0, 0, 0}); e.q((uint64_t)(sb + bd->viewB_len)); }
        FlushInstructionCache(GetCurrentProcess(), g_tramp, 4096);
        bool okb = write_jmp(sb, bd->viewB_len, g_trampB);
        bool oka = okb && write_jmp(sa, bd->viewA_len, g_trampA);
        g_viewok = oka && okb;
        vlog("[hook] view hooks %s: A exe+0x%x (mat2quat exe+0x%x) B exe+0x%x", g_viewok ? "INSTALLED" : "FAILED", (uint32_t)(sa - exe),
             (uint32_t)((uint8_t*)g_mat2quat - exe), (uint32_t)(sb - exe));
    }
    if (strcmp(bd->cf.build, "a07fae72") == 0) {   // ---- S8b render FOV
        int lf = 0; bool df = false; uint32_t rf = find_sig(exe, kFovSig, &lf, &df);
        if (!rf) { vlog("[hook] render-fov signature MISMATCH%s -> FOV override unavailable", df ? " (duplicate)" : ""); return true; }
        uint8_t* sf = exe + rf + kFovSiteOff;
        g_trampF = g_tramp + 1024;
        { Emit e{g_trampF};
          memcpy(e.p, sf, kFovKeep); e.p += kFovKeep;                               // original: ecx = fov; [r14+0x4c] = ecx
          e.b({0x9c, 0x50}); e.b({0x48, 0xb8}); e.q((uint64_t)&g_fov_bits);       // pushfq; push rax; mov rax,&bits
          e.b({0x8b, 0x00, 0x85, 0xc0, 0x74, 0x04, 0x41, 0x89, 0x46, 0x4c});      // mov eax,[rax]; test; jz +4; mov [r14+0x4c],eax
          e.b({0x48, 0xb8}); e.q((uint64_t)&g_fov_hits); e.b({0xf0, 0x48, 0xff, 0x00});   // lock inc qword [rax]
          e.b({0x48, 0xb8}); e.q((uint64_t)&g_fov_rec); e.b({0x4c, 0x89, 0x30});           // mov [rax], r14 (frame camera record, for HW-BP)
          e.b({0x58, 0x9d});                                                       // pop rax; popfq
          memcpy(e.p, sf + kFovKeep, kFovLen - kFovKeep); e.p += kFovLen - kFovKeep; // mov rax,[rbp+0x5b8]
          e.b({0xff, 0x25, 0, 0, 0, 0}); e.q((uint64_t)(sf + kFovLen)); }
        FlushInstructionCache(GetCurrentProcess(), g_tramp, 4096);
        g_fovok = write_jmp(sf, kFovLen, g_trampF);
        vlog("[hook] render-fov hook %s: site exe+0x%x -> [r14+0x4c]", g_fovok ? "INSTALLED" : "FAILED", (uint32_t)(sf - exe));
        int lp = 0; bool dp = false; uint32_t rp = find_sig(exe, kProjSig, &lp, &dp);
        if (!rp) { vlog("[hook] projection signature MISMATCH%s", dp ? " (duplicate)" : ""); return true; }
        uint8_t* sp = exe + rp + kProjSiteOff;
        g_trampP = g_tramp + 1280;
        { Emit e{g_trampP};
          memcpy(e.p, sp, kProjKeep); e.p += kProjKeep;                             // movss xmm1, fov
          e.b({0x9c, 0x50}); e.b({0x48, 0xb8}); e.q((uint64_t)&g_fov_bits);
          e.b({0x8b, 0x00, 0x85, 0xc0, 0x74, 0x04, 0x66, 0x0f, 0x6e, 0xc8});      // mov eax,[rax]; test; jz +4; movd xmm1,eax
          e.b({0x48, 0xb8}); e.q((uint64_t)&g_proj_hits); e.b({0xf0, 0x48, 0xff, 0x00});
          e.b({0x48, 0xb8}); e.q((uint64_t)&g_proj_dst); e.b({0x48, 0x89, 0x08});          // mov [rax], rcx (= rsi+0x40, projection matrix dst)
          e.b({0x58, 0x9d});
          memcpy(e.p, sp + kProjKeep, kProjLen - kProjKeep); e.p += kProjLen - kProjKeep;   // xorps xmm2,xmm2; mov eax,ebx
          e.b({0xff, 0x25, 0, 0, 0, 0}); e.q((uint64_t)(sp + kProjLen)); }
        FlushInstructionCache(GetCurrentProcess(), g_tramp, 4096);
        g_projok = write_jmp(sp, kProjLen, g_trampP);
        vlog("[hook] projection-fov hook %s: site exe+0x%x (perspective builder 0xf4e5e0)", g_projok ? "INSTALLED" : "FAILED", (uint32_t)(sp - exe));
        int ls = 0; bool ds = false; uint32_t rs = find_sig(exe, kSrcSig, &ls, &ds);
        if (!rs) { vlog("[hook] fov-source signature MISMATCH%s", ds ? " (duplicate)" : ""); return true; }
        uint8_t* ss = exe + rs + kSrcSiteOff;
        g_trampS = g_tramp + 1536;
        { Emit e{g_trampS};
          memcpy(e.p, ss, kSrcKeep); e.p += kSrcKeep;                               // movsd [r13+4672d8],xmm1; movss [r13+4672e8],xmm6
          e.b({0x9c, 0x50}); e.b({0x48, 0xb8}); e.q((uint64_t)&g_fov_bits);
          e.b({0x8b, 0x00, 0x85, 0xc0, 0x74, 0x07, 0x41, 0x89, 0x85, 0xe8, 0x72, 0x46, 0x00});   // jz +7; mov [r13+0x4672e8],eax
          e.b({0x48, 0xb8}); e.q((uint64_t)&g_src_hits); e.b({0xf0, 0x48, 0xff, 0x00});
          e.b({0x58, 0x9d});
          e.b({0xff, 0x25, 0, 0, 0, 0}); e.q((uint64_t)(ss + kSrcBack)); }          // original jmp 0x86f8ef
        FlushInstructionCache(GetCurrentProcess(), g_tramp, 4096);
        g_srcok = write_jmp(ss, kSrcLen, g_trampS);
        vlog("[hook] fov-source hook %s: site exe+0x%x -> [r13+0x4672e8]", g_srcok ? "INSTALLED" : "FAILED", (uint32_t)(ss - exe));
    }
    return true;
}

bool camhook_install() { uint8_t* exe = (uint8_t*)GetModuleHandleA(nullptr); bool r = camhook_install_at(exe); dlss_install_at(exe); return r; }

void camhook_uninstall() {
    if (!g_installed) return;
    uint8_t* site = (uint8_t*)GetModuleHandleA(nullptr) + g_site_rva;
    DWORD old;
    VirtualProtect(site, g_site_len, PAGE_EXECUTE_READWRITE, &old);
    bool ok = with_threads_suspended((uintptr_t)site, g_site_len, [&] { memcpy(site, g_orig, g_site_len); });
    VirtualProtect(site, g_site_len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, g_site_len);
    if (ok) g_installed = false;   // trampoline memory intentionally leaked (a thread may still be returning through it)
    vlog("[hook] uninstall %s", ok ? "OK" : "FAILED");
}

void camhook_set(float yaw, float pitch, float roll) {
    if (!g_data) return;
    g_data->off[0] = yaw; g_data->off[1] = pitch; g_data->off[2] = roll;
}

bool camhook_eye_ok() { return g_eye; }
void camhook_set_eye(float right) { if (g_data) g_data->off[3] = right; }
uint64_t camhook_eye_hits() { return g_data ? g_data->eyehits : 0; }
bool camhook_view_ok() { return g_viewok; }
uint64_t camhook_view_used_seq() { return (uint64_t)InterlockedCompareExchange64(&g_used_seq, 0, 0); }
void camhook_set_pull(float p) { AcquireSRWLockExclusive(&g_view_lock); g_view.pull = p; ReleaseSRWLockExclusive(&g_view_lock); }
void camhook_set_view(bool on, const float H[3][3], float eye, const float hp[3], uint64_t seq) {
    AcquireSRWLockExclusive(&g_view_lock);
    g_view.seq = seq; g_view.on = on ? 1 : 0; memcpy(g_view.H, H, sizeof(g_view.H)); g_view.eye = eye; memcpy(g_view.hp, hp, sizeof(g_view.hp));
    ReleaseSRWLockExclusive(&g_view_lock);
}
void* camhook_dbg(int i) { return g_dbg[i]; }
void camhook_view_stats(uint64_t* a, uint64_t* b) { *a = (uint64_t)g_view_a; *b = (uint64_t)g_view_b; }
void camhook_set_write(int k, uint32_t off, float val) {
    if (!g_data || k < 0 || k > 1) return;
    g_data->wr_val[k] = val; MemoryBarrier(); g_data->wr_off[k] = off;
}
bool camhook_fov_ok() { return g_fovok && g_projok; }
uint64_t camhook_proj_hits() { return (uint64_t)g_proj_hits; }
uint64_t camhook_proj_dst() { return g_proj_dst; }
uint64_t camhook_src_hits() { return (uint64_t)g_src_hits; }
void camhook_set_render_fov(float rad) { LONG b = 0; if (rad > 0.f) memcpy(&b, &rad, 4); InterlockedExchange(&g_fov_bits, b); }
uint64_t camhook_fov_hits() { return (uint64_t)g_fov_hits; }
uint64_t camhook_fov_rec() { return g_fov_rec; }
const CamFields* camhook_fields() { return g_build ? &g_build->cf : nullptr; }

void camhook_stats(uint64_t* hits, uint64_t* camobj) {
    *hits = g_data ? g_data->hits : 0; *camobj = g_data ? g_data->camobj : 0;
}
