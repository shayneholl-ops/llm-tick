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

    // ── encode, exactly as the firmware does (src/prov.cpp::encodeQr) ──
    char payload[128];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA2;S:%s;P:%s;;", kSsid, kPass);
    static uint8_t qbuf[33 * 33];
    QRCode qr;
    // ECC_LOW byte-mode capacity per version, from the library's own tables.
    static const uint8_t kEccLowByteCapacity[5] = { 0, 17, 32, 53, 78 };
    const size_t need = strlen(payload);
    uint8_t ver = 0;
    for (uint8_t v = 1; v <= 4; v++) {
        if (need <= kEccLowByteCapacity[v]) { ver = v; break; }
    }
    if (ver == 0) { fprintf(stderr, "payload too large\n"); return 1; }
    if (qrcode_initText(&qr, qbuf, ver, ECC_LOW, payload) != 0) {
        fprintf(stderr, "encode failed at version %u\n", ver);
        return 1;
    }
    int size = qr.size;

    // ── layout, mirrored from provRender() ──
    static const int kTopSafeY = 60, kTitleY = 62, kTitleH = 22;
    static const int kCredGap = 10, kLineStep = 24, kFooterY = 300;

    const int qrTop   = kTopSafeY + kTitleH;
    const int credTop = kFooterY - (kLineStep * 2 + 14);
    int qrAvail       = (credTop - kCredGap) - qrTop;
    if (qrAvail < 40) qrAvail = 40;
    int total = size + kQuietModules * 2;
    int scale = qrAvail / total;
    if ((W - 8) / total < scale) scale = (W - 8) / total;
    if (scale < 3) scale = 3;
    int qrPx = total * scale;
    int x0 = (W - qrPx) / 2;
    int y0 = qrTop + (qrAvail - qrPx) / 2;

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

    drawTextBlock(img, W / 2, kTitleY, "PROVISIONING", W, H, 2);
    char line[64];
    snprintf(line, sizeof(line), "WIFI  %s", kSsid);
    drawTextBlock(img, W / 2, credTop, line, W, H, 2);
    snprintf(line, sizeof(line), "PASS  %s", kPass);
    drawTextBlock(img, W / 2, credTop + kLineStep, line, W, H, 2);
    drawTextBlock(img, W / 2, credTop + kLineStep * 2, "OPEN  http://192.168.4.1/", W, H, 2);
    drawTextBlock(img, W / 2, kFooterY, "PROV STOP  to leave", W, H, 1);

    FILE* f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char v = img[i] ? 0 : 255;   // ink black, background white
        unsigned char p[3] = { v, v, v };
        fwrite(p, 1, 3, f);
    }
    fclose(f);

    printf("wrote %s\n", out);
    printf("  payload   : %u B -> version %u (%dx%d modules)\n",
           (unsigned)need, ver, size, size);
    printf("  QR        : %d px at %d px/module, y %d..%d\n", qrPx, scale, y0, y0 + qrPx);
    printf("  text rows : title %d, wifi %d, pass %d, open %d, footer %d\n",
           kTitleY, credTop, credTop + kLineStep, credTop + kLineStep * 2, kFooterY);
    if (y0 + qrPx > credTop - 4)
        printf("  WARNING: QR overruns the credential block\n");
    return 0;
}
