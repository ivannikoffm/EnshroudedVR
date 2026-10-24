// test_camhook v3: map the REAL enshrouded.exe image (no relocs needed for this code path), install the hook,
// execute the real game bytes exe+0xeaad00..0xeaad44 (ret patched at 0xeaad44 in OUR copy) with rbx = fake CamObj.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <initializer_list>
void vlog(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
double now_s() { return GetTickCount64() / 1000.0; }
bool camhook_install_at(uint8_t*); void camhook_set(float, float, float); void camhook_stats(uint64_t*, uint64_t*);
bool camhook_eye_ok(); void camhook_set_eye(float); uint64_t camhook_eye_hits(); void camhook_set_write(int, uint32_t, float);
bool camhook_view_ok(); void camhook_set_view(bool, const float[3][3], float, const float[3], uint64_t); void camhook_view_stats(uint64_t*, uint64_t*); void* camhook_dbg(int); void camhook_set_pull(float);
bool camhook_fov_ok(); void camhook_set_render_fov(float); uint64_t camhook_fov_hits(); uint64_t camhook_fov_rec(); uint64_t camhook_proj_hits(); uint64_t camhook_src_hits();
alignas(16) float frec[64]; alignas(16) uint64_t ffr[0x200];
alignas(16) float vdst[4]; alignas(16) float vmat[12]; alignas(16) int64_t vout[4]; alignas(16) uint64_t fr[16];
alignas(16) float xout[8];
alignas(16) float cam[0x2000];
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = argc > 1 ? argv[1] : "enshrouded_v1.0_a07fae72.exe";   // RVAs below are v1.0; current exe is checked by tools/sigscan.py
    FILE* f = fopen(path, "rb"); if (!f) { printf("no exe\n"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file(sz); fread(file.data(), 1, sz, f); fclose(f);
    auto nt = (IMAGE_NT_HEADERS*)(file.data() + ((IMAGE_DOS_HEADER*)file.data())->e_lfanew);
    uint8_t* img = (uint8_t*)VirtualAlloc(0, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(img, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        memcpy(img + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData));
    img[0xeaad44] = 0xc3;   // ret (our copy only)
    img[0xe7ed6d] = 0xc3;
    { const uint8_t ep[5] = {0x48, 0x83, 0xc4, 0x40, 0xc3}; memcpy(img + 0xe7ee11, ep, 5); }   // add rsp,0x40; ret (fake frame of stubA)
    img[0xe7eedf] = 0xc3;
    img[0x3a8a7a] = 0xc3; img[0x8558de] = 0xc3; img[0x86f8ef] = 0xc3;   // after render-fov site: return to the test thunk
    // thunk: push rbx; sub rsp,0x48; movups [rsp],xmm6; movups [rsp+16],xmm7; movups [rsp+32],xmm8; mov rbx,cam; mov rax,target; call rax;
    //        movups xmm6..8 back; add rsp,0x48; pop rbx; ret
    uint8_t* th = (uint8_t*)VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE), *p = th;
    auto B = [&](std::initializer_list<uint8_t> l) { for (auto x : l) *p++ = x; };
    auto Q = [&](uint64_t v) { memcpy(p, &v, 8); p += 8; };
    B({0x53, 0x48, 0x83, 0xec, 0x40, 0x0f, 0x11, 0x34, 0x24, 0x0f, 0x11, 0x7c, 0x24, 0x10, 0x44, 0x0f, 0x11, 0x44, 0x24, 0x20});
    B({0x48, 0xbb}); Q((uint64_t)cam);
    B({0x48, 0xb8}); Q((uint64_t)(img + 0xeaad00));
    B({0xff, 0xd0});
    B({0x0f, 0x10, 0x34, 0x24, 0x0f, 0x10, 0x7c, 0x24, 0x10, 0x44, 0x0f, 0x10, 0x44, 0x24, 0x20, 0x48, 0x83, 0xc4, 0x40, 0x5b, 0xc3});
    cam[0x38/4] = 3.0f; cam[0x40/4] = -0.1f; cam[0x48/4] = 0.05f; cam[0x68/4] = 1.f;
    ((void(*)())th)();   // unhooked run
    float y0 = cam[0x1498/4], p0 = cam[0x149c/4], r0 = cam[0x14a0/4];
    if (!camhook_install_at(img)) { printf("install failed\n"); return 2; }
    camhook_set(0.25f, -0.125f, 0.0625f);
    ((void(*)())th)();
    camhook_set_write(0, 0x20c4, 1.6f); camhook_set_write(1, 0x20b8, 1.0f);
    ((void(*)())th)();
    uint64_t h, o; camhook_stats(&h, &o);
    printf("unhooked %.4f %.4f %.4f | hooked %.4f %.4f %.4f | hits=%llu camobj_ok=%d\n", y0, p0, r0,
           cam[0x1498/4], cam[0x149c/4], cam[0x14a0/4], h, o == (uint64_t)cam);
    bool ok = y0 == 3.f && p0 == -0.1f && r0 == 0.05f && fabsf(cam[0x1498/4] - 3.25f) < 1e-6f && fabsf(cam[0x149c/4] + 0.225f) < 1e-6f &&
              fabsf(cam[0x14a0/4] - 0.1125f) < 1e-6f && h == 2 && o == (uint64_t)cam;
    // eye thunk: push rbx; mov rbx,cam; mov rax,img+0xe7ed5d; call rax; mov rax,xout; movups [rax],xmm4; movups [rax+16],xmm3; pop rbx; ret
    uint8_t* th2 = th + 512; p = th2;
    B({0x53, 0x48, 0x83, 0xec, 0x20}); B({0x48, 0xbb}); Q((uint64_t)cam); B({0x48, 0xb8}); Q((uint64_t)(img + 0xe7ed5d)); B({0xff, 0xd0});
    B({0x48, 0xb8}); Q((uint64_t)xout); B({0x0f, 0x11, 0x20, 0x0f, 0x11, 0x58, 0x10, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
    cam[0x148c/4] = 0.525f; cam[0x1490/4] = 0.1f; cam[0x1494/4] = -0.2f;
    camhook_set_eye(-0.0315f);
    ((void(*)())th2)();
    printf("eye_ok=%d xmm4=(%.4f %.4f) xmm3=%.4f eyehits=%llu state148c=%.4f\n", camhook_eye_ok(), xout[0], xout[1], xout[4], camhook_eye_hits(), cam[0x148c/4]);
    bool ok2 = camhook_eye_ok() && fabsf(xout[0] - 0.4935f) < 1e-6f && xout[1] == 0.1f && xout[4] == -0.2f && camhook_eye_hits() == 1 && cam[0x148c/4] == 0.525f;
    ok = ok && ok2;
    printf("writes: +20c4=%.3f +20b8=%.3f\n", cam[0x20c4/4], cam[0x20b8/4]);
    ok = ok && cam[0x20c4/4] == 1.6f && cam[0x20b8/4] == 1.0f;
    // ---- view hooks. thunkA: push rbx; sub rsp,0x20; mov rbx,cam; mov rcx,vdst; mov rdx,vmat; mov rax,A; call rax; add rsp,0x20; pop rbx; ret
    uint8_t* tA = th + 1024; p = tA;
    B({0x53, 0x48, 0x83, 0xec, 0x28}); B({0x48, 0xbb}); Q((uint64_t)cam); B({0x48, 0xb9}); Q((uint64_t)vdst); B({0x48, 0xba}); Q((uint64_t)vmat);   // rsp at site = 0 mod 16 like the real body
    uint8_t* stubA = th + 2048;
    B({0x48, 0xb8}); Q((uint64_t)stubA); B({0xff, 0xd0}); B({0x48, 0x83, 0xc4, 0x28, 0x5b, 0xc3});
    { uint8_t* q = stubA; const uint8_t s1[4] = {0x48, 0x83, 0xec, 0x40}; memcpy(q, s1, 4); q += 4;   // sub rsp,0x40 (frame + shadow like the real body)
      q[0] = 0x48; q[1] = 0xb8; uint64_t tg = (uint64_t)(img + 0xe7edfc); memcpy(q + 2, &tg, 8); q[10] = 0xff; q[11] = 0xe0; }   // mov rax,site; jmp rax
    // thunkB: push rbx; push rdi; sub rsp,0x28; movups [rsp],xmm6; (xmm7 -> [rsp+0x10]); mov rdi,vout; mov r11,fr+0x40; call B;
    //         movups xmm6..; add rsp,0x28; pop rdi; pop rbx; ret
    uint8_t* tB = th + 1536; p = tB;
    B({0x53, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0x11, 0x34, 0x24, 0x0f, 0x11, 0x7c, 0x24, 0x10});
    B({0x48, 0xbf}); Q((uint64_t)vout); B({0x49, 0xbb}); Q((uint64_t)(fr + 8)); B({0x48, 0xb8}); Q((uint64_t)(img + 0xe7eecd)); B({0xff, 0xd0});
    B({0x0f, 0x10, 0x34, 0x24, 0x0f, 0x10, 0x7c, 0x24, 0x10, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0x5b, 0xc3});
    fr[8 + 2] = 0x1111; fr[8 + 3] = (uint64_t)vout;   // [r11+0x10] -> rbx, [r11+0x18] -> rdi
    memset(vmat, 0, sizeof(vmat)); vmat[0] = 1; vmat[5] = 1; vmat[10] = 1;
    ((void(*)())tA)();
    printf("dbg dst=%p (vdst %p) m=%p (vmat %p)\n", camhook_dbg(0), (void*)vdst, camhook_dbg(1), (void*)vmat);
    printf("view_ok=%d off: q=(%.3f %.3f %.3f %.3f)\n", camhook_view_ok(), vdst[0], vdst[1], vdst[2], vdst[3]);
    bool ok3 = camhook_view_ok() && fabsf(vdst[3] - 1.f) < 1e-5f && fabsf(vdst[0]) < 1e-6f;
    const float H[3][3] = {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}};   // yaw 90: r2 = f, f2 = -r
    const float hp[3] = {0.f, 0.1f, 0.f};
    camhook_set_view(true, H, 0.032f, hp, 7);
    vout[0] = vout[1] = vout[2] = 0;
    ((void(*)())tA)(); ((void(*)())tB)();
    uint64_t va, vb; camhook_view_stats(&va, &vb);
    printf("view on: q=(%.4f %.4f %.4f %.4f) out=(%.4f %.4f %.4f) m  calls A=%llu B=%llu\n", vdst[0], vdst[1], vdst[2], vdst[3],
           vout[0] / 4294967296.0, vout[1] / 4294967296.0, vout[2] / 4294967296.0, va, vb);
    // expected: quat of rows {(0,0,1),(0,1,0),(-1,0,0)}: game formula y = (m20 - m02)/4w = (-1-1)/(4*0.7071) = -0.7071, w = 0.7071
    ok3 = ok3 && fabsf(vdst[1] + 0.70711f) < 1e-4f && fabsf(vdst[3] - 0.70711f) < 1e-4f &&
          fabs(vout[2] / 4294967296.0 - 0.032) < 1e-6 && fabs(vout[1] / 4294967296.0 - 0.1) < 1e-6 && va == 2 && vb == 1;
    camhook_set_pull(0.5f); vout[0] = vout[1] = vout[2] = 0;
    ((void(*)())tA)(); ((void(*)())tB)(); camhook_set_pull(0.f);
    printf("view pull: out=(%.4f %.4f %.4f)\n", vout[0] / 4294967296.0, vout[1] / 4294967296.0, vout[2] / 4294967296.0);
    ok3 = ok3 && fabs(vout[2] / 4294967296.0 - 0.532) < 1e-5 && fabs(vout[1] / 4294967296.0 - 0.1) < 1e-6;
    printf("view %s\n", ok3 ? "OK" : "BAD");
    {   // ---- render-fov hook: thunk sets rax=cam, r14=rec, rbp=frame([rbp+0x5b8]=cam), calls the real site
        uint8_t* tF = th + 2560; p = tF;
        B({0x55, 0x41, 0x56, 0x48, 0x83, 0xec, 0x10});                                    // push rbp; push r14; sub rsp,0x10
        B({0x48, 0xb8}); Q((uint64_t)cam); B({0x49, 0xbe}); Q((uint64_t)frec); B({0x48, 0xbd}); Q((uint64_t)ffr);
        B({0x49, 0xbb}); Q((uint64_t)(img + 0x3a8a69)); B({0x41, 0xff, 0xd3});           // call r11
        B({0x48, 0x83, 0xc4, 0x10, 0x41, 0x5e, 0x5d, 0xc3});
        ffr[0x5b8 / 8] = (uint64_t)cam; cam[0x20c4 / 4] = 1.39626f; frec[0x4c / 4] = 0.f;
        camhook_set_render_fov(0.f); ((void(*)())tF)(); float a = frec[0x4c / 4];
        camhook_set_render_fov(1.6581f); ((void(*)())tF)(); float b = frec[0x4c / 4];
        camhook_set_render_fov(0.f);
        printf("fov hook ok=%d off=%.5f on=%.5f hits=%llu\n", camhook_fov_ok(), a, b, camhook_fov_hits());
        bool okf = camhook_fov_rec() == (uint64_t)frec && camhook_fov_ok() && fabsf(a - 1.39626f) < 1e-6f && fabsf(b - 1.6581f) < 1e-6f && camhook_fov_hits() == 2;
        printf("fov %s\n", okf ? "OK" : "BAD"); ok3 = ok3 && okf;
        // projection site: thunk r14 = big object, call site, return xmm1 via movd eax,xmm1 into a global
        static float* bigo = (float*)VirtualAlloc(nullptr, 0x468000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        static float outv = 0.f; bigo[0x4672e8 / 4] = 1.39626f;
        uint8_t* tP = th + 3072; p = tP;
        B({0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x20}); B({0x49, 0xbe}); Q((uint64_t)bigo); B({0x31, 0xdb});   // push r14; push rbx; sub; r14=big; ebx=0
        B({0x49, 0xbb}); Q((uint64_t)(img + 0x8558d0)); B({0x41, 0xff, 0xd3});
        B({0x48, 0xb9}); Q((uint64_t)&outv); B({0xf3, 0x0f, 0x11, 0x09});                  // movss [rcx], xmm1
        B({0x48, 0x83, 0xc4, 0x20, 0x5b, 0x41, 0x5e, 0xc3});
        ((void(*)())tP)(); float pa = outv;
        camhook_set_render_fov(1.6581f); ((void(*)())tP)(); float pb = outv; camhook_set_render_fov(0.f);
        bool okp = fabsf(pa - 1.39626f) < 1e-6f && fabsf(pb - 1.6581f) < 1e-6f && camhook_proj_hits() == 2;
        printf("proj hook off=%.5f on=%.5f hits=%llu %s\n", pa, pb, camhook_proj_hits(), okp ? "OK" : "BAD"); ok3 = ok3 && okp;
        {   // fov-source site: r13 = big object, xmm6 = 1.39626, rbp=frame ([rbp+0x40] irrelevant), call 0x86f8d4
            static float six = 1.39626f; uint8_t* tS = th + 3584; p = tS;
            B({0x41, 0x55, 0x48, 0x83, 0xec, 0x30, 0x0f, 0x11, 0x34, 0x24});                    // push r13; sub rsp,0x30; movups [rsp],xmm6
            B({0x49, 0xbd}); Q((uint64_t)bigo); B({0x48, 0xb8}); Q((uint64_t)&six); B({0xf3, 0x0f, 0x10, 0x30});   // r13=big; movss xmm6,[rax]
            B({0x49, 0xbb}); Q((uint64_t)(img + 0x86f8d4)); B({0x41, 0xff, 0xd3});
            B({0x0f, 0x10, 0x34, 0x24, 0x48, 0x83, 0xc4, 0x30, 0x41, 0x5d, 0xc3});
            bigo[0x4672e8 / 4] = 0.f; ((void(*)())tS)(); float sa = bigo[0x4672e8 / 4];
            camhook_set_render_fov(1.6581f); ((void(*)())tS)(); float sb = bigo[0x4672e8 / 4]; camhook_set_render_fov(0.f);
            bool oks = fabsf(sa - 1.39626f) < 1e-6f && fabsf(sb - 1.6581f) < 1e-6f && camhook_src_hits() == 2;
            printf("src hook off=%.5f on=%.5f hits=%llu %s\n", sa, sb, camhook_src_hits(), oks ? "OK" : "BAD"); ok3 = ok3 && oks;
        }
    }
    ok = ok && ok3;
    printf(ok ? "PASS\n" : "FAIL\n"); return ok ? 0 : 3;
}
