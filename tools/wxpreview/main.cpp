// Offline preview driver: renders one wxScene frame to a PPM (and prints the
// per-row colour of the panel rows) so the background can be inspected without
// flashing the board.  Usage:  wxpreview <fam> <day> <millis> <out.ppm>
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

  // Row survey: mean of each row plus the min/max (structure detector).
  printf("row  rgb-mean          min   max   spread\n");
  for (int y = 0; y < H; y += (y < 190 ? 16 : 4)) {
    long sr = 0, sg = 0, sb = 0; int lo = 999, hi = -1;
    for (int x = 0; x < W; x++) {
      uint16_t c = (uint16_t)((buf[(size_t)y * W + x] >> 8) | (buf[(size_t)y * W + x] << 8));
      int r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
      sr += r; sg += g; sb += b;
      int l = (r + g + b) / 3; if (l < lo) lo = l; if (l > hi) hi = l;
    }
    printf("%3d  (%3ld,%3ld,%3ld)   %4d  %4d  %4d\n", y, sr / W, sg / W, sb / W, lo, hi, hi - lo);
  }
  return 0;
}
