// ui.cpp — the two UI scenes, drawn into the active framebuffer on core 0.
// Fonts are bound to the panel object (`lcd`); the effects write the raw buffer
// directly, so a UI scene re-initializes the panel driver to its default state
// before drawing (the consumer's pushSprite then blits the same buffer).
#include "tick.h"
#include "secrets.h"
#include <time.h>

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
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setCursor(barX, y);
  lcd.print(label);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(COL_BRIGHT, COL_BG);
  char p[8]; snprintf(p, sizeof(p), "%d%%", pct);
  lcd.setCursor(barX + barW + 2 - lcd.textWidth(p, &fonts::Font4), y - 2);
  lcd.print(p);
  int barY = y + 21;
  lcd.fillRoundRect(barX, barY, barW, barH, 3, COL_BAR_BG);
  int fillW = (barW * pct) / 100;
  if (fillW > barW) fillW = barW;
  if (fillW > 0) lcd.fillRoundRect(barX, barY, fillW, barH, 3, barColor(pct, accent, warn));
  if (footer && *footer) {
    lcd.setFont(&fonts::Font0);
    lcd.setTextColor(COL_TEXT, COL_BG);
    lcd.setCursor(barX, barY + 17);
    lcd.print(footer);
  }
}

static void statusRow(int dotY, const char* state, bool ok) {
  lcd.fillCircle(24, dotY, 4, ok ? COL_GREEN : COL_RED);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setCursor(34, dotY - 5);
  lcd.print(state);
  if (g_u.fetchedAgo < 60) lcd.printf(" %lus", g_u.fetchedAgo);
  else lcd.printf(" %lum", g_u.fetchedAgo / 60);
  int pages = usagePageCountOf(g_u);
  if (pages > 1)
    for (int p = 0; p < pages; p++) {
      int px = SCREEN_W - 12 - (pages - 1 - p) * 9;
      if (p == g_u.curPage) lcd.fillCircle(px, dotY, 3, COL_BRIGHT);
      else lcd.drawCircle(px, dotY, 3, COL_BAR_BG);
    }
}

static void renderUsage(uint16_t* buf, int w, int h) {
  lcd.fillScreen(COL_BG);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(COL_BLUE, COL_BG);
  lcd.setCursor(14, 8);
  lcd.print("LLM");
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setCursor(14 + lcd.textWidth("LLM", &fonts::Font4) + 10, 10);
  lcd.print(g_u.curPage == 0 ? "Usage" : g_u.curPage == 1 ? "Tokens" : "Models");
  lcd.drawFastHLine(14, 38, w - 28, 0x3186);

  if (!g_u.ok) {
    lcd.setCursor(14, 150);
    lcd.print(g_u.stale ? "stale data" : "connecting...");
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
    lcd.setFont(&fonts::Font0);
    lcd.setTextColor(COL_TEXT, COL_BG);
    lcd.setCursor(14, 42);
    lcd.printf("5h %s  $%.0f/h   wk %s", g_u.tokActive, g_u.burnHr, g_u.tokWeek);
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
  lcd.fillScreen(COL_BG);
  const WeatherData& d = g_wxData;
  if (!d.valid) {
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(COL_TEXT, COL_BG);
    lcd.setCursor(14, 150);
    lcd.print("standby");
    lcd.setCursor(14, 172);
    lcd.print("weather: n/a");
    return;
  }
  char big[8]; snprintf(big, sizeof(big), "%.0f", d.temperature);
  lcd.setFont(&fonts::Font8);
  lcd.setTextColor(COL_BRIGHT, COL_BG);
  lcd.setCursor(16, 60);
  lcd.print(big);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setCursor(16 + lcd.textWidth(big, &fonts::Font8) + 4, 72);
  lcd.print("C");
  lcd.setFont(&fonts::Font2);
  lcd.setCursor(14, 140);
  lcd.print(kIcons[wxIconGlyph(d.condition_code)]);
  lcd.setCursor(14 + 16, 140);
  lcd.print(d.condition.c_str());
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(COL_TEXT, COL_BG);
  char l2[24]; snprintf(l2, sizeof(l2), "H %.0f  L %.0f  RH %d%%",
                       d.temp_high, d.temp_low, d.humidity);
  lcd.setCursor(14, 180);
  lcd.print(l2);
  if (d.aqi > 0) { lcd.setFont(&fonts::Font0); lcd.setCursor(14, 200); lcd.printf("AQI %d", d.aqi); }
  time_t now = time(nullptr);
  struct tm* t = gmtime(&now);
  char clk[16]; strftime(clk, sizeof(clk), "%H:%M", t);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(COL_BLUE, COL_BG);
  lcd.setCursor(14, 232);
  lcd.print(clk);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setCursor(14, 254);
  lcd.print("BOOT: cycle scenes");
}

void renderUiScene(int scene, uint16_t* buf, int w, int h) {
  if (scene == 0) renderUsage(buf, w, h);
  else renderWeather(buf, w, h);
}
