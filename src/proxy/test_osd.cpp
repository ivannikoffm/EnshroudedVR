// test_osd: rasterize sample OSD lines -> raw RGBA dumps (python composites them over a checker for review)
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
bool osd_render(const wchar_t* text, int w, int h, uint8_t* out);
int main() {
    const wchar_t* t[] = {L"Stereo (IPD) 140%", L"Camera distance 120%", L"HUD panel 70\u00b0", L"Settings reloaded from vr.ini",
                          L"EnshroudedVR 1.0  \u00b7  Home = recenter"};
    const int W = 768, H = 96; std::vector<uint8_t> px(W * H * 4);
    for (int i = 0; i < 5; ++i) {
        if (!osd_render(t[i], W, H, px.data())) { printf("FAIL render %d\n", i); return 1; }
        char fn[64]; snprintf(fn, sizeof(fn), "build\\osd_%d.rgba", i);
        FILE* f = fopen(fn, "wb"); fwrite(px.data(), 1, px.size(), f); fclose(f);
        int nz = 0; for (int k = 0; k < W * H; ++k) if (px[k * 4] > 128) ++nz;
        printf("osd %d: text pixels %d\n", i, nz);
    }
    printf("PASS\n"); return 0;
}
