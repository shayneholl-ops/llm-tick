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

static const uint16_t COL_BG     = 0x1082, COL_BAR_BG = 0x2945, COL_TEXT   = 0xC618;
static const uint16_t COL_BRIGHT = 0xFFFF, COL_BLUE   = 0x3B7F, COL_GREEN  = 0x2E8B;
static const uint16_t COL_YELLOW = 0xFE60, COL_RED    = 0xF800, COL_CYAN   = 0x07FA;
static const uint16_t COL_PURPLE = 0xA95F, COL_ORANGE = 0xFCC0, COL_MINT   = 0x2FEB;
static const uint16_t MODEL_ACCENTS[4] = { COL_PURPLE, COL_MINT, COL_CYAN, COL_GREEN };

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

static void renderUsage(uint16_t* buf, int w, int h) {
  ui.fillScreen(COL_BG);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(COL_BLUE, COL_BG);
  ui.setCursor(14, 8);
  ui.print("LLM");
  ui.setFont(&fonts::Font2);
  ui.setTextColor(COL_TEXT, COL_BG);
  ui.setCursor(14 + ui.textWidth("LLM", &fonts::Font4) + 10, 10);
  ui.print(g_u.curPage == 0 ? "Usage" : g_u.curPage == 1 ? "Tokens" : "Models");
  ui.drawFastHLine(14, 38, w - 28, 0x3186);

  if (!g_u.ok) {
    ui.setCursor(14, 150);
    ui.print(g_u.stale ? "stale data" : "connecting...");
  }

  const int slotY[BARS_PER_PAGE] = { 52, 128, 204 };
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
    ui.setCursor(14, 42);
    ui.printf("5h %s  $%.0f/h   wk %s", g_u.tokActive, g_u.burnHr, g_u.tokWeek);
    for (int i = 0; i < BARS_PER_PAGE && i < g_u.tokModelCount; i++) {
      const TokModel& t = g_u.tokModels[i];
      snprintf(footer, sizeof(footer), "%s tok  $%.2f", t.tok, t.cost);
      drawBar(slotY[i], t.name, t.pct, MODEL_ACCENTS[i % 4], footer, false);
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
  statusRow(h - 24, g_u.ok ? g_u.status : "wait", g_u.ok);
}

static int wxIconGlyph(int code) {
  if (code >= 7280) return 2; if (code >= 7120) return 3;
  if (code >= 7010) return 4; if (code >= 6000) return 5;
  if (code >= 5567) return 6; if (code >= 4020) return 7;
  if (code >= 3000) return 8; return 9;
}
static const char* kIcons[] = { "!", "~", "*", ":", "f", "=", "+", "o" };

static void renderWeather(uint16_t* buf, int w, int h) {
  ui.fillScreen(COL_BG);
  const WeatherData& d = g_wxData;
  if (!d.valid) {
    ui.setFont(&fonts::Font2);
    ui.setTextColor(COL_TEXT, COL_BG);
    ui.setCursor(14, 150);
    ui.print("standby");
    ui.setCursor(14, 172);
    ui.print("weather: n/a");
    return;
  }
  char big[8]; snprintf(big, sizeof(big), "%.0f", d.temperature);
  ui.setFont(&fonts::Font8);
  ui.setTextColor(COL_BRIGHT, COL_BG);
  ui.setCursor(16, 60);
  ui.print(big);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(COL_TEXT, COL_BG);
  ui.setCursor(16 + ui.textWidth(big, &fonts::Font8) + 4, 72);
  ui.print("C");
  ui.setFont(&fonts::Font2);
  ui.setCursor(14, 140);
  ui.print(kIcons[wxIconGlyph(d.condition_code)]);
  ui.setCursor(14 + 16, 140);
  ui.print(d.condition.c_str());
  ui.setFont(&fonts::Font2);
  ui.setTextColor(COL_TEXT, COL_BG);
  char l2[24]; snprintf(l2, sizeof(l2), "H %.0f  L %.0f  RH %d%%",
                       d.temp_high, d.temp_low, d.humidity);
  ui.setCursor(14, 180);
  ui.print(l2);
  if (d.aqi > 0) { ui.setFont(&fonts::Font0); ui.setCursor(14, 200); ui.printf("AQI %d", d.aqi); }
  time_t now = time(nullptr);
  struct tm* t = gmtime(&now);
  char clk[16]; strftime(clk, sizeof(clk), "%H:%M", t);
  ui.setFont(&fonts::Font4);
  ui.setTextColor(COL_BLUE, COL_BG);
  ui.setCursor(14, 232);
  ui.print(clk);
  ui.setFont(&fonts::Font0);
  ui.setTextColor(COL_TEXT, COL_BG);
  ui.setCursor(14, 254);
  ui.print("BOOT: cycle scenes");
}

void renderUiScene(int scene, uint16_t* buf, int w, int h) {
  // Bind this frame's buffer to the UI sprite, then draw. pushSprite blits it.
  uiSpr->setBuffer(buf, w, h, 16);
  if (scene == 0) renderUsage(buf, w, h);
  else renderWeather(buf, w, h);
}
