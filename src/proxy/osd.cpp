// EnshroudedVR — in-headset OSD text rasterizer (pure GDI, no XR/Vulkan): text -> premultiplied RGBA panel.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#pragma comment(lib, "gdi32.lib")

// out: w*h*4 bytes, row-major top-down, R,G,B,A premultiplied (white text on a translucent dark panel).
bool osd_render(const wchar_t* text, int w, int h, uint8_t* out) {
    HDC dc = CreateCompatibleDC(nullptr); if (!dc) return false;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    uint8_t* px = nullptr; HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    if (!bm) { DeleteDC(dc); return false; }
    memset(px, 0, (size_t)w * h * 4);
    HGDIOBJ ob = SelectObject(dc, bm);
    HFONT f = CreateFontW(-(h * 44 / 100), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    RECT rc{0, 0, w, h};
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    GdiFlush();
    const int r = h / 4;   // rounded panel corners
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = px + ((size_t)y * w + x) * 4; uint8_t* o = out + ((size_t)y * w + x) * 4;
            int cov = (s[0] + s[1] + s[2]) / 3;                      // white-on-black coverage
            int dx = x < r ? r - x : x >= w - r ? x - (w - r - 1) : 0, dy = y < r ? r - y : y >= h - r ? y - (h - r - 1) : 0;
            bool inside = dx * dx + dy * dy <= r * r;
            int pa = inside ? 165 : 0;                               // panel alpha (black)
            int a = pa + (255 - pa) * cov / 255;
            o[0] = o[1] = o[2] = (uint8_t)cov; o[3] = (uint8_t)a;    // premultiplied: white * cov over black panel
        }
    SelectObject(dc, of); DeleteObject(f); SelectObject(dc, ob); DeleteObject(bm); DeleteDC(dc);
    return true;
}
