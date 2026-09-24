// ui.cpp — the two UI scenes, drawn into the active sprite framebuffer on core 0.
// All drawing goes through `ui` (= uiSpr), whose buffer is bound to the current
// frame via setBuffer(); the consumer's pushSprite then blits it to the panel.
// (Drawing through the panel `lcd` directly would go to the panel's own buffer,
//  which is never pushed — that was the black-screen bug.)
#include "tick.h"
#include "secrets.h"
#include "wxscene.h"
#include <time.h>

// The active UI sprite (buffer bound to the current frame) — all draws target it.
#define ui (*uiSpr)

// UI palette — design values (D_*) and runtime white-balanced copies (COL_*).
// wb565() (tick.h) compensates this unit's cool-cast backlight: the near-
// neutral colors below pre-shift toward green so they render neutral on the
// glass. Weather scene palette: near-black canvas (#181818 — "never pure
// black"), white ink, gray body, one scarce red accent on the clock.
static const uint16_t D_BG     = 0x1082, D_BAR_BG = 0x2945, D_TEXT   = 0xC618;
static const uint16_t D_BRIGHT = 0xFFFF, D_BLUE   = 0x3B7F, D_GREEN  = 0x2E8B;
static const uint16_t D_YELLOW = 0xFE60, D_RED    = 0xF800, D_CYAN   = 0x07FA;
static const uint16_t D_PURPLE = 0xA95F, D_ORANGE = 0xFCC0, D_MINT   = 0x2FEB;
static const uint16_t D_INK    = 0xFFFF;  // #ffffff display ink
static const uint16_t D_BODY   = 0x94B2;  // #969696 body gray
static const uint16_t D_MUTED  = 0x632C;  // #666666 muted
static const uint16_t D_ROSSO  = 0xD943;  // #da291c red (scarce)
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

// ── Shared animated background (2026-09-24 unification) ─────────────────────
// The user asked for the weather background on the LLM usage page too, so both
// scenes read as one surface: the same Cascadia scene (day coast / night
// aurora, driven by the one weather reading), each scene's UI knocked out
// against the local scene colour of its own rows (bgAt). wxSceneRender is
// stateless — every animation phase derives from millis() — so the background
// is frame-identical on both scenes and a PRESS switch changes only the
// overlay, never the scene behind it. No valid reading -> flat standby canvas
// (the old look).
static uint16_t bgAt(int y) { return wxRowColor(y); }
// Half-brightness of a scene row — the 1-px header rule rides the local colour.
static uint16_t dimRow(uint16_t c) {
  return (uint16_t)((((c >> 11) & 31) << 10) | ((((c >> 5) & 63) >> 1) << 5) | ((c & 31) >> 1));
}
static WxFam wxFamily(int code, bool day);   // defined below, shared with the icon
static void drawSharedBg(uint16_t* buf, int w, int h) {
  bool day = g_wxData.valid && g_wxData.is_day && !g_wxNightForce;
  int fam = g_wxData.valid ? (int)wxFamily(g_wxData.condition_code, day) : (int)WX_SUN;
  wxSceneRender(buf, w, h, fam, day, g_wxData.valid);
}

const int BARS_PER_PAGE = 3;

static uint16_t barColor(int pct, uint16_t accent, bool warn) {
  if (!warn) return accent;
  if (pct >= 90) return COL_RED;
  if (pct >= 75) return COL_YELLOW;
  return accent;
}

// label + right-aligned % + rounded track/fill + small footer
// `value`, when given, replaces the percentage as the big right-aligned number
// (2026-09-21, user request: the usage page shows actual token counts instead of
// percentages). The bar still visualises the proportion, so the budget context
// is not lost — only the headline number changes.
static void drawBar(int y, const char* label, int pct, uint16_t accent,
                    const char* footer, bool warn = true, const char* value = nullptr) {
  int barX = 14, barW = 144, barH = 12, w = SCREEN_W;
  ui.setFont(&fonts::Font2);
  ui.setTextColor(COL_TEXT, bgAt(y));
  ui.setCursor(barX, y);
  ui.print(label);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(COL_BRIGHT, bgAt(y - 2));
  char p[12];
  if (value && *value) snprintf(p, sizeof(p), "%s", value);
  else snprintf(p, sizeof(p), "%d%%", pct);
  // Auto-fit: a token count can be wider than a percentage ("12.3M" vs "64%"),
  // and the label sits immediately left of this number — drop to Font2 when the
  // wide font would collide with it (2026-09-21).
  int labelEnd = barX + ui.textWidth(label, &fonts::Font2);
  int avail = barX + barW + 2 - labelEnd - 6;
  if ((int)ui.textWidth(p, &fonts::Font4) > avail) ui.setFont(&fonts::Font2);
  ui.setCursor(barX + barW + 2 - ui.textWidth(p, &fonts::Font4), y - 2);
  ui.print(p);
  int barY = y + 21;
  ui.fillRoundRect(barX, barY, barW, barH, 3, COL_BAR_BG);
  int fillW = (barW * pct) / 100;
  if (fillW > barW) fillW = barW;
  if (fillW > 0) ui.fillRoundRect(barX, barY, fillW, barH, 3, barColor(pct, accent, warn));
  if (footer && *footer) {
    ui.setFont(&fonts::Font0);
    ui.setTextColor(COL_TEXT, bgAt(barY + 17));
    ui.setCursor(barX, barY + 17);
    ui.print(footer);
  }
}

// The GPU row: live load as the big right-aligned number (same grammar as the
// budget rows, so the page reads consistently) over a load bar, with the model
// host's temperatures underneath. Data comes from server.py, which samples
// rocm-smi on that host over SSH — this board cannot reach the GPU itself.
// Temperature turns amber at 80C and red at 90C; edge is the headline figure and
// hotspot (junction) rides along when the card reports it.
static void drawGpuRow(int y) {
  int barX = 14, barW = 144, barH = 12;
  const bool ok = g_u.gpuOk && g_u.gpuLoadPct >= 0;
  ui.setFont(&fonts::Font2);
  ui.setTextColor(COL_TEXT, bgAt(y));
  ui.setCursor(barX, y);
  ui.print("GPU");
  char v[16];
  if (ok) snprintf(v, sizeof(v), "%d%%", g_u.gpuLoadPct);
  else snprintf(v, sizeof(v), "--");
  ui.setFont(&fonts::Font4);
  ui.setTextColor(COL_BRIGHT, bgAt(y - 2));
  int avail = barX + barW + 2 - (barX + ui.textWidth("GPU", &fonts::Font2)) - 6;
  if ((int)ui.textWidth(v, &fonts::Font4) > avail) ui.setFont(&fonts::Font2);
  ui.setCursor(barX + barW + 2 - ui.textWidth(v, &fonts::Font4), y - 2);
  ui.print(v);
  int barY = y + 21;
  ui.fillRoundRect(barX, barY, barW, barH, 3, COL_BAR_BG);
  if (ok && g_u.gpuLoadPct > 0) {
    int load = g_u.gpuLoadPct > 100 ? 100 : g_u.gpuLoadPct;
    ui.fillRoundRect(barX, barY, (barW * load) / 100, barH, 3, COL_GREEN);
  }
  ui.setFont(&fonts::Font0);
  ui.setCursor(barX, barY + 17);
  if (!ok) {
    ui.setTextColor(COL_TEXT, bgAt(barY + 17));
    ui.print("gpu unavailable");
    return;
  }
  float t = g_u.gpuTempC;
  ui.setTextColor(t >= 90 ? COL_RED : (t >= 80 ? COL_YELLOW : COL_TEXT), bgAt(barY + 17));
  if (g_u.gpuTempJunctionC > 0) ui.printf("%.0fC  hs %.0fC", t, g_u.gpuTempJunctionC);
  else ui.printf("%.0fC", t);
}

static void statusRow(int dotY, const char* state, bool ok) {
  ui.fillCircle(24, dotY, 4, ok ? COL_GREEN : COL_RED);
  ui.setFont(&fonts::Font0);
  ui.setTextColor(COL_TEXT, bgAt(dotY - 5));
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
  drawSharedBg(buf, w, h);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(COL_BLUE, bgAt(64));
  ui.setCursor(14, 64);
  ui.print("LLM");
  ui.setFont(&fonts::Font2);
  ui.setTextColor(COL_TEXT, bgAt(66));
  ui.setCursor(14 + ui.textWidth("LLM", &fonts::Font4) + 10, 66);
  ui.print(g_u.curPage == 0 ? "Usage" : g_u.curPage == 1 ? "Tokens" : "Models");
  ui.drawFastHLine(14, 94, w - 28, dimRow(bgAt(94)));

  if (!g_u.ok) {
    ui.setTextColor(COL_TEXT, bgAt(150));
    ui.setCursor(14, 150);
    ui.print(g_u.stale ? "stale data" : "connecting...");
  }

  const int slotY[BARS_PER_PAGE] = { 104, 176, 248 };
  const int slotYtok[BARS_PER_PAGE] = { 112, 184, 256 };   // tokens page has an info row
  char footer[32];

  if (g_u.curPage == 0) {
    // Row pitch is normally 68 px (the airy layout this page has always had).
    // Credits is dead in this deployment (server.py hardcodes spend_pct=-1) but
    // still supported: if it ever appears the page needs four rows, so tighten
    // to 48 px — four rows at the airy pitch would run into the status row.
    const bool hasCredits = (g_u.spendPct >= 0);
    const int pitch = hasCredits ? 48 : 68;
    int y = 104;
    snprintf(footer, sizeof(footer), g_u.sessionResetMin >= 60
             ? "resets in %dh%02dm" : "resets in %dm",
             g_u.sessionResetMin / 60, g_u.sessionResetMin % 60);
    // Headline number = real token count (server sends tok_active / tok_week as
    // "225K" / "1.2M"); the bar keeps the percentage as its proportion. Falls
    // back to the percentage if the token source did not answer.
    drawBar(y, "Session (5h)", g_u.sessionPct, COL_BLUE, footer, true, g_u.ccOk ? g_u.tokActive : nullptr);
    y += pitch;
    snprintf(footer, sizeof(footer), "resets %s", g_u.weeklyReset);
    drawBar(y, "Weekly (7d)", g_u.weeklyPct, COL_CYAN, footer, true, g_u.ccOk ? g_u.tokWeek : nullptr);
    y += pitch;
    if (hasCredits) {
      snprintf(footer, sizeof(footer), "%.2f / %.2f %s",
               g_u.spendUsed, g_u.spendLimit, g_u.spendCur);
      drawBar(y, "Credits", g_u.spendPct, COL_ORANGE, footer);
      y += pitch;
    }
    drawGpuRow(y);
  } else if (g_u.curPage == 1 && g_u.ccOk && g_u.tokModelCount > 0) {
    ui.setFont(&fonts::Font0);
    ui.setTextColor(COL_TEXT, bgAt(96));
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

// Weather standby background: the canvas stays near-black (#181818 — "never
// pure black") and only takes a subtle per-condition tint — day/night x
// clear/cloudy/rain/snow — so the scene still breathes with the real weather
// without becoming a light source. All targets sit far below the lum-128
// flip, so type is always the light set. The ramp runs full-height — the old
// 60-row uniform plateau turned out to be the visible band, so it's gone.
static int lum565(uint16_t c) {
  int r = ((c >> 11) & 31) << 3 | ((c >> 11) & 31) >> 2;
  int g = ((c >> 5) & 63) << 2 | ((c >> 5) & 63) >> 4;
  int b = (c & 31) << 3 | (c & 31) >> 2;
  return (r + g + b) / 3;
}
// ── Weather background ──────────────────────────────────────────────────────
// The standby background is the animated "Cascadia" scene in wxscene.cpp — a
// day coast (sun, mist, the Lions, conifers, sea, bridge) and a night aurora
// nocturne — ported from the in-repo design export
// stitch_animated_lvgl_weather_backgrounds/. It replaces the two-stop gradient
// that used to live here (wxBgTarget/mix565/scale565, deleted 2026-09-22).
//
// Two lessons from that code still bind, and wxscene.cpp follows both:
//   * all channel maths stays in NATIVE 5/6/5 space — widening to 8-bit and
//     repacking with <<11/<<5 painted every eased colour ~8x too bright (the
//     2026-09-21 "rainbow field" capture);
//   * a shift inside a mix must apply to the DELTA only — `+` binds tighter
//     than `>>`, so shifting the whole sum wrapped channels into garbage.


// ── Weather condition icon — procedural, animated ───────────────────────────
// No image assets: every icon is drawn from primitives, so it costs no flash and
// animates for free (this scene is fully re-rendered every UI frame). Kept
// monochrome — white ink for the primary shape, gray body for puffs, muted for
// details/halo/streaks; the red accent stays on the clock alone. Everything
// lives inside the icon box (plus a few px of falling
// rain/snow) so display rows 0-59 stay pure background (top-band meander guard).
// (WxFam itself lives in wxscene.h — the icon and the background scene share it.)

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
  // Day/night comes from the reading's own is_day, unless the WXN serial
  // diagnostic is forcing the night scene for a camera check. The same `day`
  // drives the condition icon, so a forced preview flips sun -> moon too.
  bool day = d.is_day && !g_wxNightForce;
  WxFam fam = wxFamily(d.condition_code, day);
  drawSharedBg(buf, w, h);
  // Type: white ink, gray body, muted captions, and the one scarce red accent
  // on the clock. The luminance flip is kept as a safety net only — every
  // palette entry in the scene is dark, so the light set is what actually
  // renders. Polarity is judged on the scene colour at the text zone.
  bool bright = lum565(wxRowColor(h / 2)) > 128;
  uint16_t numCol  = bright ? wb565(0x0841) : COL_INK;    // big temperature
  uint16_t subCol  = bright ? wb565(0x30C6) : COL_BODY;   // small text
  uint16_t muteCol = bright ? wb565(0x30E6) : COL_MUTED;  // captions / footer
  uint16_t clkCol  = bright ? wb565(0x0040) : COL_ROSSO;  // the one accent
  // Text clips to the scene colour actually rendered at that row (bgAt —
  // sampled back out of the framebuffer by wxscene.cpp), so the knock-out
  // always lands on whatever is behind it — sky, mist, mountain, or water.
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
    drawWxIcon(ix, 62, isz, fam, numCol, subCol, muteCol, bgAt(62 + isz / 2));
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
