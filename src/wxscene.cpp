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
  K_HULL, K_SUPER, K_WIN, K_CITY, K_SAIL,           // variant ink: ships + city
  K_PLANE, K_ORCA, K_PETAL, K_GRASS, K_STONE,       // variants: plane/orca/blossom
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
  // Night water (2026-09-25): lifted out of the black. At 0x071520 -> 0x03070d
  // the ramp died to pure (0,0,0) around row 290, so the bottom fifth of the
  // glass was an empty black band with nothing in it — the "chin" the user
  // reported. wb565 halves the blue channel, so the values stay blue-heavy to
  // land as a dim inlet teal rather than a grey.
  WX_RGB(0x0c, 0x28, 0x38), WX_RGB(0x08, 0x19, 0x2a),
  WX_RGB(0x06, 0x0e, 0x18), WX_RGB(0x03, 0x08, 0x0e), WX_RGB(0x00, 0xff, 0xcc),
  WX_RGB(0xff, 0xe0, 0x4a),
  WX_RGB(0xff, 0xff, 0xff), WX_RGB(0x00, 0xff, 0xcc), WX_RGB(0xff, 0x2d, 0x78),
  WX_RGB(0x00, 0xff, 0xcc), WX_RGB(0xff, 0x2d, 0x78), WX_RGB(0xc0, 0xff, 0xf4),
  WX_RGB(0xff, 0x2d, 0x78),
  WX_RGB(0x00, 0xff, 0xcc), WX_RGB(0xc0, 0xff, 0xf4), WX_RGB(0x00, 0xff, 0xcc),
  WX_RGB(0xff, 0x2d, 0x78),
  WX_RGB(0xff, 0xe0, 0x4a), WX_RGB(0x00, 0xff, 0xcc), WX_RGB(0xc0, 0xff, 0xf4),
  WX_RGB(0xff, 0xe0, 0x4a),
  // Variant ink (2026-09-25). These still go through wb565, which halves the
  // blue channel, so the "white" sails and the plane land as a warm cream and
  // the blossom needed its blue driven right up (0xffacff) to come back as pink
  // (255,149,156) instead of orange.
  WX_RGB(0x10, 0x18, 0x24), WX_RGB(0x3a, 0x46, 0x58), WX_RGB(0xff, 0xd8, 0x88),
  WX_RGB(0x18, 0x24, 0x32), WX_RGB(0xe6, 0xf4, 0xff),
  WX_RGB(0xd8, 0xe8, 0xf4), WX_RGB(0x0a, 0x10, 0x1c), WX_RGB(0xff, 0xac, 0xff),
  WX_RGB(0x1c, 0x38, 0x24), WX_RGB(0x4a, 0x54, 0x60),
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

// One swell line: the crest height undulates across the width, so the water
// reads as water instead of a set of straight rules. (2026-09-25, user report:
// "海浪显示成直线" — every swell used to be a single hLine at a fixed y.)
static void swellLine(int yBase, int amp, float fr, float ph, int thick,
                      int alpha, int colIdx) {
  for (int x = 0; x < sW; x++) {
    int y = yBase + (int)(amp * sinf(fr * (float)x + ph) + 0.5f);
    int si = y - SEA_Y;
    if (si < 0) si = 0;
    if (si >= sSeaN) si = sSeaN - 1;
    fillRect(x, y, 1, thick, sw(mix1024(sSeaTab[si], wl(colIdx), alpha)));
  }
}

// A dashed vertical reflection that mixes over whatever the water already put
// in the buffer (so it rides the swells instead of painting over them) and fades
// with depth. Used to carry the bridge + the sun/moon light all the way down to
// the chin.
static void reflLine(int x, int y0, int y1, uint16_t col, int a0, int a1) {
  int span = (y1 > y0) ? (y1 - y0) : 1;
  for (int y = y0; y <= y1; y++) {
    if (((y - y0) / 3) & 1) continue;              // 3 on / 3 off
    blendPx(x, y, col, a0 + (a1 - a0) * (y - y0) / span);
  }
}

// The broken light path the sun (day) / moon (night) leaves on the inlet: a
// wobbly dashed column reaching the near water. Its dashes wander with the
// swell so the bottom of the glass carries the sky's light rather than fading
// out to nothing (2026-09-25, user report: "屏幕下巴的像素的空的").
static void drawLightPath(int cx, bool day, uint32_t t) {
  uint16_t col = day ? wl(K_WAVE2) : wl(K_MOON);
  float ph = TAU_F * t / 9000.0f;
  for (int y = 200; y <= 302; y++) {
    if (((y - 200) / 5) & 1) continue;             // 5 on / 5 off
    int a = 300 - (y - 200) * 2;                   // fades with distance
    if (a <= 0) continue;
    int x = cx + (int)(2.4f * sinf(ph + (float)y * 0.19f) + 0.5f);
    int wd = 1 + (y - 200) / 44;                   // nearer dashes spread wider
    for (int dx = 0; dx < wd; dx++) blendPx(x + dx, y, col, a);
  }
}

// One swell as a wavy-EDGED BAND: fill from the crest line down to the bottom of
// the glass. Bands are painted far-to-near, so each nearer band simply covers
// the farther ones and the visible edges are the crest lines. This is the
// design's "subtle horizontal banding", and because the edge is a per-x sine it
// can never read as a ruled line. (2026-09-25, user report:
// "海浪显示成直线" — the sea was three full-width hLines at fixed y.)
// `tab` is the band's already-mixed colour ramp (one entry per sea row): the
// whole inlet is ~80k pixels, so the mix has to happen per band, not per pixel.
static uint16_t sBandTab[WXH];
static void swellBand(int yBase, int amp, float fr, float ph, const uint16_t* tab) {
  for (int x = 0; x < sW; x++) {
    int y0 = yBase + (int)(amp * sinf(fr * (float)x + ph) + 0.5f);
    if (y0 < SEA_Y) y0 = SEA_Y;
    if (y0 >= sH) continue;
    uint16_t* p = sBuf + (size_t)y0 * sW + x;
    const uint16_t* tp = tab + (y0 - SEA_Y);
    for (int y = y0; y < sH; y++) { *p = *tp++; p += sW; }
  }
}

// The sea/water plate + its animated swells.
static void drawWater(bool day, uint32_t t) {
  for (int i = 0; i < sSeaN; i++) {
    uint16_t c = sw(sSeaTab[i]);
    uint16_t* row = sBuf + (size_t)(SEA_Y + i) * sW;
    for (int x = 0; x < sW; x++) row[x] = c;
  }
  // waveSurge: translateX 0 -> -6 -> 0 over 8 s
  int ox = (int)(-3.0f + 3.0f * cosf(TAU_F * t / 8000.0f));
  float ph = TAU_F * t / 11000.0f;
  // Seven swells from the horizon to the chin: the pitch widens with depth
  // (perspective) and every crest owns its own wavelength + phase, so no two
  // line up into a rule. The last crest sits ~10 px above row 319, so the bottom
  // edge of the glass is water. (2026-09-25, user report:
  // "屏幕下巴的像素的空的" — rows ~290..319 used to render as pure black.)
  const int   sy[7] = {  206,  219,  234,  251,  270,  290,  308 };
  const int   sa[7] = {    3,    4,    5,    6,    7,    8,    9 };
  const float sf[7] = { 0.062f, -0.048f, 0.040f, -0.055f, 0.034f, -0.043f, 0.030f };
  const float sp[7] = { 0.0f, 1.9f, 3.4f, 0.7f, 2.6f, 4.8f, 1.2f };
  uint16_t wave = wl(K_WAVE1);
  for (int i = 0; i < 7; i++) {
    // The band fill stays dark: wb565 halves the blue channel, so a cyan mix
    // lands green and a strong mix turns the whole inlet into paint. Only the
    // 1-px crest carries the full neon colour; the pink accent stays on the
    // bridge's dashed reflection, where it belongs to a light source.
    int a = (day ? 90 : 45) + i * 22;
    for (int k = 0; k < sSeaN; k++) sBandTab[k] = sw(mix1024(sSeaTab[k], wave, a));
    float cph = sp[i] + ph - sf[i] * (float)ox;   // -sf*ox is the surge
    swellBand(sy[i], sa[i], sf[i], cph, sBandTab);
    swellLine(sy[i], sa[i], sf[i], cph, i >= 4 ? 2 : 1, 300 + i * 24, K_WAVE1);
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
  // ...and both reflections run on to the near water (design "PORT 02" shows
  // them reaching the bottom of frame).
  reflLine(43, 246, 306, cy, 300, 90);
  reflLine(28, 232, 288, wl(K_STARP), 240, 70);
}

// ── Vancouver background variants (备选, 2026-09-25) ─────────────────────────
// Five alternates to the original Lions Gate composition. Each keeps the shared
// machinery — sky ramp, mist, aurora, stars, moon, water, weather particles —
// and swaps the landform line-up plus one signature Vancouver subject:
//
//   WXBG_ANCHOR    bulk carriers riding at anchor across English Bay, the Point
//                  Atkinson light on the point, a tug working the near lane
//   WXBG_SKYLINE   downtown's tower bar with a scatter of lit windows, Canada
//                  Place's five sails in front of it, the two Lions behind
//   WXBG_SEAPLANE  a Harbour Air float plane taxiing out of Coal Harbour with
//                  spray and wake, two more on the dock, gulls
//   WXBG_ORCA      a resident orca surfacing in the inlet — the bull's dorsal
//                  fin, saddle and eye patch, splash along the waterline
//   WXBG_BLOSSOM   the Stanley Park seawall in April: stone cap and rail, park
//                  path, two cherry canopies, petals on the breeze
//
// One rule the now-transparent text layer imposes (see ui.cpp): anything that
// crosses a caption band — weather page y 72..92, 140..152, 180..190, 200..208,
// 232..248, 254..262 — stays a DARK silhouette. Only thin bright accents are
// allowed there; a big bright shape would fight the white type that shows
// through to the scene behind it.
int g_wxBg = WXBG_LIONS;

int wxBgCount() { return WXBG_COUNT; }

const char* wxBgName(int bg) {
  static const char* n[WXBG_COUNT] = {
    "lions-gate", "anchorage", "skyline", "seaplane", "orca", "blossom"
  };
  return n[(bg < 0 || bg >= WXBG_COUNT) ? 0 : bg];
}

// Three gulls working the tide rip (two brush strokes each). Shared by the
// anchorage, seaplane and orca variants — a bit of life in the middle distance.
static void drawGulls(uint32_t t, int n) {
  static const int16_t gx[3] = { 34, 120, 74 };
  static const int16_t gy[3] = { 62, 50, 98 };
  static const float   gd[3] = { 0.0f, 0.37f, 0.71f };
  uint16_t c = wl(K_STARW);
  if (n > 3) n = 3;
  for (int i = 0; i < n; i++) {
    float p = fracf(t / 9000.0f + gd[i]);
    float e = ramp01(p);
    int x = gx[i] + (int)(-9.0f + 18.0f * e);
    int y = gy[i] + (int)(3.0f * sinf(TAU_F * p * 2.0f));
    int f = (int)(fadeWin(p, 0.20f, 0.80f, 0.50f) * 1024.0f);
    if (f <= 0) continue;
    blendPx(x - 2, y, c, f / 2);
    blendPx(x - 1, y - 1, c, f);
    blendPx(x, y, c, f);
    blendPx(x + 1, y - 1, c, f);
    blendPx(x + 2, y, c, f / 2);
  }
}

// ── variant 1: anchorage ────────────────────────────────────────────────────

// One ship at anchor: dark hull, red boot-top, a lighter house aft, short mast.
// `s` is the hull length in px; the slow bob is per-ship so they never sync.
static void drawFreighter(int x, int y, int s, uint32_t t) {
  int bob = (int)(0.9f * sinf(TAU_F * t / 7400.0f + (float)x * 0.07f) + 0.5f);
  y += bob;
  int hh = 2 + s / 11;                                            // hull depth
  // The inlet is dark at BOTH times of day (day ~lum 32, night ~lum 25), so a
  // near-black hull just vanished into the water. The hull is now a mid grey and
  // only the small superstructure carries any light.
  fillRect(x, y, s, hh, wp(K_SUPER));
  hLine(x, y + hh, s, sw(mix1024(wl(K_SUPER), wl(K_STARP), 700))); // boot-top
  int hw = 3 + s / 5;                                              // house, aft
  uint16_t sup = sw(mix1024(wl(K_SUPER), wl(K_SAIL), 420));
  fillRect(x + s - hw - 1, y - hh, hw, hh, sup);
  fillRect(x + s - hw - 3, y - hh - 3, 3, 4, sup);                 // funnel
  hLine(x + 2, y - 1, s / 2, sw(mix1024(wl(K_SUPER), wl(K_SAIL), 250)));
  dashLine(x + s / 3, y, x + s / 3, y - 6, wp(K_SUPER), 1, 1);     // fore mast
  px(x + s - hw, y - hh + 1, wp(K_WIN));                           // one lit window
}

// Point Atkinson light: white tower, red cap, a lamp on the 5 s twinkle.
static void drawLighthouse(int x, int y, uint32_t t) {
  fillRect(x - 2, y - 8, 5, 8, sw(mix1024(wl(K_CITY), wl(K_SAIL), 700)));
  fillRect(x - 3, y - 10, 7, 2, sw(mix1024(wl(K_SAIL), wl(K_STARP), 800)));
  float b = 0.25f + 0.60f * pingpong(t, 5000.0f, 0.0f);
  blendPx(x, y - 11, wl(K_BEACON), (int)(b * 1024.0f));
}

// A tug crossing the near lane on a ~20 s loop, trailing its wake.
static void drawTug(uint32_t t) {
  int x = -46 + (int)fmodf((float)t * 0.0116f, 232.0f);
  if (x > sW + 10 || x < -46) return;
  int y = 236;
  fillRect(x, y, 22, 4, wp(K_HULL));
  fillRect(x + 12, y - 6, 8, 6, wp(K_SUPER));
  vLine(x + 16, y - 10, 4, wp(K_SUPER));
  px(x + 16, y - 11, wp(K_WIN));
  hLine(x + 1, y + 4, 20, sw(mix1024(wl(K_HULL), wl(K_STARP), 420)));
  // wake: a fading V astern, mixed off the water colour already on the glass
  int si = y + 4 - SEA_Y;
  if (si < 0) si = 0;
  if (si >= sSeaN) si = sSeaN - 1;
  uint16_t wk = sw(mix1024(sSeaTab[si], wl(K_STARW), 300));
  for (int i = 1; i < 22; i++) {
    int xx = x - i;
    px(xx, y + 4 + i / 5, wk);
    px(xx, y + 6 + i / 4, wk);
  }
}

static void drawAnchorage(bool day, uint32_t t) {
  (void)day;
  drawLighthouse(157, 196, t);
  // Bulk carriers across the bay, far to near. The hulls are mid-grey so they
  // read against the dark inlet; the type is white and still wins on top.
  drawFreighter(14, 192, 34, t);
  drawFreighter(104, 195, 46, t);
  drawFreighter(36, 207, 62, t);
  drawGulls(t, 3);
  drawTug(t);
}

// ── variant 2: downtown skyline + Canada Place ──────────────────────────────

// Downtown across the water: a dozen towers with a scatter of lit windows. By
// day the glass is a mid-tone so the bar reads against the mountains; at night
// the bar goes near-black and only the windows are lit. Either way the brightest
// thing here is a 2x2 window block and there are never many in one row, so the
// white type (white ink sits straight on the scene — see ui.cpp) always wins.
static void drawCityBar(int x0, int x1, int baseY, bool day, uint32_t t) {
  static const uint8_t tw[12] = { 12, 16,  9, 14, 11, 18, 10, 13,  8, 15, 12, 10 };
  static const uint8_t th[12] = { 30, 44, 22, 52, 34, 26, 46, 38, 24, 40, 28, 36 };
  uint16_t face = day ? sw(mix1024(wl(K_CITY), wl(K_SAIL), 190)) : wp(K_CITY);
  int16_t tx[12], ty[12]; uint8_t tww[12], thh[12]; int n = 0;
  int x = x0;
  for (int i = 0; i < 12 && x < x1; i++) {
    int wdt = tw[i];
    if (x + wdt > x1) wdt = x1 - x;
    if (wdt < 4) break;
    int hgt = th[i];
    if (hgt > baseY) hgt = baseY;
    int top = baseY - hgt;
    fillRect(x, top, wdt, hgt, face);
    if ((i & 3) == 1)                 // a sloped crown
      fillTri(x, top, x + wdt, top, x + wdt / 2, top - 6, face);
    if ((i & 3) == 3)                 // a roof mast
      vLine(x + wdt / 2, top - 9, 9, face);
    hLine(x, top, wdt, sw(mix1024(wl(K_CITY), wl(K_SAIL), day ? 330 : 150)));
    tx[n] = (int16_t)x; ty[n] = (int16_t)top; tww[n] = (uint8_t)wdt; thh[n] = (uint8_t)hgt;
    n++;
    x += wdt + 1 + (i & 1);
  }
  if (day) return;                    // by day it is plain dark glass
  // Night: 2x2 window blocks on a fixed lattice, always inside a tower.
  float tk = 0.5f + 0.5f * sinf(TAU_F * t / 9000.0f);
  uint16_t lit = sw(mix1024(wl(K_CITY), wl(K_WIN), (int)((0.28f + 0.26f * tk) * 1024.0f)));
  for (int i = 0; i < n; i++) {
    for (int yy = ty[i] + 4; yy < ty[i] + thh[i] - 4; yy += 5) {
      for (int xx = tx[i] + 2; xx < tx[i] + tww[i] - 3; xx += 4) {
        if (((xx * 5 + yy * 3) % 7) != 0) continue;
        px(xx, yy, lit);
        if (tww[i] >= 11) px(xx + 1, yy, lit);
      }
    }
  }
}

// Canada Place: five sails on a low pier — the postcard Vancouver waterfront.
// The sails are a DARK silhouette with one bright leading edge each: filled
// cream they were five bright blobs sitting right in the H/L/RH row (y 180..190).
static void drawCanadaPlace(int x, int baseY, bool day) {
  fillRect(x - 2, baseY - 3, 64, 3, wp(K_CITY));                  // the pier
  uint16_t fill = sw(mix1024(wl(K_CITY), wl(K_SAIL), day ? 260 : 200));
  uint16_t edge = sw(mix1024(wl(K_CITY), wl(K_SAIL), 620));
  for (int i = 0; i < 5; i++) {
    int sx = x + i * 12;
    int hgt = 10 + ((i == 2) ? 6 : (i & 1) ? 1 : 4);
    fillTri(sx, baseY - 3, sx + 11, baseY - 3, sx + 6, baseY - 3 - hgt, fill);
    drawLine(sx + 1, baseY - 4, sx + 6, baseY - 3 - hgt, edge);
  }
}

static void drawSkylineBg(bool day, uint32_t t) {
  drawCityBar(4, 168, 190, day, t);
  drawCanadaPlace(72, 197, day);
  drawGulls(t, 2);
}

// ── variant 3: Harbour Air float plane ──────────────────────────────────────

// A float plane from the side: fuselage, high wing, tail fin, two floats, a red
// fin flash. `s` is the fuselage length.
static void drawFloatPlane(int x, int y, int s, bool day) {
  // Off-white, not white: the type sits straight on the scene, so the body is
  // knocked down to a mid cream and only the edges stay crisp.
  uint16_t body = sw(mix1024(wl(K_PLANE), wl(K_HULL), 450));
  uint16_t hull = wp(K_HULL);
  int fy = y;                                                     // centre line
  fillRect(x, fy - 2, s, 4, body);                                // fuselage
  fillTri(x + s, fy - 2, x + s, fy + 1, x + s + 4, fy - 1, body); // nose cone
  fillRect(x + s - 4, fy - 7, 3, 5, body);                        // fin
  fillRect(x + s - 9, fy - 4, 5, 2, body);                        // tailplane
  fillRect(x + s / 3, fy - 7, s / 2, 2, body);                    // high wing
  fillRect(x + s / 4, fy - 5, 2, 3, hull);                        // wing struts
  fillRect(x + s / 2, fy - 5, 2, 3, hull);
  fillRect(x - 1, fy + 4, s + 3, 2, hull);                        // the floats
  fillRect(x + 2, fy + 2, 2, 2, hull);
  fillRect(x + s - 6, fy + 2, 2, 2, hull);
  hLine(x - 1, fy + 3, s + 3, sw(mix1024(wl(K_HULL), wl(K_STARP), 520)));
  fillRect(x + s - 4, fy - 7, 2, 2, wp(K_STARP));                 // red fin flash
  if (day) px(x + s - 2, fy - 1, wp(K_WIN));                      // cockpit glass
}

// The spray the floats throw up, plus the trail of disturbed water astern.
static void drawSpray(int x, int y, uint32_t t) {
  uint16_t c = wl(K_STARW);
  for (int i = 0; i < 9; i++) {
    int sx = x - 2 - i * 3;
    if ((unsigned)sx >= (unsigned)sW) continue;
    int hop = (int)((sinf((float)i * 0.9f + (float)t / 300.0f) + 1.0f) * 2.0f);
    blendPx(sx, y + 1 - hop, c, 430 - i * 32);
    blendPx(sx, y + 2, c, 300 - i * 24);
  }
}

static void drawSeaplane(bool day, uint32_t t) {
  // Harbour Air taxis out across the near lane on a 24 s loop, re-entering from
  // the left each pass. It runs low and the two moored planes sit high, because
  // the usage page leaves only two clear water bands: y 219..237 (between the
  // Weekly footer and "GPU") and y 287..299 (between the temp row and the status
  // row). A plane spans fy-7..fy+6, so those two bands are exactly where a plane
  // fits without a fuselage crossing a glyph.
  int s = 34, y = 293;
  int x = -s - 8 + (int)fmodf((float)t * ((float)sW + 60.0f) / 24000.0f, (float)sW + 60.0f);
  drawSpray(x, y + 4, t);
  drawFloatPlane(x, y, s, day);
  // two more tied up, right of frame, in the upper clear band
  drawFloatPlane(126, 226, 18, day);
  drawFloatPlane(150, 267, 15, day);
  drawGulls(t, 2);
}

// ── variant 4: orca surfacing ───────────────────────────────────────────────

// A resident orca. A 13 s cycle: the back rolls up out of the water, the bull's
// dorsal fin arcs clear, then it sinks again. The inlet is dark, so a true-black
// whale disappeared — the back is a dark slate that reads as a silhouette, and
// the markings (saddle, eye patch) are the only light on it.
static void drawOrca(int cx, uint32_t t) {
  float p = fmodf((float)t, 13000.0f) / 13000.0f;
  float up = sinf(PI_F * p);                                      // 0..1..0
  int surf = 236;
  int bh = (int)(9.0f * up + 0.5f);                               // back showing
  if (bh < 1) return;
  uint16_t body = sw(mix1024(wl(K_SUPER), wl(K_SAIL), 180));
  for (int i = 0; i <= 58; i++) {
    int x = cx - 29 + i;
    int th = (int)((float)bh * (0.30f + 0.70f * sinf(PI_F * (float)i / 58.0f)) + 0.5f);
    if (th < 1) th = 1;
    fillRect(x, surf - th, 1, th, body);
  }
  int fh = (int)(22.0f * up + 0.5f);
  if (fh > 2)
    fillTri(cx - 3, surf - bh, cx + 5, surf - bh, cx + 4, surf - bh - fh, body);
  // the grey saddle patch behind the fin
  for (int i = 0; i < 8; i++)
    blendPx(cx + 8 + i / 2, surf - bh + 1 - (i & 1), wl(K_STARW), 250);
  // the white eye patch, forward of the fin
  for (int i = 0; i < 6; i++)
    blendPx(cx - 17 + i, surf - bh, wl(K_STARW), 300 - i * 24);
  // splash along the waterline, brightest at the top of the roll
  int f = (int)(up * 760.0f);
  for (int i = 0; i < 12; i++)
    blendPx(cx - 32 + i * 6, surf - 1 - (i & 1), wl(K_STARW), f / 2);
}

static void drawOrcaScene(bool day, uint32_t t) {
  (void)day;
  drawOrca(118, t);
  drawGulls(t, 3);
}

// ── variant 5: Stanley Park seawall in blossom ──────────────────────────────

// A cherry bough hanging into the frame from one of the top corners. Two of
// these frame the sky the way the April walk along the seawall does.
//
// Why the top corners: rows 0..60 carry no type on EITHER page (the usage page
// starts at y 64, the weather page at y 72), so a bough there can be bright
// without ever fighting a glyph. The rest of the frame is spoken for — the
// usage page's lowest type is the status row at y 299..308, so there is no free
// band at the bottom either. `dir` = +1 opens to the left, -1 to the right, and
// both stay clear of the moon's disc (x 52..132, y 16..96).
static void drawBlossomBranch(int dir, uint32_t t) {
  uint16_t bark = sw(mix1024(wl(K_ORCA), wl(K_STONE), 780));
  uint16_t pet = sw(mix1024(wl(K_ORCA), wl(K_PETAL), 800));
  uint16_t pet2 = sw(mix1024(wl(K_ORCA), wl(K_PETAL), 560));
  int sway = (int)(1.6f * sinf(TAU_F * t / 6500.0f) + 0.5f);
  const int bx[4] = { 0, 20, 38, 48 };                  // distance out along the bough
  const int by[4] = { 5, 15, 8, 20 };
  for (int i = 0; i < 3; i++) {
    int xa = dir > 0 ? bx[i] : sW - 1 - bx[i], xb = dir > 0 ? bx[i + 1] : sW - 1 - bx[i + 1];
    drawLine(xa, by[i] + sway, xb, by[i + 1] + sway, bark);
    drawLine(xa, by[i] + 1 + sway, xb, by[i + 1] + 1 + sway, bark);
  }
  const int cx4[3] = { 8, 26, 42 };                     // cluster centres
  const int cy4[3] = { 14, 5, 18 };
  for (int c = 0; c < 3; c++) {
    int x0 = dir > 0 ? cx4[c] : sW - 1 - cx4[c];
    int y0 = cy4[c] + sway;
    int r = 9 - (c & 1);
    for (int dy = -r; dy <= r; dy++) {
      int yy = y0 + dy;
      if ((unsigned)yy >= (unsigned)sH) continue;
      float u = (float)dy / (float)r;
      int ww = (int)((float)r * sqrtf(1.0f - u * u) + 0.5f);
      for (int dx = -ww; dx <= ww; dx++) {
        int xx = x0 + dx;
        if ((unsigned)xx >= (unsigned)sW) continue;
        if (((xx * 5 + yy * 3) & 3) == 0) continue;     // dithered edge: sky shows through
        px(xx, yy, ((xx * 3 + yy * 7) & 3) ? pet : pet2);
      }
    }
  }
}

// The seawall itself: stone cap, rail, park path, grass bank, and the two
// boughs overhanging the top corners. Everything below the waterline stays
// near-black on purpose — that band carries the usage page's GPU, temp and
// status rows.
static void drawBlossomFg(bool day, uint32_t t) {
  (void)day;
  // The wall sits in shadow: a full-width lit cap at lum ~90 was a grey band
  // straight through the weather page's clock row (y 232..248), so the stone is
  // knocked down to a silhouette and only the 1-px top edge catches the light.
  fillRect(0, 248, sW, sH - 248, wp(K_GRASS));                    // the park bank
  hLine(0, 248, sW, sw(mix1024(wl(K_GRASS), wl(K_STONE), 420)));  // dark grass edge
  fillRect(0, 241, sW, 7, sw(mix1024(wl(K_STONE), wl(K_ORCA), 620)));  // seawall cap
  hLine(0, 241, sW, sw(mix1024(wl(K_STONE), wl(K_SAIL), 300)));   // lit top edge
  hLine(0, 247, sW, sw(mix1024(wl(K_STONE), wl(K_ORCA), 500)));   // shadow line
  // the rail on the water side of the cap
  uint16_t rail = sw(mix1024(wl(K_STONE), wl(K_ORCA), 300));
  hLine(0, 235, sW, rail);
  for (int x = 6; x < sW; x += 15) vLine(x, 235, 7, rail);
  // the path, a lamp post and a bench below it
  hLine(0, 262, sW, sw(mix1024(wl(K_GRASS), wl(K_STONE), 260)));
  vLine(92, 262, 52, sw(mix1024(wl(K_ORCA), wl(K_STONE), 300)));
  px(92, 260, wp(K_WIN));
  fillRect(60, 296, 16, 2, sw(mix1024(wl(K_ORCA), wl(K_STONE), 420)));
  vLine(61, 298, 5, wp(K_ORCA));
  vLine(74, 298, 5, wp(K_ORCA));
  drawBlossomBranch(1, t);
  drawBlossomBranch(-1, t);
}

// Cherry petals on the breeze — the snow drift re-coloured and made lateral.
static void drawPetals(uint32_t t) {
  static const int16_t px0[10] = { 12, 40, 68, 96, 124, 152, 26, 82, 110, 166 };
  static const int16_t py0[10] = { 18, 58, 34, 88, 14, 74, 128, 118, 54, 99 };
  static const float   pd[10] = { 0.0f, 1.9f, 3.1f, 5.4f, 7.2f, 2.6f, 4.4f, 6.1f, 8.3f, 9.0f };
  uint16_t c = wl(K_PETAL);
  for (int i = 0; i < 10; i++) {
    float p = fracf(t / 11000.0f + pd[i] / 11.0f);
    float e = ramp01(p);
    int x = px0[i] + (int)(-7.0f + 22.0f * e);
    int y = py0[i] + (int)(-6.0f + 150.0f * e);
    int f = (int)(fadeWin(p, 0.15f, 0.85f, 0.62f) * 1024.0f);
    if (f <= 0) continue;
    blendPx(x, y, c, f);
    blendPx(x + 1, y + 1, c, f * 3 / 5);
    blendPx(x - 1, y + 2, c, f / 3);
  }
}

// ── variant dispatch ────────────────────────────────────────────────────────

// Landforms per variant: a day and a night silhouette line-up chosen so the
// subject in front of them has the reading it needs. WXBG_LIONS and
// WXBG_BLOSSOM share the original default.
static void drawLandformsBg(bool day, uint32_t t) {
  if (day) {
    switch (g_wxBg) {
      case WXBG_ANCHOR:                 // further shore: the ships own the middle
        fillPoly(kGrouseX, kGrouseY, 7, wp(K_MTFAR));
        fillPoly(kPineX, kPineY, 7, wp(K_MTMID));
        drawConifers(t);
        return;
      case WXBG_SKYLINE:                // the city bar will cover the lower slopes
        fillPoly(kLionsX, kLionsY, 10, wp(K_MTFAR));
        fillTri(80, 92, 74, 102, 86, 102, sw(mix1024(wl(K_MTFAR), wl(K_CREST), 660)));
        fillTri(125, 78, 118, 90, 132, 90, sw(mix1024(wl(K_MTFAR), wl(K_CREST), 760)));
        fillPoly(kGrouseX, kGrouseY, 7, wp(K_MTMID));
        return;
      case WXBG_SEAPLANE:               // Coal Harbour: the park runs to the water
        fillPoly(kGrouseX, kGrouseY, 7, wp(K_MTFAR));
        fillPoly(kPineX, kPineY, 7, wp(K_MTMID));
        drawConifers(t);
        return;
      case WXBG_ORCA:                   // an open-water view of the same coast
        fillPoly(kLionsX, kLionsY, 10, wp(K_MTFAR));
        fillPoly(kGrouseX, kGrouseY, 7, wp(K_MTMID));
        return;
      default:
        break;
    }
    fillPoly(kLionsX, kLionsY, 10, wp(K_MTFAR));
    {   // Lions glacier crests
      uint16_t c = wp(K_CREST);
      fillTri(80, 92, 74, 102, 86, 102, sw(mix1024(wl(K_MTFAR), c, 660)));
      fillTri(125, 78, 118, 90, 132, 90, sw(mix1024(wl(K_MTFAR), c, 760)));
    }
    fillPoly(kGrouseX, kGrouseY, 7, wp(K_MTMID));
    drawConifers(t);
    return;
  }
  switch (g_wxBg) {
    case WXBG_ANCHOR:
      fillPoly(kGrouseX, kGrouseY, 7, wp(K_PINE));
      return;
    case WXBG_SKYLINE:
      fillPoly(kRidgeX, kRidgeY, 10, wp(K_RIDGE));
      fillTri(84, 96, 79, 104, 89, 104, sw(mix1024(wl(K_RIDGE), wl(K_GLINT), 450)));
      fillTri(132, 82, 126, 92, 138, 92, sw(mix1024(wl(K_RIDGE), wl(K_GLINT), 550)));
      return;
    case WXBG_SEAPLANE:
      fillPoly(kPineX, kPineY, 7, wp(K_PINE));
      return;
    case WXBG_ORCA:
      fillPoly(kRidgeX, kRidgeY, 10, wp(K_RIDGE));
      fillTri(84, 96, 79, 104, 89, 104, sw(mix1024(wl(K_RIDGE), wl(K_GLINT), 450)));
      return;
    default:
      break;
  }
  fillPoly(kRidgeX, kRidgeY, 10, wp(K_RIDGE));
  {   // summit glints under starlight
    uint16_t c = wp(K_GLINT);
    fillTri(84, 96, 79, 104, 89, 104, sw(mix1024(wl(K_RIDGE), c, 450)));
    fillTri(132, 82, 126, 92, 138, 92, sw(mix1024(wl(K_RIDGE), c, 550)));
  }
  fillPoly(kPineX, kPineY, 7, wp(K_PINE));
}

// True when this variant wants the Lions Gate span in frame.
static bool bgHasBridge() {
  return g_wxBg == WXBG_LIONS || g_wxBg == WXBG_ANCHOR || g_wxBg == WXBG_BLOSSOM;
}

// The variant's subject, drawn on top of the water and the light path.
static void drawFeatureBg(bool day, uint32_t t) {
  switch (g_wxBg) {
    case WXBG_ANCHOR:   drawAnchorage(day, t); break;
    case WXBG_SKYLINE:  drawSkylineBg(day, t); break;
    case WXBG_SEAPLANE: drawSeaplane(day, t);  break;
    case WXBG_ORCA:     drawOrcaScene(day, t); break;
    default: break;                            // lions-gate / blossom
  }
}

// Nearest-layer elements, drawn after the bridge so they occlude it.
static void drawFeatureFg(bool day, uint32_t t) {
  if (g_wxBg == WXBG_BLOSSOM) {
    drawBlossomFg(day, t);
    drawPetals(t);
  }
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

// ── row colour sampling (legibility checks + the UI's 1-px header rule) ─────
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
    drawLandformsBg(true, t);
    drawWater(true, t);
    drawLightPath(130, true, t);      // sun path on the inlet
    drawFeatureBg(true, t);
    if (bgHasBridge()) drawBridgeDay();
    if (g_wxBg == WXBG_BLOSSOM) drawFeatureFg(true, t);
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
    drawLandformsBg(false, t);
    drawWater(false, t);
    drawLightPath(92, false, t);      // moon path on the inlet
    drawFeatureBg(false, t);
    if (bgHasBridge()) drawBridgeNight(t);
    if (g_wxBg == WXBG_BLOSSOM) drawFeatureFg(false, t);
    if (precip && fam != WX_SNOW) drawRain(t, fam == WX_STORM);
    if (fam == WX_SNOW) drawSnow(t, false);
    if (fam == WX_STORM) drawGusts(t);
  }
  sampleRows();
}
