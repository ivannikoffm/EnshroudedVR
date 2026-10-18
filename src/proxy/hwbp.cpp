// EnshroudedVR — hardware data-breakpoint tracer: who writes / reads a camera field.
// Sets DR0..DR3 on all threads of the process (except the caller) for `secs`, a VEH collects RIPs
// of instructions that touched the watched addresses. Two phases: write-only, then read/write.
// RIP for data breakpoints = instruction AFTER the access (trap). Logged as module+offset.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

void vlog(const char* fmt, ...);
double now_s();

namespace {
struct Hit { volatile LONG64 rip; volatile LONG count; LONG slot; };
constexpr int kHits = 512;
Hit g_hits[kHits];
std::atomic<bool> g_active{false};
std::atomic<LONG> g_overflow{0};
PVOID g_veh = nullptr;
volatile uint64_t g_cap_rip = 0;          // capture registers once when a BP traps with this RIP
volatile LONG g_cap_done = 0;
uint64_t g_cap_regs[16];                  // rax rcx rdx rbx rsp rbp rsi rdi r8..r15

void record(int slot, uint64_t rip) {
    uint64_t h = (rip * 0x9E3779B97F4A7C15ull) >> 55;   // 9 bits
    for (int i = 0; i < kHits; ++i) {
        Hit& e = g_hits[(h + i) & (kHits - 1)];
        LONG64 cur = e.rip;
        if (cur == (LONG64)rip && e.slot == slot) { InterlockedIncrement(&e.count); return; }
        if (cur == 0 && InterlockedCompareExchange64(&e.rip, (LONG64)rip, 0) == 0) {
            e.slot = slot; InterlockedIncrement(&e.count); return;
        }
    }
    g_overflow++;
}

LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 st = c->Dr6 & 0xF;
    if (!st) return EXCEPTION_CONTINUE_SEARCH;
    if (g_active) for (int s = 0; s < 4; ++s) if (st & (1ull << s)) record(s, c->Rip);
    if (g_cap_rip && c->Rip == g_cap_rip && !g_cap_done) {
        uint64_t r[16] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi,
                          c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
        memcpy(g_cap_regs, r, sizeof(r));
        InterlockedExchange(&g_cap_done, 1);
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int apply_all(const uintptr_t* addr, const int* len, int n, int rw, bool enable) {
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int ok = 0;
    for (BOOL m = Thread32First(snap, &te); m; m = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                DWORD64 dr7 = 0;
                DWORD64* drs[4] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
                for (int i = 0; i < 4; ++i) *drs[i] = 0;
                if (enable) {
                    for (int i = 0; i < n && i < 4; ++i) {
                        *drs[i] = addr[i];
                        DWORD64 lenbits = len[i] == 8 ? 2 : len[i] == 4 ? 3 : len[i] == 2 ? 1 : 0;
                        dr7 |= (1ull << (i * 2)) | ((DWORD64)rw << (16 + i * 4)) | (lenbits << (18 + i * 4));
                    }
                }
                ctx.Dr7 = dr7; ctx.Dr6 = 0;
                if (SetThreadContext(th, &ctx)) ok++;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    return ok;
}

void dump(const char* tag, const char* const* names) {
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr);
    int lines = 0;
    for (int s = 0; s < 4; ++s)
        for (int i = 0; i < kHits; ++i) {
            Hit& e = g_hits[i];
            if (!e.rip || e.slot != s) continue;
            HMODULE mod = nullptr; char mname[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)e.rip, &mod);
            if (mod) { GetModuleBaseNameA(GetCurrentProcess(), mod, mname, MAX_PATH); }
            vlog("[hwbp] %s slot=%d(%s) rip=%p %s+0x%llx count=%ld", tag, s, names[s], (void*)e.rip, mname,
                 (unsigned long long)(e.rip - (mod ? (uintptr_t)mod : exe)), e.count);
            lines++;
        }
    vlog("[hwbp] %s: %d distinct sites, overflow=%ld", tag, lines, (long)g_overflow.load());
}
} // namespace

// rw: 1 = write only, 3 = read or write. len: 1/2/4/8 (address must be aligned to len).
void hwbp_trace(const uintptr_t* addr, const int* len, const char* const* names, int n, double secs) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, veh);
    for (int i = 0; i < n; ++i) vlog("[hwbp] slot %d = %s @ %p len %d", i, names[i], (void*)addr[i], len[i]);
    const int phases[2] = {1, 3};
    const char* tags[2] = {"WRITE", "READ|WRITE"};
    for (int p = 0; p < 2; ++p) {
        memset(g_hits, 0, sizeof(g_hits)); g_overflow = 0;
        g_active = true;
        int th = apply_all(addr, len, n, phases[p], true);
        double t0 = now_s();
        while (now_s() - t0 < secs) Sleep(10);
        apply_all(addr, len, n, phases[p], false);
        g_active = false;
        vlog("[hwbp] phase %s on %d threads, %.1f s", tags[p], th, secs);
        dump(tags[p], names);
    }
}

// Arm a write BP on `addr` and wait until an instruction at RIP (=exe+rip_rva, the one AFTER the store) traps.
// Returns true and fills regs[16] (rax rcx rdx rbx rsp rbp rsi rdi r8..r15).
bool hwbp_capture(uintptr_t addr, int len, uint64_t rip_rva, double timeout, uint64_t* regs) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, veh);
    g_cap_done = 0;
    g_cap_rip = (uintptr_t)GetModuleHandleA(nullptr) + rip_rva;
    uintptr_t a[1] = {addr}; int l[1] = {len};
    int th = apply_all(a, l, 1, 1, true);
    double t0 = now_s();
    while (!g_cap_done && now_s() - t0 < timeout) Sleep(5);
    apply_all(a, l, 1, 1, false);
    g_cap_rip = 0;
    vlog("[hwbp] capture rip=exe+0x%llx on %d threads: %s", (unsigned long long)rip_rva, th, g_cap_done ? "OK" : "TIMEOUT");
    if (g_cap_done) memcpy(regs, g_cap_regs, sizeof(g_cap_regs));
    return g_cap_done != 0;
}
