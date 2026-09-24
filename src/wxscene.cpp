// wxscene.cpp — the animated "Cascadia" weather background (weather standby).
//
// PROVENANCE — the design comes from the in-repo export
//   stitch_animated_lvgl_weather_backgrounds/ (code.html + DESIGN.md): a
//   172x320 vector mockup with two scenes and a small keyframe vocabulary
//   ("calmed, subtle & slower"). Coordinates there are 1:1 with this glass, so
//   they are reused verbatim below. No code is copied from the export (it is
//   HTML/CSS/SVG, not firmware); every element is re-implemented as a
//   framebuffer draw for this unit.
//
//   day   — North Shore coast: 3-stop sky, sun glow, drifting mist, the Lions +
//           Grouse silhouettes, swaying conifers, sea, animated waves, and a
//           Lions Gate bridge line accent.
//   night — Burrard Inlet nocturne: darker sky, undulating aurora ribbons,
//           twinkling stars, crescent moon, pine ridge, and the bridge with
//           beacon + water reflections.
//
// Animation vocabulary ported from the export's keyframes (period in ms):
//   mistDrift 14 s / 18 s reverse  x -8..+10, opacity .25..5
//   treeSway   8 s                apex 0..-2.2 px (skewX -1.2 deg)
//   waveSurge  8 s                translateX 0..-6
//   galeStreak 7 s                (-15,-8)..(25,16), fade in/out
//   rainFall   4.2 s              y -15..+25, opacity 0..45..0
//   auroraShift 16 s              scaleY 1..1.15, y -3, opacity .45..75
//   starTwinkle 5 s               opacity .25..85 (staggered per star)
//   snowDrift  8.5 s              y -10..+35, x -4..+6, opacity 0..7..0
//
// Colours: every palette entry is run through wb565() (the cool-cast backlight
// compensation in tick.h) and then byte-swapped, because llm-tick pushes raw
// 16-bit pixels into an LGFX sprite. Channel blending is done in LOGICAL 565
// space and swapped only at write time (the same convention as ui.cpp).
#include <Arduino.h>
#include <math.h>
#include "wxscene.h"
#include "tick.h"

#define TAU_F 6.2831853f
#define PI_F  3.14159265f

static const int WXH = 320;     // panel geometry (see tick.h / board.h)
static const int SEA_Y = 196;   // horizon: sea/water starts here in the design

// ── palette ─────────────────────────────────────────────────────────────────
#define WX_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

enum {
  K_SKYD0, K_SKYD1, K_SKYD2, K_SEAD0, K_SEAD1,      // day sky / sea
  K_MTFAR, K_MTMID, K_CONIF, K_CREST, K_SUN,        // day landforms + sun
  K_MIST1, K_MIST2, K_WAVE1, K_WAVE2, K_BRIDGE,     // day mist / water / bridge
  K_SKYN0, K_SKYN1, K_SKYN2, K_SEAN0, K_SEAN1,      // night sky / water
  K_RIDGE, K_PINE, K_GLINT, K_MOON,                 // night landforms + moon
  K_STARW, K_STARC, K_STARP,                        // starlight
  K_AURC, K_AURP, K_DECK, K_BEACON,                 // aurora + bridge
  K_RAIN, K_SNOW1, K_SNOW2, K_SNOW3,                // precipitation
  K_GUST1, K_GUST2, K_GUST3, K_SHIM,                // gusts + clear-day shimmer
  K_STANDBY,
  K_N
};

static const uint16_t kBase[K_N] = {
  WX_RGB(0x0f, 0x2b, 0x38), WX_RGB(0x1b, 0x49, 0x56), WX_RGB(0x2c, 0x69, 0x75),
  WX_RGB(0x14, 0x3c, 0x47), WX_RGB(0x08, 0x1e, 0x24),
  WX_RGB(0x12, 0x36, 0x40), WX_RGB(0x0c, 0x25, 0x2d), WX_RGB(0x07, 0x19, 0x1f),
  WX_RGB(0xc0, 0xff, 0xf4), WX_RGB(0xff, 0xe0, 0x4a),
  WX_RGB(0xc0, 0xff, 0xf4), WX_RGB(0x80, 0xe5, 0xd4),
  WX_RGB(0x00, 0xff, 0xcc), WX_RGB(0xc0, 0xff, 0xf4), WX_RGB(0x00, 0xff, 0xcc),
  WX_RGB(0x05, 0x08, 0x11), WX_RGB(0x09, 0x13, 0x22), WX_RGB(0x0f, 0x22, 0x33),
  WX_RGB(0x07, 0x15, 0x20), WX_RGB(0x03, 0x07, 0x0d),
  WX_RGB(0x06, 0x0e, 0x18), WX_RGB(0x03, 0x08, 0x0e), WX_RGB(0x00, 0xff, 0xcc),
  WX_RGB(0xff, 0xe0, 0x4a),
  WX_RGB(0xff, 0xff, 0xff), WX_RGB(0x00, 0xff, 0xcc), WX_RGB(0xff, 0x2d, 0x78),
  WX_RGB(0x00, 0xff, 0xcc), WX_RGB(0xff, 0x2d, 0x78), WX_RGB(0xc0, 0xff, 0xf4),
  WX_RGB(0xff, 0x2d, 0x78),
  WX_RGB(0x00, 0xff, 0xcc), WX_RGB(0xc0, 0xff, 0xf4), WX_RGB(0x00, 0xff, 0xcc),
  WX_RGB(0xff, 0x2d, 0x78),
  WX_RGB(0xff, 0xe0, 0x4a), WX_RGB(0x00, 0xff, 0xcc), WX_RGB(0xc0, 0xff, 0xf4),
  WX_RGB(0xff, 0xe0, 0x4a),
  WX_RGB(0x18, 0x18, 0x18)
};

static uint16_t kLog[K_N];    // wb565-corrected, logical (sprite) byte order
static uint16_t kPan[K_N];    // panel-ready (byte-swapped) for raw writes

static inline uint16_t wp(int i) { return kPan[i]; }
static inline uint16_t wl(int i) { return kLog[i]; }
static inline uint16_t sw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

// ── drawing shim over the raw framebuffer ───────────────────────────────────
// The render task is the only writer and runs on one core, so file-static
// target state is safe (the UI scene in ui.cpp follows the same rule).
static uint16_t* sBuf = nullptr;
static int sW = 0, sH = 0;

static void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (!sBuf || w <= 0 || h <= 0) return;
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w, y1 = y + h;
  if (x1 > sW) x1 = sW;
  if (y1 > sH) y1 = sH;
  if (x0 >= x1 || y0 >= y1) return;
  for (int yy = y0; yy < y1; yy++) {
    uint16_t* row = sBuf + (size_t)yy * sW;
    for (int xx = x0; xx < x1; xx++) row[xx] = c;
  }
}

static inline void px(int x, int y, uint16_t c) {
  if ((unsigned)x < (unsigned)sW && (unsigned)y < (unsigned)sH)
    sBuf[(size_t)y * sW + x] = c;
}

static inline void hLine(int x, int y, int w, uint16_t c) { fillRect(x, y, w, 1, c); }
static inline void vLine(int x, int y, int h, uint16_t c) { fillRect(x, y, 1, h, c); }

static void drawLine(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    px(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Dashed variant, used for the bridge suspension cables + water reflections.
static void dashLine(int x0, int y0, int x1, int y1, uint16_t c, int on, int off) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, k = 0;
  for (;;) {
    if (k % (on + off) < on) px(x0, y0, c);
    k++;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Even-odd scanline fill for the landform silhouettes (small, near-convex).
static void fillPoly(const int16_t* xs, const int16_t* ys, int n, uint16_t c) {
  if (!sBuf || n < 3) return;
  int ymin = ys[0], ymax = ys[0];
  for (int i = 1; i < n; i++) {
    if (ys[i] < ymin) ymin = ys[i];
    if (ys[i] > ymax) ymax = ys[i];
  }
  if (ymax < 0 || ymin >= sH) return;
  if (ymin < 0) ymin = 0;
  if (ymax > sH - 1) ymax = sH - 1;
  for (int y = ymin; y <= ymax; y++) {
    int xi[20], m = 0;
    for (int i = 0; i < n && m < 19; i++) {
      int j = (i + 1 == n) ? 0 : i + 1;
      int ya = ys[i], yb = ys[j];
      if ((ya <= y && yb > y) || (yb <= y && ya > y))
        xi[m++] = xs[i] + (int)((long)(y - ya) * (xs[j] - xs[i]) / (yb - ya));
    }
    if (m < 2) continue;
    for (int a = 1; a < m; a++) {          // insertion sort, m is tiny
      int k = xi[a], b = a - 1;
      while (b >= 0 && xi[b] > k) { xi[b + 1] = xi[b]; b--; }
      xi[b + 1] = k;
    }
    for (int a = 0; a + 1 < m; a += 2) {
      int x0 = xi[a], x1 = xi[a + 1];
      if (x1 < 0 || x0 >= sW) continue;
      if (x0 < 0) x0 = 0;
      if (x1 > sW - 1) x1 = sW - 1;
      uint16_t* row = sBuf + (size_t)y * sW;
      for (int x = x0; x <= x1; x++) row[x] = c;
    }
  }
}

static void fillTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
  int16_t xs[3] = { (int16_t)x0, (int16_t)x1, (int16_t)x2 };
  int16_t ys[3] = { (int16_t)y0, (int16_t)y1, (int16_t)y2 };
  fillPoly(xs, ys, 3, c);
}

// Filled circle (used for the sun disk, the moon, and its glow rings).
static void fillCircle(int cx, int cy, int r, uint16_t c) {
  if (r <= 0) return;
  for (int dy = -r; dy <= r; dy++) {
    int y = cy + dy;
    if ((unsigned)y >= (unsigned)sH) continue;
    int w = (int)sqrtf((float)(r * r - dy * dy));
    int x0 = cx - w, x1 = cx + w;
    if (x1 < 0 || x0 >= sW) continue;
    if (x0 < 0) x0 = 0;
    if (x1 > sW - 1) x1 = sW - 1;
    uint16_t* row = sBuf + (size_t)y * sW;
    for (int x = x0; x <= x1; x++) row[x] = c;
  }
}

// ── colour helpers (logical 565 space) ──────────────────────────────────────
static inline uint16_t mix1024(uint16_t a, uint16_t b, int f) {
  if (f <= 0) return a;
  if (f >= 1024) return b;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = (ar * (1024 - f) + br * f) >> 10;
  int g = (ag * (1024 - f) + bg * f) >> 10;
  int bl = (ab * (1024 - f) + bb * f) >> 10;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Blend `col` over the pixel already in the buffer (particles, stars, glints).
static void blendPx(int x, int y, uint16_t col, int f) {
  if (f <= 0 || (unsigned)x >= (unsigned)sW || (unsigned)y >= (unsigned)sH) return;
  size_t o = (size_t)y * sW + x;
  sBuf[o] = sw(mix1024(sw(sBuf[o]), col, f));
}

// ── sky / sea ramps, rebuilt when the scene flips day<->night ───────────────
static uint16_t sSkyTab[WXH];
static uint16_t sSeaTab[WXH];
static int sSeaN = 0;
static int sTabDay = -1;      // -1 = not built

static void buildTabs(bool day) {
  uint16_t s0 = wb565(day ? kBase[K_SKYD0] : kBase[K_SKYN0]);
  uint16_t s1 = wb565(day ? kBase[K_SKYD1] : kBase[K_SKYN1]);
  uint16_t s2 = wb565(day ? kBase[K_SKYD2] : kBase[K_SKYN2]);
  int midY = (sH * (day ? 55 : 60)) / 100;
  if (midY < 1) midY = 1;
  if (midY > sH - 1) midY = sH - 1;
  for (int y = 0; y < midY; y++) sSkyTab[y] = mix1024(s0, s1, y * 1024 / midY);
  for (int y = midY; y < sH; y++)
    sSkyTab[y] = mix1024(s1, s2, (y - midY) * 1024 / (sH - 1 - midY));
  // sea / water: 2-stop ramp from the horizon to the bottom edge
  uint16_t w0 = wb565(day ? kBase[K_SEAD0] : kBase[K_SEAN0]);
  uint16_t w1 = wb565(day ? kBase[K_SEAD1] : kBase[K_SEAN1]);
  int y0 = SEA_Y < sH ? SEA_Y : sH;
  sSeaN = sH - y0;
  for (int i = 0; i < sSeaN; i++)
    sSeaTab[i] = mix1024(w0, w1, sSeaN > 1 ? i * 1024 / (sSeaN - 1) : 0);
  sTabDay = day ? 1 : 0;
}

// ── animation timing helpers ────────────────────────────────────────────────
static inline float fracf(float v) { return v - floorf(v); }
// smooth 0->1->0 ping-pong over `period` ms (the CSS `ease-in-out infinite`)
static inline float pingpong(uint32_t t, float period, float phase) {
  return 0.5f - 0.5f * cosf(TAU_F * (t / period + phase));
}
// smooth 0->1 one-way ramp (the CSS `ease-in-out` single direction)
static inline float ramp01(float p) { return 0.5f - 0.5f * cosf(PI_F * p); }
// hold `a` between lo..hi, ramping in from 0 and out to 0 at the ends
static inline float fadeWin(float p, float lo, float hi, float a) {
  if (p < lo) return a * (p / lo);
  if (p > hi) return a * ((1.0f - p) / (1.0f - hi));
  return a;
}

// ── scene elements ──────────────────────────────────────────────────────────

// The sea/water plate + its animated swell lines.
static void drawWater(bool day, uint32_t t) {
  for (int i = 0; i < sSeaN; i++) {
    uint16_t c = sw(sSeaTab[i]);
    uint16_t* row = sBuf + (size_t)(SEA_Y + i) * sW;
    for (int x = 0; x < sW; x++) row[x] = c;
  }
  // waveSurge: translateX 0 -> -6 -> 0 over 8 s
  int ox = (int)(-3.0f + 3.0f * cosf(TAU_F * t / 8000.0f));
  if (day) {
    const int wy[3] = { 212, 226, 244 };
    const uint16_t wc[3] = { K_WAVE1, K_WAVE2, K_WAVE1 };
    const int wa[3] = { 310, 260, 210 };
    for (int i = 0; i < 3; i++) {
      uint16_t c = sw(mix1024(sSeaTab[wy[i] - SEA_Y], wl(wc[i]), wa[i]));
      int y = wy[i];
      hLine(ox - 10, y, sW + 20, c);                  // drawn as a calm swell line
      hLine(ox - 10, y + 1, 60, sw(mix1024(sSeaTab[y + 1 - SEA_Y], wl(wc[i]), wa[i] / 2)));
    }
  } else {
    const int wy[3] = { 216, 234, 258 };
    const uint16_t wc[3] = { K_WAVE1, K_STARP, K_WAVE1 };
    const int wa[3] = { 256, 205, 185 };
    for (int i = 0; i < 3; i++) {
      uint16_t c = sw(mix1024(sSeaTab[wy[i] - SEA_Y], wl(wc[i]), wa[i]));
      hLine(ox - 10, wy[i], sW + 20, c);
    }
  }
}

// ── day scene ───────────────────────────────────────────────────────────────
static void drawSunGlow(uint32_t t) {
  // The design breathes the halo on the 5 s star cycle; keep it very subtle.
  float b = pingpong(t, 5000.0f, 0.0f);            // 0..1..0
  int cx = 130, cy = 80;
  const int rr[4] = { 44, 36, 28, 20 };
  const int fa[4] = { 60, 110, 170, 250 };
  for (int i = 0; i < 4; i++) {
    int f = fa[i] + (int)(30.0f * b);
    uint16_t col = wl(K_SUN);
    for (int dy = -rr[i]; dy <= rr[i]; dy++) {
      int y = cy + dy;
      if ((unsigned)y >= (unsigned)sH) continue;
      int w = (int)sqrtf((float)(rr[i] * rr[i] - dy * dy));
      for (int x = cx - w; x <= cx + w; x++) {
        if ((unsigned)x >= (unsigned)sW) continue;
        blendPx(x, y, col, f);
      }
    }
  }
  fillCircle(cx, cy, 13, sw(mix1024(sSkyTab[80], wl(K_SUN), 560)));
}

// Drifting mist: two groups, 14 s and 18 s (reverse), opacity .25..0.5.
static void drawMist(uint32_t t, int fam) {
  float g1 = 0.5f - 0.5f * cosf(TAU_F * t / 14000.0f);   // 0..1..0
  float g2 = 0.5f + 0.5f * cosf(TAU_F * t / 18000.0f);   // 1..0..1 (reverse)
  float boost = (fam == WX_CLOUD || fam == WX_FOG) ? 1.5f : 1.0f;
  struct M { int cx, cy, rx, ry; float a; uint16_t col; float dx; };
  const M m[3] = {
    {  60, 115, 55, 11, 0.18f, K_MIST1, -8.0f + 18.0f * g1 },
    { 120, 100, 45,  9, 0.20f, K_MIST1, -8.0f + 18.0f * g1 },
    {  90, 135, 58, 12, 0.15f, K_MIST2, -8.0f + 18.0f * g2 }
  };
  for (int i = 0; i < 3; i++) {
    // design: group opacity .25..0.5 alongside the ellipse's own .15..20
    float a = m[i].a * (0.25f + 0.25f * (i == 2 ? g2 : g1)) * 1.6f * boost;
    if (a > 0.45f) a = 0.45f;
    int f = (int)(a * 1024.0f);
    int cx = m[i].cx + (int)m[i].dx;
    for (int dy = -m[i].ry; dy <= m[i].ry; dy++) {
      int y = m[i].cy + dy;
      if ((unsigned)y >= (unsigned)sH) continue;
      int w = (int)(m[i].rx * sqrtf(1.0f - (float)(dy * dy) / (float)(m[i].ry * m[i].ry)));
      for (int x = cx - w; x <= cx + w; x++) {
        if ((unsigned)x >= (unsigned)sW) continue;
        blendPx(x, y, wl(m[i].col), f);
      }
    }
  }
}

// Landform silhouettes (design coordinates, 1:1 with the glass).
static const int16_t kLionsX[]  = { -10, 35, 50, 80, 95, 125, 142, 185, 185, -10 };
static const int16_t kLionsY[]  = { 145, 110, 120, 92, 106, 78, 98, 138, 210, 210 };
static const int16_t kGrouseX[] = { -10, 40, 90, 130, 185, 185, -10 };
static const int16_t kGrouseY[] = { 165, 140, 162, 135, 168, 240, 240 };
static const int16_t kRidgeX[]  = { -10, 30, 52, 84, 102, 132, 150, 185, 185, -10 };
static const int16_t kRidgeY[]  = { 148, 115, 125, 96, 112, 82, 102, 142, 212, 212 };
static const int16_t kPineX[]   = { -10, 45, 95, 138, 185, 185, -10 };
static const int16_t kPineY[]   = { 172, 148, 170, 144, 176, 240, 240 };

// Seven coastal conifers, "skewX -1.2 deg about the base" => apex drift only.
static void drawConifers(uint32_t t) {
  static const int16_t cx[7][3] = {
    { 12, 16, 20 }, { 18, 22, 26 }, { 28, 33, 38 }, { 45, 49, 53 },
    { 135, 139, 143 }, { 148, 153, 158 }, { 156, 160, 164 }
  };
  static const int16_t cy[7][3] = {
    { 192, 170, 192 }, { 194, 176, 194 }, { 198, 172, 198 }, { 190, 166, 190 },
    { 188, 168, 188 }, { 195, 172, 195 }, { 190, 174, 190 }
  };
  int dx = (int)(2.2f * sinf(TAU_F * t / 8000.0f) + 0.5f);
  for (int i = 0; i < 7; i++) {
    int16_t xs[3] = { cx[i][0], (int16_t)(cx[i][1] + dx), cx[i][2] };
    int16_t ys[3] = { cy[i][0], cy[i][1], cy[i][2] };
    fillPoly(xs, ys, 3, wp(K_CONIF));
  }
}

// Lions Gate line accent (day: a thin cyan span; night: towers + beacon +
// deck + cable + dashed reflections, cross-faded on the day/night flip).
static void drawBridgeDay() {
  uint16_t cy = wl(K_BRIDGE);
  hLine(20, 184, 51, sw(mix1024(sSkyTab[184], cy, 450)));          // deck
  vLine(45, 172, 13, sw(mix1024(sSkyTab[178], cy, 700)));          // tower
  dashLine(20, 184, 45, 172, sw(mix1024(sSkyTab[178], cy, 500)), 2, 2);
  dashLine(70, 184, 45, 172, sw(mix1024(sSkyTab[178], cy, 500)), 2, 2);
}

static void drawBridgeNight(uint32_t t) {
  uint16_t cy = wl(K_BRIDGE);
  vLine(42, 168, 19, sw(mix1024(sSkyTab[177], cy, 1000)));
  vLine(44, 168, 19, sw(mix1024(sSkyTab[177], cy, 620)));
  // beacon: rides the 5 s twinkle
  float b = 0.25f + 0.60f * pingpong(t, 5000.0f, 0.0f);
  int f = (int)(b * 1024.0f);
  for (int dy = -1; dy <= 1; dy++)
    for (int x = 42; x <= 44; x++) blendPx(x, 167 + dy, wl(K_BEACON), f);
  hLine(15, 184, 61, sw(mix1024(sSkyTab[184], wl(K_DECK), 900)));  // road deck
  drawLine(15, 176, 43, 182, sw(mix1024(sSkyTab[178], cy, 500)));  // suspension cable
  drawLine(43, 182, 75, 176, sw(mix1024(sSkyTab[178], cy, 500)));
  dashLine(43, 198, 43, 245, sw(mix1024(sw(sBuf[(size_t)220 * sW + 43]), cy, 400)), 3, 4);
  dashLine(28, 202, 28, 230, sw(mix1024(sw(sBuf[(size_t)215 * sW + 28]), wl(K_STARP), 300)), 2, 3);
}

// ── night scene ─────────────────────────────────────────────────────────────

static int16_t sEnvTab[65];     // sin(pi*u) * 1024 — the aurora's vertical envelope

// Aurora ribbons: top edge is the design's quadratic curve, the body fades
// vertically; auroraShift scales/raises it and rides the opacity .45..0.75.
static float quadTopY(int ribbon, float x) {
  float x0, y0, xc, yc, x1, y1, xc2, yc2, x3, y3;
  if (ribbon == 0) {
    x0 = -10; y0 = 65; xc = 40; yc = 44; x1 = 90; y1 = 68;
    xc2 = 140; yc2 = 92; x3 = 190; y3 = 48;
  } else {
    x0 = -10; y0 = 82; xc = 50; yc = 64; x1 = 110; y1 = 84;
    xc2 = 170; yc2 = 104; x3 = 190; y3 = 72;
  }
  float t, u;
  if (x <= x1) {
    t = (x - x0) / (x1 - x0);
    u = 1.0f - t;
    return u * u * y0 + 2.0f * u * t * yc + t * t * y1;
  }
  t = (x - x1) / (x3 - x1);
  u = 1.0f - t;
  return u * u * y1 + 2.0f * u * t * yc2 + t * t * y3;
}

static void drawAurora(uint32_t t, int ribbon) {
  // 16 s cycle; the second ribbon is delayed 5 s and dimmer (design: .45 vs .65)
  float ph = (ribbon == 0) ? 0.0f : (5.0f / 16.0f);
  float p = pingpong(t, 16000.0f, ph);
  float anim = (ribbon == 0 ? 0.65f : 0.45f) * (0.45f + 0.30f * p);
  float yShift = -3.0f * p;
  float ext = 8.0f * p;                                  // scaleY 1 -> 1.15
  int yBot = (int)((ribbon == 0 ? 120.0f : 130.0f) + ext);
  uint16_t cyan = wl(K_AURC), pink = wl(K_AURP);
  int hue = (int)(220.0f * p);                           // hue-rotate stand-in
  for (int x = 0; x < sW; x++) {
    float tt = (float)x / (float)sW;
    int top = (int)(quadTopY(ribbon, (float)x) + yShift);
    int y0 = top < 0 ? 0 : top;
    int y1 = yBot > sH - 1 ? sH - 1 : yBot;
    if (y1 - y0 < 3) continue;
    int span = y1 - y0;
    // horizontal envelope: 0 at the left edge, peak through the middle
    float aX = 0.20f + 0.12f * sinf(PI_F * tt);
    int ct = (int)(tt * 900.0f) + hue;
    if (ct > 1024) ct = 1024;
    uint16_t col = mix1024(cyan, pink, ct);
    for (int y = y0; y <= y1; y++) {
      int e = sEnvTab[(y - y0) * 64 / span];
      if (e < 40) continue;
      int f = (int)((float)e * aX * anim);
      if (f < 24) continue;
      px(x, y, sw(mix1024(sSkyTab[y], col, f)));
    }
  }
}

// Twinkling fixed stars (design: 6 white, 5 s, staggered delays).
static void drawStars(uint32_t t) {
  static const int16_t sx[6] = { 35, 75, 115, 155, 20, 85 };
  static const int16_t sy[6] = { 30, 22, 35, 28, 55, 65 };
  static const int16_t sd[6] = { 0, 1800, 3200, 1000, 2600, 4200 };
  for (int i = 0; i < 6; i++) {
    float a = 0.25f + 0.60f * pingpong(t + sd[i], 5000.0f, 0.0f);
    int f = (int)(a * 1024.0f);
    blendPx(sx[i], sy[i], wl(K_STARW), f);
    if (i == 3) blendPx(sx[i] + 1, sy[i], wl(K_STARW), f / 2);   // widest star
  }
}

// Extra starlight for the clear-night preset (aurora "geomagnetic" stars).
static void drawColorStars(uint32_t t) {
  static const int16_t sx[4] = { 45, 105, 135, 70 };
  static const int16_t sy[4] = { 40, 35, 65, 90 };
  static const int16_t sd[4] = { 0, 1800, 3500, 2500 };
  static const uint16_t sc[4] = { K_STARC, K_STARC, K_STARP, K_STARC };
  static const int16_t sa[4] = { 665, 614, 563, 512 };
  for (int i = 0; i < 4; i++) {
    float a = 0.25f + 0.60f * pingpong(t + sd[i], 5000.0f, 0.0f);
    blendPx(sx[i], sy[i], wl(sc[i]), (int)(a * sa[i]));
    blendPx(sx[i], sy[i] - 1, wl(sc[i]), (int)(a * sa[i] / 3));
  }
}

// Waxing crescent over the coastal range: a lit disc carved by a sky-coloured
// mask disc, so the terminator matches the gradient behind it exactly.
// The design places it at (141,57), but the condition icon box (x>=110, y>=62)
// lives there — shifted ~49 px left so the halo clears the icon.
static void drawMoon(uint32_t t) {
  float b = pingpong(t, 5000.0f, 0.0f);
  const int cx = 92, cy = 56;
  for (int i = 0; i < 2; i++) {
    int r = i == 0 ? 20 : 16;
    int f = (int)((i == 0 ? 70 : 110) + 40.0f * b);
    for (int dy = -r; dy <= r; dy++) {
      int y = cy + dy;
      if ((unsigned)y >= (unsigned)sH) continue;
      int w = (int)sqrtf((float)(r * r - dy * dy));
      for (int x = cx - w; x <= cx + w; x++) {
        if ((unsigned)x >= (unsigned)sW) continue;
        blendPx(x, y, wl(K_MOON), f);
      }
    }
  }
  fillCircle(cx, cy, 11, sw(mix1024(sSkyTab[cy], wl(K_MOON), 870)));
  fillCircle(cx + 5, cy - 3, 12, sw(sSkyTab[cy]));   // carve the crescent
}

// ── particles ───────────────────────────────────────────────────────────────

// rainFall 4.2 s, opacity 0 -> .45 -> 0 (denser under a storm)
static void drawRain(uint32_t t, bool storm) {
  static const int16_t rx[5] = { 35, 85, 130, 55, 150 };
  static const int16_t ry[5] = { 35, 20, 45, 110, 90 };
  static const int16_t bx[3] = { 10, 100, 160 };
  static const int16_t by[3] = { 70, 150, 130 };
  int n = storm ? 8 : 5;
  for (int i = 0; i < n; i++) {
    float p = fracf(t / 4200.0f + i * 0.17f);
    float e = ramp01(p);
    int x = (i < 5) ? rx[i] : bx[i - 5];
    int y = (i < 5) ? ry[i] : by[i - 5];
    int yo = (int)(-15.0f + 40.0f * e);
    int f = (int)(fadeWin(p, 0.25f, 0.75f, 0.45f) * 1024.0f);
    if (f <= 0) continue;
    int yy = y + yo;
    for (int k = 0; k < 13; k++)
      blendPx(x - (k * 2) / 13, yy - 6 + k, wl(K_RAIN), f);
  }
}

// snowDrift 8.5 s: y -10..+35, x -4..+6, rotate, opacity 0 -> .7 -> 0
static void drawSnow(uint32_t t, bool day) {
  static const int16_t sx[4] = { 45, 115, 75, 105 };
  static const int16_t sy[4] = { 45, 65, 125, 85 };
  static const float   sd[4] = { 0.0f, 3.5f, 5.5f, 1.8f };
  static const uint16_t sc[4] = { K_SNOW1, K_SNOW2, K_SNOW1, K_SNOW3 };
  int n = day ? 3 : 4;
  for (int i = 0; i < n; i++) {
    float p = fracf(t / 8500.0f + sd[i] / 8.5f);
    float e = ramp01(p);
    int x = sx[i] + (int)(-4.0f + 10.0f * e);
    int y = sy[i] + (int)(-10.0f + 45.0f * e);
    int f = (int)(fadeWin(p, 0.25f, 0.75f, 0.70f) * 1024.0f);
    if (f <= 0) continue;
    uint16_t c = wl(sc[i]);
    blendPx(x, y, c, f);
    for (int k = 1; k <= 3; k++) {
      blendPx(x, y - k, c, f * 3 / 4);
      blendPx(x, y + k, c, f * 3 / 4);
      blendPx(x - k, y, c, f * 3 / 4);
      blendPx(x + k, y, c, f * 3 / 4);
    }
    blendPx(x - 2, y - 2, c, f / 2);
    blendPx(x + 2, y + 2, c, f / 2);
  }
}

// galeStreak 7 s: translate (-15,-8) -> (25,16), opacity 0 -> .5 -> 0
static void drawGusts(uint32_t t) {
  static const int16_t gx0[3] = { 20, 75, 30 };
  static const int16_t gy0[3] = { 45, 85, 125 };
  static const int16_t gx1[3] = { 55, 120, 70 };
  static const int16_t gy1[3] = { 65, 108, 148 };
  static const uint16_t gc[3] = { K_GUST1, K_GUST2, K_GUST3 };
  static const int16_t ga[3] = { 450, 400, 350 };
  for (int i = 0; i < 3; i++) {
    float p = fracf(t / 7000.0f + i * 0.33f);
    float e = ramp01(p);
    int ox = (int)(-15.0f + 40.0f * e), oy = (int)(-8.0f + 24.0f * e);
    int f = (int)(fadeWin(p, 0.30f, 0.70f, 0.50f) * ga[i]);
    if (f <= 0) continue;
    for (int k = 0; k <= 40; k++) {
      int x = gx0[i] + (gx1[i] - gx0[i]) * k / 40 + ox;
      int y = gy0[i] + (gy1[i] - gy0[i]) * k / 40 + oy;
      blendPx(x, y, wl(gc[i]), f);
    }
  }
}

// Clear-day shimmer (the design's "calm golden sunbreak" sparkles).
static void drawShimmer(uint32_t t) {
  static const int16_t sx[3] = { 50, 120, 70 };
  static const int16_t sy[3] = { 50, 70, 115 };
  static const int16_t sd[3] = { 0, 2200, 4000 };
  static const uint16_t sc[3] = { K_SHIM, K_STARC, K_SHIM };
  static const int16_t sa[3] = { 614, 563, 512 };
  for (int i = 0; i < 3; i++) {
    float a = 0.25f + 0.60f * pingpong(t + sd[i], 5000.0f, 0.0f);
    int f = (int)(a * sa[i]);
    blendPx(sx[i], sy[i], wl(sc[i]), f);
    blendPx(sx[i] + 1, sy[i], wl(sc[i]), f / 2);
    blendPx(sx[i], sy[i] + 1, wl(sc[i]), f / 2);
  }
}

// ── row colour sampling (text clips to the local scene colour) ──────────────
static uint16_t sRowCol[WXH];

static void sampleRows() {
  int x = sW > 2 ? 2 : 0;
  for (int y = 0; y < sH; y++) sRowCol[y] = sw(sBuf[(size_t)y * sW + x]);
}

uint16_t wxRowColor(int y) {
  if (y < 0) y = 0;
  if (y > WXH - 1) y = WXH - 1;
  return sRowCol[y];
}

// ── public API ──────────────────────────────────────────────────────────────
void wxSceneInit() {
  for (int i = 0; i < K_N; i++) {
    kLog[i] = wb565(kBase[i]);
    kPan[i] = sw(kLog[i]);
  }
  for (int i = 0; i <= 64; i++)
    sEnvTab[i] = (int16_t)(1024.0f * sinf(PI_F * (float)i / 64.0f));
  sTabDay = -1;
}

void wxSceneRender(uint16_t* buf, int w, int h, int fam, bool day, bool valid) {
  sBuf = buf; sW = w; sH = h;
  if (!valid) {
    // No reading: the flat standby canvas (what this page showed before the
    // animated scene existed), so "WEATHER: N/A" still reads cleanly.
    uint16_t c = wp(K_STANDBY);
    for (int y = 0; y < h; y++) {
      uint16_t* row = buf + (size_t)y * w;
      for (int x = 0; x < w; x++) row[x] = c;
    }
    sampleRows();
    return;
  }
  if (sTabDay != (day ? 1 : 0)) buildTabs(day);
  uint32_t t = millis();

  // sky plate
  for (int y = 0; y < h; y++) {
    uint16_t c = sw(sSkyTab[y]);
    uint16_t* row = buf + (size_t)y * w;
    for (int x = 0; x < w; x++) row[x] = c;
  }

  // wxFamily() maps a clear sky to SUN by day and MOON by night (ui.cpp).
  bool clearNight = (fam == WX_MOON || fam == WX_SUN || fam == WX_PARTLY_N);
  bool precip     = (fam == WX_RAIN || fam == WX_STORM || fam == WX_SNOW);

  if (day) {
    drawSunGlow(t);
    drawMist(t, fam);
    fillPoly(kLionsX, kLionsY, 10, wp(K_MTFAR));
    {   // Lions glacier crests
      uint16_t c = wp(K_CREST);
      fillTri(80, 92, 74, 102, 86, 102, sw(mix1024(wl(K_MTFAR), c, 660)));
      fillTri(125, 78, 118, 90, 132, 90, sw(mix1024(wl(K_MTFAR), c, 760)));
    }
    fillPoly(kGrouseX, kGrouseY, 7, wp(K_MTMID));
    drawConifers(t);
    drawWater(true, t);
    drawBridgeDay();
    if (fam == WX_SUN || fam == WX_PARTLY_D) drawShimmer(t);
    if (fam == WX_RAIN || fam == WX_STORM) drawRain(t, fam == WX_STORM);
    if (fam == WX_SNOW) drawSnow(t, true);
    if (fam == WX_STORM) drawGusts(t);
  } else {
    drawAurora(t, 0);
    if (clearNight) drawAurora(t, 1);
    drawStars(t);
    if (fam == WX_MOON) drawColorStars(t);
    drawMoon(t);
    fillPoly(kRidgeX, kRidgeY, 10, wp(K_RIDGE));
    {   // summit glints under starlight
      uint16_t c = wp(K_GLINT);
      fillTri(84, 96, 79, 104, 89, 104, sw(mix1024(wl(K_RIDGE), c, 450)));
      fillTri(132, 82, 126, 92, 138, 92, sw(mix1024(wl(K_RIDGE), c, 550)));
    }
    fillPoly(kPineX, kPineY, 7, wp(K_PINE));
    drawWater(false, t);
    drawBridgeNight(t);
    if (precip && fam != WX_SNOW) drawRain(t, fam == WX_STORM);
    if (fam == WX_SNOW) drawSnow(t, false);
    if (fam == WX_STORM) drawGusts(t);
  }
  sampleRows();
}
