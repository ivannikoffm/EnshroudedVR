
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <atomic>
void vlog(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
double now_s() { return GetTickCount64() / 1000.0; }
void hwbp_trace(const uintptr_t*, const int*, const char* const*, int, double);
bool hwbp_capture(uintptr_t, int, uint64_t, double, uint64_t*);
alignas(64) volatile float cam[16];
std::atomic<bool> stop{false};
__declspec(noinline) void writer() { volatile float* p = cam; cam[11] = p[11] + 0.001f; }      // write fov
__declspec(noinline) float reader() { return cam[11] * 2.0f + cam[6]; } // read fov + quat
DWORD WINAPI worker(LPVOID) { volatile float s = 0; while (!stop) { writer(); s += reader(); Sleep(1); } return 0; }
int main() {
    HANDLE t = CreateThread(0, 0, worker, 0, 0, 0); Sleep(100);
    uintptr_t a = (uintptr_t)cam;
    uintptr_t addr[4] = {a + 0x2c, a + 0x18, a + 0x100, a};
    int len[4] = {4, 8, 4, 8};
    const char* n[4] = {"fov", "quat", "none", "pos"};
    printf("writer=%p reader=%p\n", (void*)writer, (void*)reader);
    if (__argc > 1) { uint64_t r[16]; bool ok = hwbp_capture(a + 0x2c, 4, strtoull(__argv[1], 0, 16), 2.0, r);
        printf("capture ok=%d rax=%llx rcx=%llx cam=%p\n", ok, r[0], r[1], (void*)cam); }
    else hwbp_trace(addr, len, n, 4, 1.0);
    stop = true; WaitForSingleObject(t, 1000); printf("OK alive\n"); return 0;
}