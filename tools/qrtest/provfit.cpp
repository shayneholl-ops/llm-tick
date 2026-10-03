// provfit.cpp — check the Provisioning screen's TEXT actually fits the space left
// below the QR, using the real LovyanGFX font metrics.
//
// The preview (provpreview.cpp) uses stand-in bars, so it can prove the QR placement
// but NOT whether the credentials fit. This measures the real thing: it compiles
// against LovyanGFX on the host and reports each font's height and the width of the
// widest string, then checks the total stack against the Panel.
//
// The failure this guards against: the credentials are the one thing a human must read
// correctly off the glass, so "it looked fine in a preview" is not good enough.
#include <cstdio>
#include <cstring>

#include <LovyanGFX.hpp>

static const int W = 172, H = 320;
static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    // A sprite on the host: LovyanGFX can measure text without a panel.
    static LGFX_Sprite spr;
    spr.setColorDepth(16);
    if (!spr.createSprite(W, H)) { printf("sprite alloc failed\n"); return 1; }

    const char* widest = "OPEN  http://192.168.4.1/";   // the longest line
    printf("\nProvisioning screen text fit (real LovyanGFX metrics)\n");
    printf("  longest line: \"%s\" (%zu chars)\n\n", widest, strlen(widest));

    struct { const char* name; const lgfx::IFont* f; } fonts[] = {
        { "Font0", &fonts::Font0 },
        { "Font2", &fonts::Font2 },
        { "Font4", &fonts::Font4 },
    };

    // QR block, mirrored from provRender().
    const int scale = 4, size = 21, quiet = 4;
    const int qrPx = (size + quiet * 2) * scale;   // 116
    const int y0 = 96;
    const int ty = y0 + qrPx + 10;                  // first text row

    printf("  QR block      : y %d..%d (%d px)\n", y0, y0 + qrPx, qrPx);
    printf("  text starts at: y %d\n", ty);
    printf("  space left    : %d px to the bottom (y=%d)\n\n", H - ty, H);

    for (auto& e : fonts) {
        spr.setFont(e.f);
        int h = spr.fontHeight();
        int w = spr.textWidth(widest);
        printf("  %s: height %d px, width(\"%s\") = %d px %s\n",
               e.name, h, widest, w, (w <= W - 8) ? "(fits width)" : "(TOO WIDE)");
    }

    // The layout uses Font2 for the three credential lines and Font0 for the footer.
    spr.setFont(&fonts::Font2);
    int h2 = spr.fontHeight();
    int w2 = spr.textWidth(widest);
    check(w2 <= W - 4, "the longest credential line fits the 172 px width in Font2");

    int need = h2 * 3 + 24 * 2 + 6 + 2;   // 3 rows, 2x24 pitch, gaps
    printf("\n  credential stack needs ~%d px; %d px available\n", need, H - ty);
    check(need <= H - ty, "the three credential lines fit below the QR");

    // The footer sits at H-18; it must not collide with the last credential line.
    spr.setFont(&fonts::Font0);
    int footerTop = H - 18;
    int lastLineBottom = ty + 24 + h2;
    printf("  last credential line ends y=%d; footer starts y=%d\n", lastLineBottom, footerTop);
    check(lastLineBottom <= footerTop, "the credential lines do not collide with the footer");

    // The title must not reach into the QR.
    spr.setFont(&fonts::Font4);
    int titleBottom = 30 + spr.fontHeight();
    printf("  title ends y=%d; QR starts y=%d\n", titleBottom, y0);
    check(titleBottom < y0, "the title clears the QR");

    spr.deleteSprite();
    printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
