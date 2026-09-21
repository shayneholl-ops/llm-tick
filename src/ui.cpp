// ui.cpp — the two UI scenes, drawn into the active sprite framebuffer on core 0.
// All drawing goes through `ui` (= uiSpr), whose buffer is bound to the current
// frame via setBuffer(); the consumer's pushSprite then blits it to the panel.
// (Drawing through the panel `lcd` directly would go to the panel's own buffer,
//  which is never pushed — that was the black-screen bug.)
#include "tick.h"
#include "secrets.h"
#include <time.h>

// The active UI sprite (buffer bound to the current frame) — all draws target it.
#define ui (*uiSpr)

// UI palette — design values (D_*) and runtime white-balanced copies (COL_*).
// wb565() (tick.h) compensates this unit's cool-cast backlight: the near-
// neutral colors below pre-shift toward green so they render neutral on the
// glass. Ferrari language for the weather scene (DESIGN-ferrari.md):
// near-black canvas (#181818 — "never pure black"), white ink, gray body,
// one scarce Rosso Corsa accent.
static const uint16_t D_BG     = 0x1082, D_BAR_BG = 0x2945, D_TEXT   = 0xC618;
static const uint16_t D_BRIGHT = 0xFFFF, D_BLUE   = 0x3B7F, D_GREEN  = 0x2E8B;
static const uint16_t D_YELLOW = 0xFE60, D_RED    = 0xF800, D_CYAN   = 0x07FA;
static const uint16_t D_PURPLE = 0xA95F, D_ORANGE = 0xFCC0, D_MINT   = 0x2FEB;
static const uint16_t D_INK    = 0xFFFF;  // #ffffff display ink
static const uint16_t D_BODY   = 0x94B2;  // #969696 body gray
static const uint16_t D_MUTED  = 0x632C;  // #666666 muted
static const uint16_t D_ROSSO  = 0xD943;  // #da291c Rosso Corsa (scarce)
static uint16_t COL_BG     = D_BG, COL_BAR_BG = D_BAR_BG, COL_TEXT   = D_TEXT;
static uint16_t COL_BRIGHT = D_BRIGHT, COL_BLUE = D_BLUE, COL_GREEN = D_GREEN;
static uint16_t COL_YELLOW = D_YELLOW, COL_RED = D_RED, COL_CYAN = D_CYAN;
static uint16_t COL_PURPLE = D_PURPLE, COL_ORANGE = D_ORANGE, COL_MINT = D_MINT;
static uint16_t COL_INK = D_INK, COL_BODY = D_BODY, COL_MUTED = D_MUTED, COL_ROSSO = D_ROSSO;
static uint16_t MODEL_ACCENTS[4];
// Rewrite every runtime color with the backlight's white-balance gains; call
// once in setup() before the first frame is rendered.
void uiWbInit() {
  COL_BG = wb565(D_BG);     COL_BAR_BG = wb565(D_BAR_BG); COL_TEXT = wb565(D_TEXT);
  COL_BRIGHT = wb565(D_BRIGHT); COL_BLUE = wb565(D_BLUE); COL_GREEN = wb565(D_GREEN);
  COL_YELLOW = wb565(D_YELLOW); COL_RED = wb565(D_RED);   COL_CYAN = wb565(D_CYAN);
  COL_PURPLE = wb565(D_PURPLE); COL_ORANGE = wb565(D_ORANGE); COL_MINT = wb565(D_MINT);
  COL_INK = wb565(D_INK);   COL_BODY = wb565(D_BODY);   COL_MUTED = wb565(D_MUTED);
  COL_ROSSO = wb565(D_ROSSO);
  MODEL_ACCENTS[0] = COL_PURPLE; MODEL_ACCENTS[1] = COL_MINT;
  MODEL_ACCENTS[2] = COL_CYAN;   MODEL_ACCENTS[3] = COL_GREEN;
}

const int BARS_PER_PAGE = 3;

static uint16_t barColor(int pct, uint16_t accent, bool warn) {
  if (!warn) return accent;
  if (pct >= 90) return COL_RED;
  if (pct >= 75) return COL_YELLOW;
  return accent;
}

// label + right-aligned % + rounded track/fill + small footer
static void drawBar(int y, const char* label, int pct, uint16_t accent,
                    const char* footer, bool warn = true) {
  int barX = 14, barW = 144, barH = 12, w = SCREEN_W;
  ui.setFont(&fonts::Font2);
  ui.setTextColor(COL_TEXT, COL_BG);
  ui.setCursor(barX, y);
  ui.print(label);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(COL_BRIGHT, COL_BG);
  char p[8]; snprintf(p, sizeof(p), "%d%%", pct);
  ui.setCursor(barX + barW + 2 - ui.textWidth(p, &fonts::Font4), y - 2);
  ui.print(p);
  int barY = y + 21;
  ui.fillRoundRect(barX, barY, barW, barH, 3, COL_BAR_BG);
  int fillW = (barW * pct) / 100;
  if (fillW > barW) fillW = barW;
  if (fillW > 0) ui.fillRoundRect(barX, barY, fillW, barH, 3, barColor(pct, accent, warn));
  if (footer && *footer) {
    ui.setFont(&fonts::Font0);
    ui.setTextColor(COL_TEXT, COL_BG);
    ui.setCursor(barX, barY + 17);
    ui.print(footer);
  }
}

static void statusRow(int dotY, const char* state, bool ok) {
  ui.fillCircle(24, dotY, 4, ok ? COL_GREEN : COL_RED);
  ui.setFont(&fonts::Font0);
  ui.setTextColor(COL_TEXT, COL_BG);
  ui.setCursor(34, dotY - 5);
  ui.print(state);
  if (g_u.fetchedAgo < 60) ui.printf(" %lus", g_u.fetchedAgo);
  else ui.printf(" %lum", g_u.fetchedAgo / 60);
  int pages = usagePageCountOf(g_u);
  if (pages > 1)
    for (int p = 0; p < pages; p++) {
      int px = SCREEN_W - 12 - (pages - 1 - p) * 9;
      if (p == g_u.curPage) ui.fillCircle(px, dotY, 3, COL_BRIGHT);
      else ui.drawCircle(px, dotY, 3, COL_BAR_BG);
    }
}

// Top-band flicker fix (2026-09-19): this ST7789's gate-array end (last RAM
// rows, at the glass top edge on this 180-deg-mounted unit) meanders in
// luminance by ~5% (stochastic, needs backlight on, invisible on a uniform
// field). Keep display rows 0-59 PURE BACKGROUND in the UI scenes so the
// meander has no edges to modulate -> imperceptible. See HANDOFF.md.
static void renderUsage(uint16_t* buf, int w, int h) {
  ui.fillScreen(COL_BG);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(COL_BLUE, COL_BG);
  ui.setCursor(14, 64);
  ui.print("LLM");
  ui.setFont(&fonts::Font2);
  ui.setTextColor(COL_TEXT, COL_BG);
  ui.setCursor(14 + ui.textWidth("LLM", &fonts::Font4) + 10, 66);
  ui.print(g_u.curPage == 0 ? "Usage" : g_u.curPage == 1 ? "Tokens" : "Models");
  ui.drawFastHLine(14, 94, w - 28, wb565(0x3186));

  if (!g_u.ok) {
    ui.setCursor(14, 150);
    ui.print(g_u.stale ? "stale data" : "connecting...");
  }

  const int slotY[BARS_PER_PAGE] = { 104, 176, 248 };
  const int slotYtok[BARS_PER_PAGE] = { 112, 184, 256 };   // tokens page has an info row
  char footer[32];

  if (g_u.curPage == 0) {
    int slot = 0;
    snprintf(footer, sizeof(footer), g_u.sessionResetMin >= 60
             ? "resets in %dh%02dm" : "resets in %dm",
             g_u.sessionResetMin / 60, g_u.sessionResetMin % 60);
    drawBar(slotY[slot++], "Session (5h)", g_u.sessionPct, COL_BLUE, footer);
    snprintf(footer, sizeof(footer), "resets %s", g_u.weeklyReset);
    drawBar(slotY[slot++], "Weekly (7d)", g_u.weeklyPct, COL_CYAN, footer);
    if (g_u.spendPct >= 0 && slot < BARS_PER_PAGE) {
      snprintf(footer, sizeof(footer), "%.2f / %.2f %s",
               g_u.spendUsed, g_u.spendLimit, g_u.spendCur);
      drawBar(slotY[slot++], "Credits", g_u.spendPct, COL_ORANGE, footer);
    }
  } else if (g_u.curPage == 1 && g_u.ccOk && g_u.tokModelCount > 0) {
    ui.setFont(&fonts::Font0);
    ui.setTextColor(COL_TEXT, COL_BG);
    ui.setCursor(14, 96);
    ui.printf("5h %s  $%.0f/h   wk %s", g_u.tokActive, g_u.burnHr, g_u.tokWeek);
    for (int i = 0; i < BARS_PER_PAGE && i < g_u.tokModelCount; i++) {
      const TokModel& t = g_u.tokModels[i];
      snprintf(footer, sizeof(footer), "%s tok  $%.2f", t.tok, t.cost);
      drawBar(slotYtok[i], t.name, t.pct, MODEL_ACCENTS[i % 4], footer, false);
    }
  } else {
    int first = (g_u.curPage - 1 - (g_u.ccOk && g_u.tokModelCount > 0 ? 1 : 0)) * BARS_PER_PAGE;
    for (int i = 0; i < BARS_PER_PAGE && first + i < g_u.modelCount; i++) {
      const ModelUsage& m = g_u.models[first + i];
      char label[20]; snprintf(label, sizeof(label), "%s 7d", m.name);
      snprintf(footer, sizeof(footer), "resets %s", m.reset);
      drawBar(slotY[i], label, m.pct, MODEL_ACCENTS[(first + i) % 4], footer);
    }
  }
  statusRow(h - 16, g_u.ok ? g_u.status : "wait", g_u.ok);
}

static int wxIconGlyph(int code) {
  if (code >= 7280) return 2; if (code >= 7120) return 3;
  if (code >= 7010) return 4; if (code >= 6000) return 5;
  if (code >= 5567) return 6; if (code >= 4020) return 7;
  if (code >= 3000) return 8; return 9;
}
static const char* kIcons[] = { "!", "~", "*", ":", "f", "=", "+", "o" };

// Weather standby background (Ferrari design language, DESIGN-ferrari.md):
// the canvas stays near-black (#181818 — "never pure black") and only takes
// a subtle per-condition tint — day/night x clear/cloudy/rain/snow — so the
// scene still breathes with the real weather without becoming a light
// source. All targets sit far below the lum-128 flip, so type is always the
// light set. Rows 0-59 stay one uniform color: top-band flicker fix intact.
static int lum565(uint16_t c) {
  int r = ((c >> 11) & 31) << 3 | ((c >> 11) & 31) >> 2;
  int g = ((c >> 5) & 63) << 2 | ((c >> 5) & 63) >> 4;
  int b = (c & 31) << 3 | (c & 31) >> 2;
  return (r + g + b) / 3;
}
// Move a 0..N channel 1/10 of the way to its target (rounded, always at least
// one step, never overshoots) so the approach never stalls and never jumps.
static int near10(int a, int b) {
  if (a == b) return a;
  int d = b - a;
  int step = (d > 0) ? (d + 9) / 10 : (d - 9) / 10;
  return a + step;
}
// Close 1/10 of the remaining distance to `tgt` per call (once per UI frame)
// and quantize back to 565 — ~1 s of smooth easing at the render rate.
static uint16_t stepToward565(uint16_t cur, uint16_t tgt) {
  int r0 = (cur >> 11) << 3 | (cur >> 8) & 7,  g0 = (cur >> 5) & 63, b0 = cur & 31;
  int r1 = (tgt >> 11) << 3 | (tgt >> 8) & 7,  g1 = (tgt >> 5) & 63, b1 = tgt & 31;
  int r = near10(r0, r1), g = near10(g0, g1), b = near10(b0, b1);
  return (r << 11) | (g << 5) | b;
}
static uint16_t wxBgTarget(const WeatherData& d) {
  if (!d.valid) return wb565(0x18C3);                  // #181818 base canvas
  int code = d.condition_code;
  if (code < 1000)  return d.is_day ? wb565(0x20E2) : wb565(0x18C3);  // clear: warm / neutral
  if (code < 2000)  return d.is_day ? wb565(0x18E3) : wb565(0x10A2);  // cloud: neutral gray
  if (code < 4000)  return d.is_day ? wb565(0x18EC) : wb565(0x1083);  // rain: cool blue
  return d.is_day ? wb565(0x2125) : wb565(0x18E5);        // snow: cool, lighter
}
static uint16_t g_wxBg = COL_BG;   // current (possibly mid-transition) background

static void renderWeather(uint16_t* buf, int w, int h) {
  const WeatherData& d = g_wxData;
  uint16_t target = wxBgTarget(d);
  if (g_wxBg != target) g_wxBg = stepToward565(g_wxBg, target);
  ui.fillScreen(g_wxBg);
  // Type per the Ferrari set: white ink, gray body, muted captions, and the
  // one scarce Rosso accent on the clock (the "race position" role). The
  // luminance flip is kept as a safety net only — every palette entry is
  // dark, so the light set is what actually renders.
  bool bright = lum565(g_wxBg) > 128;
  uint16_t numCol  = bright ? wb565(0x0841) : COL_INK;    // big temperature
  uint16_t subCol  = bright ? wb565(0x30C6) : COL_BODY;   // small text
  uint16_t muteCol = bright ? wb565(0x30E6) : COL_MUTED;  // captions / footer
  uint16_t clkCol  = bright ? wb565(0x0040) : COL_ROSSO;  // the one accent
  if (!d.valid) {
    ui.setFont(&fonts::Font2);
    ui.setTextColor(numCol, g_wxBg);
    ui.setCursor(14, 150);
    ui.print("STANDBY");
    ui.setTextColor(muteCol, g_wxBg);
    ui.setCursor(14, 172);
    ui.print("WEATHER: N/A");
    return;
  }
  char big[8]; snprintf(big, sizeof(big), "%.0f", d.temperature);
  ui.setFont(&fonts::Font8);
  ui.setTextColor(numCol, g_wxBg);
  ui.setCursor(16, 72);
  ui.print(big);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(subCol, g_wxBg);
  ui.setCursor(16 + ui.textWidth(big, &fonts::Font8) + 4, 84);
  ui.print("C");
  // Caption style: uppercase (the bitmap fonts have no tracking).
  char cond[19];
  int cn = d.condition.length(); if (cn > 18) cn = 18;
  for (int i = 0; i < cn; i++) cond[i] = (char)toupper((unsigned char)d.condition[i]);
  cond[cn] = 0;
  ui.setFont(&fonts::Font2);
  ui.setTextColor(subCol, g_wxBg);
  ui.setCursor(14, 140);
  ui.print(kIcons[wxIconGlyph(d.condition_code)]);
  ui.setCursor(14 + 16, 140);
  ui.print(cond);
  ui.setFont(&fonts::Font2);
  ui.setTextColor(subCol, g_wxBg);
  char l2[24]; snprintf(l2, sizeof(l2), "H %.0f  L %.0f  RH %d%%",
                       d.temp_high, d.temp_low, d.humidity);
  ui.setCursor(14, 180);
  ui.print(l2);
  if (d.aqi > 0) { ui.setFont(&fonts::Font0); ui.setTextColor(muteCol, g_wxBg); ui.setCursor(14, 200); ui.printf("AQI %d", d.aqi); }
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);   // board TZ is PST8PDT (Vancouver); gmtime showed UTC
  char clk[16]; strftime(clk, sizeof(clk), "%H:%M", t);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(clkCol, g_wxBg);
  ui.setCursor(14, 232);
  ui.print(clk);
  ui.setFont(&fonts::Font0);
  ui.setTextColor(muteCol, g_wxBg);
  ui.setCursor(14, 254);
  ui.print("PRESS: CYCLE SCENES");
}

void renderUiScene(int scene, uint16_t* buf, int w, int h) {
  // Bind this frame's buffer to the UI sprite, then draw. pushSprite blits it.
  uiSpr->setBuffer(buf, w, h, 16);
  if (scene == 0) renderUsage(buf, w, h);
  else renderWeather(buf, w, h);
}
