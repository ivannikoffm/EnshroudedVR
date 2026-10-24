// test_dlss: map the CURRENT enshrouded.exe, install the S12 DLSS-evaluate entry hook, call the real wrapper with a fake
// eval struct whose color resource is "invalid" (+0x30 != 0) -> game code returns 0xbad00005 before any NGX call.
// Proves: signature found, trampoline replays the prologue and continues in game code, mode 1 writes reset=1, regs intact.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <vector>
void vlog(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
double now_s() { return GetTickCount64() / 1000.0; }
bool dlss_install_at(uint8_t*); void dlss_set_mode(int); bool s12_dump_start(int);
void dlss_stats(uint64_t*, uint64_t*, uint64_t*, uint64_t*, float*);
alignas(16) uint8_t res[0x40]; alignas(16) uint8_t outres[0x40]; alignas(16) uint8_t ev[0x200];
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = argc > 1 ? argv[1] : "E:\\SteamLibrary\\steamapps\\common\\Enshrouded\\enshrouded.exe";
    FILE* f = fopen(path, "rb"); if (!f) { printf("no exe\n"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file(sz); fread(file.data(), 1, sz, f); fclose(f);
    auto nt = (IMAGE_NT_HEADERS*)(file.data() + ((IMAGE_DOS_HEADER*)file.data())->e_lfanew);
    uint8_t* img = (uint8_t*)VirtualAlloc(0, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(img, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        memcpy(img + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData));
    static const uint8_t pat[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x41,0x56,0x48,0x83,0xec,0x40,0x49,0x8b,0xf8,0x49,0x8b,0xd9,0x4d,0x8b,0x01,0x48,0x8b,0xea,0x4c,0x8b,0xf1};
    size_t site = 0; for (size_t o = 0x1000; o + sizeof(pat) < nt->OptionalHeader.SizeOfImage; ++o) if (img[o] == 0x48 && !memcmp(img + o, pat, sizeof(pat))) { site = o; break; }
    if (!site) { printf("signature not found\n"); return 2; }
    if (!dlss_install_at(img)) { printf("install failed\n"); return 2; }
    typedef int(__fastcall* F)(void*, void*, void*, void*);
    F fn = (F)(img + site);
    printf("wrapper at exe+0x%zx (signature), patched entry: %02x %02x ...\n", site, img[site], img[site + 1]);
    *(uint32_t*)(res + 0x30) = 1;                 // color resource type != 0 -> wrapper bails with 0xbad00005
    *(void**)(ev + 0) = res; *(void**)(ev + 8) = outres; *(uint32_t*)(outres + 0x28) = 2160; *(uint32_t*)(outres + 0x2c) = 2160;
    *(float*)(ev + 0x28) = 0.25f; *(float*)(ev + 0x2c) = -0.5f; *(float*)(ev + 0x3c) = -2160.f; *(float*)(ev + 0x40) = -2160.f;
    int fails = 0;
    int r0 = fn((void*)0x1111, (void*)0x2222, (void*)0x3333, ev);
    printf("mode 0: ret 0x%x reset=%d\n", r0, *(int32_t*)(ev + 0x38)); if ((uint32_t)r0 != 0xbad00005u || *(int32_t*)(ev + 0x38) != 0) ++fails;
    dlss_set_mode(1);
    int r1 = fn((void*)0x1111, (void*)0x2222, (void*)0x3333, ev);
    printf("mode 1: ret 0x%x reset=%d\n", r1, *(int32_t*)(ev + 0x38)); if ((uint32_t)r1 != 0xbad00005u || *(int32_t*)(ev + 0x38) != 1) ++fails;
    uint64_t c, rs, sw, fl; float l[8]; dlss_stats(&c, &rs, &sw, &fl, l);
    printf("stats calls=%llu resets=%llu fails=%llu jitter %.2f %.2f mv %.0f %.0f out %.0fx%.0f\n", c, rs, fl, l[0], l[1], l[2], l[3], l[5], l[6]);
    if (c != 2 || rs != 1 || fl || l[5] != 2160.f) ++fails;
    CreateDirectoryA("build\\enshrouded_vr", nullptr); DeleteFileA("build\\enshrouded_vr\\s12dump_1.bin");
    dlss_set_mode(0); if (!s12_dump_start(2)) ++fails;
    fn((void*)0x1111, (void*)0x2222, (void*)0x3333, ev); fn((void*)0x1111, (void*)0x2222, (void*)0x3333, ev);
    WIN32_FILE_ATTRIBUTE_DATA fa{}; bool have = GetFileAttributesExA("build\\enshrouded_vr\\s12dump_1.bin", GetFileExInfoStandard, &fa);
    printf("dump file: %s %lu bytes\n", have ? "yes" : "NO", have ? fa.nFileSizeLow : 0); if (!have || fa.nFileSizeLow < 2 * 0x900) ++fails;
    printf(fails ? "DLSS HOOK TEST FAIL\n" : "DLSS HOOK TEST PASS\n"); return fails;
}
