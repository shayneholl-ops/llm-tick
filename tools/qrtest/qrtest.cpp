// qrtest.cpp — host check that the QR library encodes the URL the AP will need.
//
// Compiled against the REAL library source (qrcode.c) with plain g++, so the encoding
// is proven before any Panel code depends on it. Prints the module matrix so the
// geometry can be checked, and asserts the version/size fit the 172x320 Panel.
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "qrcode.h"
}

static int g_fail = 0;

static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

// ASCII render, 2 chars per module, so the matrix can be eyeballed as text.
static void dump(QRCode& qr) {
    for (int y = 0; y < qr.size; y++) {
        printf("    ");
        for (int x = 0; x < qr.size; x++) printf(qrcode_getModule(&qr, x, y) ? "##" : "..");
        printf("\n");
    }
}

int main() {
    // The URI the Setup AP will advertise. SSID and passphrase are what the spec
    // proposes: a short SSID and a short WPA2 passphrase, so the QR stays low-density.
    const char* uri = "WIFI:T:WPA;S:llm-tick-prov;P:setup1234;;";

    printf("\nQR encoding for the Provisioning screen\n");
    printf("  payload: %s\n", uri);
    printf("  length : %zu bytes\n\n", strlen(uri));

    // Try versions upward, as the real code must: the smallest version that fits.
    for (uint8_t ver = 1; ver <= 10; ver++) {
        uint16_t bufsize = qrcode_getBufferSize(ver);
        std::string store(bufsize, '\0');
        QRCode qr;
        int8_t rc = qrcode_initText(&qr, (uint8_t*)store.data(), ver, ECC_LOW, uri);
        printf("  version %2u (buf %4u B): %s\n", ver, bufsize,
               rc == 0 ? "FITS" : "too small");
        if (rc == 0) {
            printf("\n  -> chosen: version %u, %ux%u modules, ecc LOW\n",
                   ver, qr.size, qr.size);
            // Geometry on the 172x320 Panel: modules + a 4-module quiet zone per side.
            // The quiet zone is REQUIRED for a scanner to lock on, so it is part of the
            // budget, not optional padding.
            int total = qr.size + 8;
            printf("     modules+quiet = %d\n", total);
            int best = 0;
            for (int scale = 5; scale >= 3; scale--) {
                int px = total * scale;
                printf("     at %d px/module -> %d px square%s\n", scale, px,
                       (px <= 172) ? "  (fits the 172 px width)" : "  (TOO WIDE)");
                if (px <= 172 && scale > best) best = scale;
            }
            printf("     -> choose %d px/module (%d px square)\n", best, total * best);
            check(best >= 3, "a workable scale exists that fits 172 px wide");
            check(total * best >= 100,
                  "the chosen size is at least 100 px, so a phone camera can resolve it");
            check(best == 5, "version 1 fits at the maximum 5 px/module");
            check(qr.size >= 21, "a real QR matrix was produced");

            // Sanity: the three finder patterns must be present in the corners.
            bool tl = qrcode_getModule(&qr, 0, 0) && qrcode_getModule(&qr, 6, 6);
            bool tr = qrcode_getModule(&qr, qr.size - 1, 0);
            bool bl = qrcode_getModule(&qr, 0, qr.size - 1);
            check(tl, "top-left finder pattern present");
            check(tr, "top-right finder pattern present");
            check(bl, "bottom-left finder pattern present");
            printf("\n");
            dump(qr);
            break;
        }
    }

    printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
