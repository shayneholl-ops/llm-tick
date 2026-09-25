// Offline preview driver: renders one wxScene frame to a PPM (and prints the
// per-row colour of the panel rows) so the background can be inspected without
// flashing the board.  Usage:
//   wxpreview <fam> <day> <millis> <out.ppm> [bg]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
uint32_t g_hostMillis = 0;

#include "wxscene.h"

static const int W = 172, H = 320;

int main(int argc, char** argv) {
  int fam = argc > 1 ? atoi(argv[1]) : 5;               // WX_RAIN
  int day = argc > 2 ? atoi(argv[2]) : 0;
  uint32_t t = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 10) : 12345u;
  const char* out = argc > 4 ? argv[4] : "wx.ppm";
  g_wxBg = argc > 5 ? atoi(argv[5]) : 0;
  g_hostMillis = t;

  static uint16_t buf[(size_t)W * H];
  wxSceneInit();
  wxSceneRender(buf, W, H, fam, day, true);

  FILE* f = fopen(out, "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", out); return 1; }
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; i++) {
    uint16_t c = (uint16_t)((buf[i] >> 8) | (buf[i] << 8));   // undo byte swap
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    unsigned char p[3] = { (unsigned char)(r * 255 / 31),
                           (unsigned char)(g * 255 / 63),
                           (unsigned char)(b * 255 / 31) };
    fwrite(p, 1, 3, f);
  }
  fclose(f);

  printf("bg=%d (%s) fam=%d day=%d t=%u\n", g_wxBg, wxBgName(g_wxBg), fam, day, t);

  // Row survey: mean of each row plus the min/max (structure detector).
  printf("row  rgb-mean          min   max   spread\n");
  for (int y = 190; y < H; y += 4) {
    long sr = 0, sg = 0, sb = 0; int lo = 999, hi = -1;
    for (int x = 0; x < W; x++) {
      uint16_t c = (uint16_t)((buf[(size_t)y * W + x] >> 8) | (buf[(size_t)y * W + x] << 8));
      int r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
      sr += r; sg += g; sb += b;
      int l = (r + g + b) / 3; if (l < lo) lo = l; if (l > hi) hi = l;
    }
    printf("%3d  (%3ld,%3ld,%3ld)   %4d  %4d  %4d\n", y, sr / W, sg / W, sb / W, lo, hi, hi - lo);
  }

  // Legibility scan: the UI draws white ink straight onto this scene, so flag
  // any row whose brightest pixel is bright enough to fight the type, and the
  // brightest-row overall. `bright` in ui.cpp flips at mean lum > 128.
  int worstY = 0; long worstL = -1; int nBright = 0;
  for (int y = 0; y < H; y++) {
    long s = 0; int hi = -1;
    for (int x = 0; x < W; x++) {
      uint16_t c = (uint16_t)((buf[(size_t)y * W + x] >> 8) | (buf[(size_t)y * W + x] << 8));
      int r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
      s += (r + g + b) / 3; if ((r + g + b) / 3 > hi) hi = (r + g + b) / 3;
    }
    if (s / W > worstL) { worstL = s / W; worstY = y; }
    if (hi > 150) nBright++;
  }
  printf("brightest row-mean: y=%d lum=%ld | rows with a pixel >150 lum: %d\n", worstY, worstL, nBright);
  printf("col x=2 lum at y=160 (the ui.cpp ink flip probes this): ");
  { uint16_t c = (uint16_t)((buf[(size_t)160 * W + 2] >> 8) | (buf[(size_t)160 * W + 2] << 8));
    printf("%d\n", (((c >> 11) & 31) * 255 / 31 + ((c >> 5) & 63) * 255 / 63 + (c & 31) * 255 / 31) / 3); }
  return 0;
}
