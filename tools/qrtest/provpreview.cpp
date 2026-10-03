// provpreview.cpp — render the Provisioning Panel screen offline to a PPM, so the QR
// layout can be seen without flashing and without a camera.
//
// This is the same trick tools/wxpreview uses for the weather scene: compile the real
// drawing code against a stub Arduino/LovyanGFX and dump the framebuffer. It means the
// layout can be iterated on in seconds instead of a flash cycle.
//
// Usage: provpreview <out.ppm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "qrcode.h"
}

static const int W = 172, H = 320;

// Constants mirrored from src/prov.cpp.
static const int kQuietModules = 4;
static const char* kSsid = "llm-tick-prov";
static const char* kPass = "setup1234";

// A 5x7 bitmap font good enough to show WHERE the text sits; the firmware uses
// LovyanGFX fonts, so this preview proves the LAYOUT, not the exact glyphs.
static const unsigned char* glyphFor(char c) {
    static const unsigned char SP[7] = {0,0,0,0,0,0,0};
    (void)c;
    return SP;
}
static void drawTextBlock(std::vector<uint8_t>& img, int cx, int y, const char* text,
                          int w, int h, int scale) {
    // Draw a simple bar the width the real font would occupy, so the layout is visible.
    int len = (int)strlen(text);
    int tw = len * 6 * scale;
    int x0 = cx - tw / 2;
    for (int yy = y; yy < y + 7 * scale && yy < h; yy++)
        for (int xx = x0; xx < x0 + tw && xx < w; xx++)
            if (xx >= 0 && yy >= 0) img[yy * w + xx] = 1;
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "prov.ppm";

    // ── encode, exactly as the firmware does ──
    char payload[128];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", kSsid, kPass);
    static uint8_t qbuf[33 * 33];
    QRCode qr;
    int size = 0;
    for (uint8_t v = 1; v <= 4; v++) {
        if (qrcode_getBufferSize(v) > sizeof(qbuf)) break;
        if (qrcode_initText(&qr, qbuf, v, ECC_LOW, payload) == 0) { size = qr.size; break; }
    }
    if (!size) { fprintf(stderr, "encode failed\n"); return 1; }

    // ── layout, mirrored from provRender() ──
    int scale = 4;
    int total = size + kQuietModules * 2;
    if (total * scale > W - 8) scale = (W - 8) / total;
    if (scale < 3) scale = 3;
    int qrPx = total * scale;
    int x0 = (W - qrPx) / 2;
    int y0 = 96;

    // 0 = background (white), 1 = ink (black)
    std::vector<uint8_t> img((size_t)W * H, 0);

    for (int my = 0; my < size; my++)
        for (int mx = 0; mx < size; mx++) {
            if (!qrcode_getModule(&qr, (uint8_t)mx, (uint8_t)my)) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++) {
                    int px = x0 + (mx + kQuietModules) * scale + dx;
                    int py = y0 + (my + kQuietModules) * scale + dy;
                    if (px >= 0 && px < W && py >= 0 && py < H) img[py * W + px] = 1;
                }
        }

    drawTextBlock(img, W / 2, 30, "SETUP MODE", W, H, 3);
    drawTextBlock(img, W / 2, 64, "scan to join", W, H, 1);
    int ty = y0 + qrPx + 10;
    char line[64];
    snprintf(line, sizeof(line), "WIFI  %s", kSsid);
    drawTextBlock(img, W / 2, ty, line, W, H, 2);
    snprintf(line, sizeof(line), "PASS  %s", kPass);
    drawTextBlock(img, W / 2, ty + 24, line, W, H, 2);
    drawTextBlock(img, W / 2, ty + 48, "OPEN  http://192.168.4.1/", W, H, 2);
    drawTextBlock(img, W / 2, H - 18, "PROV STOP to leave", W, H, 1);

    FILE* f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char v = img[i] ? 0 : 255;   // ink black, background white
        unsigned char p[3] = { v, v, v };
        fwrite(p, 1, 3, f);
    }
    fclose(f);

    printf("wrote %s: QR %dx%d modules at %d px/module (%d px), origin (%d,%d)\n",
           out, size, size, scale, qrPx, x0, y0);
    printf("text rows: title 30, hint 64, wifi %d, pass %d, open %d, footer %d\n",
           ty, ty + 24, ty + 48, H - 18);
    return 0;
}
