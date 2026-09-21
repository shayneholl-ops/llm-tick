// cyberpunk.cpp — the "cyberpunk" ambient scene (scene 2) for llm-tick.
//
// Five procedural sub-scenes auto-cycle (~9–14 s each, short black hold
// between), all drawn straight into llm-tick's raw RGB565 framebuffer:
//
//   0  MATRIX RAIN       falling ASCII + half-width katakana columns
//   1  CITY SKYLINE      procedural night skyline, blinking windows, aircraft
//   2  GLITCH STORM      channel-split tear bands, block noise, sweep line
//   3  HEX DUMP SCROLL   terminal mem-dump with ERROR / ALERT injections
//   4  NEURAL NET PULSE  jittered node grid with pulses on the edges
//
// PROVENANCE — the scene *concepts* come from
//   Oxpr0x/Waveshare-ESP32-S3-Cyberpunk-Display  ("cyberpunk_no_sd" variant,
//   a landscape 320x172 TFT_eSPI sketch). That repository declares no license,
//   so NO code was copied from it: this file is a fresh implementation for
//   llm-tick's portrait 172x320 panel and its dual-core buffer pipeline.
//   The 8x8 glyph DATA it uses IS vendored (src/font8x8.h) — see that file's
//   header for the public-domain / OFL provenance.
//
// Display notes for THIS unit: the glass is mounted 180° (board.h), so this
// code draws in display coordinates exactly like ui.cpp does. Colours are run
// through wb565() (the cool-cast backlight compensation in tick.h) and then
// byte-swapped, because llm-tick pushes raw 16-bit pixels into an LGFX sprite
// (the same reason the old effect palettes were pre-swapped).
#include <Arduino.h>
#include "cyberpunk.h"
#include "font8x8.h"
#include "tick.h"

// ── palette ─────────────────────────────────────────────────────────────────
#define CP_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

enum {
  C_BG, C_BG_DARK,
  C_GREEN, C_GREEN_HI, C_GREEN_BR, C_GREEN_MID, C_GREEN_DIM,
  C_CYAN, C_CYAN_DIM, C_PURPLE, C_PURPLE_DIM, C_PINK, C_PINK_DIM,
  C_AMBER, C_AMBER_DIM, C_RED, C_RED_DIM, C_WHITE,
  C_BLDG, C_BLDG2, C_WIN_OFF, C_HEXBAR,
  C_N
};

static const uint16_t kBase[C_N] = {
  CP_RGB(10, 10, 18),  CP_RGB(3, 3, 8),
  CP_RGB(0, 255, 65),  CP_RGB(200, 255, 215), CP_RGB(0, 200, 60),
  CP_RGB(0, 120, 40),  CP_RGB(0, 55, 20),
  CP_RGB(0, 229, 255), CP_RGB(0, 90, 110),
  CP_RGB(157, 0, 255), CP_RGB(58, 0, 96),
  CP_RGB(255, 0, 144), CP_RGB(90, 0, 50),
  CP_RGB(255, 179, 0), CP_RGB(110, 70, 0),
  CP_RGB(255, 32, 32), CP_RGB(90, 10, 10),
  0xFFFF,
  CP_RGB(13, 13, 22),  CP_RGB(20, 18, 32),  CP_RGB(6, 6, 12),
  CP_RGB(6, 8, 16)
};

static uint16_t cpool[C_N];                 // wb565 + byte-swapped, built once

// Palette lookup (constant colours) and cw() for computed ones (gradients,
// random glitch colours). Both end up panel-ready.
static inline uint16_t cp(int i) { return cpool[i]; }
static inline uint16_t cw(uint16_t raw) {
  uint16_t c = wb565(raw);
  return (uint16_t)((c >> 8) | (c << 8));
}

// ── drawing shim over the raw framebuffer ───────────────────────────────────
// The render task runs on core 0 and is the only writer, so file-static
// target state is safe (same style as the reference sketch).
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

static inline void fillScreen(uint16_t c) { fillRect(0, 0, sW, sH, c); }

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

static void fillCircle(int cx, int cy, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)sqrtf((float)(r * r - dy * dy));
    hLine(cx - dx, cy + dy, 2 * dx + 1, c);
  }
}

static void drawCircle(int cx, int cy, int r, uint16_t c) {
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    px(cx + x, cy + y, c); px(cx + y, cy + x, c);
    px(cx - y, cy + x, c); px(cx - x, cy + y, c);
    px(cx - x, cy - y, c); px(cx - y, cy - x, c);
    px(cx + y, cy - x, c); px(cx + x, cy - y, c);
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}

// 8x8 glyph, LSB-first bit order (bit0 = leftmost pixel, see font8x8.h).
static void blitGlyph(int x, int y, int gidx, uint16_t fg, uint16_t bg) {
  if (gidx < 0 || gidx > 150) return;
  if (x <= -8 || x >= sW || y <= -8 || y >= sH) return;
  const uint8_t* g = FONT8[gidx];
  for (int r = 0; r < 8; r++) {
    int yy = y + r;
    if ((unsigned)yy >= (unsigned)sH) continue;
    uint8_t bits = pgm_read_byte(&g[r]);
    uint16_t* row = sBuf + (size_t)yy * sW;
    for (int b = 0; b < 8; b++) {
      int xx = x + b;
      if ((unsigned)xx >= (unsigned)sW) continue;
      row[xx] = (bits & (1 << b)) ? fg : bg;
    }
  }
}

static void drawText(const char* s, int x, int y, uint16_t fg, uint16_t bg) {
  for (; *s; s++, x += 8) {
    int ch = (uint8_t)*s;
    if (ch < 32 || ch > 126) continue;
    blitGlyph(x, y, ch - 32, fg, bg);
  }
}

static uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t) {
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + ((br - ar) * t) / 255;
  int g = ag + ((bg - ag) * t) / 255;
  int l = ab + ((bb - ab) * t) / 255;
  return (uint16_t)((r << 11) | (g << 5) | l);
}

// ── scene 0: matrix rain ────────────────────────────────────────────────────
#define MAT_CW 8
#define MAT_CH 8
#define MAT_COLS_MAX 24
#define MAT_TRAIL 4
#define MAT_POOL_N 240

static uint8_t  matPool[MAT_POOL_N];
static int      matPoolN = 0;
static uint8_t  matGlyph[MAT_COLS_MAX];
static int16_t  matRow[MAT_COLS_MAX];
static uint8_t  matSpd[MAT_COLS_MAX];
static int      matCols = 0, matRows = 40, matX0 = 0;

static void matrixInit() {
  matCols = sW / MAT_CW;
  if (matCols > MAT_COLS_MAX) matCols = MAT_COLS_MAX;
  matRows = sH / MAT_CH;
  matX0 = (sW - matCols * MAT_CW) / 2;

  fillScreen(cp(C_BG));

  matPoolN = 0;
  const char* ascii = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:.<>[]{}#*+=/\\|";
  for (const char* p = ascii; *p && matPoolN < MAT_POOL_N; p++)
    matPool[matPoolN++] = (uint8_t)((uint8_t)*p - 32);          // ASCII glyph index
  for (uint16_t u = 0xFF66; u <= 0xFF9D && matPoolN < MAT_POOL_N; u++)
    matPool[matPoolN++] = (uint8_t)(95 + (u - 0xFF66));         // katakana glyph index

  for (int c = 0; c < matCols; c++) {
    matGlyph[c] = matPool[random(matPoolN)];
    matRow[c]   = (int16_t)-random(0, matRows + 6);
    matSpd[c]   = 1 + (uint8_t)random(0, 3);
  }
}

static void matrixFrame() {
  uint16_t bg = cp(C_BG);
  for (int c = 0; c < matCols; c++) {
    int x = matX0 + c * MAT_CW;
    int16_t h = matRow[c];
    uint8_t g = matGlyph[c];

    blitGlyph(x, (h - MAT_TRAIL) * MAT_CH, g, bg, bg);                    // erase tail
    blitGlyph(x, (h - 3) * MAT_CH, g, cp(C_GREEN_DIM), bg);
    blitGlyph(x, (h - 2) * MAT_CH, g, cp(C_GREEN_MID), bg);
    blitGlyph(x, (h - 1) * MAT_CH, g, cp(C_GREEN_BR),  bg);
    blitGlyph(x,  h      * MAT_CH, g, cp(C_GREEN_HI),  bg);               // bright head

    matRow[c] += matSpd[c];

    if (matRow[c] - MAT_TRAIL > matRows) {          // ran off the bottom
      matRow[c]   = (int16_t)-random(0, 12);
      matGlyph[c] = matPool[random(matPoolN)];
      if (random(0, 100) < 30) matSpd[c] = 1 + (uint8_t)random(0, 3);
    } else if (random(0, 100) < 5) {                // occasional scan glitch
      hLine(0, random(sH), sW, random(3) ? cp(C_GREEN_DIM) : cp(C_CYAN_DIM));
    }
  }
}

// ── scene 1: city skyline ───────────────────────────────────────────────────
#define CITY_MAXB 16
#define CITY_MAXWR 20
#define CITY_MAXWC 12
#define CITY_SKY_TOP 148        // horizon band: buildings stand below this

struct CityBldg {
  int16_t  x, w, top;
  uint8_t  wc, wr;
  int16_t  gx, gy, gw, gh;
  uint16_t win[CITY_MAXWR];     // bit c set => window lit
};

static CityBldg city[CITY_MAXB];
static int      cityN = 0;

struct Aircraft { int16_t x, y; int8_t vx; uint8_t blink; };
static Aircraft craft;

// Raw (pre-palette) sunset gradient: deep blue at the top -> purple -> warm glow
// at the horizon, where the buildings stand.
static uint16_t skyColor(int y) {
  if (y < 0) y = 0;
  if (y >= sH) y = sH - 1;
  uint16_t t = (uint16_t)((int32_t)y * 255 / (sH - 1));
  if (t < 140)
    return lerp565(CP_RGB(3, 2, 10), CP_RGB(70, 0, 84), (uint8_t)((int32_t)t * 255 / 140));
  return lerp565(CP_RGB(70, 0, 84), CP_RGB(255, 110, 22), (uint8_t)(((int32_t)t - 140) * 255 / 115));
}

static void cityDrawWindow(int bi, int r, int c) {
  if (bi < 0 || bi >= cityN) return;
  const CityBldg* b = &city[bi];
  if (r >= b->wr || c >= b->wc) return;
  int wx = b->gx + c * b->gw;
  int wy = b->gy + r * b->gh;
  if (wy + 1 >= sH) return;
  uint16_t col = cp(C_WIN_OFF);
  if ((b->win[r] >> c) & 1) {
    col = (((r * 7 + c * 13 + b->x) & 3) == 0) ? cp(C_CYAN) : cp(C_AMBER);
    if (((r + c + b->x) & 7) == 3) col = cp(C_AMBER_DIM);
  }
  fillRect(wx, wy, 2, 2, col);
}

static void cityInit() {
  for (int y = 0; y < sH; y++) hLine(0, y, sW, cw(skyColor(y)));

  cityN = 0;
  int x = -random(0, 8);
  while (x < sW && cityN < CITY_MAXB) {
    CityBldg* b = &city[cityN];
    b->w   = (int16_t)random(14, 34);
    int16_t h = (int16_t)random(60, 170);
    b->top = (int16_t)(sH - h);
    if (b->top < CITY_SKY_TOP) b->top = CITY_SKY_TOP;
    b->x   = (int16_t)x;

    b->gx = b->x + 3;
    b->gy = b->top + 5;
    b->gw = 4;
    b->gh = 5;
    int wc = (b->w - 5) / b->gw;
    int wr = (sH - b->gy - 2) / b->gh;
    if (wc > CITY_MAXWC) wc = CITY_MAXWC;
    if (wr > CITY_MAXWR) wr = CITY_MAXWR;
    if (wc < 0) wc = 0;
    if (wr < 0) wr = 0;
    b->wc = (uint8_t)wc;
    b->wr = (uint8_t)wr;

    uint16_t body = ((cityN & 3) == 1) ? cp(C_BLDG2) : cp(C_BLDG);
    fillRect(b->x, b->top, b->w, sH - b->top, body);

    if (h > 140) {                       // antenna + beacon on the tall ones
      int ax = b->x + b->w / 2;
      vLine(ax, b->top - 8, 8, cp(C_BLDG2));
      px(ax, b->top - 9, cp(C_RED_DIM));
    }

    for (int r = 0; r < b->wr; r++) {    // seed ~38% lit
      uint16_t mask = 0;
      for (int c = 0; c < b->wc; c++)
        if (random(0, 100) < 38) mask |= (1u << c);
      b->win[r] = mask;
    }
    for (int r = 0; r < b->wr; r++)
      for (int c = 0; c < b->wc; c++)
        cityDrawWindow(cityN, r, c);

    cityN++;
    x += b->w + random(1, 4);
  }

  craft.y     = (int16_t)random(6, 118);
  int8_t dir  = random(2) ? 1 : -1;
  craft.x     = (dir > 0) ? -6 : (sW + 6);
  craft.vx    = dir * (int8_t)random(1, 3);
  craft.blink = 0;
}

static void cityFrame() {
  for (int i = 0; i < 14 && cityN > 0; i++) {          // blink a few windows
    int bi = random(cityN);
    CityBldg* b = &city[bi];
    if (b->wr == 0 || b->wc == 0) continue;
    int r = random(b->wr), c = random(b->wc);
    b->win[r] ^= (1u << c);
    cityDrawWindow(bi, r, c);
  }
  if (random(0, 100) < 12 && cityN > 0) {              // sweep a whole column
    int bi = random(cityN);
    CityBldg* b = &city[bi];
    if (b->wc > 0) {
      int c = random(b->wc);
      for (int r = 0; r < b->wr; r++) {
        b->win[r] ^= (1u << c);
        cityDrawWindow(bi, r, c);
      }
    }
  }

  fillRect(craft.x - 2, craft.y, 6, 1, cw(skyColor(craft.y)));   // erase trail
  craft.x += craft.vx;
  craft.blink++;
  if (craft.x < -8 || craft.x > sW + 8) {
    craft.y     = (int16_t)random(6, 118);
    int8_t dir  = random(2) ? 1 : -1;
    craft.x     = (dir > 0) ? -6 : (sW + 6);
    craft.vx    = dir * (int8_t)random(1, 3);
  }
  if (craft.blink & 0x08) {
    fillRect(craft.x, craft.y, 3, 1, cp(C_RED));
    px(craft.x + 1, craft.y - 1, cp(C_WHITE));
  } else {
    fillRect(craft.x, craft.y, 3, 1, cw(skyColor(craft.y)));
  }
}

// ── scene 2: glitch storm ───────────────────────────────────────────────────
static int glitchSweep = 0;

static uint16_t glitchColorRaw() {
  switch (random(0, 11)) {
    case 0:  return kBase[C_RED];
    case 1:  return kBase[C_GREEN];
    case 2:  return kBase[C_CYAN];
    case 3:  return kBase[C_PURPLE];
    case 4:  return kBase[C_PINK];
    case 5:  return kBase[C_AMBER];
    case 6:  return kBase[C_WHITE];
    case 7:  return kBase[C_GREEN_DIM];
    case 8:  return kBase[C_CYAN_DIM];
    case 9:  return kBase[C_PINK_DIM];
    default: return CP_RGB((uint8_t)random(20, 90), (uint8_t)random(20, 90), (uint8_t)random(20, 90));
  }
}
static inline uint16_t pickCol(uint16_t a, uint16_t b) { return random(2) ? a : b; }

static void glitchInit() {
  fillScreen(cp(C_BG_DARK));
  glitchSweep = 0;
}

static void glitchFrame() {
  int r = random(0, 100);
  if (r < 22)      fillScreen(cp(C_BG_DARK));
  else if (r < 27) fillScreen(cp(C_RED_DIM));
  else if (r < 31) fillScreen(cw(CP_RGB(0, 25, 30)));

  int bands = random(10, 26);
  for (int i = 0; i < bands; i++) {
    uint16_t c0 = cw(glitchColorRaw());
    int y  = random(sH);
    int h  = random(1, 10);
    int bx = random(-60, sW);
    int w  = random(40, sW + 80);
    fillRect(bx, y, w, h, c0);
    fillRect(bx + random(-14, 15), y + 1, w, 1, cw(pickCol(kBase[C_RED], kBase[C_CYAN])));
    fillRect(bx + random(-14, 15), y + h, w, 1, cw(pickCol(kBase[C_GREEN], kBase[C_PINK])));
  }

  int blocks = random(60, 170);
  for (int i = 0; i < blocks; i++)
    fillRect(random(sW), random(sH), random(2, 10), random(2, 10), cw(glitchColorRaw()));

  int vbars = random(2, 6);
  for (int i = 0; i < vbars; i++)
    fillRect(random(sW), 0, random(1, 4), sH,
             cw(pickCol(glitchColorRaw(), pickCol(kBase[C_PURPLE], kBase[C_PINK]))));

  glitchSweep += random(3, 15);
  if (glitchSweep >= sH) glitchSweep = 0;
  hLine(0, glitchSweep, sW, cp(C_WHITE));
  hLine(0, glitchSweep + 1, sW, cp(C_CYAN_DIM));

  if (random(0, 100) < 55) {                 // corrupt status text
    static const char* msgs[] = { "SIGNAL LOST", "0xDEADBEEF", "SEGFAULT",
                                  "KERNEL PANIC", "ERR://0x0000", "NO CARRIER" };
    const char* m = msgs[random(6)];
    drawText(m, random(-20, sW - 60), random(8, sH - 16), cw(glitchColorRaw()), cp(C_BG_DARK));
  }
}

// ── scene 3: hex dump scroll ────────────────────────────────────────────────
#define HEX_BYTES 3               // 172 px / 8 px = 21 columns -> 3 bytes fit
#define HEX_LINES 64
#define HEX_STEP_MS 130

struct HexLine { uint16_t addr; uint8_t kind; uint8_t data[HEX_BYTES]; };   // kind 0 ok, 1 ERROR, 2 ALERT

static HexLine  hexbuf[HEX_LINES];
static int      hexHead = 0;
static uint16_t hexAddr = 0;
static uint32_t hexLast = 0;
static int      hexRows = 38;
static uint8_t  hexCursorOn = 0;

static void hexMakeLine() {
  hexHead = (hexHead + 1) % HEX_LINES;
  HexLine* L = &hexbuf[hexHead];
  L->addr = hexAddr;
  for (int i = 0; i < HEX_BYTES; i++) L->data[i] = (uint8_t)random(256);
  int r = random(0, 100);
  L->kind = 0;
  if (r < 4)       L->kind = 2;
  else if (r < 11) L->kind = 1;
  hexAddr = (uint16_t)(hexAddr + HEX_BYTES);
  if (hexAddr > 0xEF00) hexAddr = 0x1000;
}

static void hexFormat(const HexLine* L, char* out, size_t n) {
  if (L->kind == 1) { snprintf(out, n, "!!ERROR %04X RETRY", (unsigned)L->addr); return; }
  if (L->kind == 2) { snprintf(out, n, "##ALERT %04X ACCESS", (unsigned)L->addr); return; }
  int p = snprintf(out, n, "%04X  ", (unsigned)L->addr);
  for (int i = 0; i < HEX_BYTES && p < (int)n - 8; i++)
    p += snprintf(out + p, n - p, "%02X ", L->data[i]);
  if (p < (int)n - 6) out[p++] = '|';
  for (int i = 0; i < HEX_BYTES && p < (int)n - 2; i++) {
    uint8_t c = L->data[i];
    out[p++] = (c >= 32 && c < 127) ? (char)c : '.';
  }
  if (p < (int)n - 1) out[p++] = '|';
  out[p] = '\0';
}

static const int hexAgeIdx[8] = {
  C_GREEN_HI, C_GREEN, C_GREEN, C_GREEN_BR, C_GREEN_BR, C_GREEN_MID, C_GREEN_MID, C_GREEN_DIM
};

static void hexInit() {
  fillScreen(cp(C_BG));
  hexRows = (sH - 16) / 8;
  if (hexRows > 38) hexRows = 38;
  hexAddr = (uint16_t)(0x1000 + random(0, 0xDF00));
  hexHead = 0;
  for (int i = 0; i < HEX_LINES; i++) hexMakeLine();
  hexLast = 0;
}

static void hexFrame() {
  uint32_t now = millis();
  if (now - hexLast < HEX_STEP_MS) return;
  hexLast = now;
  hexMakeLine();

  int textH = hexRows * 8;
  fillRect(0, 0, sW, textH, cp(C_BG));
  char buf[40];
  for (int row = 0; row < hexRows; row++) {
    int idx = (hexHead - row + HEX_LINES * 2) % HEX_LINES;
    const HexLine* L = &hexbuf[idx];
    hexFormat(L, buf, sizeof(buf));
    int y = (hexRows - 1 - row) * 8;
    uint16_t col = (L->kind == 2) ? cp(C_AMBER)
                 : (L->kind == 1) ? cp(C_RED)
                 : cp(hexAgeIdx[row < 8 ? row : 7]);
    drawText(buf, 2, y, col, cp(C_BG));
  }

  uint16_t bar = cp(C_HEXBAR);
  fillRect(0, textH, sW, sH - textH, bar);
  drawText("SYS://core.trace", 2, textH + 4, cp(C_CYAN), bar);
  drawText("LIVE", sW - 42, textH + 4, cp(C_GREEN), bar);
  hexCursorOn ^= 1;
  if (hexCursorOn) drawText("_", sW - 10, textH + 4, cp(C_PINK), bar);
}

// ── scene 4: neural net pulse ───────────────────────────────────────────────
#define NN_MAXNODES 32
#define NN_MAXEDGES 64
#define NN_PULSES 14

struct NNNode  { int16_t x, y; uint8_t glow; };
struct NNEdge  { uint8_t a, b; };
struct NNPulse { int8_t edge; uint8_t t, spd; };

static NNNode  nnNodes[NN_MAXNODES];
static NNEdge  nnEdges[NN_MAXEDGES];
static int     nnN = 0, nnEdgeN = 0;
static NNPulse nnPulses[NN_PULSES];
static int     nnCols = 4, nnRowCount = 6;

static bool nnEdgeExists(uint8_t a, uint8_t b) {
  for (int i = 0; i < nnEdgeN; i++)
    if ((nnEdges[i].a == a && nnEdges[i].b == b) ||
        (nnEdges[i].a == b && nnEdges[i].b == a)) return true;
  return false;
}

static void nnAddEdge(uint8_t a, uint8_t b) {
  if (a == b || nnEdgeN >= NN_MAXEDGES || nnEdgeExists(a, b)) return;
  nnEdges[nnEdgeN].a = a;
  nnEdges[nnEdgeN].b = b;
  nnEdgeN++;
}

static void nnInit() {
  fillScreen(cp(C_BG));
  nnEdgeN = 0;
  nnCols = 4;
  nnRowCount = 6;
  nnN = nnCols * nnRowCount;
  if (nnN > NN_MAXNODES) nnN = NN_MAXNODES;

  int i = 0;
  for (int r = 0; r < nnRowCount; r++) {
    for (int c = 0; c < nnCols; c++) {
      int x = 30 + c * ((sW - 60) / (nnCols - 1));
      int y = 40 + r * ((sH - 80) / (nnRowCount - 1));
      x += random(-12, 13);
      y += random(-14, 15);
      nnNodes[i].x = (int16_t)constrain(x, 12, sW - 12);
      nnNodes[i].y = (int16_t)constrain(y, 22, sH - 12);
      nnNodes[i].glow = 0;
      i++;
    }
  }

  for (int a = 0; a < nnN; a++) {
    int made = 0;
    for (int tryN = 0; tryN < 14 && made < 3; tryN++) {
      int b = random(nnN);
      if (b == a) continue;
      int32_t dx = nnNodes[a].x - nnNodes[b].x;
      int32_t dy = nnNodes[a].y - nnNodes[b].y;
      if (dx * dx + dy * dy > 95 * 95) continue;
      if (nnEdgeExists((uint8_t)a, (uint8_t)b)) continue;
      nnAddEdge((uint8_t)a, (uint8_t)b);
      made++;
    }
  }

  for (int p = 0; p < NN_PULSES; p++) {
    if (nnEdgeN == 0) { nnPulses[p].edge = -1; continue; }
    nnPulses[p].edge = (int8_t)random(nnEdgeN);
    nnPulses[p].t    = (uint8_t)random(256);
    nnPulses[p].spd  = (uint8_t)random(5, 16);
  }
}

static void nnFrame() {
  fillScreen(cp(C_BG));

  for (int e = 0; e < nnEdgeN; e++) {
    const NNNode* a = &nnNodes[nnEdges[e].a];
    const NNNode* b = &nnNodes[nnEdges[e].b];
    drawLine(a->x, a->y, b->x, b->y, cp(C_PURPLE_DIM));
  }

  for (int p = 0; p < NN_PULSES; p++) {
    NNPulse* pu = &nnPulses[p];
    if (pu->edge < 0 || nnEdgeN == 0) continue;
    const NNNode* a = &nnNodes[nnEdges[pu->edge].a];
    const NNNode* b = &nnNodes[nnEdges[pu->edge].b];

    int x = a->x + (int32_t)(b->x - a->x) * pu->t / 255;
    int y = a->y + (int32_t)(b->y - a->y) * pu->t / 255;
    fillCircle(x, y, 2, cp(C_CYAN));

    uint8_t tt = (pu->t > 26) ? (uint8_t)(pu->t - 26) : 0;
    int tx = a->x + (int32_t)(b->x - a->x) * tt / 255;
    int ty = a->y + (int32_t)(b->y - a->y) * tt / 255;
    fillCircle(tx, ty, 1, cp(C_CYAN_DIM));

    int nt = (int)pu->t + (int)pu->spd;
    if (nt >= 255) {
      nnNodes[nnEdges[pu->edge].b].glow = 255;
      pu->edge = (int8_t)random(nnEdgeN);
      pu->t    = 0;
      pu->spd  = (uint8_t)random(5, 16);
    } else {
      pu->t = (uint8_t)nt;
    }
  }

  for (int n = 0; n < nnN; n++) {
    uint8_t g = nnNodes[n].glow;
    uint16_t col;
    if      (g > 170) col = cp(C_WHITE);
    else if (g > 105) col = cp(C_CYAN);
    else if (g > 45)  col = cp(C_PURPLE);
    else              col = cp(C_PURPLE_DIM);
    int rad = 2 + (g >> 6);
    fillCircle(nnNodes[n].x, nnNodes[n].y, rad, col);
    if (g > 70) drawCircle(nnNodes[n].x, nnNodes[n].y, rad + 2, cp(C_CYAN_DIM));
    nnNodes[n].glow = (g > 11) ? (uint8_t)(g - 11) : 0;
  }
}

// ── sub-scene manager ───────────────────────────────────────────────────────
#define CYBER_SUBS 5
#define CYBER_FRAME_MS 30      // ~33 fps pacing (the panel push is the real limit)

static const uint32_t kDur[CYBER_SUBS] = { 12000, 14000, 9000, 12000, 13000 };

static int      sSub = 0;
static uint32_t sSubStart = 0;
static uint32_t sLastStep = 0;
static int      sHold = 0;     // >0 = black-hold frames before the next sub-scene
static bool     sNeedInit = true;

static void startSub(int s) {
  sSub = s;
  sSubStart = millis();
  switch (s) {
    case 0: matrixInit(); break;
    case 1: cityInit();   break;
    case 2: glitchInit(); break;
    case 3: hexInit();    break;
    default: nnInit();    break;
  }
  sLastStep = millis();
  static const char* kName[CYBER_SUBS] = { "matrix", "city", "glitch", "hexdump", "neural" };
  Serial.printf("[cyber] sub-scene %d (%s)\n", s, kName[s]);
}

void cyberInit() {
  randomSeed(esp_random());
  for (int i = 0; i < C_N; i++) {
    uint16_t c = wb565(kBase[i]);
    cpool[i] = (uint16_t)((c >> 8) | (c << 8));
  }
  sSub = 0;
  sHold = 0;
  sNeedInit = true;          // first cyberFrame() paints sub-scene 0 into a real buffer
}

void cyberFrame(uint16_t* buf, int w, int h) {
  sBuf = buf; sW = w; sH = h;
  uint32_t now = millis();

  if (sNeedInit) { sNeedInit = false; startSub(0); return; }

  if (sHold > 0) {
    // Short black hold + a collapsing scan line: a deliberate cut between
    // sub-scenes (a per-frame interlace fade would flicker, because the two
    // framebuffers alternate and would hold different fade levels).
    fillScreen(0);
    hLine(0, (sHold - 1) * sH / 3, sW, cp(C_CYAN_DIM));
    if (++sHold > 3) { sHold = 0; startSub((sSub + 1) % CYBER_SUBS); }
    return;
  }

  if (now - sSubStart >= kDur[sSub]) { sHold = 1; return; }
  if (now - sLastStep < CYBER_FRAME_MS) return;
  sLastStep = now;

  switch (sSub) {
    case 0: matrixFrame(); break;
    case 1: cityFrame();   break;
    case 2: glitchFrame(); break;
    case 3: hexFrame();    break;
    default: nnFrame();    break;
  }
}
