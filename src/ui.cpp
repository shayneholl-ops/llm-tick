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
  // Channels stay in NATIVE 5/6/5 space. Expanding them to 8-bit and packing
  // the result back with <<11/<<5 (this function's original form, and mix565/
  // scale565 below) stuffs an 8-bit value into a 5-bit field: every eased
  // colour came out ~8x too bright and hue-shifted, which is what painted the
  // weather field magenta/red instead of the tuned dark tint (2026-09-21).
  int r0 = (cur >> 11) & 31, g0 = (cur >> 5) & 63, b0 = cur & 31;
  int r1 = (tgt >> 11) & 31, g1 = (tgt >> 5) & 63, b1 = tgt & 31;
  int r = near10(r0, r1), g = near10(g0, g1), b = near10(b0, b1);
  return (uint16_t)((r << 11) | (g << 5) | b);
}
static uint16_t wxBgTarget(const WeatherData& d) {
  if (!d.valid) return wb565(0x18C3);                  // #181818 base canvas
  int code = d.condition_code;
  if (code < 1000)  return d.is_day ? wb565(0x20E2) : wb565(0x18C3);  // clear: warm / neutral
  if (code < 2000)  return d.is_day ? wb565(0x18E3) : wb565(0x10A2);  // cloud: neutral gray
  if (code < 4000)  return d.is_day ? wb565(0x18EC) : wb565(0x1083);  // rain: cool blue
  return d.is_day ? wb565(0x2125) : wb565(0x18E5);        // snow: cool, lighter
}
// Blend `a` toward `b` by f1024 (0..1024) per 565 channel — the gradient mixer.
static uint16_t mix565(uint16_t a, uint16_t b, int f1024) {
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  // Native 5/6/5 space (see stepToward565), and the shift applies to the DELTA
  // only — `+` binds tighter than `>>`, so the unparenthesised form shifted the
  // whole sum and wrapped channels into bright garbage (2026-09-21 white-screen
  // bug). Both mistakes together produced the "rainbow field" capture.
  int r  = ar + (((br - ar) * f1024) >> 10);
  int g  = ag + (((bg - ag) * f1024) >> 10);
  int bl = ab + (((bb - ab) * f1024) >> 10);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}
// Scale a 565 color toward black by f1000 (0..1000): derives the darker bottom
// stop of the weather gradient from the tuned top stop (WB-safe — both are
// multiplicative, so the backlight balance is preserved).
static uint16_t scale565(uint16_t c, int f1000) {
  int r = ((c >> 11) & 31) * f1000 / 1000;
  int g = ((c >> 5) & 63) * f1000 / 1000;
  int b = (c & 31) * f1000 / 1000;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
// The weather background is a full-height two-stop vertical gradient
// (2026-09-21): wxBgTarget() is the TOP color — per-condition x day/night
// (standby: base canvas) — deepening to a static ~55%-luminance bottom stop.
// It runs edge to edge with no uniform plateau: an earlier 60-row flat top
// (the old meander guard) put a hard colour band across the glass top sixth,
// because the *plateau edge* was the artifact, not the ramp.
// Both stops ease 1/10 per frame only while a condition changes (~1 s); the
// field itself is never re-animated per frame. All targets stay far below the
// lum-128 flip, so type is always the light set.
static uint16_t g_wxTop = COL_BG;   // current top stop (possibly mid-transition)
static uint16_t g_wxBot = COL_BG;   // current bottom stop (possibly mid-transition)

// ── Weather condition icon — procedural, animated ───────────────────────────
// No image assets: every icon is drawn from primitives, so it costs no flash and
// animates for free (this scene is fully re-rendered every UI frame). Kept
// monochrome per the Ferrari set — white ink for the primary shape, gray body
// for puffs, muted for details/halo/streaks; the Rosso Corsa accent stays on the
// clock alone. Everything lives inside the icon box (plus a few px of falling
// rain/snow) so display rows 0-59 stay pure background (top-band meander guard).
enum WxFam { WX_SUN, WX_MOON, WX_PARTLY_D, WX_PARTLY_N, WX_CLOUD, WX_RAIN, WX_SNOW, WX_STORM, WX_FOG };

static WxFam wxFamily(int code, bool day) {
  if (code == 1000) return day ? WX_SUN : WX_MOON;                       // clear
  if (code == 1003) return day ? WX_PARTLY_D : WX_PARTLY_N;              // partly cloudy
  if (code == 1030 || code == 1135 || code == 1147) return WX_FOG;       // mist / fog
  if (code == 1087 || (code >= 1273 && code <= 1282)) return WX_STORM;   // thunder
  if (code == 1066 || code == 1114 || code == 1117 ||
      (code >= 1204 && code <= 1237) || (code >= 1249 && code <= 1264)) return WX_SNOW;
  if (code == 1063 || code == 1069 || code == 1072 || code == 1150 || code == 1153 ||
      (code >= 1180 && code <= 1201) || (code >= 1240 && code <= 1246)) return WX_RAIN;
  return WX_CLOUD;
}

// One cloud cluster (three puffs over a rounded base) with a small sideways
// drift so it breathes rather than sitting dead still.
static void wxCloud(int x, int y, int s, uint16_t col, int drift) {
  int bw = s * 3 / 4, base = y + s / 2;
  ui.fillCircle(x + drift + bw / 5,     base,          s / 6, col);
  ui.fillCircle(x + drift + bw / 2,     base - s / 9,  s / 5, col);
  ui.fillCircle(x + drift + bw * 4 / 5, base,          s / 7, col);
  ui.fillRoundRect(x + drift + bw / 6,  base, bw * 2 / 3, s / 4, s / 12, col);
}

static void drawWxIcon(int x, int y, int s, WxFam fam, uint16_t ink,
                       uint16_t body, uint16_t mute, uint16_t bg) {
  const uint32_t t = millis();
  const int cx = x + s / 2, cy = y + s / 2;
  switch (fam) {
    case WX_SUN:                                       // rays breathe in and out
      for (int i = 0; i < 8; i++) {
        float a = i * (PI / 4.0f);
        int r0 = s / 5, r1 = s / 5 + 2 + (int)(2.5f * sinf(t / 420.0f + i * 1.7f));
        ui.drawLine(cx + (int)(cosf(a) * r0), cy + (int)(sinf(a) * r0),
                    cx + (int)(cosf(a) * r1), cy + (int)(sinf(a) * r1), ink);
      }
      ui.fillCircle(cx, cy, s / 6, ink);
      break;
    case WX_MOON:                                      // slow halo pulse + crescent
      ui.drawCircle(cx, cy, s / 3 + 1 + (int)(1.5f * sinf(t / 950.0f)), mute);
      ui.fillCircle(cx, cy, s / 5, ink);
      ui.fillCircle(cx + s / 7, cy - s / 9, s / 6, bg);
      break;
    case WX_PARTLY_D:                                  // sun behind a drifting cloud
      for (int i = 0; i < 6; i++) {
        float a = i * (PI / 3.0f);
        int r0 = s / 7, r1 = s / 7 + 2 + (int)(2.0f * sinf(t / 420.0f + i));
        ui.drawLine(x + s / 3 + (int)(cosf(a) * r0), y + s / 3 + (int)(sinf(a) * r0),
                    x + s / 3 + (int)(cosf(a) * r1), y + s / 3 + (int)(sinf(a) * r1), ink);
      }
      ui.fillCircle(x + s / 3, y + s / 3, s / 8, ink);
      wxCloud(x, y + s / 5, s, body, (int)(1.5f * sinf(t / 700.0f)));
      break;
    case WX_PARTLY_N:                                  // moon behind a drifting cloud
      ui.fillCircle(x + s / 3, y + s / 3, s / 7, ink);
      ui.fillCircle(x + s / 3 + s / 12, y + s / 4, s / 9, bg);
      wxCloud(x, y + s / 5, s, body, (int)(1.5f * sinf(t / 700.0f)));
      break;
    case WX_CLOUD:
      wxCloud(x, y + s / 6, s, body, (int)(1.5f * sinf(t / 760.0f)));
      break;
    case WX_RAIN:                                      // streaks fall under the cloud
      wxCloud(x, y, s, body, (int)(1.2f * sinf(t / 800.0f)));
      for (int i = 0; i < 3; i++) {
        int px = x + s / 4 + i * (s / 4);
        int py = y + s * 3 / 5 + (int)((t / 55 + i * 11) % (s / 3));
        ui.drawLine(px, py, px - 1, py + 4, mute);
      }
      break;
    case WX_SNOW:                                      // flakes drift and sway
      wxCloud(x, y, s, body, (int)(1.2f * sinf(t / 800.0f)));
      for (int i = 0; i < 5; i++) {
        int py = y + s * 3 / 5 + (int)((t / 85 + i * 13) % (s / 3));
        int px = x + s / 5 + i * (s / 6) + (int)(2.0f * sinf(t / 340.0f + i));
        ui.fillCircle(px, py, 1, mute);
      }
      break;
    case WX_STORM: {                                   // bolt + a local flash only
      bool flash = (t % 3600) < 110;
      wxCloud(x, y, s, flash ? ink : body, (int)(1.2f * sinf(t / 800.0f)));
      int bx = x + s / 2;
      ui.fillTriangle(bx, y + s * 3 / 5, bx - s / 7, y + s * 3 / 4, bx + s / 14, y + s * 3 / 4,
                      flash ? ink : body);
      ui.fillTriangle(bx + s / 20, y + s * 3 / 5 + s / 8, bx - s / 10, y + s * 4 / 5,
                      bx + s / 6, y + s * 4 / 5, flash ? ink : body);
      break;
    }
    case WX_FOG:
    default:                                           // bands slide sideways
      for (int i = 0; i < 4; i++) {
        int wlen = s * 3 / 5 + (int)((s / 5) * sinf(t / 900.0f + i * 1.3f));
        int fx = x + s / 6 + (int)(3.0f * sinf(t / 1100.0f + i));
        ui.fillRoundRect(fx, y + s / 4 + i * (s / 7), wlen, 2, 1, mute);
      }
      break;
  }
}

static void renderWeather(uint16_t* buf, int w, int h) {
  const WeatherData& d = g_wxData;
  uint16_t topTgt = wxBgTarget(d);
  // The bottom stop is STATIC per condition (calm canvas). The earlier per-frame
  // ~40 s "breath" was pulled on a MISDIAGNOSIS — that capture's stripes were the
  // 8-bit-into-5-bit packing bug in stepToward565/mix565/scale565, not the motion.
  // Static is kept because the design reads as a still canvas and per-frame
  // full-field writes buy nothing.
  uint16_t botTgt = scale565(topTgt, 550);
  if (g_wxTop != topTgt) g_wxTop = stepToward565(g_wxTop, topTgt);
  if (g_wxBot != botTgt) g_wxBot = stepToward565(g_wxBot, botTgt);
  // Full-height background: rows 0..h-1, one direct byte-swapped path.
  // The earlier shape kept rows 0-59 as a uniform plateau (the old meander
  // guard) and started the ramp at 60; on-glass that read as a hard colour band
  // across the top sixth — the plateau edge was the artifact, not the ramp (a
  // flat control field drifts smoothly across the same rows). A smooth ramp has
  // no edge for the top-band quirk to catch, so the guard is not needed here.
  {
    uint16_t* row = buf;
    for (int y = 0; y < h; y++, row += w) {
      uint16_t c = mix565(g_wxTop, g_wxBot, y * 1024 / (h - 1));
      // Raw writes into the sprite need the byte swap (LGFX covers fillRect and
      // text, raw writes do not — same convention as the WB fields). Verified
      // on-glass with alternating 40-row representation bands: swapped rows
      // matched the LGFX-filled reference, unswapped rows came out magenta.
      c = (uint16_t)((c >> 8) | (c << 8));
      for (int x = 0; x < w; x++) row[x] = c;
    }
  }
  // Type per the Ferrari set: white ink, gray body, muted captions, and the
  // one scarce Rosso accent on the clock (the "race position" role). The
  // luminance flip is kept as a safety net only — every palette entry is
  // dark, so the light set is what actually renders. Polarity is judged on
  // the gradient's midpoint (the text zone).
  bool bright = lum565(mix565(g_wxTop, g_wxBot, 512)) > 128;
  uint16_t numCol  = bright ? wb565(0x0841) : COL_INK;    // big temperature
  uint16_t subCol  = bright ? wb565(0x30C6) : COL_BODY;   // small text
  uint16_t muteCol = bright ? wb565(0x30E6) : COL_MUTED;  // captions / footer
  uint16_t clkCol  = bright ? wb565(0x0040) : COL_ROSSO;  // the one accent
  // Background color at a given row — text clips to the local gradient color.
  auto bgAt = [&](int y) -> uint16_t {
    return mix565(g_wxTop, g_wxBot, y * 1024 / (h - 1));
  };
  if (!d.valid) {
    ui.setFont(&fonts::Font2);
    ui.setTextColor(numCol, bgAt(150));
    ui.setCursor(14, 150);
    ui.print("STANDBY");
    ui.setTextColor(muteCol, bgAt(172));
    ui.setCursor(14, 172);
    ui.print("WEATHER: N/A");
    return;
  }
  char big[8]; snprintf(big, sizeof(big), "%.0f", d.temperature);
  ui.setFont(&fonts::Font8);
  ui.setTextColor(numCol, bgAt(72));
  ui.setCursor(16, 72);
  ui.print(big);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(subCol, bgAt(84));
  ui.setCursor(16 + ui.textWidth(big, &fonts::Font8) + 4, 84);
  ui.print("C");
  // Animated condition icon in the free block right of the temperature; shrink
  // it if the reading is wide (e.g. "-10") so the two never collide.
  {
    int tempEnd = 16 + ui.textWidth(big, &fonts::Font8) + 4 + ui.textWidth("C", &fonts::Font4);
    int isz = 48, ix = 116;
    if (tempEnd + 6 > ix) { isz = 40; ix = w - 8 - isz; }
    if (tempEnd + 4 > ix) { isz = 32; ix = w - 6 - isz; }
    drawWxIcon(ix, 62, isz, wxFamily(d.condition_code, d.is_day), numCol, subCol, muteCol, bgAt(62 + isz / 2));
  }
  // Caption style: uppercase (the bitmap fonts have no tracking).
  char cond[19];
  int cn = d.condition.length(); if (cn > 18) cn = 18;
  for (int i = 0; i < cn; i++) cond[i] = (char)toupper((unsigned char)d.condition[i]);
  cond[cn] = 0;
  ui.setFont(&fonts::Font2);
  ui.setTextColor(subCol, bgAt(140));
  ui.setCursor(14, 140);
  ui.print(kIcons[wxIconGlyph(d.condition_code)]);
  ui.setCursor(14 + 16, 140);
  ui.print(cond);
  ui.setFont(&fonts::Font2);
  ui.setTextColor(subCol, bgAt(180));
  char l2[24]; snprintf(l2, sizeof(l2), "H %.0f  L %.0f  RH %d%%",
                       d.temp_high, d.temp_low, d.humidity);
  ui.setCursor(14, 180);
  ui.print(l2);
  if (d.aqi > 0) { ui.setFont(&fonts::Font0); ui.setTextColor(muteCol, bgAt(200)); ui.setCursor(14, 200); ui.printf("AQI %d", d.aqi); }
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);   // board TZ is PST8PDT (Vancouver); gmtime showed UTC
  char clk[16]; strftime(clk, sizeof(clk), "%H:%M", t);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(clkCol, bgAt(232));
  ui.setCursor(14, 232);
  ui.print(clk);
  ui.setFont(&fonts::Font0);
  ui.setTextColor(muteCol, bgAt(254));
  ui.setCursor(14, 254);
  ui.print("PRESS: CYCLE SCENES");
}

void renderUiScene(int scene, uint16_t* buf, int w, int h) {
  // Bind this frame's buffer to the UI sprite, then draw. pushSprite blits it.
  uiSpr->setBuffer(buf, w, h, 16);
  if (scene == 0) renderUsage(buf, w, h);
  else renderWeather(buf, w, h);
}
