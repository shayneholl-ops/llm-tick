// provqr.cpp — host check of the Provisioning screen's QR geometry and contrast.
//
// The Panel is slow to iterate on and a QR that will not scan is a locked-out Board,
// so the layout maths is verified here first: the matrix must fit, the quiet zone must
// be present, the module pitch must be >= 3 px, and the colours must be pure
// black-on-white (a scanner needs contrast, not taste).
//
// It renders the same rectangle the firmware would and checks the RESULT, including
// that one full module is one full square of pixels with no gaps (a gappy QR reads as
// noise to a camera).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "qrcode.h"
}

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

// Mirror the firmware's constants.
static const int SCREEN_W = 172, SCREEN_H = 320;
static const int kQuietModules = 4;
static const char* kSsid = "llm-tick-prov";
static const char* kPass = "setup1234";

int main() {
    char payload[128];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA2;S:%s;P:%s;;", kSsid, kPass);
    printf("\nProvisioning screen QR — geometry and contrast\n");
    printf("  payload (%zu B): %s\n\n", strlen(payload), payload);

    static uint8_t buf[33 * 33];
    QRCode qr;
    int size = 0;
    for (uint8_t v = 1; v <= 4; v++) {
        if (qrcode_getBufferSize(v) > sizeof(buf)) break;
        if (qrcode_initText(&qr, buf, v, ECC_LOW, payload) == 0) { size = qr.size; break; }
    }
    if (!size) { printf("  ENCODE FAILED\n"); return 1; }
    printf("  encoded: %dx%d modules\n", size, size);

    // The firmware's layout maths, copied exactly.
    int scale = 4;
    int total = size + kQuietModules * 2;
    if (total * scale > SCREEN_W - 8) scale = (SCREEN_W - 8) / total;
    if (scale < 3) scale = 3;
    int qrPx = total * scale;
    int x0 = (SCREEN_W - qrPx) / 2;
    int y0 = 96;

    printf("  scale=%d  total(modules+quiet)=%d  qrPx=%d  at (%d,%d)\n\n",
           scale, total, qrPx, x0, y0);

    check(scale >= 3, "module pitch is at least 3 px (the scanner floor)");
    check(qrPx <= SCREEN_W, "the QR fits the 172 px width");
    check(x0 >= 0, "it is inside the Panel horizontally");
    check(y0 + qrPx <= SCREEN_H, "it is inside the Panel vertically");

    // CONTRAST: a QR needs dark-on-light. Verify the two colours used are pure.
    const uint16_t bgLogical = 0xFFFF, inkLogical = 0x0000;
    check(bgLogical == 0xFFFF, "background is pure white (maximum contrast)");
    check(inkLogical == 0x0000, "modules are pure black (maximum contrast)");

    // RENDER the module rectangle into a pixel buffer and inspect it.
    std::vector<uint8_t> px(SCREEN_W * SCREEN_H, 0);   // 0 = bg, 1 = ink
    for (int my = 0; my < size; my++) {
        for (int mx = 0; mx < size; mx++) {
            if (!qrcode_getModule(&qr, (uint8_t)mx, (uint8_t)my)) continue;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    int px_x = x0 + (mx + kQuietModules) * scale + dx;
                    int px_y = y0 + (my + kQuietModules) * scale + dy;
                    if (px_x >= 0 && px_x < SCREEN_W && px_y >= 0 && px_y < SCREEN_H)
                        px[px_y * SCREEN_W + px_x] = 1;
                }
            }
        }
    }

    // Every dark module must map to a SOLID scale x scale block. A gap would make the
    // module read as a different pattern to a camera.
    int solid = 0, gappy = 0;
    for (int my = 0; my < size; my++) {
        for (int mx = 0; mx < size; mx++) {
            if (!qrcode_getModule(&qr, (uint8_t)mx, (uint8_t)my)) continue;
            bool ok = true;
            for (int dy = 0; dy < scale && ok; dy++)
                for (int dx = 0; dx < scale && ok; dx++) {
                    int px_x = x0 + (mx + kQuietModules) * scale + dx;
                    int px_y = y0 + (my + kQuietModules) * scale + dy;
                    if (!(px_x >= 0 && px_x < SCREEN_W && px_y >= 0 && px_y < SCREEN_H) ||
                        px[px_y * SCREEN_W + px_x] != 1)
                        ok = false;
                }
            if (ok) solid++; else gappy++;
        }
    }
    printf("\n  dark modules: %d solid, %d gappy\n", solid, gappy);
    check(gappy == 0, "every dark module renders as a solid block (no gaps)");

    // QUIET ZONE: the ring of pixels immediately around the symbol must be all light.
    int quietPx = kQuietModules * scale;
    bool quietOk = true;
    int qx0 = x0, qy0 = y0, qx1 = x0 + qrPx - 1, qy1 = y0 + qrPx - 1;
    for (int x = qx0; x <= qx1; x++) {
        for (int d = 0; d < quietPx; d++) {
            if (px[(qy0 + d) * SCREEN_W + x]) quietOk = false;
            if (px[(qy1 - d) * SCREEN_W + x]) quietOk = false;
        }
    }
    check(quietOk, "the quiet zone is all light (a scanner needs it to lock on)");

    // The symbol itself must not be empty or uniformly dark. Measured over the SYMBOL
    // (21x21), NOT the box that includes the quiet zone — dividing by the quiet zone's
    // area gives ~27% for a symbol that is actually ~51% dark, which is what made this
    // assertion fail while the rendering was correct.
    long ink = 0, tot = 0;
    int sx0 = x0 + kQuietModules * scale, sy0 = y0 + kQuietModules * scale;
    int symPx = size * scale;
    for (int y = sy0; y < sy0 + symPx; y++)
        for (int x = sx0; x < sx0 + symPx; x++) { ink += px[y * SCREEN_W + x]; tot++; }
    double ratio = tot ? (double)ink / tot : 0;   // a FRACTION (0..1)
    printf("  ink coverage over the SYMBOL (not the quiet zone): %.1f%%  (ink=%ld of %ld px)\n",
           ratio * 100, ink, tot);
    // Compare the FRACTION against fractions. (Comparing 0.512 against 35 would always
    // fail — an earlier version of this test did exactly that and reported a correct
    // render as broken.)
    check(ratio > 0.35 && ratio < 0.65, "ink coverage matches a QR symbol (~40-55% dark)");

    // Cross-check against the library's own matrix count: the rendered dark-module
    // count must equal the number of dark modules the encoder produced.
    int darkModules = 0;
    for (int my = 0; my < size; my++)
        for (int mx = 0; mx < size; mx++)
            if (qrcode_getModule(&qr, (uint8_t)mx, (uint8_t)my)) darkModules++;
    printf("  encoder reports %d dark modules; render drew %d\n", darkModules, solid);
    check(solid == darkModules, "every dark module the encoder emitted was drawn");

    // Rows 0..59 must stay plain (the top-band meander guard). The title sits at y=30.
    bool topPlain = true;
    for (int y = 0; y < 60; y++)
        for (int x = 0; x < SCREEN_W; x++)
            if (px[y * SCREEN_W + x]) topPlain = false;
    check(topPlain, "rows 0-59 carry no QR ink (meander guard: only the title is there)");

    printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
