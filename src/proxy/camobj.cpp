// EnshroudedVR — stage 2 of camera discovery: the game camera object that feeds keen::ecs::ClientCamera.
// Run 6 (hw-bp + disasm): ClientCamera (+0x20 inside its component) is written at exe+0x32d119..0x32d17e:
//   rax = [r14+0x530] = CamObj;  fov=[CamObj+0x1b24], aspect=+0x1b18, near=+0x1b1c, far=+0x1b20, distance=+0xef0
//   pos/quat = fn exe+0xcb9c10(out, CamObj): base matrix CamObj+0xf00..0xf2f, rotated by -angles at +0xee4/+0xee8/+0xeec,
//   local offset +0xed4..+0xedc.  Pinning any ClientCamera copy (fov AND quat) never changed the picture -> render reads elsewhere.
// This stage: capture CamObj, dump it, trace its readers, causal pins with beep groups.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

void vlog(const char* fmt, ...);
double now_s();
bool hwbp_capture(uintptr_t addr, int len, uint64_t rip_rva, double timeout, uint64_t* regs);
void hwbp_trace(const uintptr_t* addr, const int* len, const char* const* names, int n, double secs);

namespace {
constexpr uint64_t kRipAfterFovStore = 0x32d14f;   // instruction after "mov [r15+0x4c], ecx" (fov)

bool rd(uintptr_t a, void* d, size_t n) { __try { memcpy(d, (void*)a, n); return true; } __except (1) { return false; } }
bool wr(uintptr_t a, float v) { __try { *(volatile float*)a = v; return true; } __except (1) { return false; } }

bool camobj_valid(uintptr_t o, float aspect) {
    float f[4];
    if (!rd(o + 0x1b18, f, 16)) return false;
    return fabsf(f[0] - aspect) < 0.01f && f[1] > 0.0009f && f[1] < 5.f && f[2] > 10.f && f[2] < 1e7f && f[3] > 0.2f && f[3] < 3.f;
}

void dump(uintptr_t o, uint32_t from, uint32_t to, const char* tag) {
    for (uint32_t off = from; off < to; off += 0x20) {
        float f[8]; if (!rd(o + off, f, 32)) { vlog("[obj] %s +%04x unreadable", tag, off); return; }
        bool any = false; for (float v : f) if (v != 0.f) any = true;
        if (!any) continue;
        vlog("[obj] %s +%04x: %11.5g %11.5g %11.5g %11.5g | %11.5g %11.5g %11.5g %11.5g", tag, off,
             f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
}

void beepn(int n) { for (int i = 0; i < n; ++i) { Beep(1200, 60); Sleep(80); } }

// pin one float for `secs` from this thread (tight loop with 1 ms sleeps), counting overwrites by the game
void pin(uintptr_t o, uint32_t off, float v, double secs, float aspect, const char* name) {
    double t0 = now_s(); unsigned long long rev = 0, skip = 0;
    while (now_s() - t0 < secs) {
        if (!camobj_valid(o, aspect) && off != 0x1b24) { skip++; Sleep(1); continue; }
        float cur; if (!rd(o + off, &cur, 4)) break;
        if (cur != v) { rev++; wr(o + off, v); }
        YieldProcessor();
    }
    vlog("[obj] pin %s (+0x%x)=%.4f for %.0f s: game rewrote %llu times, skipped %llu", name, off, v, secs, rev, skip);
}
} // namespace

void camobj_stage(uintptr_t clientcam_addr, float aspect) {
    uint64_t r[16];
    vlog("[obj] stage start, ClientCamera source copy %p", (void*)clientcam_addr);
    if (!hwbp_capture(clientcam_addr + 0x2c, 4, kRipAfterFovStore, 3.0, r)) { beepn(5); return; }
    uintptr_t o = (uintptr_t)r[0];
    vlog("[obj] regs rax(CamObj)=%p r14=%p r15=%p (r15+0x20 should be ClientCamera %p)", (void*)r[0], (void*)r[14], (void*)r[15], (void*)clientcam_addr);
    if (!camobj_valid(o, aspect)) { vlog("[obj] CamObj validation FAILED"); dump(o, 0x1b00, 0x1b40, "bad"); beepn(5); return; }
    dump(o, 0xe00, 0x1c00, "t0");
    Sleep(1000);
    dump(o, 0xe80, 0xf40, "t1");

    uintptr_t a[4] = {o + 0x1b24, o + 0xf00, o + 0xee8, o + 0xef0};
    int l[4] = {4, 8, 4, 4};
    const char* n[4] = {"CamObj.fov+1b24", "CamObj.mat+f00", "CamObj.ang+ee8", "CamObj.dist+ef0"};
    hwbp_trace(a, l, n, 4, 2.0);

    // causal pins. group = number of beeps
    struct P { uint32_t off; const char* name; float delta; bool mul; } ps[] = {
        {0x1b24, "fov x0.6", 0.6f, true},
        {0xee4, "ang ee4 +0.35", 0.35f, false},
        {0xee8, "ang ee8 +0.35", 0.35f, false},
        {0xeec, "ang eec +0.35", 0.35f, false},
    };
    for (int g = 0; g < 4; ++g) {
        Sleep(2000);
        float cur = 0; rd(o + ps[g].off, &cur, 4);
        float v = ps[g].mul ? cur * ps[g].delta : cur + ps[g].delta;
        beepn(g + 1);
        pin(o, ps[g].off, v, 4.0, aspect, ps[g].name);
        if (ps[g].mul) wr(o + ps[g].off, cur);   // restore fov immediately
    }
    Sleep(500); beepn(3); Sleep(300); beepn(3);
    vlog("[obj] stage done");
}
